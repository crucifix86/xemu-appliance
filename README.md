# xemu Appliance

A standalone bootable Linux system that runs [xemu](https://xemu.app) - the original Xbox emulator. Boot directly into the emulator from USB or install to internal disk.

## Features

- **Boots directly into xemu** - No desktop environment, just the emulator
- **USB bootable** - Create a bootable USB drive in minutes
- **Disk installer** - Install to internal drive for permanent setup
- **Audio support** - PulseAudio with automatic ALSA unmuting
- **Network bridging** - TAP interface with DHCP for FTP game transfers
- **Vulkan support** - Optional Vulkan rendering via kernel parameter

## Quick Start

### Build a USB Drive

**Option 1: GUI Tool**
```bash
python3 usb-builder-gui.py
```

**Option 2: Command Line**
```bash
./build-usb.sh /dev/sdX
```

**Warning:** This will erase the target device completely.

### Boot Options

At the GRUB menu:
- **Debian GNU/Linux** - Normal boot, starts xemu with OpenGL
- **Install to Disk** - Interactive installer for internal drive
- **xemu (Vulkan Renderer)** - Boot with Vulkan instead of OpenGL
- **xemu (Intel Shader Fix)** - OpenGL with Mesa workarounds for Intel GPUs

### GPU Compatibility

| GPU | Recommended Boot |
|-----|------------------|
| Most GPUs | Debian GNU/Linux (default) |
| Intel Haswell (Iris Pro) | xemu (Intel Shader Fix) |
| Intel Apollo Lake | Works with default or Vulkan |

**Note:** Vulkan does not work on Intel Haswell GPUs due to incomplete Mesa support.

## System Architecture

```
EFI → GRUB → Kernel → systemd
                         ├── xbox-network.service (TAP + DHCP)
                         ├── getty@tty1 (auto-login)
                         └── xemu.service
                              └── xinit → xemu
```

## Network Setup

The appliance creates a virtual network for Xbox connectivity:

| Component | Value |
|-----------|-------|
| Host IP | 192.168.100.1 |
| DHCP Range | 192.168.100.10-50 |
| Interface | tap0 |

This allows FTP transfers to the emulated Xbox for loading games.

## Directory Structure

```
/opt/xbox/
├── xemu              # Emulator binary
├── xbox_hdd.qcow2    # Virtual Xbox HDD (8GB)
├── bios/
│   ├── mcpx_1.0.bin  # MCPX bootrom
│   └── Complex_4627.bin  # Flash ROM
├── eeprom.bin        # EEPROM
├── setup-network.sh  # Network init script
└── start-xemu.sh     # Emulator wrapper

/home/xbox/
├── .xinitrc          # X session config
├── .profile          # Auto-start logic
└── .local/share/xemu/  # Shader cache & config
```

## Build Tools

| File | Description |
|------|-------------|
| `build-usb.sh` | Command-line USB builder |
| `usb-builder-gui.py` | GUI USB builder (Tkinter) |

Both tools:
1. Create GPT partition table (EFI + ext4)
2. Format with LABEL=XEMU_ROOT
3. Copy rootfs via rsync
4. Update fstab with correct EFI UUID

## System Specs

- **Base OS:** Debian 13 (trixie)
- **Kernel:** 6.12.57+deb13-amd64
- **Display:** X.Org 1.21.1.7
- **Audio:** PulseAudio
- **User:** xbox (auto-login)

## Requirements

- UEFI-capable system
- USB drive (8GB+ recommended)
- Xbox BIOS files (not included)

## License

xemu is licensed under GPLv2. This appliance distribution includes:
- Debian base system (various licenses)
- xemu (GPLv2)
- Custom scripts and configuration (MIT)
