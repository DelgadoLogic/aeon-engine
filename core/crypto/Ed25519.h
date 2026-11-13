#pragma once
// AeonBrowser — core/crypto/Ed25519.h
// Self-contained RFC 8032 Ed25519 signature verification & signing (TweetNaCl GF(2^255-19) + Windows CNG SHA-512).
// Zero external dependencies beyond Windows bcrypt.lib.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace AeonCrypto {
namespace Ed25519 {

namespace detail {

using gf = int64_t[16];

inline constexpr gf kGf0 = {0};
inline constexpr gf kGf1 = {1};
inline constexpr gf kD   = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203
};
inline constexpr gf kD2  = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406
};
inline constexpr gf kX   = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169
};
inline constexpr gf kY   = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666
};
inline constexpr gf kI   = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83
};
inline constexpr uint64_t kL[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10
};

inline bool Sha512Three(
    const uint8_t* p1, size_t n1,
    const uint8_t* p2, size_t n2,
    const uint8_t* p3, size_t n3,
    uint8_t out64[64])
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA512_ALGORITHM, nullptr, 0) < 0 || !hAlg)
        return false;

    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) >= 0 && hHash) {
        bool hOk = true;
        if (p1 && n1 > 0 && BCryptHashData(hHash, (PUCHAR)p1, (ULONG)n1, 0) < 0) hOk = false;
        if (hOk && p2 && n2 > 0 && BCryptHashData(hHash, (PUCHAR)p2, (ULONG)n2, 0) < 0) hOk = false;
        if (hOk && p3 && n3 > 0 && BCryptHashData(hHash, (PUCHAR)p3, (ULONG)n3, 0) < 0) hOk = false;
        if (hOk && BCryptFinishHash(hHash, out64, 64, 0) >= 0) ok = true;
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

inline void set25519(gf r, const gf a) {
    for (int i = 0; i < 16; ++i) r[i] = a[i];
}

inline void car25519(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= (c << 16);
    }
}

inline void sel25519(gf p, gf q, int b) {
    int64_t c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

inline void pack25519(uint8_t o[32], const gf n) {
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i]     = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

inline int neq25519(const gf a, const gf b) {
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i) diff |= (c[i] ^ d[i]);
    return diff != 0 ? 1 : 0;
}

inline uint8_t par25519(const gf a) {
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1;
}

inline void unpack25519(gf o, const uint8_t n[32]) {
    for (int i = 0; i < 16; ++i)
        o[i] = (int64_t)n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

inline void A(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

inline void Z(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

inline void M(gf o, const gf a, const gf b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i)
        t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o);
    car25519(o);
}

inline void S(gf o, const gf a) { M(o, a, a); }

inline void inv25519(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

inline void pow2523(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 250; a >= 0; --a) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

inline void cswap(gf p[4], gf q[4], uint8_t b) {
    for (int i = 0; i < 4; ++i) sel25519(p[i], q[i], b);
}

inline void add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, kD2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);

    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

inline void pack(uint8_t r[32], gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (par25519(tx) << 7);
}

inline void scalarmult(gf p[4], gf q[4], const uint8_t* s) {
    set25519(p[0], kGf0);
    set25519(p[1], kGf1);
    set25519(p[2], kGf1);
    set25519(p[3], kGf0);
    for (int i = 255; i >= 0; --i) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

inline void scalarbase(gf p[4], const uint8_t* s) {
    gf q[4];
    set25519(q[0], kX);
    set25519(q[1], kY);
    set25519(q[2], kGf1);
    M(q[3], kX, kY);
    scalarmult(p, q, s);
}

inline int unpackneg(gf r[4], const uint8_t p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], kGf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, kD);
    Z(num, num, r[2]);
    A(den, r[2], den);

    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);

    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], kI);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;

    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], kGf0, r[0]);

    M(r[3], r[0], r[1]);
    return 0;
}

inline void modL(uint8_t r[32], int64_t x[64]) {
    for (int i = 63; i >= 32; --i) {
        int64_t carry = 0;
        int j = i - 32;
        int k = i - 12;
        for (; j < k; ++j) {
            x[j] += carry - 16 * x[i] * (int64_t)kL[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    int64_t carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * (int64_t)kL[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) x[j] -= carry * (int64_t)kL[j];
    for (int i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

inline void reduce(uint8_t r[64]) {
    int64_t x[64];
    for (int i = 0; i < 64; ++i) x[i] = (uint64_t)r[i];
    uint8_t out[32];
    modL(out, x);
    memset(r, 0, 64);
    memcpy(r, out, 32);
}

inline bool ScalarLessThanL(const uint8_t s[32]) {
    for (int i = 31; i >= 0; --i) {
        if (s[i] < (uint8_t)kL[i]) return true;
        if (s[i] > (uint8_t)kL[i]) return false;
    }
    return false;
}

} // namespace detail

// Derives a 32-byte Ed25519 public key from a 32-byte RFC 8032 secret seed.
inline bool DerivePublicKey(const uint8_t seed32[32], uint8_t outPub32[32]) {
    uint8_t d[64] = {};
    if (!detail::Sha512Three(seed32, 32, nullptr, 0, nullptr, 0, d))
        return false;
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;

    detail::gf p[4];
    detail::scalarbase(p, d);
    detail::pack(outPub32, p);
    SecureZeroMemory(d, sizeof(d));
    return true;
}

// Signs an arbitrary message using a 32-byte RFC 8032 secret seed and its 32-byte public key.
inline bool SignDetached(
    const uint8_t* message,
    size_t messageLen,
    const uint8_t seed32[32],
    const uint8_t pub32[32],
    uint8_t outSig64[64])
{
    uint8_t d[64] = {};
    if (!detail::Sha512Three(seed32, 32, nullptr, 0, nullptr, 0, d))
        return false;
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;

    uint8_t r[64] = {};
    if (!detail::Sha512Three(d + 32, 32, message, messageLen, nullptr, 0, r)) {
        SecureZeroMemory(d, sizeof(d));
        return false;
    }
    detail::reduce(r);

    detail::gf p[4];
    detail::scalarbase(p, r);
    detail::pack(outSig64, p);

    uint8_t hram[64] = {};
    if (!detail::Sha512Three(outSig64, 32, pub32, 32, message, messageLen, hram)) {
        SecureZeroMemory(d, sizeof(d));
        SecureZeroMemory(r, sizeof(r));
        return false;
    }
    detail::reduce(hram);

    int64_t x[64] = {0};
    for (int i = 0; i < 32; ++i) x[i] = (uint64_t)r[i];
    for (int i = 0; i < 32; ++i)
        for (int j = 0; j < 32; ++j)
            x[i + j] += (int64_t)hram[i] * (int64_t)d[j];

    detail::modL(outSig64 + 32, x);
    SecureZeroMemory(d, sizeof(d));
    SecureZeroMemory(r, sizeof(r));
    return true;
}

// Verifies a 64-byte detached RFC 8032 Ed25519 signature over `message` against `pub32`.
// Constant-time point equality check + strict S < L non-malleability gate.
inline bool VerifyDetached(
    const uint8_t* message,
    size_t messageLen,
    const uint8_t sig64[64],
    const uint8_t pub32[32])
{
    if (!sig64 || !pub32) return false;
    if (!detail::ScalarLessThanL(sig64 + 32)) return false;

    detail::gf p[4], q[4];
    if (detail::unpackneg(q, pub32) != 0) return false;

    uint8_t h[64] = {};
    if (!detail::Sha512Three(sig64, 32, pub32, 32, message, messageLen, h))
        return false;
    detail::reduce(h);

    detail::scalarmult(p, q, h);
    detail::scalarbase(q, sig64 + 32);
    detail::add(p, q);

    uint8_t checkR[32] = {};
    detail::pack(checkR, p);

    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i) diff |= (sig64[i] ^ checkR[i]);
    return diff == 0;
}

} // namespace Ed25519
} // namespace AeonCrypto
