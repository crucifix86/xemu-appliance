#!/bin/bash
LOGFILE="/var/log/system-specs.log"

echo "=== SYSTEM SPECS $(date) ===" > $LOGFILE

echo -e "\n=== CPU ===" >> $LOGFILE
lscpu >> $LOGFILE 2>&1

echo -e "\n=== MEMORY ===" >> $LOGFILE
free -h >> $LOGFILE 2>&1

echo -e "\n=== GPU ===" >> $LOGFILE
lspci | grep -i vga >> $LOGFILE 2>&1
cat /sys/class/drm/card*/device/uevent 2>/dev/null >> $LOGFILE

echo -e "\n=== OPENGL ===" >> $LOGFILE
glxinfo 2>/dev/null | grep -E "OpenGL vendor|OpenGL renderer|OpenGL version" >> $LOGFILE

echo -e "\n=== KERNEL ===" >> $LOGFILE
uname -a >> $LOGFILE 2>&1

echo -e "\n=== BLOCK DEVICES ===" >> $LOGFILE
lsblk -o NAME,SIZE,TYPE,MOUNTPOINT >> $LOGFILE 2>&1

echo -e "\n=== USB DEVICES ===" >> $LOGFILE
lsusb >> $LOGFILE 2>&1

echo "=== END SPECS ===" >> $LOGFILE
