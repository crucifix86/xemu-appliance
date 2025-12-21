# xemu Appliance - Master Documentation

This document contains everything needed to understand, build, and maintain the xemu appliance.

## What This Project Is

A standalone bootable Linux system that runs xemu (original Xbox emulator). Boots directly into the emulator from USB or internal disk - no desktop environment, no manual setup.

---

## What Makes a Working USB

### Partition Layout

| Partition | Size | Type | Label | Purpose |
|-----------|------|------|-------|---------|
| 1 | 512 MiB | FAT32 (ESP) | XEMU_EFI | EFI boot files |
| 2 | ~19.5 GiB | ext4 | XEMU_ROOT | Root filesystem |

**CRITICAL: Both partitions MUST use LABEL, not UUID.**

### Boot Configuration Files

All boot configs must use LABEL-based search, never UUID:

#### /boot/efi/EFI/BOOT/grub.cfg
```
search.fs_label XEMU_ROOT root
set prefix=($root)'/boot/grub'
configfile $prefix/grub.cfg
```

#### /boot/grub/x86_64-efi/load.cfg
```
search.fs_label XEMU_ROOT root
set prefix=($root)'/boot/grub'
```

#### /etc/fstab
```
LABEL=XEMU_ROOT / ext4 defaults,rw 0 1
LABEL=XEMU_EFI /boot/efi vfat defaults 0 2
tmpfs /tmp tmpfs defaults,nosuid,nodev,mode=1777 0 0
```

#### /boot/grub/grub.cfg
- All `search` commands must use `--label` not `--fs-uuid`
- All `root=` kernel parameters must use `root=LABEL=XEMU_ROOT`

### Why LABEL, Not UUID

When you format a partition, it gets a NEW UUID. If boot configs have hardcoded UUIDs from the source image, the new USB won't boot - systemd times out waiting for a device that doesn't exist.

LABEL is set during format (`mkfs.ext4 -L XEMU_ROOT`, `mkfs.vfat -n XEMU_EFI`) and referenced in configs. This works regardless of which USB drive is used.

---

## System Specifications

### Base OS
- **Distribution:** Debian 13 (trixie)
- **Hostname:** xbox
- **Total Size:** ~4.4 GB

### Kernel
- **Primary:** 6.12.57+deb13-amd64 (12 MB)
- **Fallback:** 6.1.0-39-amd64 (8 MB)
- **initrd:** ~35-37 MB each

### Key Directories
| Path | Size | Contents |
|------|------|----------|
| /opt/xbox | 1.7 GB | xemu binary, HDD image, BIOS |
| /usr | 1.7 GB | System packages |
| /var | 982 MB | Logs, cache |
| /boot | 107 MB | Kernels, grub |
| /home | 3.7 MB | xbox user config |

---

## xemu Setup

### Files in /opt/xbox/

| File | Size | Description |
|------|------|-------------|
| xemu | 112 MB | Emulator binary (ELF 64-bit, debug symbols) |
| xbox_hdd.qcow2 | 1.6 GB | Virtual Xbox HDD (8 GB virtual) |
| eeprom.bin | 256 B | Xbox EEPROM |
| bios/mcpx_1.0.bin | 512 B | MCPX bootrom |
| bios/Complex_4627.bin | 1 MB | Flash ROM |
| start-xemu.sh | 286 B | Startup wrapper |
| setup-network.sh | 313 B | TAP network setup |

### xemu Configuration

Location: `/home/xbox/.local/share/xemu/xemu/xemu.toml`

```toml
[general]
show_welcome = false

[input.bindings]
port1_driver = 'usb-xbox-gamepad'
port1 = '030081b85e0400008e02000010010000'

[sys.files]
bootrom_path = '/opt/xbox/bios/mcpx_1.0.bin'
flashrom_path = '/opt/xbox/bios/Complex_4627.bin'
eeprom_path = '/opt/xbox/eeprom.bin'
hdd_path = '/opt/xbox/xbox_hdd.qcow2'
```

---

## Boot Process

### EFI Boot Chain
```
UEFI Firmware
    ↓
/boot/efi/EFI/BOOT/BOOTX64.EFI (shim)
    ↓
/boot/efi/EFI/BOOT/grubx64.efi
    ↓
/boot/efi/EFI/BOOT/grub.cfg (searches for XEMU_ROOT label)
    ↓
/boot/grub/grub.cfg (main menu)
    ↓
Linux kernel + initrd
    ↓
systemd
```

### EFI Boot Files

| File | Size | Purpose |
|------|------|---------|
| BOOTX64.EFI | 957 KB | Shim bootloader |
| grubx64.efi | 2.6 MB | GRUB EFI binary |
| mmx64.efi | 850 KB | MOK manager |
| grub.cfg | 91 B | Initial grub config |

### Systemd Boot Flow
```
multi-user.target
    ├── xbox-network.service (TAP + NAT setup)
    ├── dnsmasq.service (DHCP for Xbox)
    └── getty@tty1 → auto-login xbox user
                         ↓
                    ~/.profile
                         ↓
              (if installer param) → install-to-disk
              (else) → startx → ~/.xinitrc → xemu
```

---

## Auto-Start Configuration

### User: xbox
- UID: 1000
- Shell: /bin/bash
- Home: /home/xbox

### ~/.profile (Auto-start Logic)
```bash
if [ -z "$DISPLAY" ] && [ "$(tty)" = "/dev/tty1" ]; then
    if grep -q "installer" /proc/cmdline; then
        sudo /usr/local/bin/install-to-disk
    else
        startx
    fi
fi
```

### ~/.xinitrc
```bash
#!/bin/bash

# Mesa workarounds for Intel shader issues (enabled via kernel param)
if grep -q "intelfix" /proc/cmdline; then
    export MESA_GL_VERSION_OVERRIDE=4.3
    export MESA_GLSL_VERSION_OVERRIDE=430
    export MESA_NO_ERROR=1
fi

# Set renderer based on kernel command line
if grep -q "vulkan" /proc/cmdline; then
    RENDERER='VULKAN'
else
    RENDERER='OPENGL'
fi

# Always write config to ensure correct renderer
cat > /home/xbox/.local/share/xemu/xemu/xemu.toml << CONF
[general]
show_welcome = false

[display]
renderer = '${RENDERER}'

[sys.files]
bootrom_path = '/opt/xbox/bios/mcpx_1.0.bin'
flashrom_path = '/opt/xbox/bios/Complex_4627.bin'
eeprom_path = '/opt/xbox/eeprom.bin'
hdd_path = '/opt/xbox/xbox_hdd.qcow2'

[net]
enable = false
CONF

# Unmute ALSA and set volume (often muted by default)
amixer sset Master unmute 2>/dev/null
amixer sset Master 100% 2>/dev/null
amixer sset PCM unmute 2>/dev/null
amixer sset PCM 100% 2>/dev/null
amixer sset Speaker unmute 2>/dev/null
amixer sset Headphone unmute 2>/dev/null

# Start PulseAudio for audio support
pulseaudio --start --daemonize

exec /opt/xbox/xemu -m 64 -net nic,model=nvnet -net tap,ifname=tap0,script=no,downscript=no
```

---

## Network Configuration

### Architecture
```
Host (192.168.100.1)
    │
    tap0 interface
    │
    ├── DHCP (dnsmasq: 192.168.100.10-50)
    ├── NAT (iptables MASQUERADE)
    │
Emulated Xbox (192.168.100.x)
    │
    FTP server for game transfers
```

### /opt/xbox/setup-network.sh
```bash
#!/bin/bash
ip tuntap add tap0 mode tap user xbox
ip link set tap0 up
ip addr add 192.168.100.1/24 dev tap0
sysctl -w net.ipv4.ip_forward=1
iptables -t nat -A POSTROUTING -s 192.168.100.0/24 -j MASQUERADE
```

### /etc/dnsmasq.d/xbox.conf
```
interface=tap0
dhcp-range=192.168.100.10,192.168.100.50,12h
bind-interfaces
```

---

## Audio Configuration

### Architecture
```
ALSA (kernel driver: snd-hda-intel)
    │
    amixer (unmute + volume)
    │
PulseAudio (user daemon)
    │
SDL2 audio backend
    │
xemu (AC97 emulation)
```

### Kernel Modules
`/etc/modules-load.d/audio.conf`:
```
snd-hda-intel
snd-hda-codec-realtek
snd-hda-codec-hdmi
```

### Audio Initialization in ~/.xinitrc
```bash
# Unmute ALSA and set volume (often muted by default)
amixer sset Master unmute 2>/dev/null
amixer sset Master 100% 2>/dev/null
amixer sset PCM unmute 2>/dev/null
amixer sset PCM 100% 2>/dev/null
amixer sset Speaker unmute 2>/dev/null
amixer sset Headphone unmute 2>/dev/null

# Start PulseAudio for audio support
pulseaudio --start --daemonize
```

### Why Explicit Unmuting?
ALSA often initializes with channels muted by default. Without explicit unmuting:
- PulseAudio starts successfully
- xemu's SDL audio backend connects
- No sound output because ALSA mixer is muted

The `amixer` commands unmute common channel names. Errors are suppressed (`2>/dev/null`) since not all systems have all channels.

---

## Systemd Services

### xbox-network.service
```ini
[Unit]
Description=Xbox TAP Network Setup
Before=dnsmasq.service

[Service]
Type=oneshot
ExecStart=/opt/xbox/setup-network.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

### xemu.service
```ini
[Unit]
Description=xemu Xbox Emulator
After=network.target xbox-network.service
Wants=xbox-network.service

[Service]
Type=simple
User=xbox
ExecStart=/opt/xbox/start-xemu.sh
Restart=no

[Install]
WantedBy=multi-user.target
```

### Enabled Services
- dnsmasq.service
- xbox-network.service
- grub-common.service
- e2scrub_reap.service
- remote-fs.target

---

## Build Scripts

### build-usb.sh
Command-line USB builder. Usage: `./build-usb.sh /dev/sdX [-y]`

Key operations:
1. Create GPT partition table
2. Format EFI partition: `mkfs.vfat -F32 -n XEMU_EFI`
3. Format root partition: `mkfs.ext4 -F -L XEMU_ROOT`
4. Copy rootfs with rsync
5. Copy EFI files
6. Update fstab: replace `EFI_UUID_PLACEHOLDER` with `LABEL=XEMU_EFI`

### usb-builder-gui.py
Tkinter GUI version. Same operations with progress display.

---

## GRUB Menu Entries

### Default Boot (Debian GNU/Linux)
Boots into xemu with OpenGL renderer. Works on most hardware.

### Install to Disk
Kernel parameter: `installer`
Runs /usr/local/bin/install-to-disk for internal disk installation.

### Vulkan Renderer
Kernel parameter: `vulkan`
Creates Vulkan-mode xemu.toml before starting emulator.
**Note:** Does not work on Intel Haswell GPUs (incomplete Mesa Vulkan support).

### Intel Shader Fix
Kernel parameter: `intelfix`
Applies Mesa workarounds for Intel GPUs with shader issues:
- `MESA_GL_VERSION_OVERRIDE=4.3`
- `MESA_GLSL_VERSION_OVERRIDE=430`
- `MESA_NO_ERROR=1`

**Use this for:** Intel Iris Pro (Haswell), and other Intel GPUs that crash with shader assertion errors.

---

## GPU Compatibility

### Tested Hardware

| GPU | OpenGL | Vulkan | Notes |
|-----|--------|--------|-------|
| Intel HD Graphics 505 (Apollo Lake) | ✅ Works | ✅ Works (minor artifacts) | Pentium J4205 |
| Intel Iris Pro 5200 (Haswell) | ⚠️ Needs Intel Fix | ❌ Broken | i5-4570R - Use "Intel Shader Fix" boot option |

### Intel GPU Issues

**Haswell (4th gen) and older Intel GPUs** may experience:
- Shader assertion failures: `apply_uniform_updates: Assertion 'glGetError() == GL_NO_ERROR' failed`
- Vulkan crashes: `vk_result = -2` (VK_ERROR_OUT_OF_DEVICE_MEMORY)
- Mesa warning: "Haswell Vulkan support is incomplete"

**Solution:** Use the "xemu (Intel Shader Fix)" boot option which applies Mesa workarounds.

---

## Troubleshooting

### "Timed out waiting for device"
**Cause:** fstab or grub configs have UUID instead of LABEL.
**Fix:** Ensure all configs use `LABEL=XEMU_ROOT` and `LABEL=XEMU_EFI`.

### Boot drops to grub command line
**Cause:** grub.cfg search commands use UUID that doesn't exist.
**Fix:** Check /boot/efi/EFI/BOOT/grub.cfg and /boot/grub/x86_64-efi/load.cfg use `search.fs_label`.

### xemu doesn't start
**Check:** /var/log/xemu.log for errors.
**Common issues:** Missing BIOS files, permissions on /opt/xbox/.

### xemu crashes immediately (Intel GPU)
**Symptom:** Log shows `Assertion 'glGetError() == GL_NO_ERROR' failed` in shaders.c
**Cause:** Intel Mesa driver incompatibility with xemu's NV2A shader code.
**Fix:** Boot using "xemu (Intel Shader Fix)" option.

### Vulkan mode crashes on Intel
**Symptom:** Log shows `vk_result = -2` and "Haswell Vulkan support is incomplete"
**Cause:** Mesa's Vulkan support for older Intel GPUs is incomplete.
**Fix:** Use OpenGL mode instead (default or Intel Shader Fix).

### No audio output
**Symptom:** xemu runs but no sound from games.
**Cause:** ALSA channels muted by default, or PulseAudio not running.
**Check:**
- Run `amixer` to see mixer state
- Run `pactl info` to check PulseAudio status
- Check `/home/xbox/.config/pulse/` for PulseAudio state files
**Fix:** Ensure `.xinitrc` runs amixer unmute commands before PulseAudio starts.

---

## File Checklist for Working USB

| File | Must Contain |
|------|--------------|
| /etc/fstab | `LABEL=XEMU_ROOT` and `LABEL=XEMU_EFI` |
| /boot/efi/EFI/BOOT/grub.cfg | `search.fs_label XEMU_ROOT root` |
| /boot/grub/x86_64-efi/load.cfg | `search.fs_label XEMU_ROOT root` |
| /boot/grub/grub.cfg | `root=LABEL=XEMU_ROOT` in all linux lines |
| Partition 1 | Label: XEMU_EFI |
| Partition 2 | Label: XEMU_ROOT |

---

## Custom GRUB Entries

### /etc/grub.d/40_custom

This is a **dynamic script** (not static text) that generates menu entries at `update-grub` time. It detects the root device configuration and adapts accordingly:

- **USB boot**: Uses `root=LABEL=XEMU_ROOT` (partition has label)
- **HDD after install**: Uses `root=/dev/sdaX` or UUID (no label on installed partition)

This solves the issue where Vulkan boot worked on USB but failed after install to HDD.

The script uses the same root detection logic as Debian's `/etc/grub.d/10_linux`:
1. Checks `GRUB_DISABLE_LINUX_UUID` setting
2. Detects if partition has a label via `blkid`
3. Falls back to device path if no label exists

Generated entries include proper `insmod`, `search`, and device hints for reliable booting.

---

## Project Files

| File | Purpose |
|------|---------|
| rootfs/ | Complete root filesystem |
| build-usb.sh | CLI USB builder |
| usb-builder-gui.py | GUI USB builder |
| README.md | Quick start guide |
| XEMU-APPLIANCE-MASTER.md | This document |
