#!/bin/sh
# Build tls_test.exe — standalone test binary
# Run after build_full.sh (requires bearssl.lib + bearssl_bridge.o)
set -e

CC="wcc386"
CFLAGS="-bt=nt -za99 -W3 -I. -Ibearssl/inc -Ibearssl/src"

echo "=== Building tls_test.exe ==="

$CC $CFLAGS tls_test.c
echo "  Compiled: tls_test.o"

wlink NAME tls_test.exe SYSTEM nt \
    FILE tls_test.o \
    FILE bearssl_bridge.o \
    LIBRARY bearssl.lib \
    LIBRARY kernel32.lib \
    LIBRARY wsock32.lib

if [ -f tls_test.exe ]; then
    SIZE=$(ls -la tls_test.exe | awk '{print $5}')
    echo "  tls_test.exe: $SIZE bytes"
    echo "  BUILD PASSED"
else
    echo "  BUILD FAILED"
    exit 1
fi
