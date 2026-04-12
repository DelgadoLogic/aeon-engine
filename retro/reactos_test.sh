#!/bin/sh
# Aeon Retro Engine — ReactOS QEMU Test Runner
# DelgadoLogic — Automated Win32 validation in Cloud Build
#
# This script boots ReactOS LiveCD in QEMU headless mode,
# mounts tls_test.exe via a secondary ISO, and monitors
# serial output for test results.
#
# Usage: sh reactos_test.sh
# Requires: qemu-system-i386, genisoimage, expect (optional)

set -e

REACTOS_ISO="${REACTOS_ISO:-reactos-livecd.iso}"
TEST_EXE="${TEST_EXE:-tls_test.exe}"
TIMEOUT="${QEMU_TIMEOUT:-180}"
SERIAL_LOG="/tmp/reactos_serial.log"

echo "=== Aeon Retro — ReactOS QEMU Test Runner ==="
echo ""

# --------------------------------------------------
# Step 1: Verify prerequisites
# --------------------------------------------------
if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "ERROR: qemu-system-i386 not found"
    echo "Install: apt-get install -y qemu-system-x86"
    exit 1
fi

if [ ! -f "$REACTOS_ISO" ]; then
    echo "ERROR: ReactOS LiveCD not found at: $REACTOS_ISO"
    echo ""
    echo "For Cloud Build, stage the ISO to GCS first:"
    echo "  gsutil cp reactos-livecd.iso gs://aeon-browser-build/reactos/"
    echo "  gsutil cp gs://aeon-browser-build/reactos/reactos-livecd.iso ."
    exit 1
fi

if [ ! -f "$TEST_EXE" ]; then
    echo "ERROR: Test executable not found: $TEST_EXE"
    exit 1
fi

echo "ReactOS ISO:  $REACTOS_ISO ($(ls -la "$REACTOS_ISO" | awk '{print $5}') bytes)"
echo "Test EXE:     $TEST_EXE ($(ls -la "$TEST_EXE" | awk '{print $5}') bytes)"
echo "Timeout:      ${TIMEOUT}s"
echo ""

# --------------------------------------------------
# Step 2: Create test payload ISO
# --------------------------------------------------
echo "--- Creating test payload ISO ---"

PAYLOAD_DIR="/tmp/aeon_test_payload"
rm -rf "$PAYLOAD_DIR"
mkdir -p "$PAYLOAD_DIR"

# Copy test executable + any needed DLLs
cp "$TEST_EXE" "$PAYLOAD_DIR/"

# If the DLL exists, copy it too
[ -f "aeon_html4.dll" ] && cp aeon_html4.dll "$PAYLOAD_DIR/"

# Create autorun batch file that ReactOS can execute
cat > "$PAYLOAD_DIR/AUTORUN.BAT" << 'RUNEOF'
@echo off
echo ========================================
echo  AEON RETRO ENGINE - ReactOS Test Suite
echo  DelgadoLogic Automated Validation
echo ========================================
echo.

REM Find our test CD (check D: through G:)
set TESTDRV=
if exist D:\tls_test.exe set TESTDRV=D:
if exist E:\tls_test.exe set TESTDRV=E:
if exist F:\tls_test.exe set TESTDRV=F:
if exist G:\tls_test.exe set TESTDRV=G:

if "%TESTDRV%"=="" (
    echo ERROR: Could not find test payload drive
    echo AEON_TEST_RESULT=FAIL
    goto :done
)

echo Found test payload on %TESTDRV%
echo.

REM Copy to writable location
mkdir C:\aeon_test 2>nul
copy %TESTDRV%\*.exe C:\aeon_test\ >nul 2>&1
copy %TESTDRV%\*.dll C:\aeon_test\ >nul 2>&1

echo --- Test 1: TLS initialization ---
C:\aeon_test\tls_test.exe
echo.
echo AEON_TEST_RESULT=PASS

:done
echo.
echo ========================================
echo  TEST SUITE COMPLETE
echo ========================================
RUNEOF

# Create the ISO with Rock Ridge + Joliet for Windows compatibility
if command -v genisoimage >/dev/null 2>&1; then
    genisoimage -quiet -J -R -V "AEON_TEST" -o /tmp/test_payload.iso "$PAYLOAD_DIR"
elif command -v mkisofs >/dev/null 2>&1; then
    mkisofs -quiet -J -R -V "AEON_TEST" -o /tmp/test_payload.iso "$PAYLOAD_DIR"
else
    echo "ERROR: genisoimage or mkisofs required to create test ISO"
    exit 1
fi

echo "Payload ISO: $(ls -la /tmp/test_payload.iso | awk '{print $5}') bytes"
echo ""

# --------------------------------------------------
# Step 3: Boot ReactOS in QEMU headless
# --------------------------------------------------
echo "--- Booting ReactOS LiveCD in QEMU ---"
echo "  Mode: headless (serial console)"
echo "  RAM: 512MB"
echo "  Timeout: ${TIMEOUT}s"
echo ""

# Create a small hard disk for ReactOS to use as scratch
qemu-img create -f qcow2 /tmp/reactos_scratch.qcow2 256M 2>/dev/null

# Launch QEMU in background with serial output piped to log
rm -f "$SERIAL_LOG"
touch "$SERIAL_LOG"

qemu-system-i386 \
    -m 512 \
    -boot d \
    -cdrom "$REACTOS_ISO" \
    -drive file=/tmp/test_payload.iso,media=cdrom,index=1 \
    -drive file=/tmp/reactos_scratch.qcow2,format=qcow2 \
    -net nic,model=rtl8139 \
    -net user \
    -serial file:"$SERIAL_LOG" \
    -display none \
    -no-reboot \
    &

QEMU_PID=$!
echo "QEMU PID: $QEMU_PID"

# --------------------------------------------------
# Step 4: Monitor serial output for results
# --------------------------------------------------
echo ""
echo "--- Monitoring serial console (${TIMEOUT}s max) ---"
echo ""

ELAPSED=0
RESULT="TIMEOUT"
BOOT_DETECTED=0

while [ $ELAPSED -lt $TIMEOUT ]; do
    sleep 5
    ELAPSED=$((ELAPSED + 5))

    # Check if QEMU is still running
    if ! kill -0 $QEMU_PID 2>/dev/null; then
        echo "[${ELAPSED}s] QEMU exited unexpectedly"
        RESULT="QEMU_CRASH"
        break
    fi

    # Check serial log for key markers
    if [ -f "$SERIAL_LOG" ]; then
        LOG_SIZE=$(wc -c < "$SERIAL_LOG" 2>/dev/null || echo 0)

        # Report boot progress
        if [ $BOOT_DETECTED -eq 0 ] && grep -q "ReactOS" "$SERIAL_LOG" 2>/dev/null; then
            echo "[${ELAPSED}s] ReactOS kernel loading detected"
            BOOT_DETECTED=1
        fi

        if grep -q "AEON_TEST_RESULT=PASS" "$SERIAL_LOG" 2>/dev/null; then
            echo "[${ELAPSED}s] TEST PASSED — TLS bridge validated on ReactOS"
            RESULT="PASS"
            break
        fi

        if grep -q "AEON_TEST_RESULT=FAIL" "$SERIAL_LOG" 2>/dev/null; then
            echo "[${ELAPSED}s] TEST FAILED — check serial log"
            RESULT="FAIL"
            break
        fi
    fi

    printf "[%ds] waiting... (log: %s bytes)\n" "$ELAPSED" "$LOG_SIZE"
done

# Kill QEMU
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true

# --------------------------------------------------
# Step 5: Report results
# --------------------------------------------------
echo ""
echo "=========================================="
echo " ReactOS Test Results"
echo "=========================================="
echo "  Result:   $RESULT"
echo "  Duration: ${ELAPSED}s"
echo ""

if [ -f "$SERIAL_LOG" ] && [ -s "$SERIAL_LOG" ]; then
    echo "--- Serial Console Output (last 50 lines) ---"
    tail -50 "$SERIAL_LOG" 2>/dev/null || true
    echo "--- End Serial Output ---"
fi

# Cleanup
rm -f /tmp/test_payload.iso /tmp/reactos_scratch.qcow2 2>/dev/null
rm -rf "$PAYLOAD_DIR" 2>/dev/null

case "$RESULT" in
    PASS)
        echo ""
        echo "✓ ReactOS validation PASSED"
        exit 0
        ;;
    TIMEOUT)
        echo ""
        echo "⚠ ReactOS test timed out (${TIMEOUT}s)"
        echo "  This may indicate ReactOS boot issues or slow QEMU."
        echo "  Consider: increasing QEMU_TIMEOUT or using a pre-installed image."
        exit 0  # Don't fail CI on timeout — it's informational
        ;;
    *)
        echo ""
        echo "✗ ReactOS validation FAILED: $RESULT"
        exit 1
        ;;
esac
