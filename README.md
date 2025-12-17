# xemu Appliance

Standalone Xbox emulator appliance - boots directly into xemu, no desktop environment.

## Features

- Auto-boot directly into xemu
- Built-in installer to write to internal disk
- TAP networking with FTP access to Xbox
- Vulkan renderer option for Intel GPU compatibility
- NevolutionX/XBMC dashboard support

## Download

Pre-built image (8.5GB compressed):
https://www.mediafire.com/file_premium/elwe6sgs6mnpl8w/xemu-appliance-working.img.tar.gz/file

Extract and write to USB:
```bash
tar -xzf xemu-appliance-working.img.tar.gz
sudo dd if=xemu-appliance-working.img of=/dev/sdX bs=4M status=progress
```

## Boot Menu Options

1. **Debian GNU/Linux** - Default boot, OpenGL renderer
2. **xemu (Vulkan Renderer)** - For Intel GPUs with OpenGL issues
3. **Install to Disk** - Write appliance to internal storage

## Building

See `iso-build/BUILD_GUIDE.md` for full build instructions.

### Quick Start

1. Create Debian Trixie rootfs with debootstrap
2. Install dependencies (xorg, mesa, vulkan, sdl2, etc.)
3. Copy configs from `iso-build/configs/`
4. Copy xemu binary and Xbox files to `/opt/xbox/`
5. Create bootable image with GRUB

## Requirements

- xemu binary (build from https://github.com/xemu-project/xemu)
- Xbox BIOS files (mcpx_1.0.bin, Complex_4627.bin)
- Xbox HDD image (qcow2)
- Xbox EEPROM

## Network

FTP access to Xbox at 192.168.100.x (credentials: xbox/xbox)

## License

MIT
