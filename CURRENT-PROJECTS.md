# Current Projects

## WiFi Detection Menu for xemu Settings

**Status:** Planning
**Goal:** Add WiFi network detection and connection UI to xemu settings, allowing users to connect to wireless networks without command-line tools.

### Why This Is Needed
- Many users boot the appliance on laptops or systems with WiFi only
- Currently no way to connect to WiFi from the UI
- Need network for FTP file transfers to Xbox HDD

### Research Needed
- How to scan for WiFi networks (iwlist, nmcli, wpa_supplicant)
- How to connect to networks (WPA2, open, etc.)
- What tools are available on the minimal rootfs
- UI design for network list and password entry

### Files to Modify
| File | Changes |
|------|---------|
| `xemu-source/ui/xui/main-menu.cc` | Add WiFi section to Network settings |
| `xemu-source/ui/xemu-wifi.c` (NEW) | WiFi scanning and connection wrapper |
| `xemu-source/ui/xemu-wifi.h` (NEW) | Header for WiFi functions |
| `xemu-source/ui/meson.build` | Add new source files |

### UI Design (Proposed)
```
[Network Settings]
├── WiFi
│   ├── Status: Connected to "MyNetwork" / Not Connected
│   ├── [Scan for Networks]
│   ├── Available Networks:
│   │   ├── MyNetwork      [====] 80%  [Connect]
│   │   ├── Neighbor_5G    [===]  60%  [Connect]
│   │   └── CoffeeShop     [==]   40%  [Connect]
│   └── [Disconnect]
└── Ethernet (existing)
```

### Implementation Steps
1. Research WiFi tools available on minimal Debian
2. Create WiFi wrapper (`xemu-wifi.c/.h`)
3. Add UI elements to Network settings tab
4. Test with various network types (WPA2, open)
5. Handle password input dialog

---

## Completed Tasks

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
