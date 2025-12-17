# XEMU Standalone Appliance

## Project Goal

Create a dedicated Xbox emulator appliance that boots directly into xemu - no desktop environment, no login, just power on and play.

---

## Phase 1: Working xemu Setup (COMPLETE)

**Dashboard:** Xbox Media Center (XBMC) - `/home/doug/Downloads/XBMC`
**Network:** TAP interface + dnsmasq DHCP
**FTP:** Direct to Xbox IP (192.168.100.x), credentials: xbox/xbox
**RAM:** 64MB (stock Xbox spec, 128MB causes GPU crashes)

### Launch Command
```bash
~/Downloads/xemu-sourc/dist/xemu -m 64 -net nic,model=nvnet -net tap,ifname=tap0,script=no,downscript=no
```

### Network Setup
```bash
# TAP interface
sudo ip tuntap add tap0 mode tap user $USER
sudo ip link set tap0 up
sudo ip addr add 192.168.100.1/24 dev tap0

# IP forwarding + NAT
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -s 192.168.100.0/24 -o wlo1 -j MASQUERADE

# DHCP server
sudo dnsmasq --interface=tap0 --dhcp-range=192.168.100.10,192.168.100.50,12h --bind-interfaces --no-daemon &
```

---

## Phase 2: Standalone Appliance Build

### Overview

```
┌─────────────────────────────────────────┐
│              POWER ON                   │
└─────────────┬───────────────────────────┘
              │
┌─────────────▼───────────────────────────┐
│         Debian Kernel                   │
│  - All drivers included                 │
│  - Intel/AMD/NVIDIA GPU support         │
│  - USB, Audio, Network                  │
└─────────────┬───────────────────────────┘
              │
┌─────────────▼───────────────────────────┐
│       Minimal Rootfs (~500MB)           │
│  - busybox or minimal Debian            │
│  - Mesa (OpenGL)                        │
│  - SDL2, PulseAudio/ALSA                │
│  - dnsmasq                              │
└─────────────┬───────────────────────────┘
              │
┌─────────────▼───────────────────────────┐
│         Auto-start xemu                 │
│  - getty autologin                      │
│  - xinit → xemu fullscreen              │
│  - TAP network auto-configured          │
└─────────────┬───────────────────────────┘
              │
┌─────────────▼───────────────────────────┐
│            XBMC Dashboard               │
│  - Games on HDD                         │
│  - FTP accessible from network          │
└─────────────────────────────────────────┘
```

### Components

| Component | Choice | Reason |
|-----------|--------|--------|
| Kernel | Debian stock | All drivers included, well-tested |
| Init | systemd or runit | Service management |
| Display | Xorg minimal | xemu needs OpenGL |
| Graphics | Mesa | Intel/AMD, or NVIDIA proprietary |
| Audio | PulseAudio or ALSA | xemu audio output |
| Network | dnsmasq + iproute2 | TAP + DHCP for Xbox |
| Rootfs | debootstrap | Minimal Debian base |

### Build Steps

1. **Create minimal rootfs with debootstrap**
   ```bash
   sudo debootstrap --variant=minbase trixie /mnt/xemu-rootfs
   ```
   Note: Must use Trixie (not bookworm) - xemu requires glibc 2.38+

2. **Install required packages**
   ```bash
   chroot /mnt/xemu-rootfs apt install \
     linux-image-amd64 \
     xorg xserver-xorg-input-evdev \
     mesa-utils libgl1-mesa-dri \
     libsdl2-2.0-0 \
     pulseaudio libasound2 \
     dnsmasq \
     iproute2 iptables \
     libslirp0 libgtk-3-0 libepoxy0 libpcap0.8 \
     libcurl4 libsamplerate0 xxhash \
     rsync parted dosfstools e2fsprogs
   ```

3. **Copy xemu and Xbox files**
   - xemu binary
   - Xbox HDD (qcow2)
   - BIOS files
   - EEPROM

4. **Configure auto-start**
   - /etc/systemd/system/xemu.service
   - Auto-login on tty1
   - xinit script to launch xemu fullscreen

5. **Configure networking**
   - systemd-networkd for TAP interface
   - dnsmasq service for DHCP
   - iptables rules persistent

6. **Create bootable image**
   - Partition: EFI + rootfs
   - Install GRUB
   - Write to USB/SSD

### Target Output

- Bootable USB/SSD image (~2GB)
- Power on → 10 seconds → XBMC dashboard
- FTP accessible at Xbox IP
- Controller support via USB

---

## Phase 3: Expand Xbox HDD

Current HDD is 8GB - fine for testing, need more for games.

### Xbox Partition Layout

| Partition | Drive | Size (Stock) | Purpose |
|-----------|-------|--------------|---------|
| 0 | - | 750KB | Config |
| 1 | C: | 500MB | Dashboard, system |
| 2 | E: | 4.8GB | Games, apps |
| 3 | X: | 750MB | Cache |
| 4 | Y: | 750MB | Cache |
| 5 | Z: | 750MB | Cache |
| 6 | F: | - | Extended (softmod) |
| 7 | G: | - | Extended (softmod) |

### Expansion Options

1. **Expand E: partition** - Resize existing partition
2. **Add F: and G: partitions** - Xbox supports up to 137GB per extended partition
3. **Create larger qcow2 image** - New 500GB+ virtual HDD

### Steps to Expand

```bash
# 1. Create new larger qcow2 (e.g., 500GB)
qemu-img create -f qcow2 xbox_hdd_500gb.qcow2 500G

# 2. Use fatxfs or XboxHDM to format with extended partitions
# F: and G: partitions unlock with larger drives

# 3. Copy existing C: and E: contents from old HDD
# Or start fresh with XBMC install
```

### Tools

- **qemu-img** - Create/resize qcow2 images
- **fatxfs** - Mount FATX partitions
- **XboxHDM** - Windows tool for Xbox HDD management
- **xboxhdm23usb** - Linux alternative

---

## Key Files

| File | Location |
|------|----------|
| xemu binary | `/home/doug/Downloads/xemu-sourc/dist/xemu` |
| Xbox HDD | `/home/doug/Downloads/xbox_hdd.qcow2` |
| XBMC | `/home/doug/Downloads/XBMC` |
| xemu config | `~/.local/share/xemu/xemu/xemu.toml` |
| BIOS | `/home/doug/Downloads/Complex_4627.bin` |
| MCPX ROM | `/home/doug/Downloads/Boot ROM Image/mcpx_1.0.bin` |

---

## Notes

- 128MB RAM causes NV2A GPU assertion crashes, stick with 64MB
- xemu NAT/SLIRP networking is broken, must use TAP
- Old-style `-net` args work, new `-netdev` style ignored

---

## Troubleshooting

### X Lock File Error
**Error:** `fatal error could not create lock file in /tmp/.X0-lock`

**Fix:** /tmp permissions or missing tmpfs mount

```bash
# In rootfs, ensure /tmp has correct permissions
chmod 1777 /tmp

# Or add tmpfs mount to /etc/fstab
tmpfs /tmp tmpfs defaults,nosuid,nodev,mode=1777 0 0
```

### Xauthority Lock Error
**Error:** `error in locking authority file /home/xbox/.Xauthority`

**Fix:** Create .Xauthority file with correct ownership

```bash
touch /home/xbox/.Xauthority
chown 1000:1000 /home/xbox/.Xauthority
chmod 600 /home/xbox/.Xauthority
```

### Xorg Log Directory Missing
**Error:** `cannot open log file /home/xbox/.local/share/xorg/Xorg.0.log`

**Fix:** Create the xorg directory

```bash
mkdir -p /home/xbox/.local/share/xorg
chown -R 1000:1000 /home/xbox/.local
```

### Hostname Resolution Failure
**Error:** `hostname resolution failure`

**Fix:** Configure /etc/hostname and /etc/hosts

```bash
# /etc/hostname
xbox

# /etc/hosts
127.0.0.1    localhost
127.0.1.1    xbox
```

---

## Appliance Files

| File | Location |
|------|----------|
| Source rootfs | `/home/doug/xemu-appliance/rootfs/` |
| Bootable image | `/home/doug/xemu-appliance/xemu-appliance.img` |
| Xbox files in appliance | `/opt/xbox/` |
| xemu config in appliance | `/home/xbox/.local/share/xemu/xemu/` |

---

## Writing Image to USB

```bash
# Write image to USB (replace /dev/sdX with your USB device)
sudo dd if=/home/doug/xemu-appliance/xemu-appliance.img of=/dev/sdX bs=4M status=progress

# Optional: Expand partition to use full USB capacity
sudo sgdisk -e /dev/sdX                    # Fix GPT to use all space
sudo sgdisk -d 2 -n 2:1050624:0 -t 2:8300 /dev/sdX  # Recreate partition 2
sudo e2fsck -f /dev/sdX2                   # Check filesystem
sudo resize2fs /dev/sdX2                   # Expand filesystem
```

---

## Build Issues Fixed (2025-12-17)

### Problem 1: Image Too Small
- **Issue:** Created 4GB image, partition only 3.5GB for rootfs
- **Fix:** Create 8GB image with proper partition sizes

### Problem 2: Wrong Debian Version
- **Issue:** Rootfs built with bookworm (glibc 2.36), xemu needs glibc 2.38+
- **Fix:** Use Debian Trixie for rootfs (glibc 2.41)

### Problem 3: Missing xemu Dependencies
- **Issue:** xemu binary copied but shared libraries not installed
- **Error:** `libslirp.so.0: cannot open shared object file`
- **Fix:** Install all xemu dependencies in chroot:
  ```bash
  chroot /mnt/rootfs apt install libslirp0 libgtk-3-0 libsdl2-2.0-0 \
    libepoxy0 libpcap0.8 libcurl4 libsamplerate0 xxhash pulseaudio libasound2
  ```

### Correct Build Process
1. Create image with sufficient size (8GB+)
2. Use Debian Trixie (not bookworm) to match host glibc
3. Install ALL xemu dependencies via apt in chroot
4. Copy xemu binary and Xbox files
5. Install GRUB with `--removable` flag for USB portability

---

**Last Updated:** 2025-12-17
