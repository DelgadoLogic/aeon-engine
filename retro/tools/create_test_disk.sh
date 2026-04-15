#!/bin/sh
# Create a FAT16 disk image with payload for ReactOS testing
set -e

apt-get update -qq > /dev/null 2>&1
apt-get install -y -qq --no-install-recommends dosfstools mtools > /dev/null 2>&1

# Create 32MB FAT16 image (no partition table — ReactOS can handle raw FAT)
dd if=/dev/zero of=/work/test_disk.img bs=1M count=32 2>/dev/null
mkfs.vfat -F 16 -n "AEONTEST" /work/test_disk.img

# Inject files
mcopy -i /work/test_disk.img /work/payload/tls_test.exe ::/
mcopy -i /work/test_disk.img /work/payload/aeon_html4.dll ::/
mcopy -i /work/test_disk.img /work/payload/run.bat ::/

echo "=== Disk image contents ==="
mdir -i /work/test_disk.img ::/
echo ""
ls -la /work/test_disk.img
echo "DISK IMAGE READY"
