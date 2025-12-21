/*
 * xemu WiFi Network Management
 * Uses iw and wpa_supplicant for WiFi operations
 */

#include "xemu-wifi.h"
#include "xemu-net.h"
#include "xemu-settings.h"
#include "xemu-notifications.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <arpa/inet.h>

/* External function from nvnet.c for built-in DHCP server */
extern void nvnet_set_dhcp_config(uint32_t client_ip, uint32_t gateway, uint32_t server_ip);

static char wifi_interface[32] = {0};
static bool wifi_initialized = false;
static xemu_wifi_network_t networks[XEMU_WIFI_MAX_NETWORKS];
static int network_count = 0;
static char current_ssid[XEMU_WIFI_SSID_MAX] = {0};

#define WIFI_LOG_PATH "/home/xbox/wifi.log"

static void wifi_log(const char *fmt, ...)
{
    FILE *log = fopen(WIFI_LOG_PATH, "a");
    if (!log) {
        /* Try alternate paths */
        log = fopen("/tmp/wifi.log", "a");
    }
    if (!log) {
        log = fopen("/var/log/wifi.log", "a");
    }
    if (log) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        fprintf(log, "[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);
        va_list args;
        va_start(args, fmt);
        vfprintf(log, fmt, args);
        fprintf(log, "\n");
        fflush(log);
        va_end(args);
        fclose(log);
    }
}

int xemu_wifi_init(void)
{
    FILE *fp;
    char line[256];

    wifi_log("=== xemu_wifi_init called ===");

    if (wifi_initialized) {
        wifi_log("Already initialized, interface=%s", wifi_interface);
        return 0;
    }

    /* Find wireless interface using iw */
    wifi_log("Trying iw dev...");
    fp = popen("iw dev 2>/dev/null | grep Interface | head -1 | awk '{print $2}'", "r");
    if (!fp) {
        wifi_log("popen failed for iw");
        return -1;
    }

    if (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = 0;
        wifi_log("iw returned: '%s'", line);
        if (strlen(line) > 0) {
            strncpy(wifi_interface, line, sizeof(wifi_interface) - 1);
            wifi_initialized = true;
        }
    }
    pclose(fp);

    if (!wifi_initialized) {
        /* Try /sys/class/net as fallback */
        wifi_log("Trying /sys/class/net fallback...");
        fp = popen("ls /sys/class/net/ 2>/dev/null | while read iface; do "
                   "if [ -d /sys/class/net/$iface/wireless ]; then echo $iface; break; fi; done", "r");
        if (fp) {
            if (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\n")] = 0;
                wifi_log("fallback returned: '%s'", line);
                if (strlen(line) > 0) {
                    strncpy(wifi_interface, line, sizeof(wifi_interface) - 1);
                    wifi_initialized = true;
                }
            }
            pclose(fp);
        }
    }

    wifi_log("Init result: initialized=%d, interface=%s", wifi_initialized, wifi_interface);
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
    snprintf(cmd, sizeof(cmd), "sudo rfkill unblock wifi 2>/dev/null");
    system(cmd);
    return true;
}

static int xemu_wifi_scan_iw(void)
{
    FILE *fp;
    char line[512];
    char cmd[256];
    int idx = -1;

    /* Scan using iw (nl80211) - needs sudo */
    snprintf(cmd, sizeof(cmd), "sudo iw dev %s scan 2>/dev/null", wifi_interface);
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
            if (strlen(p) > 0) {
                strncpy(networks[idx].ssid, p, XEMU_WIFI_SSID_MAX - 1);
            }
        }

        /* Parse signal strength */
        p = strstr(line, "signal: ");
        if (p) {
            float dbm;
            if (sscanf(p, "signal: %f dBm", &dbm) == 1) {
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

    return network_count;
}

static int xemu_wifi_scan_iwlist(void)
{
    FILE *fp;
    char line[512];
    char cmd[256];
    int idx = -1;

    /* Scan using iwlist (wext - for broadcom wl driver) - needs sudo */
    snprintf(cmd, sizeof(cmd), "sudo iwlist %s scan 2>/dev/null", wifi_interface);
    fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) && network_count < XEMU_WIFI_MAX_NETWORKS) {
        /* New Cell entry */
        if (strstr(line, "Cell ") && strstr(line, "Address:")) {
            idx = network_count;
            network_count++;
            networks[idx].signal_strength = 0;
            networks[idx].encrypted = false;
            networks[idx].connected = false;
            networks[idx].ssid[0] = '\0';
        }

        if (idx < 0) continue;

        /* Parse ESSID */
        char *p = strstr(line, "ESSID:\"");
        if (p) {
            p += 7;
            char *end = strchr(p, '"');
            if (end && end > p) {
                size_t len = end - p;
                if (len >= XEMU_WIFI_SSID_MAX) len = XEMU_WIFI_SSID_MAX - 1;
                strncpy(networks[idx].ssid, p, len);
                networks[idx].ssid[len] = '\0';
            }
        }

        /* Parse signal level (Quality or Signal level) */
        p = strstr(line, "Signal level=");
        if (p) {
            int dbm;
            if (sscanf(p, "Signal level=%d dBm", &dbm) == 1) {
                int pct = (dbm + 90) * 100 / 60;
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                networks[idx].signal_strength = pct;
            } else {
                /* Try percentage format */
                int pct;
                if (sscanf(p, "Signal level=%d/100", &pct) == 1) {
                    networks[idx].signal_strength = pct;
                }
            }
        }
        /* Also try Quality format */
        p = strstr(line, "Quality=");
        if (p && networks[idx].signal_strength == 0) {
            int qual, max_qual;
            if (sscanf(p, "Quality=%d/%d", &qual, &max_qual) == 2 && max_qual > 0) {
                networks[idx].signal_strength = (qual * 100) / max_qual;
            }
        }

        /* Check for encryption */
        if (strstr(line, "Encryption key:on")) {
            networks[idx].encrypted = true;
        }
        if (strstr(line, "WPA") || strstr(line, "WPA2")) {
            networks[idx].encrypted = true;
        }
    }
    pclose(fp);

    return network_count;
}

int xemu_wifi_scan(void)
{
    char cmd[256];

    if (!wifi_initialized) {
        return -1;
    }

    network_count = 0;
    memset(networks, 0, sizeof(networks));

    /* Ensure interface is up */
    snprintf(cmd, sizeof(cmd), "sudo ip link set %s up 2>/dev/null", wifi_interface);
    system(cmd);

    /* Unblock rfkill */
    xemu_wifi_check_rfkill();

    /* Small delay for interface to come up */
    usleep(100000);

    /* Try iw first (nl80211 drivers) */
    xemu_wifi_scan_iw();

    /* If no networks found, try iwlist (wext drivers like broadcom wl) */
    if (network_count == 0) {
        xemu_wifi_scan_iwlist();
    }

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
    int retry;
    int ret;

    wifi_log("=== WiFi Connect Start ===");
    wifi_log("SSID: %s, Interface: %s", ssid, wifi_interface);

    if (!wifi_initialized || !ssid) {
        wifi_log("ERROR: Not initialized or no SSID");
        return false;
    }

    /* Save credentials for auto-connect on next boot */
    FILE *saved = fopen("/home/xbox/.wifi_saved", "w");
    if (saved) {
        fprintf(saved, "WIFI_SSID=\"%s\"\n", ssid);
        fprintf(saved, "WIFI_PSK=\"%s\"\n", password ? password : "");
        fclose(saved);
        wifi_log("Saved WiFi credentials for auto-connect");
    }

    /* Kill any existing wpa_supplicant and dhclient */
    wifi_log("Killing existing wpa_supplicant/dhclient...");
    system("sudo pkill -9 wpa_supplicant 2>/dev/null");
    system("sudo pkill -9 dhclient 2>/dev/null");
    system("sudo pkill -9 dhcpcd 2>/dev/null");
    usleep(500000);

    /* Ensure interface is up */
    wifi_log("Bringing interface up...");
    snprintf(cmd, sizeof(cmd), "sudo ip link set %s up 2>/dev/null", wifi_interface);
    system(cmd);

    /* Create wpa_supplicant config */
    wifi_log("Creating wpa_supplicant config...");
    fp = fopen(conf_path, "w");
    if (!fp) {
        wifi_log("ERROR: Cannot create %s", conf_path);
        return false;
    }

    fprintf(fp, "ctrl_interface=/var/run/wpa_supplicant\n");
    fprintf(fp, "update_config=1\n\n");
    fprintf(fp, "network={\n");
    fprintf(fp, "    ssid=\"%s\"\n", ssid);

    if (password && strlen(password) > 0) {
        fprintf(fp, "    psk=\"%s\"\n", password);
        fprintf(fp, "    key_mgmt=WPA-PSK\n");
        wifi_log("Using WPA-PSK authentication");
    } else {
        fprintf(fp, "    key_mgmt=NONE\n");
        wifi_log("Using open authentication");
    }
    fprintf(fp, "    scan_ssid=1\n");
    fprintf(fp, "}\n");
    fclose(fp);

    /* Ensure /var/run/wpa_supplicant exists */
    system("sudo mkdir -p /var/run/wpa_supplicant 2>/dev/null");

    /* Start wpa_supplicant - try nl80211 first, then wext for broadcom */
    wifi_log("Starting wpa_supplicant with nl80211...");
    snprintf(cmd, sizeof(cmd),
             "sudo wpa_supplicant -B -D nl80211 -i %s -c %s >> /home/xbox/wifi.log 2>&1",
             wifi_interface, conf_path);
    ret = system(cmd);
    wifi_log("wpa_supplicant nl80211 returned: %d", ret);

    /* Check if wpa_supplicant is running */
    usleep(500000);
    if (system("pgrep wpa_supplicant >/dev/null 2>&1") != 0) {
        /* nl80211 failed, try wext */
        wifi_log("nl80211 failed, trying wext driver...");
        snprintf(cmd, sizeof(cmd),
                 "sudo wpa_supplicant -B -D wext -i %s -c %s >> /home/xbox/wifi.log 2>&1",
                 wifi_interface, conf_path);
        ret = system(cmd);
        wifi_log("wpa_supplicant wext returned: %d", ret);
    }

    /* Verify wpa_supplicant is running */
    usleep(500000);
    ret = system("pgrep wpa_supplicant >/dev/null 2>&1");
    wifi_log("wpa_supplicant running check: %d (0=running)", ret);

    /* Wait for WPA authentication (up to 15 seconds) */
    wifi_log("Waiting for WPA authentication...");
    for (retry = 0; retry < 30; retry++) {
        usleep(500000); /* 0.5 second */
        if (xemu_wifi_is_connected()) {
            wifi_log("WPA authenticated after %d retries (%.1f sec)", retry, retry * 0.5);
            break;
        }
    }
    if (retry >= 30) {
        wifi_log("WARNING: WPA auth timeout after 15 seconds");
    }

    /* Check WPA status */
    snprintf(cmd, sizeof(cmd), "wpa_cli -i %s status >> /home/xbox/wifi.log 2>&1", wifi_interface);
    system(cmd);

    /* Request DHCP - try dhclient with longer timeout */
    wifi_log("Requesting DHCP...");
    snprintf(cmd, sizeof(cmd), "sudo dhclient -v -timeout 30 %s >> /home/xbox/wifi.log 2>&1", wifi_interface);
    ret = system(cmd);
    wifi_log("dhclient returned: %d", ret);

    /* If dhclient failed, try dhcpcd */
    if (ret != 0) {
        wifi_log("Trying dhcpcd as fallback...");
        snprintf(cmd, sizeof(cmd), "sudo dhcpcd -t 30 %s >> /home/xbox/wifi.log 2>&1", wifi_interface);
        ret = system(cmd);
        wifi_log("dhcpcd returned: %d", ret);
    }

    /* Wait a moment for DHCP to complete */
    usleep(2000000);

    /* Log current IP state */
    snprintf(cmd, sizeof(cmd), "ip addr show %s > /tmp/wifi_ip.log 2>&1", wifi_interface);
    system(cmd);

    /* Verify we got an IP */
    snprintf(cmd, sizeof(cmd), "ip addr show %s | grep -q 'inet ' 2>/dev/null", wifi_interface);
    if (system(cmd) != 0) {
        wifi_log("ERROR: No IP address on %s - connection failed", wifi_interface);
        current_ssid[0] = '\0';
        return false;
    }
    wifi_log("Got IP address on %s", wifi_interface);

    /* Update current SSID */
    strncpy(current_ssid, ssid, XEMU_WIFI_SSID_MAX - 1);

    /* Get WiFi IP info for DHCP setup - do this BEFORE enabling xemu network */
    char wifi_ip[32] = {0};
    char gateway[32] = {0};
    FILE *ip_fp;

    /* Get WiFi IP */
    snprintf(cmd, sizeof(cmd), "ip -4 addr show %s | grep inet | awk '{print $2}' | cut -d/ -f1", wifi_interface);
    ip_fp = popen(cmd, "r");
    if (ip_fp) {
        if (fgets(wifi_ip, sizeof(wifi_ip), ip_fp)) {
            wifi_ip[strcspn(wifi_ip, "\n")] = 0;
        }
        pclose(ip_fp);
    }
    wifi_log("WiFi IP: %s", wifi_ip);

    /* Get gateway - look for "via X.X.X.X" pattern */
    ip_fp = popen("ip route show default 2>/dev/null | grep -oE 'via [0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+' | awk '{print $2}'", "r");
    if (ip_fp) {
        if (fgets(gateway, sizeof(gateway), ip_fp)) {
            gateway[strcspn(gateway, "\n")] = 0;
        }
        pclose(ip_fp);
    }
    wifi_log("Gateway: %s", gateway);

    /* Check if we got a real IP (not link-local 169.254.x.x) */
    if (strncmp(wifi_ip, "169.254", 7) == 0) {
        wifi_log("WARNING: Got link-local IP, DHCP may have failed!");
    }

    /* Calculate Xbox IP (WiFi IP + 1) */
    char xbox_ip[32] = {0};
    if (wifi_ip[0]) {
        int a, b, c, d;
        if (sscanf(wifi_ip, "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
            d++; /* Add 1 to last octet */
            if (d > 254) d = 2;
            snprintf(xbox_ip, sizeof(xbox_ip), "%d.%d.%d.%d", a, b, c, d);
        }
    }
    wifi_log("Xbox IP will be: %s", xbox_ip);

    /* Configure built-in DHCP in NVNet emulation */
    if (xbox_ip[0] && gateway[0] && wifi_ip[0]) {
        uint32_t client = inet_addr(xbox_ip);
        uint32_t gw = inet_addr(gateway);
        uint32_t server = inet_addr(wifi_ip);  /* Use our WiFi IP as server */
        nvnet_set_dhcp_config(client, gw, server);
        wifi_log("NVNet DHCP configured: client=%s gw=%s server=%s", xbox_ip, gateway, wifi_ip);
    }

    /*
     * NVNet Direct Proxy Mode - No TAP needed!
     * All network traffic is handled internally by nvnet.c
     * It intercepts packets and proxies them through real host sockets.
     * We still enable NAT backend to initialize the NIC, but NVNet intercepts
     * all packets before they reach slirp.
     */
    wifi_log("NVNet proxy mode - no TAP/bridge needed");
    wifi_log("Xbox IP: %s, Gateway: %s, Host: %s", xbox_ip, gateway, wifi_ip);

    /* Add Xbox IP as secondary address on WiFi interface for incoming connections */
    if (xbox_ip[0] && wifi_interface[0]) {
        char ip_cmd[256];
        snprintf(ip_cmd, sizeof(ip_cmd), "sudo ip addr add %s/24 dev %s 2>/dev/null", xbox_ip, wifi_interface);
        system(ip_cmd);
        wifi_log("Added %s as secondary IP on %s", xbox_ip, wifi_interface);
    }

    /* Enable NAT backend to initialize NIC (proxy intercepts before slirp) */
    if (!xemu_net_is_enabled()) {
        g_config.net.backend = CONFIG_NET_BACKEND_NAT;
        xemu_net_enable();
        wifi_log("Network backend enabled (NAT as dummy, proxy intercepts)");
    }

    /* Show notification */
    char notify_buf[128];
    snprintf(notify_buf, sizeof(notify_buf), "WiFi connected to %s", ssid);
    xemu_queue_notification(notify_buf);

    wifi_log("=== WiFi Connect Complete ===");
    return true;
}

bool xemu_wifi_disconnect(void)
{
    char cmd[128];

    /* Disable NVNet proxy */
    nvnet_set_dhcp_config(0, 0, 0);

    system("sudo pkill -9 wpa_supplicant 2>/dev/null");
    system("sudo pkill -9 dhclient 2>/dev/null");
    system("sudo pkill -9 dhcpcd 2>/dev/null");

    snprintf(cmd, sizeof(cmd), "sudo ip link set %s down 2>/dev/null", wifi_interface);
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

    current_ssid[0] = '\0';

    /* Try iw first (nl80211) */
    snprintf(cmd, sizeof(cmd), "iw dev %s link 2>/dev/null", wifi_interface);
    fp = popen(cmd, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            char *p = strstr(line, "SSID: ");
            if (p) {
                p += 6;
                p[strcspn(p, "\n")] = 0;
                strncpy(current_ssid, p, XEMU_WIFI_SSID_MAX - 1);
                pclose(fp);
                goto found;
            }
        }
        pclose(fp);
    }

    /* Try iwconfig (wext - for broadcom wl) */
    snprintf(cmd, sizeof(cmd), "iwconfig %s 2>/dev/null", wifi_interface);
    fp = popen(cmd, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            char *p = strstr(line, "ESSID:\"");
            if (p) {
                p += 7;
                char *end = strchr(p, '"');
                if (end && end > p) {
                    size_t len = end - p;
                    if (len >= XEMU_WIFI_SSID_MAX) len = XEMU_WIFI_SSID_MAX - 1;
                    strncpy(current_ssid, p, len);
                    current_ssid[len] = '\0';
                    pclose(fp);
                    goto found;
                }
            }
        }
        pclose(fp);
    }

    return false;

found:
    /* Mark as connected in network list */
    for (int i = 0; i < network_count; i++) {
        networks[i].connected = (strcmp(networks[i].ssid, current_ssid) == 0);
    }
    return true;
}

const char* xemu_wifi_get_current_ssid(void)
{
    if (current_ssid[0] != '\0') {
        return current_ssid;
    }
    return NULL;
}
