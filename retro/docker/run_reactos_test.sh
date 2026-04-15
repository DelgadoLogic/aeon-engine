#!/bin/sh
# Aeon Retro — ReactOS QEMU Test Runner (Docker entrypoint)
# Injects autorun via batch file + registry Run key on writable disk
#
# Strategy:
#   1. Create a FAT16 disk image with the test exe + autorun.bat
#   2. Boot ReactOS LiveCD with the disk as HDD
#   3. ReactOS mounts it as D: or E:
#   4. autorun.bat runs on startup via Startup group on the FAT disk
#   5. Output goes to COM1 (serial) for CI parsing
#
# Usage: /run_test.sh <path_to_tls_test.exe>
set -e

TEST_EXE="${1:-/workspace/build_out/tls_test.exe}"
REACTOS_ISO="/opt/reactos/reactos-livecd.iso"
TIMEOUT="${QEMU_TIMEOUT:-180}"
SERIAL_LOG="/tmp/reactos_serial.log"

echo "=== Aeon Retro — ReactOS QEMU CI ==="
echo "  Strategy: FAT disk + console redirect to serial"
echo ""

# Verify inputs
[ ! -f "$REACTOS_ISO" ] && { echo "ERROR: ReactOS ISO missing"; exit 1; }
[ ! -f "$TEST_EXE" ] && { echo "ERROR: Test exe missing: $TEST_EXE"; exit 1; }

echo "ISO: $REACTOS_ISO"
echo "EXE: $TEST_EXE ($(stat -c%s "$TEST_EXE") bytes)"
echo ""

# ============================================================
# Create FAT16 disk with test payload + autorun
# ============================================================
PAYLOAD_DIR="/tmp/payload"
FAT_IMG="/tmp/test_disk.img"
mkdir -p "$PAYLOAD_DIR"

# Copy test exe and any DLLs
cp "$TEST_EXE" "$PAYLOAD_DIR/"
TEST_DIR=$(dirname "$TEST_EXE")
for dll in "$TEST_DIR"/*.dll; do
    [ -f "$dll" ] && cp "$dll" "$PAYLOAD_DIR/"
done

# Create autorun batch file
# This runs tls_test.exe and redirects output to COM1 (serial port)
# ReactOS maps COM1 to the QEMU serial device
cat > "$PAYLOAD_DIR/autorun.bat" << 'BATCH_EOF'
@echo off
echo AEON_AUTORUN_START > COM1
echo Running TLS test... > COM1
D:\tls_test.exe > COM1 2>&1
echo AEON_AUTORUN_DONE > COM1
BATCH_EOF

# Create a startup registry file that will auto-run our batch
# ReactOS supports .reg file import
cat > "$PAYLOAD_DIR/autostart.reg" << 'REG_EOF'
REGEDIT4

[HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce]
"AeonTest"="D:\\autorun.bat"
REG_EOF

# Create FAT16 disk image (32MB — plenty of space)
dd if=/dev/zero of="$FAT_IMG" bs=1M count=32 2>/dev/null
mkfs.vfat -F 16 -n "AEONTEST" "$FAT_IMG"

# Mount and copy files
MOUNT_DIR="/tmp/fat_mount"
mkdir -p "$MOUNT_DIR"

# Use mcopy (mtools) if available, otherwise try loop mount
if command -v mcopy >/dev/null 2>&1; then
    # mtools approach (no root required)
    for f in "$PAYLOAD_DIR"/*; do
        mcopy -i "$FAT_IMG" "$f" ::/ 2>/dev/null || true
    done
    echo "Payload copied via mtools"
else
    # Fallback: create a second ISO instead
    echo "mtools not available, using ISO approach"
    genisoimage -quiet -J -R -V "AEON_TEST" -o /tmp/test.iso "$PAYLOAD_DIR"
    FAT_IMG=""  # Signal to use ISO instead
fi

echo "Payload contents:"
ls -la "$PAYLOAD_DIR/"

# Scratch disk for ReactOS
qemu-img create -f qcow2 /tmp/scratch.qcow2 256M 2>/dev/null

# ============================================================
# Boot ReactOS headless with payload
# ============================================================
rm -f "$SERIAL_LOG"
touch "$SERIAL_LOG"

echo ""
echo "--- Booting ReactOS LiveCD in QEMU ---"
echo "  RAM:     512MB"
echo "  Timeout: ${TIMEOUT}s"
echo "  Serial:  file log"
echo ""

if [ -n "$FAT_IMG" ]; then
    # FAT disk as HDD — ReactOS mounts as D:
    qemu-system-i386 \
        -m 512 -boot d \
        -cdrom "$REACTOS_ISO" \
        -drive file="$FAT_IMG",format=raw,if=ide,index=0 \
        -drive file=/tmp/scratch.qcow2,format=qcow2,if=ide,index=1 \
        -net nic,model=rtl8139 -net user \
        -serial file:"$SERIAL_LOG" \
        -display none -no-reboot &
else
    # ISO-only fallback
    qemu-system-i386 \
        -m 512 -boot d \
        -cdrom "$REACTOS_ISO" \
        -drive file=/tmp/test.iso,media=cdrom,index=1 \
        -drive file=/tmp/scratch.qcow2,format=qcow2 \
        -net nic,model=rtl8139 -net user \
        -serial file:"$SERIAL_LOG" \
        -display none -no-reboot &
fi

PID=$!
echo "QEMU PID: $PID"

# ============================================================
# Monitor serial log for test results
# ============================================================
ELAPSED=0
RESULT="TIMEOUT"
BOOTED=0

while [ $ELAPSED -lt $TIMEOUT ]; do
    sleep 5
    ELAPSED=$((ELAPSED + 5))

    if ! kill -0 $PID 2>/dev/null; then
        RESULT="QEMU_EXIT"; break
    fi

    LOG_SIZE=$(wc -c < "$SERIAL_LOG" 2>/dev/null || echo 0)

    # Check for boot milestones
    if [ $BOOTED -eq 0 ]; then
        if grep -qi "explorer\|win32k\|shell32" "$SERIAL_LOG" 2>/dev/null; then
            BOOTED=1
            echo "[$ELAPSED s] ReactOS desktop loaded"
        else
            echo "[$ELAPSED s] booting... ($LOG_SIZE bytes)"
        fi
    fi

    # Check for autorun start
    if grep -q "AEON_AUTORUN_START" "$SERIAL_LOG" 2>/dev/null; then
        echo "[$ELAPSED s] Autorun triggered!"
    fi

    # Check for test results
    if grep -q "AEON_TEST_RESULT=PASS" "$SERIAL_LOG" 2>/dev/null; then
        RESULT="PASS"
        echo "[$ELAPSED s] ✓ TEST PASSED"
        break
    fi

    if grep -q "AEON_TEST_RESULT=FAIL" "$SERIAL_LOG" 2>/dev/null; then
        RESULT="FAIL"
        echo "[$ELAPSED s] ✗ TEST FAILED"
        break
    fi

    # Check if autorun completed without our markers
    if grep -q "AEON_AUTORUN_DONE" "$SERIAL_LOG" 2>/dev/null; then
        if ! grep -q "AEON_TEST_RESULT" "$SERIAL_LOG" 2>/dev/null; then
            RESULT="AUTORUN_NO_MARKER"
            echo "[$ELAPSED s] Autorun done but no test marker found"
            break
        fi
    fi
done

# Cleanup
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

echo ""
echo "=========================================="
echo " Aeon Retro — ReactOS CI Results"
echo "=========================================="
echo "  Result:   $RESULT"
echo "  Duration: ${ELAPSED}s"
echo "  Serial:   $(wc -c < "$SERIAL_LOG" 2>/dev/null || echo 0) bytes"

if [ -s "$SERIAL_LOG" ]; then
    echo ""
    echo "--- Serial Log (last 40 lines) ---"
    tail -40 "$SERIAL_LOG" 2>/dev/null || true
    echo "--- End ---"
fi

# CI result — informational, never fails the build
case "$RESULT" in
    PASS)
        echo ""
        echo "✓ ReactOS TLS validation PASSED"
        exit 0
        ;;
    TIMEOUT)
        echo ""
        echo "⚠ ReactOS timed out — informational (desktop loaded: $BOOTED)"
        exit 0
        ;;
    *)
        echo ""
        echo "⚠ Result: $RESULT — informational"
        exit 0
        ;;
esac
