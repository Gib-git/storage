#include "storage/core/sha256.hpp"

#include <algorithm>
#include <cstring>

#if defined(__aarch64__) && defined(__ARM_FEATURE_SHA2)
#include <arm_neon.h>
#define STORAGE_SHA_ARM 1
#elif defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define STORAGE_SHA_X86 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define STORAGE_TARGET_SHA
#else
#include <cpuid.h>
#define STORAGE_TARGET_SHA __attribute__((target("sha,ssse3,sse4.1")))
#endif
#endif

namespace storage {

namespace {

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void blocksPortable(uint32_t* h_, const uint8_t* p, size_t n) {
    for (; n; --n, p += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) |
                   uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }
}

#if defined(STORAGE_SHA_ARM)
// ARMv8 Cryptography Extensions: 4 rounds per vsha256h/h2 pair.
void blocksArm(uint32_t* state, const uint8_t* p, size_t n) {
    uint32x4_t s0 = vld1q_u32(state);
    uint32x4_t s1 = vld1q_u32(state + 4);
    for (; n; --n, p += 64) {
        const uint32x4_t save0 = s0, save1 = s1;
        uint32x4_t msg[4];
        for (int i = 0; i < 4; ++i) msg[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * i)));
        for (int i = 0; i < 16; ++i) {
            const uint32x4_t wk = vaddq_u32(msg[i & 3], vld1q_u32(K + 4 * i));
            const uint32x4_t prev = s0;
            s0 = vsha256hq_u32(s0, s1, wk);
            s1 = vsha256h2q_u32(s1, prev, wk);
            // msg[i&3] now becomes W[4i+16..4i+19] (only needed for the first 48 rounds).
            if (i < 12)
                msg[i & 3] = vsha256su1q_u32(vsha256su0q_u32(msg[i & 3], msg[(i + 1) & 3]), msg[(i + 2) & 3], msg[(i + 3) & 3]);
        }
        s0 = vaddq_u32(s0, save0);
        s1 = vaddq_u32(s1, save1);
    }
    vst1q_u32(state, s0);
    vst1q_u32(state + 4, s1);
}
#endif

#if defined(STORAGE_SHA_X86)
// Intel SHA extensions. The hardware keeps state as ABEF/CDGH, hence the shuffles.
STORAGE_TARGET_SHA void blocksX86(uint32_t* state, const uint8_t* p, size_t n) {
    const __m128i bswap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state));
    __m128i s1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state + 4));
    tmp = _mm_shuffle_epi32(tmp, 0xB1);          // CDAB
    s1 = _mm_shuffle_epi32(s1, 0x1B);            // EFGH
    __m128i s0 = _mm_alignr_epi8(tmp, s1, 8);    // ABEF
    s1 = _mm_blend_epi16(s1, tmp, 0xF0);         // CDGH
    for (; n; --n, p += 64) {
        const __m128i save0 = s0, save1 = s1;
        __m128i msg[4];
        for (int i = 0; i < 4; ++i)
            msg[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * i)), bswap);
        for (int i = 0; i < 16; ++i) {
            __m128i wk = _mm_add_epi32(msg[i & 3], _mm_loadu_si128(reinterpret_cast<const __m128i*>(K + 4 * i)));
            s1 = _mm_sha256rnds2_epu32(s1, s0, wk);
            wk = _mm_shuffle_epi32(wk, 0x0E);
            s0 = _mm_sha256rnds2_epu32(s0, s1, wk);
            if (i < 12) {
                __m128i next = _mm_sha256msg1_epu32(msg[i & 3], msg[(i + 1) & 3]);
                next = _mm_add_epi32(next, _mm_alignr_epi8(msg[(i + 3) & 3], msg[(i + 2) & 3], 4));
                msg[i & 3] = _mm_sha256msg2_epu32(next, msg[(i + 3) & 3]);
            }
        }
        s0 = _mm_add_epi32(s0, save0);
        s1 = _mm_add_epi32(s1, save1);
    }
    tmp = _mm_shuffle_epi32(s0, 0x1B);       // FEBA
    s1 = _mm_shuffle_epi32(s1, 0xB1);        // DCHG
    s0 = _mm_blend_epi16(tmp, s1, 0xF0);     // DCBA
    s1 = _mm_alignr_epi8(s1, tmp, 8);        // HGFE
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state), s0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state + 4), s1);
}

bool cpuHasShaNi() {
#if defined(_MSC_VER) && !defined(__clang__)
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const bool sse41 = (r[2] >> 19) & 1, ssse3 = (r[2] >> 9) & 1;
    __cpuidex(r, 7, 0);
    return sse41 && ssse3 && ((r[1] >> 29) & 1);
#else
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
    const bool sse41 = (c >> 19) & 1, ssse3 = (c >> 9) & 1;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
    return sse41 && ssse3 && ((b >> 29) & 1);
#endif
}
#endif

using BlocksFn = void (*)(uint32_t*, const uint8_t*, size_t);

struct Impl {
    BlocksFn fn;
    const char* name;
};

Impl pickImpl() {
#if defined(STORAGE_SHA_ARM)
    return {blocksArm, "armv8-sha2"};
#elif defined(STORAGE_SHA_X86)
    if (cpuHasShaNi()) return {blocksX86, "x86-sha-ni"};
#endif
    return {blocksPortable, "portable"};
}

const Impl& impl() {
    static const Impl i = pickImpl();
    return i;
}

}  // namespace

void Sha256::reset() {
    static constexpr uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, init, sizeof h_);
    bufLen_ = 0;
    total_ = 0;
}

void Sha256::blocks(const uint8_t* p, size_t n) { impl().fn(h_, p, n); }

const char* Sha256::implementation() { return impl().name; }

void Sha256::update(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    total_ += len;
    if (bufLen_) {
        const size_t take = std::min(len, 64 - bufLen_);
        std::memcpy(buf_ + bufLen_, p, take);
        bufLen_ += take;
        p += take;
        len -= take;
        if (bufLen_ == 64) {
            blocks(buf_, 1);
            bufLen_ = 0;
        }
    }
    if (len >= 64) {
        const size_t n = len / 64;
        blocks(p, n);
        p += n * 64;
        len -= n * 64;
    }
    if (len) {
        std::memcpy(buf_, p, len);
        bufLen_ = len;
    }
}

Sha256::Digest Sha256::finish() {
    const uint64_t bits = total_ * 8;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (bufLen_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);

    Digest d;
    for (int i = 0; i < 8; ++i) {
        d[i * 4] = static_cast<uint8_t>(h_[i] >> 24);
        d[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
        d[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
        d[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
    }
    reset();
    return d;
}

std::string Sha256::hex(const Digest& d) {
    static const char* digits = "0123456789abcdef";
    std::string s(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        s[i * 2] = digits[d[i] >> 4];
        s[i * 2 + 1] = digits[d[i] & 15];
    }
    return s;
}

std::string Sha256::hexOf(const void* data, size_t len) {
    Sha256 h;
    h.update(data, len);
    return hex(h.finish());
}

}  // namespace storage
