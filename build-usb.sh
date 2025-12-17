#!/bin/bash
# Build xemu appliance to USB drive
# Usage: ./build-usb.sh /dev/sdX [-y]

set -e

SKIP_CONFIRM=0
DEVICE=""

# Parse arguments
for arg in "$@"; do
    case $arg in
        -y|--yes)
            SKIP_CONFIRM=1
            ;;
        /dev/*)
            DEVICE=$arg
            ;;
    esac
done

if [ -z "$DEVICE" ]; then
    echo "Usage: $0 /dev/sdX [-y]"
    echo "  -y, --yes    Skip confirmation prompt"
    echo "WARNING: This will ERASE the target device!"
    exit 1
fi

SOURCE=/home/doug/xemu-appliance/rootfs
ROOT_LABEL="XEMU_ROOT"

# Safety check
if [[ ! "$DEVICE" =~ ^/dev/sd[a-z]$ ]]; then
    echo "Error: Device must be like /dev/sda, /dev/sdb, etc."
    exit 1
fi

echo "=== Building xemu appliance to $DEVICE ==="
echo "WARNING: This will ERASE $DEVICE!"

if [ $SKIP_CONFIRM -eq 0 ]; then
    read -p "Continue? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# 1. Unmount if mounted
echo "[1/10] Unmounting..."
sudo umount ${DEVICE}* 2>/dev/null || true

# 2. Create partition table
echo "[2/10] Creating partition table..."
sudo parted $DEVICE --script mklabel gpt
sudo parted $DEVICE --script mkpart ESP fat32 1MiB 513MiB
sudo parted $DEVICE --script set 1 esp on
sudo parted $DEVICE --script mkpart root ext4 513MiB 8705MiB

# 3. Format partitions
echo "[3/10] Formatting partitions..."
sudo mkfs.vfat -F32 ${DEVICE}1
sudo mkfs.ext4 -F -L $ROOT_LABEL ${DEVICE}2

# 4. Verify label was set
echo "[4/10] Verifying label..."
blkid ${DEVICE}2 | grep -q "LABEL=\"$ROOT_LABEL\"" || {
    echo "Error: Failed to set label"
    exit 1
}

# 5. Get EFI UUID for fstab
EFI_UUID=$(blkid -s UUID -o value ${DEVICE}1)
echo "[5/10] EFI partition UUID: $EFI_UUID"

# 6. Mount partitions
echo "[6/10] Mounting partitions..."
sudo mkdir -p /mnt/usb-root /mnt/usb-efi
sudo mount ${DEVICE}2 /mnt/usb-root
sudo mount ${DEVICE}1 /mnt/usb-efi

# 7. Copy rootfs
echo "[7/10] Copying rootfs (this may take a while)..."
sudo rsync -aHAX --info=progress2 $SOURCE/ /mnt/usb-root/

# 8. Copy EFI files
echo "[8/10] Copying EFI files..."
sudo rsync -av $SOURCE/boot/efi/ /mnt/usb-efi/

# 9. Update fstab with correct EFI UUID
echo "[9/10] Updating fstab with EFI UUID..."
sudo sed -i "s/UUID=5AFC-AA86/UUID=$EFI_UUID/" /mnt/usb-root/etc/fstab

# Verify
echo "Updated fstab:"
cat /mnt/usb-root/etc/fstab

# 10. Sync and unmount
echo "[10/10] Syncing and unmounting..."
sync
sudo umount /mnt/usb-efi /mnt/usb-root

echo ""
echo "=== BUILD COMPLETE ==="
echo "USB is ready to boot!"
echo ""
echo "Partition info:"
blkid ${DEVICE}1 ${DEVICE}2
