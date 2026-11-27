// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// wolfCrypt built for the host tests: only what the device code under test uses (AES-XTS and
// AES-GCM for src/files/sdcrypt.c, HKDF-SHA256 for src/core/keyprov.c, SHA-1/256/384/512 and
// HMAC for src/lua/lua_hash.c), with the same AES options as config/user_settings.h.

#define WOLFCRYPT_ONLY
// Align wolfCrypt's internal buffers. Without it, GHASH's 64-bit loads from a local byte array
// are misaligned (harmless on x86, but UBSan stops the test).
#define WOLFSSL_USE_ALIGN
#define NO_FILESYSTEM
#define SINGLE_THREADED
#define WOLFSSL_AES_DIRECT
#define WOLFSSL_AES_XTS
#define HAVE_AESGCM
#define GCM_TABLE_4BIT
#define NO_RSA
#define NO_DH
#define NO_DSA
#define NO_DES3
#define NO_MD5
#define WOLFSSL_SHA384      // and SHA-1, SHA-512: src/lua/lua_hash.c (sos.hash, sos.hmac)
#define WOLFSSL_SHA512
#define NO_OLD_TLS
#define WC_NO_RNG
#define NO_ASN
#define NO_CODING
#define NO_PWDBASED
#define HAVE_HKDF
#define NO_SIG_WRAPPER
