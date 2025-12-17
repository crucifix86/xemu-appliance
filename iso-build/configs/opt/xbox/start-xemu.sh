#!/bin/bash
exec >> /var/log/xemu.log 2>&1
echo "=== Starting at $(date) ==="
echo "User: $(whoami)"
echo "TTY: $(tty)"
echo "DISPLAY: $DISPLAY"
echo "Groups: $(groups)"
echo "--- Running xinit ---"
/usr/bin/xinit /opt/xbox/xemu -m 64 -- :0 vt1
echo "=== xinit exited with code $? ==="
