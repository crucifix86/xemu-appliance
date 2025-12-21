/*
 * xemu Driver Manager
 * Hardware detection and driver suggestions
 */

#ifndef XEMU_DRIVERS_H
#define XEMU_DRIVERS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XEMU_DRIVERS_MAX_DEVICES 32
#define XEMU_DRIVERS_MAX_SUGGESTIONS 16

typedef struct {
    char category[32];    /* cpu, graphics card, sound, network, etc. */
    char name[128];       /* Device name */
    char driver[64];      /* Current driver if known */
} xemu_hardware_device_t;

typedef struct {
    char package[64];     /* Package name to install */
    char description[128]; /* What it's for */
    bool installed;       /* Already installed? */
} xemu_driver_suggestion_t;

/* Scan hardware - returns number of devices found */
int xemu_drivers_scan_hardware(void);

/* Get number of detected devices */
int xemu_drivers_get_device_count(void);

/* Get device by index */
const xemu_hardware_device_t* xemu_drivers_get_device(int index);

/* Get driver suggestions - returns count */
int xemu_drivers_get_suggestions(void);

/* Get suggestion count */
int xemu_drivers_get_suggestion_count(void);

/* Get suggestion by index */
const xemu_driver_suggestion_t* xemu_drivers_get_suggestion(int index);

/* Install a package (runs apt-get in background) */
bool xemu_drivers_install_package(const char *package);

/* Check if a package is installed */
bool xemu_drivers_is_installed(const char *package);

#ifdef __cplusplus
}
#endif

#endif /* XEMU_DRIVERS_H */
