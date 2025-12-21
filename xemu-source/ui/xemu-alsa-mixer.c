/*
 * xemu ALSA Mixer Integration
 * Provides mixer control for the Audio settings tab
 */

#include "xemu-alsa-mixer.h"
#include <stdio.h>
#include <string.h>
#include <alsa/asoundlib.h>

static snd_mixer_t *mixer_handle = NULL;
static bool mixer_initialized = false;

/* Storage for mixer controls */
static struct {
    snd_mixer_elem_t *elem;
    char name[64];
    long vol_min;
    long vol_max;
    bool has_switch;
} mixer_controls[XEMU_MIXER_MAX_CONTROLS];

static int mixer_count = 0;

int xemu_mixer_init(void)
{
    if (mixer_initialized) {
        return 0;
    }

    int err;

    err = snd_mixer_open(&mixer_handle, 0);
    if (err < 0) {
        fprintf(stderr, "xemu-mixer: Cannot open mixer: %s\n", snd_strerror(err));
        return -1;
    }

    err = snd_mixer_attach(mixer_handle, "default");
    if (err < 0) {
        fprintf(stderr, "xemu-mixer: Cannot attach to mixer: %s\n", snd_strerror(err));
        snd_mixer_close(mixer_handle);
        mixer_handle = NULL;
        return -1;
    }

    err = snd_mixer_selem_register(mixer_handle, NULL, NULL);
    if (err < 0) {
        fprintf(stderr, "xemu-mixer: Cannot register mixer: %s\n", snd_strerror(err));
        snd_mixer_close(mixer_handle);
        mixer_handle = NULL;
        return -1;
    }

    err = snd_mixer_load(mixer_handle);
    if (err < 0) {
        fprintf(stderr, "xemu-mixer: Cannot load mixer: %s\n", snd_strerror(err));
        snd_mixer_close(mixer_handle);
        mixer_handle = NULL;
        return -1;
    }

    /* Enumerate playback controls */
    mixer_count = 0;
    snd_mixer_elem_t *elem;

    for (elem = snd_mixer_first_elem(mixer_handle);
         elem && mixer_count < XEMU_MIXER_MAX_CONTROLS;
         elem = snd_mixer_elem_next(elem)) {

        if (!snd_mixer_selem_is_active(elem)) {
            continue;
        }

        if (!snd_mixer_selem_has_playback_volume(elem)) {
            continue;
        }

        const char *name = snd_mixer_selem_get_name(elem);

        mixer_controls[mixer_count].elem = elem;
        strncpy(mixer_controls[mixer_count].name, name, 63);
        mixer_controls[mixer_count].name[63] = '\0';

        snd_mixer_selem_get_playback_volume_range(elem,
            &mixer_controls[mixer_count].vol_min,
            &mixer_controls[mixer_count].vol_max);

        mixer_controls[mixer_count].has_switch =
            snd_mixer_selem_has_playback_switch(elem);

        mixer_count++;
    }

    mixer_initialized = true;
    fprintf(stderr, "xemu-mixer: Initialized with %d controls\n", mixer_count);

    return 0;
}

void xemu_mixer_cleanup(void)
{
    if (mixer_handle) {
        snd_mixer_close(mixer_handle);
        mixer_handle = NULL;
    }
    mixer_initialized = false;
    mixer_count = 0;
}

void xemu_mixer_refresh(void)
{
    if (mixer_handle) {
        snd_mixer_handle_events(mixer_handle);
    }
}

int xemu_mixer_get_count(void)
{
    return mixer_count;
}

const char* xemu_mixer_get_name(int index)
{
    if (index < 0 || index >= mixer_count) {
        return NULL;
    }
    return mixer_controls[index].name;
}

int xemu_mixer_get_volume(int index)
{
    if (index < 0 || index >= mixer_count) {
        return 0;
    }

    long vol = 0;
    snd_mixer_selem_get_playback_volume(mixer_controls[index].elem,
                                         SND_MIXER_SCHN_MONO, &vol);

    long min = mixer_controls[index].vol_min;
    long max = mixer_controls[index].vol_max;

    if (max == min) {
        return 100;
    }

    return (int)(((vol - min) * 100) / (max - min));
}

void xemu_mixer_set_volume(int index, int volume)
{
    if (index < 0 || index >= mixer_count) {
        return;
    }

    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    long min = mixer_controls[index].vol_min;
    long max = mixer_controls[index].vol_max;
    long vol = min + ((max - min) * volume) / 100;

    snd_mixer_selem_set_playback_volume_all(mixer_controls[index].elem, vol);
}

bool xemu_mixer_get_switch(int index)
{
    if (index < 0 || index >= mixer_count) {
        return false;
    }

    if (!mixer_controls[index].has_switch) {
        return true; /* No switch means always on */
    }

    int sw = 0;
    snd_mixer_selem_get_playback_switch(mixer_controls[index].elem,
                                         SND_MIXER_SCHN_MONO, &sw);
    return sw != 0;
}

void xemu_mixer_set_switch(int index, bool on)
{
    if (index < 0 || index >= mixer_count) {
        return;
    }

    if (!mixer_controls[index].has_switch) {
        return;
    }

    snd_mixer_selem_set_playback_switch_all(mixer_controls[index].elem, on ? 1 : 0);
}

bool xemu_mixer_has_switch(int index)
{
    if (index < 0 || index >= mixer_count) {
        return false;
    }
    return mixer_controls[index].has_switch;
}
