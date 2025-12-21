/*
 * xemu PulseAudio Output Device Selection
 * Provides audio output device enumeration and selection for the Audio settings tab
 */

#ifndef XEMU_PULSE_OUTPUT_H
#define XEMU_PULSE_OUTPUT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XEMU_PULSE_MAX_SINKS 16

/* Initialize/refresh the sink list - call before using other functions */
int xemu_pulse_refresh(void);

/* Get number of available output devices (sinks) */
int xemu_pulse_get_count(void);

/* Get sink display name by index */
const char* xemu_pulse_get_name(int index);

/* Get sink internal name by index (for setting default) */
const char* xemu_pulse_get_id(int index);

/* Get index of current default sink (-1 if not found) */
int xemu_pulse_get_default_index(void);

/* Set default sink by index */
bool xemu_pulse_set_default(int index);

#ifdef __cplusplus
}
#endif

#endif /* XEMU_PULSE_OUTPUT_H */
