#!/bin/sh
# BearSSL Skip List Audit
# Verifies we're not missing any needed files

echo "=== BearSSL Skip List Audit ==="
echo ""

TOTAL=$(find bearssl/src -name '*.c' | wc -l)
COMPILED=0
SKIPPED=0
SKIP_LIST=""

for src in $(find bearssl/src -name '*.c' | sort); do
    base=$(basename "$src" .c)
    skip=0
    
    case "$base" in
        *_x86ni*|*_pclmul*|*_pwr8*|*_sse2*|ghash_pclmul)
            skip=1 ;;
        ec_c25519_m62|ec_c25519_m64|ec_p256_m62|ec_p256_m64)
            skip=1 ;;
        *_ct64*|aes_ct64*|poly1305_ctmulq)
            skip=1 ;;
        i62_modpow2|rsa_i62_*)
            skip=1 ;;
    esac
    
    if [ $skip -eq 1 ]; then
        SKIPPED=$((SKIPPED + 1))
        SKIP_LIST="$SKIP_LIST  $base\n"
    else
        COMPILED=$((COMPILED + 1))
    fi
done

echo "Compiled: $COMPILED"
echo "Skipped:  $SKIPPED"
echo "Total:    $TOTAL"
echo ""

echo "--- Skipped files ---"
printf "$SKIP_LIST"
echo ""

# Verify skipped files all contain 64-bit or intrinsics code
echo "--- Validation: skipped files contain platform-specific code ---"
VALID=0
INVALID=0
for src in $(find bearssl/src -name '*.c' | sort); do
    base=$(basename "$src" .c)
    case "$base" in
        *_x86ni*|*_pclmul*|*_pwr8*|*_sse2*|ghash_pclmul|ec_c25519_m62|ec_c25519_m64|ec_p256_m62|ec_p256_m64|*_ct64*|aes_ct64*|poly1305_ctmulq|i62_modpow2|rsa_i62_*)
            if grep -qE 'uint64_t|__m128|__SSE|__AES|BR_POWER8|_mm_|m64|i62' "$src" 2>/dev/null; then
                VALID=$((VALID + 1))
            else
                INVALID=$((INVALID + 1))
                echo "  WARNING: $base might not need skipping"
            fi
            ;;
    esac
done
echo "  $VALID files correctly contain platform-specific code"
if [ $INVALID -gt 0 ]; then
    echo "  $INVALID files may not need skipping (review manually)"
fi
