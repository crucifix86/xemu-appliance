# xemu Appliance

Standalone Xbox emulator appliance - boots directly into xemu, no desktop environment. Power on and play.

## Features

- **Direct boot** - Power on → Xbox dashboard in ~10 seconds
- **No desktop** - Minimal Debian, straight to xemu fullscreen
- **Built-in installer** - Write to internal SSD with graphical installer
- **TAP networking** - FTP access to Xbox at 192.168.100.x
- **Raw partition support** - Xbox HDD on dedicated partition for best I/O
- **Performance optimized** - Hugepages, I/O scheduler tuning, mitigations=off

## Download

Pre-built image:
https://www.mediafire.com/file_premium/elwe6sgs6mnpl8w/xemu-appliance-working.img.tar.gz/file

Extract and write to USB:
```bash
tar -xzf xemu-appliance-working.img.tar.gz
sudo dd if=xemu-appliance-working.img of=/dev/sdX bs=4M status=progress
```

## Boot Menu Options

| Option | Description |
|--------|-------------|
| Debian GNU/Linux | Default boot, OpenGL renderer |
| xemu (Vulkan) | For GPUs where OpenGL has issues |
| Install to Disk | Write appliance to internal SSD |

## Installation

The installer creates 3 partitions:

| Partition | Size | Purpose |
|-----------|------|---------|
| EFI | 512MB | GRUB bootloader |
| ROOT | 10GB | Debian system + xemu |
| XBOX_HDD | Remaining | Raw Xbox HDD (no qcow2 overhead) |

## Performance Optimizations

### Active (Confirmed Working)

| Optimization | Benefit |
|--------------|---------|
| `mitigations=off` | 5-15% CPU gain |
| `cache_shaders = true` | Reduces shader compile stutter |
| `hard_fpu = true` | Hardware FPU emulation |
| `vsync = false` | Uncapped framerate, less input lag |
| Raw Xbox HDD partition | Direct block I/O, no qcow2 overhead |
| Hugepages (128MB) | Reduced TLB misses for Xbox RAM |
| I/O scheduler `mq-deadline` | Optimal for SSD |

### Avoided (Tested, Caused Issues)

| Optimization | Problem |
|--------------|---------|
| `threadirqs` kernel param | Made entire OS sluggish |
| `use_dsp = false` | Freezes games (audio timing dependent) |
| `num_workers = 1` audio | Audio crackling |
| IRQ affinity | Breaks on 4-core systems |

## Hardware Compatibility

### Tested Working

| CPU | GPU | Status |
|-----|-----|--------|
| Intel Pentium J4205 | HD Graphics 505 | Works (slow but playable) |
| AMD Ryzen 3 2200G | Vega 8 | Planned (should be excellent) |

### Known Issues

| CPU | GPU | Issue |
|-----|-----|-------|
| Intel i5-4570R | Iris Pro 5200 | Shader assertion crash |

## Xbox HDD Storage

| Boot Mode | Storage Type | Path |
|-----------|--------------|------|
| USB Boot | qcow2 image | `/opt/xbox/xbox_hdd.qcow2` |
| Installed | Raw partition | `/dev/sdX3` |

## Network Setup

- Xbox gets DHCP from appliance (192.168.100.x range)
- FTP: Connect to Xbox IP, credentials `xbox/xbox`
- Dashboard handles all network config

## Building from Source

1. Create Debian Trixie rootfs with debootstrap
2. Install dependencies: `xorg mesa-vulkan-drivers libsdl2-2.0-0 pulseaudio dnsmasq`
3. Copy configs from `iso-build/configs/`
4. Copy xemu binary and Xbox files to `/opt/xbox/`
5. Use `build-usb.sh` to create bootable USB

## Requirements

- xemu binary (build from https://github.com/xemu-project/xemu)
- Xbox BIOS: `mcpx_1.0.bin`, `Complex_4627.bin`
- Xbox HDD image (qcow2 or raw)
- Xbox EEPROM: `eeprom.bin`

## Files

| File | Description |
|------|-------------|
| `iso-build/configs/` | System configuration files |
| `xemu-unleashx-integration-plan.md` | Detailed project documentation |
| `PERFORMANCE_RESEARCH.md` | Performance testing notes |

## License

MIT
