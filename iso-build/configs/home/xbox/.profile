echo "=== Login at $(date) ===" >> /var/log/xemu.log 2>&1
echo "User: $(whoami)" >> /var/log/xemu.log 2>&1
echo "TTY: $(tty)" >> /var/log/xemu.log 2>&1
if [ -z "$DISPLAY" ] && [ "$(tty)" = "/dev/tty1" ]; then
    if grep -q "installer" /proc/cmdline; then
        echo "--- Starting Installer ---" >> /var/log/xemu.log 2>&1
        sudo /usr/local/bin/install-to-disk
    else
        echo "--- Starting X ---" >> /var/log/xemu.log 2>&1
        startx >> /var/log/xemu.log 2>&1
        echo "=== startx exited $? ===" >> /var/log/xemu.log 2>&1
    fi
fi
