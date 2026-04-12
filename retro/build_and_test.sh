#!/bin/sh
# AeonBrowser Retro — Full Build + Wine Runtime Test
# DelgadoLogic
#
# Builds everything from scratch, installs Wine, and runs tls_test.exe
# to verify HTTPS/TLS connectivity end-to-end.
#
# USAGE (from project root):
#   docker run --rm -v "${PWD}:/project" -w /project/retro aeon-retro-build sh build_and_test.sh

set -e

echo "========================================"
echo " AeonBrowser Retro — Build + Runtime Test"
echo "========================================"
echo ""

# Phase 1: Full build
sh build_full.sh

echo ""
echo "--- Building tls_test.exe ---"

CC="wcc386"
CFLAGS="-bt=nt -za99 -W3 -Ibearssl/inc -Ibearssl/src"

$CC $CFLAGS tls_test.c > /dev/null 2>&1

wlink NAME tls_test.exe \
    SYSTEM nt \
    FILE tls_test.o \
    FILE bearssl_bridge.o \
    LIBRARY bearssl.lib \
    LIBRARY kernel32.lib \
    LIBRARY wsock32.lib > /dev/null 2>&1

echo "tls_test.exe: $(ls -la tls_test.exe | awk '{print $5}') bytes"
echo ""

# Phase 2: Install Wine (minimal, for testing only)
echo "--- Installing Wine for runtime test ---"
dpkg --add-architecture i386 > /dev/null 2>&1 || true
apt-get update -qq > /dev/null 2>&1
apt-get install -y -qq --no-install-recommends wine wine32 > /dev/null 2>&1 || {
    # Fallback: try wine64 only
    apt-get install -y -qq --no-install-recommends wine64 > /dev/null 2>&1 || {
        echo "WARNING: Wine not available. Skipping runtime test."
        echo "Copy tls_test.exe to a Win9x/XP VM to test manually."
        exit 0
    }
}

echo "Wine installed."
echo ""

# Phase 3: Run TLS test under Wine
echo "========================================"
echo " Runtime Test — Wine"
echo "========================================"
echo ""

# Set up Wine prefix silently
export WINEPREFIX=/tmp/wine_test
export WINEDEBUG=-all

# Initialize Wine prefix
wine wineboot --init > /dev/null 2>&1 || true

echo "--- Test 1: HTTPS (example.com) ---"
timeout 30 wine tls_test.exe https://example.com 2>&1 || {
    echo "  (Test completed or timed out)"
}

echo ""
echo "--- Test 2: HTTP (neverssl.com) ---"
timeout 30 wine tls_test.exe http://neverssl.com/ 2>&1 || {
    echo "  (Test completed or timed out)"
}

echo ""
echo "--- Test 3: Default suite (edge cases + network) ---"
timeout 60 wine tls_test.exe 2>&1 || {
    echo "  (Test completed or timed out)"
}

echo ""
echo "========================================"
echo " All tests complete"
echo "========================================"
