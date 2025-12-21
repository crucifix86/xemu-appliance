# Current Projects

No active projects at the moment.

---

## Completed Tasks

### Driver Manager Tab (Dec 2024)
- Added new "Drivers" tab to xemu main menu
- Uses `hwinfo` for hardware detection
- Uses `isenkram-lookup` for driver/package suggestions
- Shows detected hardware by category (GPU, sound, network, etc.)
- Shows recommended packages with Install buttons
- Created `xemu-drivers.c/.h` for detection wrapper
- Added hwinfo, isenkram-cli, and dependencies to rootfs

### WiFi Detection Menu (Dec 2024)
- Added WiFi section to Settings > Network tab
- Scan for available networks with signal strength
- Password input for WPA networks
- Connect/Disconnect functionality
- Created `xemu-wifi.c/.h` using iw and wpa_supplicant
- Added iw, wpa_supplicant, wpa_cli, rfkill to rootfs
- Added libnl-3 and libnl-genl-3 libraries

### ALSA Mixer & Output Device Integration (Dec 2024)
- Added ALSA mixer controls to Settings > Audio tab
- Added PulseAudio output device selector (speakers/headphones/HDMI)
- Created `xemu-alsa-mixer.c/.h` for mixer volume/mute control
- Created `xemu-pulse-output.c/.h` for output device selection
- Modified `build.sh` to enable ALSA (`--enable-alsa`)
- Modified `ui/meson.build` to link ALSA library

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
