#!/bin/bash

# Check if Vulkan mode requested
if grep -q "vulkan" /proc/cmdline; then
    # Create Vulkan config
    cat > /home/xbox/.local/share/xemu/xemu/xemu.toml << 'CONF'
[general]
show_welcome = false

[display]
renderer = 'VULKAN'

[sys.files]
bootrom_path = '/opt/xbox/bios/mcpx_1.0.bin'
flashrom_path = '/opt/xbox/bios/Complex_4627.bin'
eeprom_path = '/opt/xbox/eeprom.bin'
hdd_path = '/opt/xbox/xbox_hdd.qcow2'

[net]
enable = false
CONF
fi

exec /opt/xbox/xemu -m 64 -net nic,model=nvnet -net tap,ifname=tap0,script=no,downscript=no
