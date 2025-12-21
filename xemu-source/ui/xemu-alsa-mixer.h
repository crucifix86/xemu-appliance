/*
 * xemu ALSA Mixer Integration
 * Provides mixer control for the Audio settings tab
 */

#ifndef XEMU_ALSA_MIXER_H
#define XEMU_ALSA_MIXER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XEMU_MIXER_MAX_CONTROLS 16

/* Initialize the mixer - call once at startup */
int xemu_mixer_init(void);

/* Cleanup - call at shutdown */
void xemu_mixer_cleanup(void);

/* Refresh mixer state from hardware */
void xemu_mixer_refresh(void);

/* Get number of available playback controls */
int xemu_mixer_get_count(void);

/* Get control name by index */
const char* xemu_mixer_get_name(int index);

/* Get volume (0-100) */
int xemu_mixer_get_volume(int index);

/* Set volume (0-100) */
void xemu_mixer_set_volume(int index, int volume);

/* Get playback switch state (unmuted = true) */
bool xemu_mixer_get_switch(int index);

/* Set playback switch state (unmuted = true) */
void xemu_mixer_set_switch(int index, bool on);

/* Check if control has a switch */
bool xemu_mixer_has_switch(int index);

#ifdef __cplusplus
}
#endif

#endif /* XEMU_ALSA_MIXER_H */
