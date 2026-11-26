// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "tls/certstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "ff.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/asn.h>
#include <wolfssl/wolfcrypt/hash.h>

#include "core/config.h"
#include "core/heaptag.h"
#include "core/log.h"
#include "core/settings.h"
#include "files/sdcard.h"
#include "time/timeconv.h"
#include "time/timesync.h"
#include "tls/certbundle.h"
#include "tls/tls.h"
#include "tls/tls_roots.h"

typedef enum {
    ST_NOT_APPROVED,    // no hash pinned
    ST_OK,              // file matches the approved hash
    ST_CHANGED,         // file differs from what was approved
    ST_MISSING,         // no card, or no file
    ST_UNREADABLE,      // read error, empty or too big
} certs_state_t;

static const char *const state_text[] = {
    "none approved", "approved", "changed since approval: not used",
    "missing: not used", "unreadable: not used",
};

static bool pinned(void) {
    const uint8_t *h = settings()->certs_hash;
    for (unsigned i = 0; i < CONFIG_CERTS_HASH_LEN; i++)
        if (h[i])
            return true;
    return false;
}

// Read the whole file into a new buffer. ST_OK on success (*buf to free).
static certs_state_t read_file(uint8_t **buf, size_t *len, const char **why) {
    *buf = NULL;
    *len = 0;
    if (!sdcard_mount(NULL)) {
        *why = "no SD card";
        return ST_MISSING;
    }
    FIL f;
    FRESULT fr = f_open(&f, CERTSTORE_FILE, FA_READ);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        *why = "no " CERTSTORE_FILE;
        return ST_MISSING;
    }
    if (fr != FR_OK) {
        *why = "can't open it (in use?)";
        return ST_UNREADABLE;
    }
    FSIZE_t size = f_size(&f);
    if (size == 0 || size > CERTSTORE_FILE_MAX) {
        f_close(&f);
        *why = size ? "larger than 64 KB" : "empty";
        return ST_UNREADABLE;
    }
    uint8_t *b = heap_alloc(HEAP_NET, (size_t)size);
    UINT got = 0;
    if (!b || f_read(&f, b, (UINT)size, &got) != FR_OK || got != size) {
        heap_release(HEAP_NET, b);
        f_close(&f);
        *why = b ? "read error" : "out of memory";
        return ST_UNREADABLE;
    }
    f_close(&f);
    *buf = b;
    *len = (size_t)size;
    return ST_OK;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    wc_Sha256Hash(data, (word32)len, out);
}

// Read the file and compare it with the approved hash.
static certs_state_t read_checked(uint8_t **buf, size_t *len, const char **why) {
    if (!pinned()) {
        *why = NULL;
        return ST_NOT_APPROVED;
    }
    certs_state_t st = read_file(buf, len, why);
    if (st != ST_OK)
        return st;
    uint8_t h[32];
    sha256(*buf, *len, h);
    if (memcmp(h, settings()->certs_hash, sizeof(h))) {
        heap_release(HEAP_NET, *buf);
        *buf = NULL;
        *why = "SHA-256 differs; re-approve it on the console";
        return ST_CHANGED;
    }
    return ST_OK;
}

// Log the state when it changes (TLS calls this per connection).
static void note(certs_state_t st, const char *why, unsigned loaded) {
    static int last = -1;
    if ((int)st == last)
        return;
    last = (int)st;
    if (st == ST_OK)
        log_msg(SOS_LOG_NOTICE, "tls: using %u root certificate(s) from %s", loaded, CERTSTORE_FILE);
    else if (st != ST_NOT_APPROVED)
        log_msg(SOS_LOG_WARNING, "tls: %s %s (%s); built-in roots only", CERTSTORE_FILE,
                state_text[st], why ? why : "");
}

// Parse the file into DER certificates: returns the buffer to free (NULL on error).
static uint8_t *parse(const uint8_t *file, size_t len, certbundle_cert_t *certs, size_t *count,
                      const char **why) {
    uint8_t *der = heap_alloc(HEAP_NET, len);
    if (!der) {
        *why = "out of memory";
        return NULL;
    }
    *count = certbundle_parse(file, len, der, len, certs, CERTBUNDLE_MAX_CERTS, why);
    if (!*count) {
        heap_release(HEAP_NET, der);
        return NULL;
    }
    return der;
}

unsigned certstore_load(struct WOLFSSL_CTX *ctx) {
    uint8_t *file;
    size_t len;
    const char *why = NULL;
    certs_state_t st = read_checked(&file, &len, &why);
    if (st != ST_OK) {
        note(st, why, 0);
        return 0;
    }
    static certbundle_cert_t certs[CERTBUNDLE_MAX_CERTS];
    size_t count = 0;
    uint8_t *der = parse(file, len, certs, &count, &why);
    heap_release(HEAP_NET, file);
    unsigned loaded = 0;
    for (size_t i = 0; der && i < count; i++)
        if (tls_load_root(ctx, der + certs[i].off, certs[i].len) == WOLFSSL_SUCCESS)
            loaded++;
    heap_release(HEAP_NET, der);
    if (!der && !why)
        why = "no certificates";
    note(der ? ST_OK : ST_UNREADABLE, why, loaded);
    return loaded;
}

// --- shell ---------------------------------------------------------------------------------------

static void print_hex(shell_t *sh, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        shell_printf(sh, "%s%02X", i ? ":" : "", b[i]);
}

// The certificate's notAfter as Unix time (UTCTime or GeneralizedTime); false if unparsable.
static bool cert_expiry(const uint8_t *d, int n, int64_t *t) {
    if (n > 2 && (d[0] == 0x17 || d[0] == 0x18) && d[1] == n - 2) {
        d += 2;                                 // skip the ASN.1 tag and length
        n -= 2;
    }
    int digits = n == 13 ? 12 : n == 15 ? 14 : 0;
    if (!digits || d[n - 1] != 'Z')
        return false;
    int v[7] = {0};
    for (int i = 0; i < digits; i++) {
        if (d[i] < '0' || d[i] > '9')
            return false;
    }
    int k = 0;
    if (digits == 12) {
        int yy = (d[0] - '0') * 10 + (d[1] - '0');
        v[0] = yy < 50 ? 2000 + yy : 1900 + yy;  // RFC 5280 UTCTime window
        k = 2;
    } else {
        v[0] = (d[0] - '0') * 1000 + (d[1] - '0') * 100 + (d[2] - '0') * 10 + (d[3] - '0');
        k = 4;
    }
    for (int i = 1; i < 6; i++, k += 2)
        v[i] = (d[k] - '0') * 10 + (d[k + 1] - '0');
    timeconv_cal_t cal = {.year = v[0], .month = v[1], .day = v[2], .hour = v[3], .min = v[4],
                          .sec = v[5]};
    return timeconv_from_cal(&cal, t);
}

// Print one certificate; returns false if wolfSSL can't parse it.
static bool describe(shell_t *sh, unsigned n, const uint8_t *der, size_t len) {
    DecodedCert *dc = heap_alloc(HEAP_NET, sizeof(*dc));
    if (!dc)
        return false;
    wc_InitDecodedCert(dc, der, (word32)len, NULL);
    bool ok = wc_ParseCert(dc, CERT_TYPE, NO_VERIFY, NULL) == 0;
    shell_printf(sh, "%2u. ", n);
    if (!ok) {
        shell_printf(sh, "(wolfSSL can't parse this certificate)\n");
    } else {
        if (dc->subjectCN && dc->subjectCNLen > 0) {
            // The name is the file's to choose: only printable ASCII reaches the terminal, so
            // it can't redraw what the approval shows.
            char cn[65];
            int n = dc->subjectCNLen < (int)sizeof(cn) - 1 ? dc->subjectCNLen : (int)sizeof(cn) - 1;
            for (int i = 0; i < n; i++) {
                unsigned char c = (unsigned char)dc->subjectCN[i];
                cn[i] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
            }
            cn[n] = '\0';
            shell_printf(sh, "%s", cn);
        }
        else
            shell_printf(sh, "(no common name)");
        shell_printf(sh, ": %s%s", dc->keyOID == RSAk ? "RSA" : dc->keyOID == ECDSAk ? "ECDSA"
                     : "other key type", dc->isCA ? ", CA" : ", NOT A CA");
        int64_t t;
        if (cert_expiry(dc->afterDate, dc->afterDateLen, &t)) {
            char buf[24];
            timeconv_format(t, buf, sizeof(buf));
            buf[10] = '\0';                     // date only
            struct timeval tv;
            gettimeofday(&tv, NULL);
            bool expired = timesync_clock_set() && t < tv.tv_sec;
            shell_printf(sh, ", %s %s", expired ? "EXPIRED" : "valid until", buf);
        }
        shell_printf(sh, "\n");
    }
    uint8_t fp[32];
    sha256(der, len, fp);
    shell_printf(sh, "    SHA-256 ");
    print_hex(sh, fp, sizeof(fp));
    shell_printf(sh, "\n");
    wc_FreeDecodedCert(dc);
    heap_release(HEAP_NET, dc);
    return ok;
}

static uint8_t pending_hash[32];

static void on_approve(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer || strcmp(answer, "yes")) {
        shell_printf(sh, "not approved\n");
        return;
    }
    memcpy(settings()->certs_hash, pending_hash, sizeof(pending_hash));
    if (!settings_save())
        shell_printf(sh, "error: saving to flash failed; the approval will be lost at reboot\n");
    log_msg(SOS_LOG_NOTICE, "config: %s approved for TLS", CERTSTORE_FILE);
    shell_printf(sh, "approved: TLS uses these roots from the next connection\n");
}

static void cmd_approve(shell_t *sh) {
    uint8_t *file;
    size_t len;
    const char *why = NULL;
    if (read_file(&file, &len, &why) != ST_OK) {
        shell_printf(sh, "%s: %s\n", CERTSTORE_FILE, why);
        return;
    }
    static certbundle_cert_t certs[CERTBUNDLE_MAX_CERTS];
    size_t count = 0;
    uint8_t *der = parse(file, len, certs, &count, &why);
    sha256(file, len, pending_hash);            // exactly the bytes listed here
    heap_release(HEAP_NET, file);
    if (!der) {
        shell_printf(sh, "%s: %s\n", CERTSTORE_FILE, why);
        return;
    }

    // Check TLS can use each one: load them into a throwaway context.
    static bool wolf_init;
    if (!wolf_init)
        wolf_init = wolfSSL_Init() == WOLFSSL_SUCCESS;
    WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    unsigned usable = 0;
    shell_printf(sh, "%s holds %u certificate(s):\n", CERTSTORE_FILE, (unsigned)count);
    for (size_t i = 0; i < count; i++) {
        describe(sh, (unsigned)i + 1, der + certs[i].off, certs[i].len);
        if (ctx && tls_load_root(ctx, der + certs[i].off, certs[i].len) == WOLFSSL_SUCCESS)
            usable++;
        else
            shell_printf(sh, "    (TLS can't use this one: unsupported key type or size?)\n");
    }
    if (ctx)
        wolfSSL_CTX_free(ctx);
    heap_release(HEAP_NET, der);
    if (!usable) {
        shell_printf(sh, "none of them can be used by TLS: not approved\n");
        return;
    }
    shell_printf(sh, "File SHA-256: ");
    print_hex(sh, pending_hash, sizeof(pending_hash));
    shell_printf(sh, "\n");
    static char prompt[96];                     // shell_ask() keeps the pointer
    snprintf(prompt, sizeof(prompt), "Trust %s certificate(s) above for TLS? Type 'yes': ",
             usable == count ? "the" : "the usable");
    shell_ask(sh, prompt, false, on_approve, NULL);
}

static void cmd_status(shell_t *sh) {
    shell_printf(sh, "built-in: ");
    for (unsigned i = 0; i < tls_root_count; i++)
        shell_printf(sh, "%s%s", i ? ", " : "", tls_roots[i].name);
    shell_printf(sh, "\n");
    uint8_t *file;
    size_t len;
    const char *why = NULL;
    certs_state_t st = read_checked(&file, &len, &why);
    shell_printf(sh, "sd:       %s %s", CERTSTORE_FILE, state_text[st]);
    if (st == ST_OK) {
        static certbundle_cert_t certs[CERTBUNDLE_MAX_CERTS];
        size_t count = 0;
        uint8_t *der = parse(file, len, certs, &count, &why);
        heap_release(HEAP_NET, file);
        heap_release(HEAP_NET, der);
        shell_printf(sh, ", %u certificate(s)\n", (unsigned)count);
    } else {
        shell_printf(sh, why ? " (%s)\n" : "\n", why);
    }
    if (st == ST_NOT_APPROVED)
        shell_printf(sh, "          'tls roots approve' on the console trusts the file\n");
    else {
        shell_printf(sh, "pinned:   ");
        print_hex(sh, settings()->certs_hash, 8);
        shell_printf(sh, "...\n");
    }
}

void certstore_command(shell_t *sh, char *args) {
    if (!args || !*args) {
        cmd_status(sh);
        return;
    }
    bool approve = !strcmp(args, "approve"), forget = !strcmp(args, "forget");
    if (!approve && !forget) {
        shell_printf(sh, "usage: tls roots [approve | forget]\n");
        return;
    }
    if (!sh->io->admin) {
        shell_printf(sh, "trusted certificates can only be changed on the serial console\n");
        return;
    }
    if (approve) {
        cmd_approve(sh);
        return;
    }
    memset(settings()->certs_hash, 0, CONFIG_CERTS_HASH_LEN);
    if (!settings_save())
        shell_printf(sh, "error: saving to flash failed; the change will be lost at reboot\n");
    log_msg(SOS_LOG_NOTICE, "config: %s no longer trusted", CERTSTORE_FILE);
    shell_printf(sh, "forgotten: TLS uses the built-in roots only\n");
}
