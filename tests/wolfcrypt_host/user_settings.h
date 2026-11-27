// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// wolfCrypt built for the host tests: only what the device code under test uses (AES-XTS and
// AES-GCM for src/files/sdcrypt.c, HKDF-SHA256 for src/core/keyprov.c, SHA-1/256/384/512 and
// HMAC for src/lua/lua_hash.c, and Ed25519 and X25519 for src/core/selftest.c's known answers).
//
// For every primitive these tests run, the options are the firmware's (config/user_settings.h),
// so that a test passing here means the same code path passes on the board: the `crypto` health
// check's known answers close the key and encryption gates on every board if one is wrong for
// the firmware build. That happened once (#242): the firmware has an RNG, so wolfCrypt turns on
// Curve25519 blinding there, which needs an RNG on the key; this file had WC_NO_RNG, so the host
// ran the unblinded path and passed.
//
// Blinding on the board depends on WOLFSSL_ARMASM staying per file (aes.c and the assembly only,
// CMakeLists.txt): wolfSSL's settings.h leaves blinding off when WOLFSSL_ARMASM is global. If it
// ever becomes global, the board stops blinding while this file still blinds, and both builds
// keep passing: nothing would notice. Change this file with it.
//
// What still differs from the firmware, and why it can't matter to the tests:
// - WOLFCRYPT_ONLY, and no RSA, DH, ECC, ML-KEM, ChaCha, AES-SIV, CMAC, SHA-3: not tested here.
// - The RNG's seed: getrandom() here (WOLFSSL_GETRANDOM, as NO_FILESYSTEM rules out
//   /dev/urandom), wc_pico_rng_gen_block (pico_rand) on the board. Only the seed source; the
//   Hash-DRBG on top is the same.
// - AES: the firmware builds aes.c with WOLFSSL_ARMASM (Thumb-2 assembly, CMakeLists.txt); the
//   host has the C code. Same results by definition; the board's own `crypto` check covers the
//   assembly.
// - SHA-256's tables in RAM on the board (FLASH_QUALIFIER): placement only.
// - TARGET_EMBEDDED's platform settings (WOLFSSL_RPIPICO, WOLFSSL_USER_IO, NO_WRITEV...): not
//   crypto.
// - NO_CURVED25519_128BIT: on the host only. Without it a 64-bit host would pick the 128-bit
//   field code (fe_x25519_128.h); the board has no 128-bit type and uses the 32-bit C code, which
//   this forces here too.

#define WOLFCRYPT_ONLY
#define NO_FILESYSTEM
#define SINGLE_THREADED

// --- as in config/user_settings.h --------------------------------------------------------------
#define WOLFSSL_USE_ALIGN       // also keeps GHASH's 64-bit loads aligned for UBSan
#define WOLFSSL_SMALL_STACK
#define WOLFSSL_AES_DIRECT
#define WOLFSSL_AES_XTS
#define HAVE_AES_CBC
#define HAVE_AESGCM
#define GCM_TABLE_4BIT
#define WOLFSSL_SHA512
#define WOLFSSL_SHA384
#define HAVE_HKDF
#define HAVE_HASHDRBG           // the RNG is on, as on the board: Curve25519 blinding follows
#define HAVE_CURVE25519
#define HAVE_ED25519
#define HAVE_ED25519_KEY_IMPORT
#define HAVE_ED25519_KEY_EXPORT
#define HAVE_ED25519_MAKE_KEY
#define WOLFSSL_ED25519_STREAMING_VERIFY
#define NO_DES3
#define NO_MD5
#define NO_DSA
#define NO_PWDBASED
#define NO_OLD_TLS

// --- host only (see above) -----------------------------------------------------------------------
#define NO_CURVED25519_128BIT
#define WOLFSSL_GETRANDOM
#define NO_RSA
#define NO_DH
#define NO_ASN
#define NO_CODING
#define NO_SIG_WRAPPER
