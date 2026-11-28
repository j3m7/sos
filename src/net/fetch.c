// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// `fetch`: see fetch.h and docs/design/archives.md §3.
//
// A shell job, like cp: each step polls the request and moves what has arrived into a 4 KB
// buffer, written to the card (and hashed) when full, for up to STEP_US. The request is pulled,
// not pushed (net/http.h), so a slow card slows the server down. The pure parts (where a
// redirect goes, the name a URL suggests, whether a name can be a file) are in http_msg.c and
// files/path.c, host tested.
//
// Over https, or plain http (archives.md §3.3). The transport is http.c's business: this file
// only warns that a plain hop isn't encrypted, and says when a plain body's size wasn't known
// (it ended at the TCP close, which a cut connection also gives): the hash is the check.
//
// The running SHA-256 is on the RP2350's SHA-256 block when it's free as the body starts
// (files/sha256hw.h), held until finish(), which every end goes through; else wolfCrypt's.

#include "net/fetch.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include "ff.h"

#include "core/heaptag.h"
#include "files/bigjob.h"
#include "files/files.h"
#include "files/path.h"
#include "files/sdcard.h"
#include "files/sha256hw.h"
#include "net/http.h"
#include "shell/termtext.h"

#define PATH_LEN      256           // as the file commands (FatFs long names: 255 characters)
#define PART_SUFFIX   ".part"
#define BUF_SIZE      4096          // body bytes gathered before each write
#define STEP_US       20000         // work per job step, as the file commands
#define DRAW_US       500000        // the progress line is redrawn at most this often
#define ERROR_BODY    256           // body bytes shown after a status that isn't 2xx
#define NAME_MAX_UTF8 (FF_MAX_LFN * 3 + 1)
#define PLAIN_WARNING "http: not encrypted; check its hash\n"

typedef enum {
    F_HEADERS,                      // connecting, redirects, waiting for the headers
    F_BODY,                         // 2xx: saving the body
    F_ERROR_BODY,                   // another status: showing the start of the body
} fetch_phase_t;

typedef struct {
    http_req_t *req;
    fetch_phase_t phase;
    bool force;
    bool progress;                  // draw the progress line (the console, not piped)
    bool drawn;                     // a progress line is on screen
    bool open;                      // f is open
    bool created;                   // this job created the .part (and still owns it)
    bool kept;                      // install() kept the .part and said where it is
    unsigned hops;                  // redirects reported
    FIL f;
    wc_Sha256 sha;
    bool sha_ok;                    // sha initialised
    bool sha_hw;                    // hashing on the SHA-256 block instead (held until the end)
    char path[PATH_LEN];
    char part[PATH_LEN];
    uint64_t body_us, drawn_us, synced_at;
    uint64_t received;              // body bytes taken from the request
    uint64_t written;               // ... and written to the card
    size_t buf_len;
    union {
        _Alignas(4) uint8_t buf[BUF_SIZE];  // the block takes aligned data a word at a time
        struct {                    // before the job starts: the URL and the name it gives
            http_url_t url;
            char name[NAME_MAX_UTF8];
        } setup;
    } u;
} fetch_job_t;

_Static_assert(sizeof(((fetch_job_t *)0)->u.buf) >= ERROR_BODY * 2 + 3, "error body text");

// Text from the server or a certificate, through the terminal filter (shell/termtext.h).
static void write_text(shell_t *sh, const char *s, size_t len) {
    termtext_t t = TERMTEXT_INIT;
    char out[128 + 3];
    while (len) {
        size_t n = len < 128 ? len : 128;
        shell_write(sh, out, termtext_filter(&t, (const uint8_t *)s, n, out));
        s += n;
        len -= n;
    }
    shell_write(sh, out, termtext_flush(&t, out));
}

static void clear_progress(shell_t *sh, fetch_job_t *j) {
    if (j->drawn)
        shell_write(sh, "\r\x1b[K", 4);
    j->drawn = false;
}

static unsigned rate_kbs(uint64_t bytes, uint64_t us) {
    return us ? (unsigned)(bytes * 1000000 / 1024 / us) : 0;
}

static void draw_progress(shell_t *sh, fetch_job_t *j) {
    uint64_t now = time_us_64();
    if (!j->progress || now - j->drawn_us < DRAW_US)
        return;
    j->drawn_us = now;
    const http_resp_t *p = http_response(j->req);
    unsigned rate = rate_kbs(j->received, now - j->body_us);
    if (p->has_length && p->length)
        shell_printf(sh, "\r\x1b[K%llu of %llu KB (%u%%), %u KB/s",
                     (unsigned long long)(j->received / 1024),
                     (unsigned long long)(p->length / 1024),
                     (unsigned)(j->received * 100 / p->length), rate);
    else
        shell_printf(sh, "\r\x1b[K%llu KB, %u KB/s", (unsigned long long)(j->received / 1024),
                     rate);
    j->drawn = true;
}

// Each redirect followed: `-> <url>` (printable ASCII: http_url_parse() takes nothing else).
static void report_hops(shell_t *sh, fetch_job_t *j) {
    unsigned n = http_redirects(j->req);
    if (n == j->hops)
        return;
    j->hops = n;
    const char *url = http_url(j->req);
    shell_write(sh, "-> ", 3);
    shell_write(sh, url, strlen(url));
    shell_write(sh, "\n", 1);
    if (http_plain(j->req))
        shell_printf(sh, PLAIN_WARNING);
}

// After a failed request: if the handshake failed for want of a trusted root, say which and
// what to do about it.
static void tls_hint(shell_t *sh, const fetch_job_t *j) {
    const tls_stats_t *ts = http_tls_stats(j->req);
    if (!ts || !ts->untrusted)
        return;
    if (ts->untrusted_issuer[0]) {
        shell_printf(sh, "fetch: no trusted root for ");
        write_text(sh, ts->untrusted_issuer, strlen(ts->untrusted_issuer));
        shell_write(sh, "\n", 1);
    }
    shell_printf(sh, "fetch: this board has 2 built-in roots; to trust another, put it in "
                     "/certs/roots.pem and run `tls roots approve` on the console (man tls)\n");
}

// Write the gathered body to the .part, hashing it. FR_OK, or why not.
static FRESULT flush(fetch_job_t *j) {
    if (!j->buf_len)
        return FR_OK;
    UINT bw;
    FRESULT fr = f_write(&j->f, j->u.buf, (UINT)j->buf_len, &bw);
    if (fr == FR_OK && bw != j->buf_len)
        fr = FR_DENIED;                         // card full
    if (fr != FR_OK)
        return fr;
    if (j->sha_hw)
        sha256hw_update(j->u.buf, j->buf_len);
    else
        wc_Sha256Update(&j->sha, j->u.buf, (word32)j->buf_len);
    j->written += j->buf_len;
    j->buf_len = 0;
    return files_sync_if_due(&j->f, &j->synced_at);
}

// The .part is complete and closed: put it in place of <file> (files_install_part(), which moves
// an old <file> aside with -f). NULL, or why not. When the .part must be kept, j->created is
// cleared and j->kept set, so finish() neither deletes it nor says "nothing saved"; the helper
// has said where the download is.
static const char *install(shell_t *sh, fetch_job_t *j) {
    const char *why;
    files_install_t r = files_install_part(sh, "fetch", "download", j->part, j->path, j->force,
                                           &why);
    if (r == FILES_KEPT) {
        j->created = false;
        j->kept = true;
    }
    return r == FILES_INSTALLED ? NULL : why;
}

static void print_summary(shell_t *sh, fetch_job_t *j) {
    uint64_t us = time_us_64() - j->body_us;
    uint8_t digest[WC_SHA256_DIGEST_SIZE];
    char hex[2 * WC_SHA256_DIGEST_SIZE + 1];
    if (j->sha_hw) {
        sha256hw_finish(digest);
        j->sha_hw = false;
    } else {
        wc_Sha256Final(&j->sha, digest);
    }
    for (size_t i = 0; i < sizeof(digest); i++)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    shell_printf(sh, "saved %s: %llu bytes in %llu.%01llu s (%u KB/s)\n", j->path,
                 (unsigned long long)j->written, (unsigned long long)(us / 1000000),
                 (unsigned long long)(us / 100000 % 10), rate_kbs(j->written, us));
    shell_printf(sh, "sha256: %s\n", hex);
    // Plain http without a length ended at the TCP close: a cut connection looks the same.
    if (http_plain(j->req) && http_response(j->req)->ended_by_close)
        shell_printf(sh, "size not known: check its hash\n");
}

typedef enum {
    END_DONE,                       // the request is complete: install the .part
    END_FAILED,                     // why says what went wrong
    END_STOPPED,                    // Ctrl-C, or the session ended
    END_SAID,                       // the job has said all there is to say
} fetch_end_t;

// The job ends. Frees everything. Returns false (for the step).
static bool finish(shell_t *sh, fetch_job_t *j, fetch_end_t how, const char *why) {
    clear_progress(sh, j);
    if (how == END_DONE) {
        FRESULT fr = flush(j);
        FRESULT cr = f_close(&j->f);
        j->open = false;
        if (fr == FR_OK)
            fr = cr;
        why = fr == FR_OK ? install(sh, j) : files_error(fr);
        if (why) {
            how = END_FAILED;
            shell_printf(sh, "fetch: %s: %s\n", j->path, why);
        } else {
            print_summary(sh, j);
        }
    } else if (how == END_FAILED) {
        shell_printf(sh, "fetch: %s\n", why);
        if (j->phase == F_HEADERS)
            tls_hint(sh, j);
    }
    if (j->open) {
        f_close(&j->f);
        j->open = false;
    }
    if (how != END_DONE && j->created)
        f_unlink(j->part);                      // a cut download never looks finished
    if (how != END_DONE && j->phase == F_BODY && !j->kept) {
        shell_printf(sh, "fetch: %s after %llu bytes; nothing saved\n",
                     how == END_STOPPED ? "stopped" : "failed", (unsigned long long)j->received);
    } else if (how == END_STOPPED) {
        shell_printf(sh, "fetch: stopped; nothing saved\n");
    }
    if (j->sha_hw)
        sha256hw_abort();                       // not finished: an error, Ctrl-C, the session
    if (j->sha_ok)
        wc_Sha256Free(&j->sha);
    http_close(j->req);
    heap_release(HEAP_FILES, j);
    files_release();
    bigjob_end();
    return false;
}

// The headers of the last hop are in. A 2xx opens the .part; anything else shows its status.
// False if the job can't go on (after saying why).
static bool headers_in(shell_t *sh, fetch_job_t *j) {
    const http_resp_t *p = http_response(j->req);
    if (p->status / 100 != 2) {
        shell_printf(sh, "fetch: HTTP/1.%d %d ", p->minor, p->status);
        write_text(sh, p->reason, strlen(p->reason));
        shell_write(sh, "\n", 1);
        j->phase = F_ERROR_BODY;
        return true;
    }
    FRESULT fr = f_open(&j->f, j->part, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        shell_printf(sh, "fetch: %s: %s\n", j->part, files_error(fr));
        return false;
    }
    j->open = j->created = true;
    j->sha_hw = sha256hw_begin();
    if (!j->sha_hw) {
        if (wc_InitSha256(&j->sha) != 0) {
            shell_printf(sh, "fetch: the hash couldn't start\n");
            return false;
        }
        j->sha_ok = true;
    }
    j->phase = F_BODY;
    j->body_us = j->synced_at = time_us_64();
    j->drawn_us = j->body_us;                   // the first line after DRAW_US
    return true;
}

// A status that isn't 2xx: show up to ERROR_BODY bytes of what came with it, filtered, and
// end. True while more may come.
static bool error_body(shell_t *sh, fetch_job_t *j) {
    int n = 0;
    if (j->buf_len < ERROR_BODY) {
        n = http_read(j->req, j->u.buf + j->buf_len, ERROR_BODY - j->buf_len);
        if (n > 0)
            j->buf_len += (size_t)n;
        if (n == 0)
            return true;
    }
    termtext_t t = TERMTEXT_INIT;
    char *text = (char *)j->u.buf + ERROR_BODY;
    size_t len = termtext_filter(&t, j->u.buf, j->buf_len, text);
    len += termtext_flush(&t, text + len);
    shell_write(sh, text, len);
    if (len && text[len - 1] != '\n')
        shell_write(sh, "\n", 1);
    shell_printf(sh, "fetch: nothing saved\n");
    return false;
}

static bool fetch_step(shell_t *sh, void *arg, bool cancel) {
    fetch_job_t *j = arg;
    if (cancel)
        return finish(sh, j, END_STOPPED, NULL);
    uint64_t t0 = time_us_64();
    while (time_us_64() - t0 < STEP_US) {
        if (j->phase == F_HEADERS) {
            http_state_t s = http_poll(j->req);
            report_hops(sh, j);
            if (s == HTTP_FAILED)
                return finish(sh, j, END_FAILED, http_error(j->req));
            if (s < HTTP_BODY)
                return true;
            if (!headers_in(sh, j))
                return finish(sh, j, END_SAID, NULL);
        }
        if (j->phase == F_ERROR_BODY)
            return error_body(sh, j) || finish(sh, j, END_SAID, NULL);
        int n = http_read(j->req, j->u.buf + j->buf_len, sizeof(j->u.buf) - j->buf_len);
        if (n < 0) {
            if (http_poll(j->req) != HTTP_DONE)
                return finish(sh, j, END_FAILED, http_error(j->req));
            return finish(sh, j, END_DONE, NULL);
        }
        if (n == 0)
            break;
        j->buf_len += (size_t)n;
        j->received += (size_t)n;
        if (j->buf_len == sizeof(j->u.buf)) {
            FRESULT fr = flush(j);
            if (fr != FR_OK)
                return finish(sh, j, END_FAILED, files_error(fr));
        }
    }
    draw_progress(sh, j);
    return true;
}

static void usage(shell_t *sh) {
    shell_printf(sh, "usage: fetch [-f] [-k] <url> [<file>]\n");
}

// Where the body goes: <file>, the URL's name inside <file> if it's a directory, or the URL's
// name in the working directory. Fills j->path and j->part; false after saying why.
static bool destination(shell_t *sh, fetch_job_t *j, const char *file) {
    const char *name = j->u.setup.name;
    const char *bad = http_url_name(j->u.setup.url.path, j->u.setup.name, NAME_MAX_UTF8);
    if (!bad)
        bad = path_name_check(name);
    if (!path_resolve(sh->cwd, file ? file : "", j->path, PATH_LEN)) {
        shell_printf(sh, "fetch: path too long\n");
        return false;
    }
    if (!sdcard_mount(sh))
        return false;
    FILINFO fi;
    bool dir = !strcmp(j->path, "/") || (f_stat(j->path, &fi) == FR_OK && (fi.fattrib & AM_DIR));
    if (dir) {
        if (bad) {
            shell_printf(sh, "fetch: no file name from the URL (%s): give one after the URL\n",
                         bad);
            return false;
        }
        char d[PATH_LEN];
        strcpy(d, j->path);
        if (!path_join(d, name, j->path, PATH_LEN)) {
            shell_printf(sh, "fetch: path too long\n");
            return false;
        }
    }
    size_t len = strlen(j->path);
    if (len + sizeof(PART_SUFFIX) > PATH_LEN) {
        shell_printf(sh, "fetch: path too long\n");
        return false;
    }
    memcpy(j->part, j->path, len);
    memcpy(j->part + len, PART_SUFFIX, sizeof(PART_SUFFIX));
    if (!files_write_allowed(sh, j->path) || !files_write_allowed(sh, j->part)) {
        shell_printf(sh, "fetch: %s: not allowed to change this path\n", j->path);
        return false;
    }
    if (f_stat(j->path, &fi) == FR_OK) {
        if (fi.fattrib & AM_DIR) {
            shell_printf(sh, "fetch: %s: is a directory\n", j->path);
            return false;
        }
        if (!j->force) {
            shell_printf(sh, "fetch: %s: exists (-f replaces it)\n", j->path);
            return false;
        }
    }
    return true;
}

void fetch_command(shell_t *sh, int argc, char **argv) {
    bool force = false, downgrade = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'f') {
                force = true;
            } else if (*o == 'k') {
                downgrade = true;
            } else {
                usage(sh);
                return;
            }
        }
    if (argc - i < 1 || argc - i > 2 || !*argv[i] || (argv[i + 1] && !*argv[i + 1])) {
        usage(sh);
        return;
    }
    const char *url = argv[i], *file = argv[i + 1];

    const char *other = bigjob_try_begin("fetch");
    if (other) {
        shell_printf(sh, BIGJOB_BUSY_FMT, other);
        return;
    }
    fetch_job_t *j = heap_zalloc(HEAP_FILES, sizeof(*j));
    if (!j) {
        shell_printf(sh, "fetch: out of memory\n");
        bigjob_end();
        return;
    }
    j->force = force;
    const char *err = http_url_parse_any(url, &j->u.setup.url, true);
    if (err) {
        shell_printf(sh, "fetch: %s\n", err);
        goto fail;
    }
    if (!destination(sh, j, file))
        goto fail;
    http_opts_t o = {
        .method = "GET",
        .url = url,
        .max_redirects = HTTP_REDIRECTS_MAX,
        .allow_plain = true,
        .allow_downgrade = downgrade,
    };
    j->req = http_start(&o, &err);
    if (!j->req) {
        shell_printf(sh, "fetch: %s\n", err);
        goto fail;
    }
    if (http_plain(j->req))
        shell_printf(sh, PLAIN_WARNING);
    j->progress = sh->io->admin && !shell_output_piped(sh);
    j->buf_len = 0;                             // the setup data is done with
    files_hold();
    shell_run_job(sh, fetch_step, j);
    return;
fail:
    heap_release(HEAP_FILES, j);
    bigjob_end();
}
