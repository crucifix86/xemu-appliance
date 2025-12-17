# xemu Binary

The xemu binary is not included in this repo (too large, ~118MB).

## Option 1: Download Pre-built Binary

Download from the xemu releases page:
https://github.com/xemu-project/xemu/releases

Extract and copy the `xemu` binary to `/opt/xbox/` in your rootfs.

## Option 2: Build from Source

```bash
# Clone xemu
git clone https://github.com/xemu-project/xemu.git
cd xemu

# Build (requires many dependencies)
./build.sh

# Binary will be in dist/xemu
cp dist/xemu /path/to/rootfs/opt/xbox/
```

## Required Xbox Files

You also need (not included for legal reasons):
- BIOS: `Complex_4627.bin` or similar → `/opt/xbox/bios/`
- MCPX ROM: `mcpx_1.0.bin` → `/opt/xbox/bios/`
- EEPROM: `eeprom.bin` → `/opt/xbox/`
- HDD Image: `xbox_hdd.qcow2` → `/opt/xbox/`
