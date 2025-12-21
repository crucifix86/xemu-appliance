/*
 * xemu WiFi Network Management
 * Provides WiFi scanning and connection for the Network settings tab
 */

#ifndef XEMU_WIFI_H
#define XEMU_WIFI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XEMU_WIFI_MAX_NETWORKS 32
#define XEMU_WIFI_SSID_MAX 64

typedef struct {
    char ssid[XEMU_WIFI_SSID_MAX];
    int signal_strength;  /* 0-100 percentage */
    bool encrypted;
    bool connected;
} xemu_wifi_network_t;

/* Initialize WiFi subsystem - finds wireless interface */
int xemu_wifi_init(void);

/* Check if WiFi hardware is available */
bool xemu_wifi_available(void);

/* Get the wireless interface name (e.g., "wlan0") */
const char* xemu_wifi_get_interface(void);

/* Scan for networks - returns number found, -1 on error */
int xemu_wifi_scan(void);

/* Get number of networks from last scan */
int xemu_wifi_get_count(void);

/* Get network info by index */
const xemu_wifi_network_t* xemu_wifi_get_network(int index);

/* Connect to a network (NULL password for open networks) */
bool xemu_wifi_connect(const char *ssid, const char *password);

/* Disconnect from current network */
bool xemu_wifi_disconnect(void);

/* Get current connection status */
bool xemu_wifi_is_connected(void);

/* Get currently connected SSID (NULL if not connected) */
const char* xemu_wifi_get_current_ssid(void);

/* Check/unblock rfkill */
bool xemu_wifi_check_rfkill(void);

#ifdef __cplusplus
}
#endif

#endif /* XEMU_WIFI_H */
