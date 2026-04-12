#!/bin/sh
# AeonBrowser Retro — Full 32-bit DLL build (with adapter)
# DelgadoLogic
#
# Mounts entire AeonBrowser project so adapter can resolve
# ../core/engine/AeonEngine_Interface.h
#
# Run from project root:
#   docker run --rm -v "${PWD}:/project" -w /project/retro aeon-retro-build sh build_full.sh

set -e

CC="wcc386"
CFLAGS="-bt=nt -za99 -W3 -I. -Ibearssl/inc -Ibearssl/src -I../core/engine"
BEARSSL_SRC="bearssl/src"

echo "=== AeonBrowser Retro — Full 32-bit DLL Build ==="
echo "Compiler: $(which $CC)"
echo ""

# Clean
rm -f *.o *.err bearssl.lib aeon_html4.dll aeon_html4.lib aeon_html4.map 2>/dev/null || true
rm -rf bearssl_obj 2>/dev/null || true
mkdir -p bearssl_obj

# ==========================================
# Phase 1: BearSSL Library (per-file)
# ==========================================
echo "--- Phase 1: BearSSL Library ---"

ERRORS=0
COMPILED=0
SKIPPED=0

for src in $(find $BEARSSL_SRC -name '*.c' | sort); do
    base=$(basename "$src" .c)
    
    case "$base" in
        *_x86ni*|*_pclmul*|*_pwr8*|*_sse2*|ghash_pclmul)
            SKIPPED=$((SKIPPED + 1)); continue ;;
        ec_c25519_m62|ec_c25519_m64|ec_p256_m62|ec_p256_m64)
            SKIPPED=$((SKIPPED + 1)); continue ;;
        *_ct64*|aes_ct64*|poly1305_ctmulq)
            SKIPPED=$((SKIPPED + 1)); continue ;;
        i62_modpow2|rsa_i62_*)
            SKIPPED=$((SKIPPED + 1)); continue ;;
    esac
    
    outname="bearssl_obj/${base}.o"
    if $CC -bt=nt -za99 -Ibearssl/inc -Ibearssl/src -fo="$outname" "$src" > /dev/null 2>&1; then
        COMPILED=$((COMPILED + 1))
    else
        echo "  FAIL: $src"
        ERRORS=$((ERRORS + 1))
    fi
done

echo "BearSSL: $COMPILED compiled, $SKIPPED skipped, $ERRORS errors"

if [ $ERRORS -gt 0 ]; then
    echo "FATAL: BearSSL compilation failed"
    exit 1
fi

echo "Creating bearssl.lib..."
wlib -n -q bearssl.lib bearssl_obj/*.o
echo "bearssl.lib: $(ls -la bearssl.lib | awk '{print $5}') bytes"

# ==========================================
# Phase 2: Application Code
# ==========================================
echo ""
echo "--- Phase 2: Application Code ---"

echo "[1/3] aeon_html4.c..."
$CC $CFLAGS -DAEON_HTML4_BUILD aeon_html4.c
echo "  -> OK ($(ls -la aeon_html4.o | awk '{print $5}') bytes)"

echo "[2/3] bearssl_bridge.c (+ trust anchors)..."
$CC $CFLAGS -DAEON_HTML4_BUILD bearssl_bridge.c
echo "  -> OK ($(ls -la bearssl_bridge.o | awk '{print $5}') bytes)"

echo "[3/3] aeon_html4_adapter.c (+ ABI shim)..."
$CC $CFLAGS -DAEON_HTML4_BUILD aeon_html4_adapter.c
echo "  -> OK ($(ls -la aeon_html4_adapter.o | awk '{print $5}') bytes)"

# ==========================================
# Phase 3: Link DLL
# ==========================================
echo ""
# Note: EXPORT directives not needed — __declspec(dllexport) in source
# code handles symbol export automatically.
wlink NAME aeon_html4.dll \
    SYSTEM nt_dll INITINSTANCE TERMINSTANCE \
    OPTION MAP \
    OPTION IMPLIB=aeon_html4.lib \
    LIBRARY kernel32.lib \
    LIBRARY user32.lib \
    LIBRARY gdi32.lib \
    LIBRARY wsock32.lib \
    LIBRARY advapi32.lib \
    FILE aeon_html4.o \
    FILE aeon_html4_adapter.o \
    FILE bearssl_bridge.o \
    LIBRARY bearssl.lib

echo ""
echo "========================================="
echo "   BUILD RESULTS"
echo "========================================="
if [ -f aeon_html4.dll ]; then
    DLL_SIZE=$(ls -la aeon_html4.dll | awk '{print $5}')
    LIB_SIZE=$(ls -la aeon_html4.lib | awk '{print $5}')
    BSL_SIZE=$(ls -la bearssl.lib | awk '{print $5}')
    echo "  aeon_html4.dll: $DLL_SIZE bytes"
    echo "  aeon_html4.lib: $LIB_SIZE bytes"
    echo "  bearssl.lib:    $BSL_SIZE bytes"
    echo ""
    echo "  BUILD PASSED ✓"
else
    echo "  DLL NOT GENERATED"
    echo "  BUILD FAILED ✗"
    exit 1
fi
