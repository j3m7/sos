// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Known-answer tests of SOS's crypto (core/selftest.h). The vectors are published ones, cited at
// each. When they were written in they were checked against OpenSSL's command line (all but GCM)
// and wolfSSL's own test suite (GCM).
//
// What each covers, and why it matters here:
// - SHA-256, HMAC-SHA-256: key derivation (HKDF), the console password verifier, the DRBG.
// - SHA-512: Ed25519 hashes with it (and so does `sos.hash`).
// - AES-256-GCM: sealing the configuration and the host key record, the card's key header.
// - XTS-AES-128: encrypted SD cards (files/sdcrypt.c), through the sector call SOS uses.
// - Ed25519: the SSH host key (signing), and SSH public key logins (verifying).
// - X25519: the SSH key exchange (alone, and inside the hybrid ML-KEM one).
// Not covered: ChaCha20-Poly1305 and AES-CTR (SSH ciphers: a broken one breaks the session, it
// can't hide), AES-SIV (NTS: the server's tag would fail), ML-KEM, ECDSA and RSA (TLS and SSH
// peers check those).

#include "core/selftest.h"

#include <stdint.h>
#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/curve25519.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/sha512.h>

#include "core/heaptag.h"

// The contexts, one test at a time: on the heap, as they are too big together for the stack
// (two AES key schedules for XTS, GCM's table).
typedef union {
    Aes gcm;
    XtsAes xts;
    Hmac hmac;
    ed25519_key ed;
    struct {
        curve25519_key priv, peer;
    } x;
} work_t;

static int fault = -1;                  // SELFTEST_TEST: the test that sees a fault
static unsigned current;                // the test running

#ifdef SELFTEST_TEST
void selftest_crypto_fault(int index) {
    fault = index;
}
#endif

static bool faulty(void) {
    return fault >= 0 && (unsigned)fault == current;
}

// The computed bytes match the vector. A fault flips the first bit of what was computed.
static bool same(uint8_t *got, const uint8_t *want, size_t n) {
    if (faulty())
        got[0] ^= 1;
    return memcmp(got, want, n) == 0;
}

// For a refusal test: the bit to flip in the input (0 under a fault: the input stays as it was).
static uint8_t flip(void) {
    return faulty() ? 0 : 1;
}

// --- SHA-2 and HMAC --------------------------------------------------------------------------

// FIPS 180-2, Appendix B.1 and C.1 (NIST's "abc" examples).
static const uint8_t abc[] = {'a', 'b', 'c'};
static const uint8_t sha256_abc[WC_SHA256_DIGEST_SIZE] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};
static const uint8_t sha512_abc[WC_SHA512_DIGEST_SIZE] = {
    0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba, 0xcc, 0x41, 0x73, 0x49, 0xae, 0x20, 0x41, 0x31,
    0x12, 0xe6, 0xfa, 0x4e, 0x89, 0xa9, 0x7e, 0xa2, 0x0a, 0x9e, 0xee, 0xe6, 0x4b, 0x55, 0xd3, 0x9a,
    0x21, 0x92, 0x99, 0x2a, 0x27, 0x4f, 0xc1, 0xa8, 0x36, 0xba, 0x3c, 0x23, 0xa3, 0xfe, 0xeb, 0xbd,
    0x45, 0x4d, 0x44, 0x23, 0x64, 0x3c, 0xe8, 0x0e, 0x2a, 0x9a, 0xc9, 0x4f, 0xa5, 0x4c, 0xa4, 0x9f,
};

static const char *t_sha256(work_t *w) {
    (void)w;
    uint8_t out[WC_SHA256_DIGEST_SIZE];
    if (wc_Sha256Hash(abc, sizeof(abc), out) != 0 || !same(out, sha256_abc, sizeof(out)))
        return "sha-256 gave a wrong digest";
    return NULL;
}

static const char *t_sha512(work_t *w) {
    (void)w;
    uint8_t out[WC_SHA512_DIGEST_SIZE];
    if (wc_Sha512Hash(abc, sizeof(abc), out) != 0 || !same(out, sha512_abc, sizeof(out)))
        return "sha-512 gave a wrong digest";
    return NULL;
}

// RFC 4231, test case 1: key 20 bytes of 0x0b, data "Hi There".
static const uint8_t hmac_data[] = {'H', 'i', ' ', 'T', 'h', 'e', 'r', 'e'};
static const uint8_t hmac_sha256_tc1[WC_SHA256_DIGEST_SIZE] = {
    0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
    0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
};

static const char *t_hmac(work_t *w) {
    uint8_t key[20], out[WC_SHA256_DIGEST_SIZE];
    memset(key, 0x0b, sizeof(key));
    if (wc_HmacInit(&w->hmac, NULL, INVALID_DEVID) != 0)
        return "hmac-sha-256 init failed";
    bool ok = wc_HmacSetKey(&w->hmac, WC_SHA256, key, sizeof(key)) == 0 &&
              wc_HmacUpdate(&w->hmac, hmac_data, sizeof(hmac_data)) == 0 &&
              wc_HmacFinal(&w->hmac, out) == 0;
    wc_HmacFree(&w->hmac);
    if (!ok || !same(out, hmac_sha256_tc1, sizeof(out)))
        return "hmac-sha-256 gave a wrong tag";
    return NULL;
}

// --- AES-256-GCM -----------------------------------------------------------------------------

// The GCM specification's test case 16 (McGrew and Viega, "The Galois/Counter Mode of Operation",
// revised 2005; the AES-256 case with a 96-bit IV and additional data), as in NIST's GCM tests.
static const uint8_t gcm_key[32] = {
    0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c, 0x6d, 0x6a, 0x8f, 0x94, 0x67, 0x30, 0x83, 0x08,
    0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c, 0x6d, 0x6a, 0x8f, 0x94, 0x67, 0x30, 0x83, 0x08,
};
static const uint8_t gcm_iv[12] = {
    0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad, 0xde, 0xca, 0xf8, 0x88,
};
static const uint8_t gcm_aad[20] = {
    0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef, 0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef,
    0xab, 0xad, 0xda, 0xd2,
};
static const uint8_t gcm_plain[60] = {
    0xd9, 0x31, 0x32, 0x25, 0xf8, 0x84, 0x06, 0xe5, 0xa5, 0x59, 0x09, 0xc5, 0xaf, 0xf5, 0x26, 0x9a,
    0x86, 0xa7, 0xa9, 0x53, 0x15, 0x34, 0xf7, 0xda, 0x2e, 0x4c, 0x30, 0x3d, 0x8a, 0x31, 0x8a, 0x72,
    0x1c, 0x3c, 0x0c, 0x95, 0x95, 0x68, 0x09, 0x53, 0x2f, 0xcf, 0x0e, 0x24, 0x49, 0xa6, 0xb5, 0x25,
    0xb1, 0x6a, 0xed, 0xf5, 0xaa, 0x0d, 0xe6, 0x57, 0xba, 0x63, 0x7b, 0x39,
};
static const uint8_t gcm_cipher[60] = {
    0x52, 0x2d, 0xc1, 0xf0, 0x99, 0x56, 0x7d, 0x07, 0xf4, 0x7f, 0x37, 0xa3, 0x2a, 0x84, 0x42, 0x7d,
    0x64, 0x3a, 0x8c, 0xdc, 0xbf, 0xe5, 0xc0, 0xc9, 0x75, 0x98, 0xa2, 0xbd, 0x25, 0x55, 0xd1, 0xaa,
    0x8c, 0xb0, 0x8e, 0x48, 0x59, 0x0d, 0xbb, 0x3d, 0xa7, 0xb0, 0x8b, 0x10, 0x56, 0x82, 0x88, 0x38,
    0xc5, 0xf6, 0x1e, 0x63, 0x93, 0xba, 0x7a, 0x0a, 0xbc, 0xc9, 0xf6, 0x62,
};
static const uint8_t gcm_tag[16] = {
    0x76, 0xfc, 0x6e, 0xce, 0x0f, 0x4e, 0x17, 0x68, 0xcd, 0xdf, 0x88, 0x53, 0xbb, 0x2d, 0x55, 0x1b,
};

// Encrypt (dir 1), decrypt (0), or decrypt with one ciphertext bit flipped (-1). True if
// wolfCrypt said yes; the output goes to out.
static bool gcm(work_t *w, int dir, uint8_t out[60], uint8_t tag[16]) {
    if (wc_AesInit(&w->gcm, NULL, INVALID_DEVID) != 0)
        return false;
    bool ok = wc_AesGcmSetKey(&w->gcm, gcm_key, sizeof(gcm_key)) == 0;
    if (ok && dir > 0) {
        ok = wc_AesGcmEncrypt(&w->gcm, out, gcm_plain, sizeof(gcm_plain), gcm_iv, sizeof(gcm_iv),
                              tag, 16, gcm_aad, sizeof(gcm_aad)) == 0;
    } else if (ok) {
        uint8_t in[60];
        memcpy(in, gcm_cipher, sizeof(in));
        if (dir < 0)
            in[17] ^= flip();
        ok = wc_AesGcmDecrypt(&w->gcm, out, in, sizeof(in), gcm_iv, sizeof(gcm_iv), gcm_tag, 16,
                              gcm_aad, sizeof(gcm_aad)) == 0;
    }
    wc_AesFree(&w->gcm);
    return ok;
}

static const char *t_gcm_encrypt(work_t *w) {
    uint8_t out[60], tag[16];
    if (!gcm(w, 1, out, tag) || !same(out, gcm_cipher, sizeof(out)) ||
        memcmp(tag, gcm_tag, sizeof(tag)) != 0)
        return "aes-gcm encrypt gave a wrong result";
    return NULL;
}

static const char *t_gcm_decrypt(work_t *w) {
    uint8_t out[60], tag[16];
    if (!gcm(w, 0, out, tag) || !same(out, gcm_plain, sizeof(out)))
        return "aes-gcm decrypt gave a wrong result";
    return NULL;
}

static const char *t_gcm_refuse(work_t *w) {
    uint8_t out[60], tag[16];
    if (gcm(w, -1, out, tag))
        return "aes-gcm accepted a changed ciphertext";
    return NULL;
}

// --- XTS-AES-128 -----------------------------------------------------------------------------

// IEEE Std 1619-2007, Annex B, vector 2: key1 = 16 x 0x11, key2 = 16 x 0x22, data unit sequence
// number 0x3333333333, 32 bytes of 0x44. The sequence number is the sector number SOS passes
// (wc_AesXtsEncryptSector: little-endian tweak).
#define XTS_SECTOR 0x3333333333ull
static const uint8_t xts_cipher[32] = {
    0xc4, 0x54, 0x18, 0x5e, 0x6a, 0x16, 0x93, 0x6e, 0x39, 0x33, 0x40, 0x38, 0xac, 0xef, 0x83, 0x8b,
    0xfb, 0x18, 0x6f, 0xff, 0x74, 0x80, 0xad, 0xc4, 0x28, 0x93, 0x82, 0xec, 0xd6, 0xd3, 0x94, 0xf0,
};

static bool xts(work_t *w, int dir, const uint8_t *in, uint8_t *out) {
    uint8_t key[32];
    memset(key, 0x11, 16);
    memset(key + 16, 0x22, 16);
    if (wc_AesXtsSetKey(&w->xts, key, sizeof(key), dir, NULL, INVALID_DEVID) != 0)
        return false;
    int ret = dir == AES_ENCRYPTION
                  ? wc_AesXtsEncryptSector(&w->xts, out, in, sizeof(xts_cipher), XTS_SECTOR)
                  : wc_AesXtsDecryptSector(&w->xts, out, in, sizeof(xts_cipher), XTS_SECTOR);
    wc_AesXtsFree(&w->xts);
    return ret == 0;
}

static const char *t_xts_encrypt(work_t *w) {
    uint8_t in[32], out[32];
    memset(in, 0x44, sizeof(in));
    if (!xts(w, AES_ENCRYPTION, in, out) || !same(out, xts_cipher, sizeof(out)))
        return "aes-xts encrypt gave a wrong result";
    return NULL;
}

static const char *t_xts_decrypt(work_t *w) {
    uint8_t want[32], out[32];
    memset(want, 0x44, sizeof(want));
    if (!xts(w, AES_DECRYPTION, xts_cipher, out) || !same(out, want, sizeof(out)))
        return "aes-xts decrypt gave a wrong result";
    return NULL;
}

// --- Ed25519 ---------------------------------------------------------------------------------

// RFC 8032 §7.1, TEST 2: a one-byte message, 0x72. Ed25519 signatures are deterministic, so the
// signature is a known answer too.
static const uint8_t ed_secret[ED25519_KEY_SIZE] = {
    0x4c, 0xcd, 0x08, 0x9b, 0x28, 0xff, 0x96, 0xda, 0x9d, 0xb6, 0xc3, 0x46, 0xec, 0x11, 0x4e, 0x0f,
    0x5b, 0x8a, 0x31, 0x9f, 0x35, 0xab, 0xa6, 0x24, 0xda, 0x8c, 0xf6, 0xed, 0x4f, 0xb8, 0xa6, 0xfb,
};
static const uint8_t ed_public[ED25519_PUB_KEY_SIZE] = {
    0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a, 0x92, 0xb7, 0x0a, 0xa7, 0x4d, 0x1b, 0x7e, 0xbc,
    0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4, 0x96, 0x8c, 0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c,
};
static const uint8_t ed_message[1] = {0x72};
static const uint8_t ed_signature[ED25519_SIG_SIZE] = {
    0x92, 0xa0, 0x09, 0xa9, 0xf0, 0xd4, 0xca, 0xb8, 0x72, 0x0e, 0x82, 0x0b, 0x5f, 0x64, 0x25, 0x40,
    0xa2, 0xb2, 0x7b, 0x54, 0x16, 0x50, 0x3f, 0x8f, 0xb3, 0x76, 0x22, 0x23, 0xeb, 0xdb, 0x69, 0xda,
    0x08, 0x5a, 0xc1, 0xe4, 0x3e, 0x15, 0x99, 0x6e, 0x45, 0x8f, 0x36, 0x13, 0xd0, 0xf1, 0x1d, 0x8c,
    0x38, 0x7b, 0x2e, 0xae, 0xb4, 0x30, 0x2a, 0xee, 0xb0, 0x0d, 0x29, 0x16, 0x12, 0xbb, 0x0c, 0x00,
};

// The public key from the secret (as the host key is made: a base point multiplication).
static const char *t_ed_public(work_t *w) {
    uint8_t pub[ED25519_PUB_KEY_SIZE];
    if (wc_ed25519_init(&w->ed) != 0)
        return "ed25519 init failed";
    bool ok = wc_ed25519_import_private_only(ed_secret, sizeof(ed_secret), &w->ed) == 0 &&
              wc_ed25519_make_public(&w->ed, pub, sizeof(pub)) == 0;
    wc_ed25519_free(&w->ed);
    if (!ok || !same(pub, ed_public, sizeof(pub)))
        return "ed25519 gave a wrong public key";
    return NULL;
}

static bool ed_key(work_t *w) {
    if (wc_ed25519_init(&w->ed) != 0)
        return false;
    if (wc_ed25519_import_private_key(ed_secret, sizeof(ed_secret), ed_public, sizeof(ed_public),
                                      &w->ed) == 0)
        return true;
    wc_ed25519_free(&w->ed);
    return false;
}

static const char *t_ed_sign(work_t *w) {
    uint8_t sig[ED25519_SIG_SIZE];
    word32 len = sizeof(sig);
    if (!ed_key(w))
        return "ed25519 key import failed";
    bool ok = wc_ed25519_sign_msg(ed_message, sizeof(ed_message), sig, &len, &w->ed) == 0 &&
              len == sizeof(sig);
    wc_ed25519_free(&w->ed);
    if (!ok || !same(sig, ed_signature, sizeof(sig)))
        return "ed25519 gave a wrong signature";
    return NULL;
}

// Verify the vector's signature over the message, or over the message with one bit flipped.
static bool ed_verify(work_t *w, bool changed, bool *valid) {
    uint8_t msg[sizeof(ed_message)];
    memcpy(msg, ed_message, sizeof(msg));
    if (changed)
        msg[0] ^= flip();
    int res = 0;
    if (!ed_key(w))
        return false;
    int ret = wc_ed25519_verify_msg(ed_signature, sizeof(ed_signature), msg, sizeof(msg), &res,
                                    &w->ed);
    wc_ed25519_free(&w->ed);
    *valid = ret == 0 && res == 1;
    return true;
}

static const char *t_ed_verify(work_t *w) {
    bool valid = false;
    if (!ed_verify(w, false, &valid))
        return "ed25519 key import failed";
    if (faulty())
        valid = false;
    return valid ? NULL : "ed25519 refused a good signature";
}

static const char *t_ed_refuse(work_t *w) {
    bool valid = true;
    if (!ed_verify(w, true, &valid))
        return "ed25519 key import failed";
    return valid ? "ed25519 verify accepted a changed message" : NULL;
}

// --- X25519 ----------------------------------------------------------------------------------

// RFC 7748 §6.1: Alice's private key with Bob's public key gives the shared secret K. Byte order
// as on the wire, which is how wolfSSH calls it (EC25519_LITTLE_ENDIAN).
static const uint8_t x_alice_priv[CURVE25519_KEYSIZE] = {
    0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d, 0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
    0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a, 0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
};
static const uint8_t x_bob_pub[CURVE25519_KEYSIZE] = {
    0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
    0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d, 0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f,
};
static const uint8_t x_shared[CURVE25519_KEYSIZE] = {
    0x4a, 0x5d, 0x9d, 0x5b, 0xa4, 0xce, 0x2d, 0xe1, 0x72, 0x8e, 0x3b, 0xf4, 0x80, 0x35, 0x0f, 0x25,
    0xe0, 0x7e, 0x21, 0xc9, 0x47, 0xd1, 0x9e, 0x33, 0x76, 0xf0, 0x9b, 0x3c, 0x1e, 0x16, 0x17, 0x42,
};

static const char *t_x25519(work_t *w) {
    // X25519 clamps the scalar itself (RFC 7748 §5); wolfCrypt wants it clamped on import. The
    // result is the same.
    uint8_t priv[CURVE25519_KEYSIZE], out[CURVE25519_KEYSIZE];
    memcpy(priv, x_alice_priv, sizeof(priv));
    priv[0] &= 248;
    priv[31] = (uint8_t)((priv[31] & 127) | 64);
    word32 len = sizeof(out);
    if (wc_curve25519_init(&w->x.priv) != 0)
        return "x25519 init failed";
    if (wc_curve25519_init(&w->x.peer) != 0) {
        wc_curve25519_free(&w->x.priv);
        return "x25519 init failed";
    }
    bool ok =
        wc_curve25519_import_private_ex(priv, sizeof(priv), &w->x.priv, EC25519_LITTLE_ENDIAN) == 0 &&
        wc_curve25519_check_public(x_bob_pub, sizeof(x_bob_pub), EC25519_LITTLE_ENDIAN) == 0 &&
        wc_curve25519_import_public_ex(x_bob_pub, sizeof(x_bob_pub), &w->x.peer,
                                       EC25519_LITTLE_ENDIAN) == 0 &&
        wc_curve25519_shared_secret_ex(&w->x.priv, &w->x.peer, out, &len,
                                       EC25519_LITTLE_ENDIAN) == 0 &&
        len == sizeof(out);
    wc_curve25519_free(&w->x.peer);
    wc_curve25519_free(&w->x.priv);
    if (!ok || !same(out, x_shared, sizeof(out)))
        return "x25519 gave a wrong shared secret";
    return NULL;
}

// --- running them ----------------------------------------------------------------------------

static const char *(*const tests[])(work_t *) = {
    t_sha256, t_sha512, t_hmac,
    t_gcm_encrypt, t_gcm_decrypt, t_gcm_refuse,
    t_xts_encrypt, t_xts_decrypt,
    t_ed_public, t_ed_sign, t_ed_verify, t_ed_refuse,
    t_x25519,
};

#define TEST_COUNT (sizeof(tests) / sizeof(tests[0]))

unsigned selftest_crypto_count(void) {
    return TEST_COUNT;
}

const char *selftest_crypto(unsigned *ran) {
    *ran = 0;
    work_t *w = heap_alloc(HEAP_NET, sizeof(*w));
    if (!w)
        return "no memory to run the tests";
    const char *why = NULL;
    for (current = 0; current < TEST_COUNT && !why; current++) {
        why = tests[current](w);
        (*ran)++;
    }
    // The contexts held key material, though only the vectors' published keys.
    memset(w, 0, sizeof(*w));
    heap_release(HEAP_NET, w);
    return why;
}
