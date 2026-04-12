#!/bin/sh
# AeonBrowser Retro — Full 32-bit DLL build via Docker/OW2
# DelgadoLogic
#
# Compiles all BearSSL files individually (avoiding symbol collisions),
# archives into bearssl.lib, then links the full DLL.

set -e

CC="wcc386"
CFLAGS="-bt=nt -za99 -W3 -Ibearssl/inc -Ibearssl/src"
BEARSSL_SRC="bearssl/src"

echo "=== AeonBrowser Retro 32-bit DLL Build ==="
echo "Compiler: $(which $CC)"
echo ""

# Clean
rm -f *.o *.err bearssl.lib 2>/dev/null || true
rm -rf bearssl_obj 2>/dev/null || true
mkdir -p bearssl_obj

ERRORS=0
COMPILED=0
SKIPPED=0

# -------------------------
# Phase 1: Compile BearSSL
# -------------------------
echo "--- Phase 1: BearSSL Library ---"

for src in $(find $BEARSSL_SRC -name '*.c' | sort); do
    base=$(basename "$src" .c)
    
    # Skip platform-specific files (64-bit, intrinsics, POWER8)
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
    if $CC $CFLAGS -fo="$outname" "$src" > /dev/null 2>&1; then
        COMPILED=$((COMPILED + 1))
    else
        echo "  FAIL: $src"
        # Try without -W3 for noisy files
        if $CC -bt=nt -za99 -Ibearssl/inc -Ibearssl/src -fo="$outname" "$src" > /dev/null 2>&1; then
            COMPILED=$((COMPILED + 1))
            echo "  -> Recovered without -W3"
        else
            ERRORS=$((ERRORS + 1))
        fi
    fi
done

echo "BearSSL: $COMPILED compiled, $SKIPPED skipped, $ERRORS errors"

if [ $ERRORS -gt 0 ]; then
    echo "FAILED: $ERRORS BearSSL files had errors"
    exit 1
fi

# Archive into static library
echo ""
echo "--- Creating bearssl.lib ---"
wlib -n bearssl.lib bearssl_obj/*.o
echo "bearssl.lib: $(ls -la bearssl.lib | awk '{print $5}') bytes"

# -------------------------
# Phase 2: Application code
# -------------------------
echo ""
echo "--- Phase 2: Application Code ---"

echo "[1/2] bearssl_bridge.c (+ trust_anchors.h)..."
$CC $CFLAGS bearssl_bridge.c
echo "  -> bearssl_bridge.o OK"

echo "[2/2] aeon_html4.c..."
$CC $CFLAGS -DAEON_HTML4_BUILD aeon_html4.c
echo "  -> aeon_html4.o OK"

# -------------------------
# Phase 3: Link DLL
# -------------------------
echo ""
echo "--- Phase 3: Link aeon_html4.dll ---"

# Note: aeon_html4_adapter.c can't compile without ../core/engine headers
# which are outside the retro directory. Skip adapter in Docker build.
# The real build on Windows will include the adapter.
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
    FILE bearssl_bridge.o \
    LIBRARY bearssl.lib

echo ""
echo "=== BUILD RESULTS ==="
ls -la aeon_html4.dll aeon_html4.lib bearssl.lib 2>/dev/null
echo ""
echo "BUILD PASSED"
