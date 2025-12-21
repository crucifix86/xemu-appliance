/*
 * xemu WiFi Network Management
 * Uses iw and wpa_supplicant for WiFi operations
 */

#include "xemu-wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char wifi_interface[32] = {0};
static bool wifi_initialized = false;
static xemu_wifi_network_t networks[XEMU_WIFI_MAX_NETWORKS];
static int network_count = 0;
static char current_ssid[XEMU_WIFI_SSID_MAX] = {0};

int xemu_wifi_init(void)
{
    FILE *fp;
    char line[256];

    if (wifi_initialized) {
        return 0;
    }

    /* Find wireless interface using iw */
    fp = popen("iw dev 2>/dev/null | grep Interface | head -1 | awk '{print $2}'", "r");
    if (!fp) {
        return -1;
    }

    if (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) > 0) {
            strncpy(wifi_interface, line, sizeof(wifi_interface) - 1);
            wifi_initialized = true;
        }
    }
    pclose(fp);

    if (!wifi_initialized) {
        /* Try /sys/class/net as fallback */
        fp = popen("ls /sys/class/net/ 2>/dev/null | while read iface; do "
                   "if [ -d /sys/class/net/$iface/wireless ]; then echo $iface; break; fi; done", "r");
        if (fp) {
            if (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\n")] = 0;
                if (strlen(line) > 0) {
                    strncpy(wifi_interface, line, sizeof(wifi_interface) - 1);
                    wifi_initialized = true;
                }
            }
            pclose(fp);
        }
    }

    return wifi_initialized ? 0 : -1;
}

bool xemu_wifi_available(void)
{
    return wifi_initialized && wifi_interface[0] != '\0';
}

const char* xemu_wifi_get_interface(void)
{
    return wifi_interface[0] ? wifi_interface : NULL;
}

bool xemu_wifi_check_rfkill(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rfkill unblock wifi 2>/dev/null");
    system(cmd);
    return true;
}

int xemu_wifi_scan(void)
{
    FILE *fp;
    char line[512];
    char cmd[256];
    int idx = -1;

    if (!wifi_initialized) {
        return -1;
    }

    network_count = 0;
    memset(networks, 0, sizeof(networks));

    /* Ensure interface is up */
    snprintf(cmd, sizeof(cmd), "ip link set %s up 2>/dev/null", wifi_interface);
    system(cmd);

    /* Unblock rfkill */
    xemu_wifi_check_rfkill();

    /* Scan using iw */
    snprintf(cmd, sizeof(cmd), "iw dev %s scan 2>/dev/null", wifi_interface);
    fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) && network_count < XEMU_WIFI_MAX_NETWORKS) {
        /* New BSS entry */
        if (strstr(line, "BSS ") == line) {
            idx = network_count;
            network_count++;
            networks[idx].signal_strength = 0;
            networks[idx].encrypted = false;
            networks[idx].connected = false;
            networks[idx].ssid[0] = '\0';
        }

        if (idx < 0) continue;

        /* Parse SSID */
        char *p = strstr(line, "SSID: ");
        if (p) {
            p += 6;
            p[strcspn(p, "\n")] = 0;
            /* Skip hidden networks */
            if (strlen(p) > 0) {
                strncpy(networks[idx].ssid, p, XEMU_WIFI_SSID_MAX - 1);
            }
        }

        /* Parse signal strength */
        p = strstr(line, "signal: ");
        if (p) {
            float dbm;
            if (sscanf(p, "signal: %f dBm", &dbm) == 1) {
                /* Convert dBm to percentage (roughly -30 = 100%, -90 = 0%) */
                int pct = (int)((dbm + 90) * 100 / 60);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                networks[idx].signal_strength = pct;
            }
        }

        /* Check for encryption */
        if (strstr(line, "WPA") || strstr(line, "RSN") || strstr(line, "Privacy")) {
            networks[idx].encrypted = true;
        }
    }
    pclose(fp);

    /* Remove entries with empty SSID */
    int valid_count = 0;
    for (int i = 0; i < network_count; i++) {
        if (networks[i].ssid[0] != '\0') {
            if (i != valid_count) {
                networks[valid_count] = networks[i];
            }
            valid_count++;
        }
    }
    network_count = valid_count;

    /* Check which one we're connected to */
    xemu_wifi_is_connected();

    return network_count;
}

int xemu_wifi_get_count(void)
{
    return network_count;
}

const xemu_wifi_network_t* xemu_wifi_get_network(int index)
{
    if (index < 0 || index >= network_count) {
        return NULL;
    }
    return &networks[index];
}

bool xemu_wifi_connect(const char *ssid, const char *password)
{
    char cmd[512];
    char conf_path[] = "/tmp/wpa_xemu.conf";
    FILE *fp;

    if (!wifi_initialized || !ssid) {
        return false;
    }

    /* Kill any existing wpa_supplicant */
    system("pkill -9 wpa_supplicant 2>/dev/null");
    usleep(500000);

    /* Create wpa_supplicant config */
    fp = fopen(conf_path, "w");
    if (!fp) {
        return false;
    }

    fprintf(fp, "ctrl_interface=/var/run/wpa_supplicant\n");
    fprintf(fp, "update_config=1\n\n");
    fprintf(fp, "network={\n");
    fprintf(fp, "    ssid=\"%s\"\n", ssid);

    if (password && strlen(password) > 0) {
        fprintf(fp, "    psk=\"%s\"\n", password);
        fprintf(fp, "    key_mgmt=WPA-PSK\n");
    } else {
        fprintf(fp, "    key_mgmt=NONE\n");
    }
    fprintf(fp, "}\n");
    fclose(fp);

    /* Ensure /var/run/wpa_supplicant exists */
    system("mkdir -p /var/run/wpa_supplicant 2>/dev/null");

    /* Start wpa_supplicant */
    snprintf(cmd, sizeof(cmd),
             "wpa_supplicant -B -i %s -c %s 2>/dev/null",
             wifi_interface, conf_path);

    if (system(cmd) != 0) {
        return false;
    }

    usleep(1000000); /* Wait 1 second for connection */

    /* Request DHCP */
    snprintf(cmd, sizeof(cmd), "dhclient %s 2>/dev/null &", wifi_interface);
    system(cmd);

    /* Update current SSID */
    strncpy(current_ssid, ssid, XEMU_WIFI_SSID_MAX - 1);

    return true;
}

bool xemu_wifi_disconnect(void)
{
    char cmd[128];

    system("pkill -9 wpa_supplicant 2>/dev/null");
    system("pkill -9 dhclient 2>/dev/null");

    snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", wifi_interface);
    system(cmd);

    current_ssid[0] = '\0';
    return true;
}

bool xemu_wifi_is_connected(void)
{
    FILE *fp;
    char line[256];
    char cmd[128];

    if (!wifi_initialized) {
        return false;
    }

    snprintf(cmd, sizeof(cmd), "iw dev %s link 2>/dev/null", wifi_interface);
    fp = popen(cmd, "r");
    if (!fp) {
        return false;
    }

    current_ssid[0] = '\0';

    while (fgets(line, sizeof(line), fp)) {
        char *p = strstr(line, "SSID: ");
        if (p) {
            p += 6;
            p[strcspn(p, "\n")] = 0;
            strncpy(current_ssid, p, XEMU_WIFI_SSID_MAX - 1);

            /* Mark as connected in network list */
            for (int i = 0; i < network_count; i++) {
                networks[i].connected = (strcmp(networks[i].ssid, current_ssid) == 0);
            }
            pclose(fp);
            return true;
        }
    }
    pclose(fp);
    return false;
}

const char* xemu_wifi_get_current_ssid(void)
{
    if (current_ssid[0] != '\0') {
        return current_ssid;
    }
    return NULL;
}
