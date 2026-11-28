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
// - Ed25519: the SSH host key (making it, signing with it), and SSH public key logins
//   (verifying).
// - X25519: the SSH key exchange (alone, and inside the hybrid ML-KEM one).
// Not covered: ChaCha20-Poly1305 and AES-CTR (SSH ciphers: a broken one breaks the session, it
// can't hide), AES-SIV (NTS: the server's tag would fail), ML-KEM, ECDSA and RSA (TLS and SSH
// peers check those).
//
// Every call is the one SOS (or wolfSSH) makes, with the same options: X25519 through the _ex
// calls with EC25519_LITTLE_ENDIAN, as wolfSSH does, and with an RNG on the private key, which
// wolfCrypt's Curve25519 blinding needs (it is on in every build with an RNG and the C code; on
// the board, wolfSSH's keys get theirs from wc_curve25519_make_key).

#include "core/selftest.h"

#include <stdio.h>
#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/aes.h>
#if SOS_HAVE_SSH || SOS_HAVE_TLS
#include <wolfssl/wolfcrypt/curve25519.h>
#endif
#if SOS_HAVE_SSH
#include <wolfssl/wolfcrypt/ed25519.h>
#endif
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/sha512.h>

#include "core/heaptag.h"

// The contexts, one test at a time: on the heap, as they are too big together for the stack
// (two AES key schedules for XTS, GCM's table).
struct selftest {
    union {
        Aes gcm;
        XtsAes xts;
        Hmac hmac;
#if SOS_HAVE_SSH
        ed25519_key ed;
#endif
#if SOS_HAVE_SSH || SOS_HAVE_TLS
        struct {
            curve25519_key priv, peer;
            WC_RNG rng;
        } x;
#endif
    } u;
};

static int fault = -1;                  // SELFTEST_TEST: the test that sees a fault
static unsigned current;                // the test running
static char why[64];                    // a phrase with an error code in it

#ifdef SELFTEST_TEST
void selftest_fault(int index) {
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

// "x25519 shared secret failed: error -173"
static const char *error(const char *what, int ret) {
    snprintf(why, sizeof(why), "%s failed: error %d", what, ret);
    return why;
}

// --- the vectors -----------------------------------------------------------------------------

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

// RFC 4231, test case 1: key 20 bytes of 0x0b, data "Hi There".
static const uint8_t hmac_data[] = {'H', 'i', ' ', 'T', 'h', 'e', 'r', 'e'};

static const uint8_t hmac_sha256_tc1[WC_SHA256_DIGEST_SIZE] = {
    0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
    0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
};

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

// IEEE Std 1619-2007, Annex B, vector 2: key1 = 16 x 0x11, key2 = 16 x 0x22, data unit sequence
// number 0x3333333333, 32 bytes of 0x44. The sequence number is the sector number SOS passes
// (wc_AesXtsEncryptSector: little-endian tweak).
#define XTS_SECTOR 0x3333333333ull

static const uint8_t xts_cipher[32] = {
    0xc4, 0x54, 0x18, 0x5e, 0x6a, 0x16, 0x93, 0x6e, 0x39, 0x33, 0x40, 0x38, 0xac, 0xef, 0x83, 0x8b,
    0xfb, 0x18, 0x6f, 0xff, 0x74, 0x80, 0xad, 0xc4, 0x28, 0x93, 0x82, 0xec, 0xd6, 0xd3, 0x94, 0xf0,
};

#if SOS_HAVE_SSH
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
#endif

#if SOS_HAVE_SSH || SOS_HAVE_TLS
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
#endif

// --- SHA-2 and HMAC --------------------------------------------------------------------------

static const char *t_sha256(selftest_t *st) {
    (void)st;
    uint8_t out[WC_SHA256_DIGEST_SIZE];
    int ret = wc_Sha256Hash(abc, sizeof(abc), out);
    if (ret != 0)
        return error("sha-256", ret);
    return same(out, sha256_abc, sizeof(out)) ? NULL : "sha-256 gave a wrong digest";
}

static const char *t_sha512(selftest_t *st) {
    (void)st;
    uint8_t out[WC_SHA512_DIGEST_SIZE];
    int ret = wc_Sha512Hash(abc, sizeof(abc), out);
    if (ret != 0)
        return error("sha-512", ret);
    return same(out, sha512_abc, sizeof(out)) ? NULL : "sha-512 gave a wrong digest";
}

static const char *t_hmac(selftest_t *st) {
    Hmac *h = &st->u.hmac;
    uint8_t key[20], out[WC_SHA256_DIGEST_SIZE];
    memset(key, 0x0b, sizeof(key));
    int ret = wc_HmacInit(h, NULL, INVALID_DEVID);
    if (ret != 0)
        return error("hmac-sha-256", ret);
    ret = wc_HmacSetKey(h, WC_SHA256, key, sizeof(key));
    if (ret == 0)
        ret = wc_HmacUpdate(h, hmac_data, sizeof(hmac_data));
    if (ret == 0)
        ret = wc_HmacFinal(h, out);
    wc_HmacFree(h);
    if (ret != 0)
        return error("hmac-sha-256", ret);
    return same(out, hmac_sha256_tc1, sizeof(out)) ? NULL : "hmac-sha-256 gave a wrong tag";
}

// --- AES-256-GCM -----------------------------------------------------------------------------

// Encrypt (dir 1), decrypt (0), or decrypt with one ciphertext bit flipped (-1). wolfCrypt's
// result: 0, or its error (AES_GCM_AUTH_E for a tag that doesn't match).
static int gcm(selftest_t *st, int dir, uint8_t out[60], uint8_t tag[16]) {
    Aes *a = &st->u.gcm;
    int ret = wc_AesInit(a, NULL, INVALID_DEVID);
    if (ret != 0)
        return ret;
    ret = wc_AesGcmSetKey(a, gcm_key, sizeof(gcm_key));
    if (ret == 0 && dir > 0) {
        ret = wc_AesGcmEncrypt(a, out, gcm_plain, sizeof(gcm_plain), gcm_iv, sizeof(gcm_iv), tag,
                               16, gcm_aad, sizeof(gcm_aad));
    } else if (ret == 0) {
        uint8_t in[60];
        memcpy(in, gcm_cipher, sizeof(in));
        if (dir < 0)
            in[17] ^= flip();
        ret = wc_AesGcmDecrypt(a, out, in, sizeof(in), gcm_iv, sizeof(gcm_iv), gcm_tag, 16,
                               gcm_aad, sizeof(gcm_aad));
    }
    wc_AesFree(a);
    return ret;
}

static const char *t_gcm_encrypt(selftest_t *st) {
    uint8_t out[60], tag[16];
    int ret = gcm(st, 1, out, tag);
    if (ret != 0)
        return error("aes-gcm encrypt", ret);
    if (!same(out, gcm_cipher, sizeof(out)) || memcmp(tag, gcm_tag, sizeof(tag)) != 0)
        return "aes-gcm encrypt gave a wrong result";
    return NULL;
}

static const char *t_gcm_decrypt(selftest_t *st) {
    uint8_t out[60], tag[16];
    int ret = gcm(st, 0, out, tag);
    if (ret != 0)
        return error("aes-gcm decrypt", ret);
    return same(out, gcm_plain, sizeof(out)) ? NULL : "aes-gcm decrypt gave a wrong result";
}

static const char *t_gcm_refuse(selftest_t *st) {
    uint8_t out[60], tag[16];
    return gcm(st, -1, out, tag) == 0 ? "aes-gcm accepted a changed ciphertext" : NULL;
}

// --- XTS-AES-128 -----------------------------------------------------------------------------

static int xts(selftest_t *st, int dir, const uint8_t *in, uint8_t *out) {
    XtsAes *x = &st->u.xts;
    uint8_t key[32];
    memset(key, 0x11, 16);
    memset(key + 16, 0x22, 16);
    int ret = wc_AesXtsSetKey(x, key, sizeof(key), dir, NULL, INVALID_DEVID);
    if (ret != 0)
        return ret;
    ret = dir == AES_ENCRYPTION
              ? wc_AesXtsEncryptSector(x, out, in, sizeof(xts_cipher), XTS_SECTOR)
              : wc_AesXtsDecryptSector(x, out, in, sizeof(xts_cipher), XTS_SECTOR);
    wc_AesXtsFree(x);
    return ret;
}

static const char *t_xts_encrypt(selftest_t *st) {
    uint8_t in[32], out[32];
    memset(in, 0x44, sizeof(in));
    int ret = xts(st, AES_ENCRYPTION, in, out);
    if (ret != 0)
        return error("aes-xts encrypt", ret);
    return same(out, xts_cipher, sizeof(out)) ? NULL : "aes-xts encrypt gave a wrong result";
}

static const char *t_xts_decrypt(selftest_t *st) {
    uint8_t want[32], out[32];
    memset(want, 0x44, sizeof(want));
    int ret = xts(st, AES_DECRYPTION, xts_cipher, out);
    if (ret != 0)
        return error("aes-xts decrypt", ret);
    return same(out, want, sizeof(out)) ? NULL : "aes-xts decrypt gave a wrong result";
}

#if SOS_HAVE_SSH
// --- Ed25519 ---------------------------------------------------------------------------------

// The public key from the secret (as the host key is made: a base point multiplication).
static const char *t_ed_public(selftest_t *st) {
    ed25519_key *k = &st->u.ed;
    uint8_t pub[ED25519_PUB_KEY_SIZE];
    int ret = wc_ed25519_init(k);
    if (ret != 0)
        return error("ed25519 init", ret);
    ret = wc_ed25519_import_private_only(ed_secret, sizeof(ed_secret), k);
    if (ret == 0)
        ret = wc_ed25519_make_public(k, pub, sizeof(pub));
    wc_ed25519_free(k);
    if (ret != 0)
        return error("ed25519 public key", ret);
    return same(pub, ed_public, sizeof(pub)) ? NULL : "ed25519 gave a wrong public key";
}

static int ed_key(selftest_t *st) {
    int ret = wc_ed25519_init(&st->u.ed);
    if (ret != 0)
        return ret;
    ret = wc_ed25519_import_private_key(ed_secret, sizeof(ed_secret), ed_public,
                                        sizeof(ed_public), &st->u.ed);
    if (ret != 0)
        wc_ed25519_free(&st->u.ed);
    return ret;
}

static const char *t_ed_sign(selftest_t *st) {
    uint8_t sig[ED25519_SIG_SIZE];
    word32 len = sizeof(sig);
    int ret = ed_key(st);
    if (ret != 0)
        return error("ed25519 key import", ret);
    ret = wc_ed25519_sign_msg(ed_message, sizeof(ed_message), sig, &len, &st->u.ed);
    wc_ed25519_free(&st->u.ed);
    if (ret != 0)
        return error("ed25519 sign", ret);
    if (len != sizeof(sig) || !same(sig, ed_signature, sizeof(sig)))
        return "ed25519 gave a wrong signature";
    return NULL;
}

// Verify the vector's signature over the message, or over the message with one bit flipped.
// wolfCrypt's result, and *valid if it said the signature is good.
static int ed_verify(selftest_t *st, bool changed, bool *valid) {
    uint8_t msg[sizeof(ed_message)];
    memcpy(msg, ed_message, sizeof(msg));
    if (changed)
        msg[0] ^= flip();
    int res = 0;
    *valid = false;
    int ret = ed_key(st);
    if (ret != 0)
        return ret;
    ret = wc_ed25519_verify_msg(ed_signature, sizeof(ed_signature), msg, sizeof(msg), &res,
                                &st->u.ed);
    wc_ed25519_free(&st->u.ed);
    *valid = ret == 0 && res == 1;
    return ret;
}

static const char *t_ed_verify(selftest_t *st) {
    bool valid;
    int ret = ed_verify(st, false, &valid);
    if (ret != 0)
        return error("ed25519 verify", ret);
    return valid && !faulty() ? NULL : "ed25519 refused a good signature";
}

// A changed message: wolfCrypt says no (res 0, and SIG_VERIFY_E or 0 as its result). Any other
// error is an error, not a refusal.
static const char *t_ed_refuse(selftest_t *st) {
    bool valid;
    int ret = ed_verify(st, true, &valid);
    if (ret != 0 && ret != SIG_VERIFY_E)
        return error("ed25519 verify", ret);
    return valid ? "ed25519 verify accepted a changed message" : NULL;
}
#endif

#if SOS_HAVE_SSH || SOS_HAVE_TLS
// --- X25519 ----------------------------------------------------------------------------------

static const char *t_x25519(selftest_t *st) {
    // X25519 clamps the scalar itself (RFC 7748 §5); wolfCrypt wants it clamped on import. The
    // result is the same.
    uint8_t priv[CURVE25519_KEYSIZE], out[CURVE25519_KEYSIZE];
    memcpy(priv, x_alice_priv, sizeof(priv));
    priv[0] &= 248;
    priv[31] = (uint8_t)((priv[31] & 127) | 64);
    word32 len = sizeof(out);
    curve25519_key *k = &st->u.x.priv, *peer = &st->u.x.peer;
    int ret = wc_InitRng(&st->u.x.rng);
    if (ret != 0)
        return error("x25519 rng", ret);
    ret = wc_curve25519_init(k);
    if (ret == 0) {
        ret = wc_curve25519_init(peer);
        if (ret != 0)
            wc_curve25519_free(k);
    }
    if (ret != 0) {
        wc_FreeRng(&st->u.x.rng);
        return error("x25519 init", ret);
    }
    const char *step = "x25519 key import";
    ret = wc_curve25519_import_private_ex(priv, sizeof(priv), k, EC25519_LITTLE_ENDIAN);
#ifdef WOLFSSL_CURVE25519_BLINDING
    if (ret == 0)
        ret = wc_curve25519_set_rng(k, &st->u.x.rng);
#endif
    if (ret == 0)
        ret = wc_curve25519_check_public(x_bob_pub, sizeof(x_bob_pub), EC25519_LITTLE_ENDIAN);
    if (ret == 0)
        ret = wc_curve25519_import_public_ex(x_bob_pub, sizeof(x_bob_pub), peer,
                                             EC25519_LITTLE_ENDIAN);
    if (ret == 0) {
        step = "x25519 shared secret";
        ret = wc_curve25519_shared_secret_ex(k, peer, out, &len, EC25519_LITTLE_ENDIAN);
    }
    wc_curve25519_free(peer);
    wc_curve25519_free(k);
    wc_FreeRng(&st->u.x.rng);
    if (ret != 0)
        return error(step, ret);
    if (len != sizeof(out) || !same(out, x_shared, sizeof(out)))
        return "x25519 gave a wrong shared secret";
    return NULL;
}
#endif

// --- the table -------------------------------------------------------------------------------

static const struct {
    selftest_info_t info;
    const char *(*run)(selftest_t *);
} tests[] = {
    {{"sha256", SELFTEST_SHA, true}, t_sha256},
    {{"sha512", SELFTEST_SHA, true}, t_sha512},
    {{"hmac", SELFTEST_SHA, true}, t_hmac},
    {{"gcm-encrypt", SELFTEST_GCM, true}, t_gcm_encrypt},
    {{"gcm-decrypt", SELFTEST_GCM, true}, t_gcm_decrypt},
    {{"gcm-refuse", SELFTEST_GCM, true}, t_gcm_refuse},
    {{"xts-encrypt", SELFTEST_XTS, true}, t_xts_encrypt},
    {{"xts-decrypt", SELFTEST_XTS, true}, t_xts_decrypt},
#if SOS_HAVE_SSH
    // Ed25519 is SSH's alone (the host key, logins): docs/design/build-features.md §4.3.
    {{"ed-public", SELFTEST_ED_PUB, true}, t_ed_public},
    {{"ed-sign", SELFTEST_ED_SIGN, false}, t_ed_sign},
    {{"ed-verify", SELFTEST_ED_VERIFY, false}, t_ed_verify},
    {{"ed-refuse", SELFTEST_ED_VERIFY, false}, t_ed_refuse},
#endif
#if SOS_HAVE_SSH || SOS_HAVE_TLS
    // X25519: SSH's key exchange and TLS's.
    {{"x25519", SELFTEST_X25519, false}, t_x25519},
#endif
};

#define TEST_COUNT (sizeof(tests) / sizeof(tests[0]))

unsigned selftest_count(void) {
    return TEST_COUNT;
}

const selftest_info_t *selftest_info(unsigned index) {
    return index < TEST_COUNT ? &tests[index].info : NULL;
}

const char *selftest_group_name(selftest_group_t g) {
    static const char *const names[SELFTEST_GROUPS] = {
        [SELFTEST_SHA] = "sha",          [SELFTEST_GCM] = "gcm",
        [SELFTEST_XTS] = "xts",          [SELFTEST_ED_PUB] = "ed-pub",
        [SELFTEST_ED_SIGN] = "ed-sign",  [SELFTEST_ED_VERIFY] = "ed-verify",
        [SELFTEST_X25519] = "x25519",
    };
    return (unsigned)g < SELFTEST_GROUPS ? names[g] : "?";
}

selftest_t *selftest_open(void) {
    return heap_zalloc(HEAP_NET, sizeof(selftest_t));
}

void selftest_close(selftest_t *st) {
    if (!st)
        return;
    // The contexts held key material, though only the vectors' published keys.
    memset(st, 0, sizeof(*st));
    heap_release(HEAP_NET, st);
}

const char *selftest_run(selftest_t *st, unsigned index) {
    if (index >= TEST_COUNT)
        return "no such test";
    current = index;
    return tests[index].run(st);
}

// "0.4" under 10 ms, "19" above.
static void ms_text(char *out, size_t cap, uint32_t us) {
    if (us < 9950) {
        unsigned tenths = (unsigned)((us + 50) / 100);
        snprintf(out, cap, "%u.%u", tenths / 10, tenths % 10);
    } else {
        snprintf(out, cap, "%u", (unsigned)((us + 500) / 1000));
    }
}

void selftest_text(char *out, size_t cap, unsigned ran, uint32_t total_us,
                   const uint32_t group_us[SELFTEST_GROUPS],
                   const unsigned group_runs[SELFTEST_GROUPS]) {
    if (!cap)
        return;
    char ms[12];
    ms_text(ms, sizeof(ms), total_us);
    int n = snprintf(out, cap, "%u known answers in %s ms:", ran, ms);
    for (unsigned g = 0; g < SELFTEST_GROUPS && n > 0 && (size_t)n < cap; g++) {
        if (!group_runs[g])
            continue;
        ms_text(ms, sizeof(ms), group_us[g]);
        char part[24];
        int len = snprintf(part, sizeof(part), " %s %s", selftest_group_name((selftest_group_t)g),
                           ms);
        if (len < 0 || (size_t)n + (size_t)len >= cap)
            break;                      // whole groups only
        memcpy(out + n, part, (size_t)len + 1);
        n += len;
    }
}
