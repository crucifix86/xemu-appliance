/*
 * xemu Driver Manager
 * Uses hwinfo and isenkram-lookup for hardware detection
 */

#include "xemu-drivers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static xemu_hardware_device_t devices[XEMU_DRIVERS_MAX_DEVICES];
static int device_count = 0;

static xemu_driver_suggestion_t suggestions[XEMU_DRIVERS_MAX_SUGGESTIONS];
static int suggestion_count = 0;

int xemu_drivers_scan_hardware(void)
{
    FILE *fp;
    char line[256];
    char current_category[32] = {0};

    device_count = 0;
    memset(devices, 0, sizeof(devices));

    /* Use hwinfo --short to get hardware list */
    fp = popen("hwinfo --short 2>/dev/null", "r");
    if (!fp) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) && device_count < XEMU_DRIVERS_MAX_DEVICES) {
        line[strcspn(line, "\n")] = 0;

        /* Skip empty lines */
        if (strlen(line) == 0) continue;

        /* Category line (no leading whitespace) */
        if (line[0] != ' ' && line[0] != '\t') {
            /* Remove trailing colon */
            char *colon = strchr(line, ':');
            if (colon) *colon = '\0';
            strncpy(current_category, line, sizeof(current_category) - 1);
            continue;
        }

        /* Device line (has leading whitespace) */
        if (current_category[0] && (line[0] == ' ' || line[0] == '\t')) {
            /* Skip device path if present (e.g., /dev/input/event0) */
            char *name = line;
            while (*name == ' ' || *name == '\t') name++;

            /* Skip if it starts with /dev */
            char *dev_name = name;
            if (strncmp(name, "/dev/", 5) == 0) {
                dev_name = strchr(name, ' ');
                if (dev_name) {
                    while (*dev_name == ' ') dev_name++;
                } else {
                    continue;
                }
            }

            if (strlen(dev_name) > 0) {
                strncpy(devices[device_count].category, current_category,
                        sizeof(devices[device_count].category) - 1);
                strncpy(devices[device_count].name, dev_name,
                        sizeof(devices[device_count].name) - 1);
                device_count++;
            }
        }
    }
    pclose(fp);

    return device_count;
}

int xemu_drivers_get_device_count(void)
{
    return device_count;
}

const xemu_hardware_device_t* xemu_drivers_get_device(int index)
{
    if (index < 0 || index >= device_count) {
        return NULL;
    }
    return &devices[index];
}

int xemu_drivers_get_suggestions(void)
{
    FILE *fp;
    char line[128];

    suggestion_count = 0;
    memset(suggestions, 0, sizeof(suggestions));

    /* Use isenkram-lookup to get package suggestions */
    fp = popen("isenkram-lookup 2>/dev/null", "r");
    if (!fp) {
        /* Fallback: check for common drivers based on detected hardware */
        return xemu_drivers_get_suggestion_count();
    }

    while (fgets(line, sizeof(line), fp) && suggestion_count < XEMU_DRIVERS_MAX_SUGGESTIONS) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) == 0) continue;

        strncpy(suggestions[suggestion_count].package, line,
                sizeof(suggestions[suggestion_count].package) - 1);

        /* Generate description based on package name */
        if (strstr(line, "nvidia")) {
            strcpy(suggestions[suggestion_count].description, "NVIDIA GPU driver");
        } else if (strstr(line, "firmware-iwlwifi")) {
            strcpy(suggestions[suggestion_count].description, "Intel WiFi firmware");
        } else if (strstr(line, "firmware-realtek")) {
            strcpy(suggestions[suggestion_count].description, "Realtek network firmware");
        } else if (strstr(line, "firmware-intel")) {
            strcpy(suggestions[suggestion_count].description, "Intel graphics firmware");
        } else if (strstr(line, "firmware-amd")) {
            strcpy(suggestions[suggestion_count].description, "AMD GPU firmware");
        } else if (strstr(line, "bluez")) {
            strcpy(suggestions[suggestion_count].description, "Bluetooth support");
        } else if (strstr(line, "pulseaudio") || strstr(line, "pipewire")) {
            strcpy(suggestions[suggestion_count].description, "Audio system");
        } else {
            strcpy(suggestions[suggestion_count].description, "Recommended package");
        }

        /* Check if installed */
        suggestions[suggestion_count].installed = xemu_drivers_is_installed(line);

        suggestion_count++;
    }
    pclose(fp);

    return suggestion_count;
}

int xemu_drivers_get_suggestion_count(void)
{
    return suggestion_count;
}

const xemu_driver_suggestion_t* xemu_drivers_get_suggestion(int index)
{
    if (index < 0 || index >= suggestion_count) {
        return NULL;
    }
    return &suggestions[index];
}

bool xemu_drivers_install_package(const char *package)
{
    char cmd[256];

    if (!package || strlen(package) == 0) {
        return false;
    }

    /* Run apt-get install in background with a terminal */
    snprintf(cmd, sizeof(cmd),
             "x-terminal-emulator -e 'sudo apt-get install -y %s; echo Press Enter to close; read' &",
             package);

    return system(cmd) == 0;
}

bool xemu_drivers_is_installed(const char *package)
{
    char cmd[256];
    int ret;

    if (!package || strlen(package) == 0) {
        return false;
    }

    snprintf(cmd, sizeof(cmd), "dpkg -s '%s' >/dev/null 2>&1", package);
    ret = system(cmd);

    return (ret == 0);
}
