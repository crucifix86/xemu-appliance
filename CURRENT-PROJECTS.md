# Current Projects

## ALSA Mixer Integration for xemu Settings

**Status:** Implementation Complete - Needs Testing
**Goal:** Add ALSA mixer controls to xemu's Audio settings tab so users can adjust Master, PCM, Speaker, Headphone volumes directly from the emulator.

### Why This Is Needed
- ALSA channels are often muted by default on Linux
- Currently requires command-line `amixer` to unmute
- Users should be able to control audio from xemu UI

### Files to Modify

| File | Changes |
|------|---------|
| `xemu-source/ui/xui/main-menu.cc` | Add mixer sliders to `MainMenuAudioView::Draw()` |
| `xemu-source/ui/xui/main-menu.hh` | Add mixer state variables to class |
| `xemu-source/ui/xemu-alsa-mixer.c` (NEW) | ALSA mixer wrapper functions |
| `xemu-source/ui/xemu-alsa-mixer.h` (NEW) | Header for mixer functions |
| `xemu-source/ui/meson.build` | Add new source files |

### Current Audio Tab Location
- **File:** `xemu-source/ui/xui/main-menu.cc`
- **Lines:** 604-616
- **Class:** `MainMenuAudioView::Draw()`

```cpp
void MainMenuAudioView::Draw()
{
    SectionTitle("Volume");
    char buf[32];
    snprintf(buf, sizeof(buf), "Limit output volume (%d%%)",
             (int)(g_config.audio.volume_limit * 100));
    Slider("Output volume limit", &g_config.audio.volume_limit, buf);

    SectionTitle("Quality");
    Toggle("Real-time DSP processing", &g_config.audio.use_dsp,
           "Enable improved audio accuracy (experimental)");
}
```

### New ALSA Mixer Wrapper API

Create `xemu-source/ui/xemu-alsa-mixer.h`:
```c
#ifndef XEMU_ALSA_MIXER_H
#define XEMU_ALSA_MIXER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int xemu_mixer_init(void);
void xemu_mixer_cleanup(void);
int xemu_mixer_get_count(void);
const char* xemu_mixer_get_name(int index);
int xemu_mixer_get_volume(int index);
void xemu_mixer_set_volume(int index, int vol);
bool xemu_mixer_get_switch(int index);
void xemu_mixer_set_switch(int index, bool on);
void xemu_mixer_refresh(void);

#ifdef __cplusplus
}
#endif

#endif
```

### Extended Audio Tab UI Design

```
[Audio Settings]
├── Volume
│   └── Output volume limit [====|====] 100%
├── ALSA Mixer
│   ├── Master [========|==] 80%  [x] Unmute
│   ├── PCM    [==========] 100%  [x] Unmute
│   ├── Speaker [========|==] 80% [x] Unmute
│   └── Headphone [======|====] 60% [ ] Muted
└── Quality
    └── [x] Real-time DSP processing
```

### Implementation Steps

1. **Create ALSA mixer wrapper** (`xemu-alsa-mixer.c/.h`)
   - Use `snd_mixer_*` functions from ALSA lib
   - Enumerate playback volume controls
   - Get/set volume and mute switch

2. **Update meson.build**
   - Add new source files
   - Link against libasound

3. **Modify MainMenuAudioView::Draw()**
   - Call `xemu_mixer_init()` on first draw
   - Add SectionTitle("ALSA Mixer")
   - Loop through mixer controls
   - Add Slider for each volume
   - Add Toggle for each mute switch

4. **Test and iterate**

### Key ALSA Functions Reference

```c
#include <alsa/asoundlib.h>

snd_mixer_t *handle;
snd_mixer_open(&handle, 0);
snd_mixer_attach(handle, "default");
snd_mixer_selem_register(handle, NULL, NULL);
snd_mixer_load(handle);

// Iterate elements
snd_mixer_elem_t *elem;
for (elem = snd_mixer_first_elem(handle); elem; elem = snd_mixer_elem_next(elem)) {
    if (snd_mixer_selem_has_playback_volume(elem)) {
        const char *name = snd_mixer_selem_get_name(elem);
        long min, max, vol;
        snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
        snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_MONO, &vol);
        // vol is between min and max
    }
}
```

### UI Framework Reference

xemu uses Dear ImGui. Available widgets in `widgets.cc`:
- `SectionTitle(const char *title)` - Section header
- `Slider(const char *str_id, float *v, const char *description)` - 0.0-1.0 range
- `Toggle(const char *str_id, bool *v, const char *description)` - Boolean toggle

### Build Command
```bash
cd xemu-source
./build.sh
```

Binary output: `xemu-source/dist/xemu`

---

## Completed Tasks

### Audio Support (Dec 2024)
- Added PulseAudio startup to .xinitrc
- Added ALSA unmute commands (amixer)
- Added /etc/modules-load.d/audio.conf for HDA driver loading
- Copied amixer binary to rootfs

### Intel GPU Support (Dec 2024)
- Added "Intel Shader Fix" boot option
- Mesa workarounds: MESA_GL_VERSION_OVERRIDE, MESA_GLSL_VERSION_OVERRIDE, MESA_NO_ERROR

### Dynamic GRUB Entries (Dec 2024)
- Converted /etc/grub.d/40_custom to dynamic script
- Properly detects root device (LABEL vs UUID) at update-grub time
- Works for both USB boot and HDD install
