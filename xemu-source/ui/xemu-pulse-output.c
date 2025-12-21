/*
 * xemu PulseAudio Output Device Selection
 * Uses pactl commands for simplicity
 */

#include "xemu-pulse-output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    char id[256];      /* Internal sink name */
    char name[256];    /* Display name */
} sinks[XEMU_PULSE_MAX_SINKS];

static int sink_count = 0;
static char default_sink[256] = {0};

int xemu_pulse_refresh(void)
{
    FILE *fp;
    char line[512];
    int current_sink = -1;

    sink_count = 0;
    default_sink[0] = '\0';

    /* Get default sink */
    fp = popen("pactl get-default-sink 2>/dev/null", "r");
    if (fp) {
        if (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\n")] = 0;
            strncpy(default_sink, line, sizeof(default_sink) - 1);
        }
        pclose(fp);
    }

    /* List all sinks */
    fp = popen("pactl list sinks 2>/dev/null", "r");
    if (!fp) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) && sink_count < XEMU_PULSE_MAX_SINKS) {
        /* Look for "Sink #N" to start a new sink entry */
        if (strncmp(line, "Sink #", 6) == 0) {
            current_sink = sink_count;
            sink_count++;
            sinks[current_sink].id[0] = '\0';
            sinks[current_sink].name[0] = '\0';
        }

        if (current_sink < 0) continue;

        /* Get the sink name (internal ID) */
        char *p = strstr(line, "Name: ");
        if (p) {
            p += 6;
            p[strcspn(p, "\n")] = 0;
            strncpy(sinks[current_sink].id, p, sizeof(sinks[current_sink].id) - 1);
        }

        /* Get description (display name) */
        p = strstr(line, "Description: ");
        if (p) {
            p += 13;
            p[strcspn(p, "\n")] = 0;
            strncpy(sinks[current_sink].name, p, sizeof(sinks[current_sink].name) - 1);
        }
    }

    pclose(fp);

    /* If no description found, use ID as name */
    for (int i = 0; i < sink_count; i++) {
        if (sinks[i].name[0] == '\0' && sinks[i].id[0] != '\0') {
            strncpy(sinks[i].name, sinks[i].id, sizeof(sinks[i].name) - 1);
        }
    }

    return sink_count;
}

int xemu_pulse_get_count(void)
{
    return sink_count;
}

const char* xemu_pulse_get_name(int index)
{
    if (index < 0 || index >= sink_count) {
        return NULL;
    }
    return sinks[index].name;
}

const char* xemu_pulse_get_id(int index)
{
    if (index < 0 || index >= sink_count) {
        return NULL;
    }
    return sinks[index].id;
}

int xemu_pulse_get_default_index(void)
{
    for (int i = 0; i < sink_count; i++) {
        if (strcmp(sinks[i].id, default_sink) == 0) {
            return i;
        }
    }
    return -1;
}

bool xemu_pulse_set_default(int index)
{
    if (index < 0 || index >= sink_count) {
        return false;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "pactl set-default-sink '%s' 2>/dev/null", sinks[index].id);

    int ret = system(cmd);
    if (ret == 0) {
        strncpy(default_sink, sinks[index].id, sizeof(default_sink) - 1);
        return true;
    }

    return false;
}
