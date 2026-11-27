// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The manual pages: plain text files on the card, /man/<topic>.txt, one per command, written in
// the repository (man/) and copied to the card either over SFTP (tools/man-install.sh) or by
// `man install`, which downloads them from GitHub over HTTPS. The firmware holds no page.
//
// `man <topic>` shows a page in the pager (edit_view_file: the file is read as you move, so a
// page can't send the terminal a sequence). `man` alone lists the pages: a job (shell/shell.h)
// that reads the directory, sorts the names and prints each page's one-line description from
// its NAME section, a few pages per pass. Without any pages, both offer to download them.
//
// `man install` fetches one file, man/pages.bundle, built from the repository's pages by
// tools/man-bundle.sh: a line "@@ <topic>" starts each page. It is taken from the commit the
// firmware was built from, so the pages match the running firmware, or from main if the build
// isn't a pushed commit (or the commit has no pages yet). Each page is written to
// /man/<topic>.~new and renamed into place when it is complete, so a download that fails
// midway leaves the pages before it whole and drops the one it was on; /man is made at the
// first page, so a download that fails before any leaves nothing behind.

#include "man/man.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "core/heaptag.h"
#include "core/version.h"
#include "edit/edit.h"
#include "files/files.h"
#include "files/sdcard.h"
#include "man/man_topic.h"
#include "net/http.h"
#include "shell/complete.h"
#include "shell/termtext.h"

#define MAN_DIR         "/man"
#define MAN_NEW_EXT     ".~new"         // a page being downloaded
#define MAN_PAGES_MAX   96              // pages the listing can sort
#define MAN_HEAD_BYTES  256             // the start of a page: NAME and its description line
#define MAN_DESC_MAX    64              // description shown in the listing
// A listing line: 2 spaces, the name, a space, the description (the filter may grow it by a
// few bytes), CRLF.
#define MAN_LIST_LINE   (MAN_TOPIC_MAX + MAN_DESC_MAX + 16)
#define MAN_PER_PASS    4               // pages opened per job step
#define MAN_REPO        "j3m7/sos"
#define MAN_URL_FMT     "https://raw.githubusercontent.com/" MAN_REPO "/%s/man/pages.bundle"
#define MAN_STEP_US     20000           // most of a main-loop pass the install job takes

#define MAN_HOW_TO_INSTALL                                                                     \
    "no manual pages on the card: 'man install' downloads them from GitHub (the board needs\n" \
    "the network), or copy them over SFTP from the repository: tools/man-install.sh <host>\n"


static void page_path(char *out, size_t cap, const char *topic, const char *ext) {
    snprintf(out, cap, MAN_DIR "/%s%s", topic, ext);
}

// Is there at least one page on the card? (A /man directory left empty by a failed download
// counts as no pages, so the offer to download comes again.)
static bool have_pages(void) {
    DIR d;
    if (f_opendir(&d, MAN_DIR) != FR_OK)
        return false;
    FILINFO fi;
    bool found = false;
    char topic[MAN_TOPIC_MAX + 1];
    while (!found && f_readdir(&d, &fi) == FR_OK && fi.fname[0])
        found = !(fi.fattrib & AM_DIR) && man_topic_of(fi.fname, topic, sizeof(topic));
    f_closedir(&d);
    return found;
}

// --- installing ----------------------------------------------------------------------------------

typedef struct {
    http_req_t *req;
    char ref[16];                           // the commit, or "main"
    bool from_commit;                       // ref is the commit: 404 falls back to main
    bool shown;                             // the status line was handled
    char url[128];
    char buf[1024];
    // The bundle, a byte at a time: a line "@@ <topic>" starts a page.
    bool at_line_start;
    char hdr[MAN_TOPIC_MAX + 8];            // a line that may be a header, while it comes in
    size_t hdr_len;
    bool in_hdr;
    bool skip_line;                         // a '@' line too long for hdr: not a page, dropped
    bool dir_made;                          // /man exists (made at the first page)
    FIL f;
    bool f_open;
    char topic[MAN_TOPIC_MAX + 1];          // the page being written
    unsigned pages, skipped;
    bool failed;
} install_job_t;

// Where the bundle is fetched from: the commit this firmware was built from, if it is one (12
// hex digits), else main. True if it is the commit.
static bool bundle_url(char *ref, size_t ref_cap, char *url, size_t url_cap) {
    const char *v = sos_version;
    bool commit = strlen(v) == 12;
    for (const char *p = v; commit && *p; p++)
        commit = isxdigit((unsigned char)*p);
    snprintf(ref, ref_cap, "%s", commit ? v : "main");
    snprintf(url, url_cap, MAN_URL_FMT, ref);
    return commit;
}

static bool install_start(shell_t *sh, install_job_t *j) {
    http_opts_t o = { .method = "GET", .url = j->url };
    const char *err;
    j->req = http_start(&o, &err);
    if (!j->req) {
        shell_printf(sh, "man install: %s\n", err);
        return false;
    }
    j->shown = false;
    return true;
}

// The page being written is cut short (the download failed or was stopped): dropped.
static void drop_page(install_job_t *j) {
    if (!j->f_open)
        return;
    f_close(&j->f);
    j->f_open = false;
    char tmp[sizeof(MAN_DIR) + MAN_TOPIC_MAX + sizeof(MAN_NEW_EXT)];
    page_path(tmp, sizeof(tmp), j->topic, MAN_NEW_EXT);
    f_unlink(tmp);
}

// The page being written is complete: into place.
static void finish_page(shell_t *sh, install_job_t *j) {
    if (!j->f_open)
        return;
    char tmp[sizeof(MAN_DIR) + MAN_TOPIC_MAX + sizeof(MAN_NEW_EXT)];
    char final[sizeof(MAN_DIR) + MAN_TOPIC_MAX + sizeof(MAN_EXT)];
    page_path(tmp, sizeof(tmp), j->topic, MAN_NEW_EXT);
    page_path(final, sizeof(final), j->topic, MAN_EXT);
    FRESULT fr = f_close(&j->f);
    j->f_open = false;
    if (fr == FR_OK) {
        FRESULT ur = f_unlink(final);
        if (ur != FR_OK && ur != FR_NO_FILE)
            fr = ur;
        else
            fr = f_rename(tmp, final);
    }
    if (fr != FR_OK) {
        shell_printf(sh, "man install: %s: %s\n", final, files_error(fr));
        f_unlink(tmp);
        j->skipped++;
    } else {
        j->pages++;
    }
}

// A header line came in: start its page (an unusable topic is skipped: its text goes nowhere).
// The directory is made at the first page, so a download that fails before any leaves none.
static void start_page(shell_t *sh, install_job_t *j, char *topic) {
    finish_page(sh, j);
    if (!man_topic_ok(topic)) {
        j->skipped++;
        return;
    }
    char tmp[sizeof(MAN_DIR) + MAN_TOPIC_MAX + sizeof(MAN_NEW_EXT)];
    char final[sizeof(MAN_DIR) + MAN_TOPIC_MAX + sizeof(MAN_EXT)];
    snprintf(j->topic, sizeof(j->topic), "%s", topic);
    page_path(tmp, sizeof(tmp), topic, MAN_NEW_EXT);
    page_path(final, sizeof(final), topic, MAN_EXT);
    if (!files_write_allowed(sh, tmp) || !files_write_allowed(sh, final)) {
        shell_printf(sh, "man install: %s: this path can only be changed on the console\n", final);
        j->skipped++;
        return;
    }
    if (!j->dir_made) {
        FRESULT fr = f_mkdir(MAN_DIR);
        if (fr != FR_OK && fr != FR_EXIST) {
            shell_printf(sh, "man install: %s: %s\n", MAN_DIR, files_error(fr));
            j->failed = true;
            return;
        }
        j->dir_made = true;
    }
    FRESULT fr = f_open(&j->f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        shell_printf(sh, "man install: %s: %s\n", tmp, files_error(fr));
        j->skipped++;
        return;
    }
    j->f_open = true;
}

static void write_text(shell_t *sh, install_job_t *j, const char *s, size_t n) {
    if (!j->f_open || !n)
        return;
    UINT wrote;
    FRESULT fr = f_write(&j->f, s, (UINT)n, &wrote);
    if (fr != FR_OK || wrote != n) {
        shell_printf(sh, "man install: writing %s: %s\n", j->topic,
                     fr != FR_OK ? files_error(fr) : "card full");
        drop_page(j);
        j->skipped++;
        j->failed = true;
    }
}

// Take n bytes of the bundle: text goes to the page being written, a line "@@ <topic>" starts
// the next page. A line that begins with '@' is held until its end shows what it is.
static void take(shell_t *sh, install_job_t *j, const char *s, size_t n) {
    size_t from = 0;                        // text not written yet starts here
    for (size_t i = 0; i < n && !j->failed; i++) {
        char c = s[i];
        if (j->skip_line) {                 // the rest of a '@' line too long to be a header
            if (c == '\n') {
                j->skip_line = false;
                j->at_line_start = true;
                from = i + 1;
            }
            continue;
        }
        if (j->in_hdr) {
            if (c != '\n' && j->hdr_len < sizeof(j->hdr) - 1) {
                j->hdr[j->hdr_len++] = c;
                continue;
            }
            j->in_hdr = false;
            j->hdr[j->hdr_len] = '\0';
            if (c != '\n') {               // too long for a topic: not a page of ours
                finish_page(sh, j);        // whatever follows isn't the previous page's either
                j->skipped++;
                j->skip_line = true;
                continue;
            }
            if (!strncmp(j->hdr, "@@ ", 3)) {
                start_page(sh, j, j->hdr + 3);
                j->at_line_start = true;
                from = i + 1;
                continue;
            }
            write_text(sh, j, j->hdr, j->hdr_len);     // not a header: text, and so is c
            from = i;
        }
        if (j->at_line_start && c == '@') {
            write_text(sh, j, s + from, i - from);
            j->in_hdr = true;
            j->hdr_len = 1;
            j->hdr[0] = '@';
            j->at_line_start = false;
            continue;
        }
        j->at_line_start = c == '\n';
    }
    if (!j->in_hdr && !j->skip_line && from < n && !j->failed)
        write_text(sh, j, s + from, n - from);
}

static bool install_end(shell_t *sh, install_job_t *j, bool ok) {
    if (ok)
        finish_page(sh, j);
    else
        drop_page(j);                       // cut short: the pages before it stay whole
    http_close(j->req);
    if (ok) {
        shell_printf(sh, "man install: %u page%s installed in " MAN_DIR "%s\n", j->pages,
                     j->pages == 1 ? "" : "s", j->skipped ? " (some skipped, see above)" : "");
        if (j->pages)
            shell_printf(sh, "'man' lists them, 'man <topic>' shows one\n");
    }
    files_release();
    heap_release(HEAP_FILES, j);
    return false;
}

static bool install_step(shell_t *sh, void *arg, bool cancel) {
    install_job_t *j = arg;
    if (cancel)
        return install_end(sh, j, false);
    uint64_t t0 = time_us_64();
    while (time_us_64() - t0 < MAN_STEP_US) {
        http_state_t s = http_poll(j->req);
        if (s == HTTP_FAILED) {
            shell_printf(sh, "man install: %s: %s\n", j->url, http_error(j->req));
            return install_end(sh, j, false);
        }
        if (s < HTTP_BODY)
            return true;
        if (!j->shown) {
            j->shown = true;
            int status = http_response(j->req)->status;
            if (status == 404 && j->from_commit) {
                // The pages of this commit aren't on GitHub (not pushed, or older than the
                // pages): main's are the next best.
                shell_printf(sh, "no pages for commit %s on GitHub, trying main\n", j->ref);
                http_close(j->req);
                j->req = NULL;
                snprintf(j->ref, sizeof(j->ref), "main");
                j->from_commit = false;
                snprintf(j->url, sizeof(j->url), MAN_URL_FMT, j->ref);
                if (!install_start(sh, j)) {
                    files_release();
                    heap_release(HEAP_FILES, j);
                    return false;
                }
                return true;
            }
            if (status != 200) {
                shell_printf(sh, "man install: %s: HTTP %d\n", j->url, status);
                return install_end(sh, j, false);
            }
            shell_printf(sh, "downloading %s\n", j->url);
        }
        int n = http_read(j->req, j->buf, sizeof(j->buf));
        if (n < 0) {
            if (http_poll(j->req) != HTTP_DONE) {
                shell_printf(sh, "man install: %s: %s\n", j->url, http_error(j->req));
                return install_end(sh, j, false);
            }
            return install_end(sh, j, !j->failed);
        }
        if (n == 0)
            return true;                    // nothing more yet
        take(sh, j, j->buf, (size_t)n);
        if (j->failed)
            return install_end(sh, j, false);
    }
    return true;
}

static void install(shell_t *sh) {
    if (!sdcard_mount(sh))
        return;
    if (!files_write_allowed(sh, MAN_DIR)) {
        shell_printf(sh, "man install: %s can only be changed on the console\n", MAN_DIR);
        return;
    }
    install_job_t *j = heap_zalloc(HEAP_FILES, sizeof(*j));
    if (!j) {
        shell_printf(sh, "man install: out of memory\n");
        return;
    }
    j->from_commit = bundle_url(j->ref, sizeof(j->ref), j->url, sizeof(j->url));
    j->at_line_start = true;
    if (!install_start(sh, j)) {
        heap_release(HEAP_FILES, j);
        return;
    }
    files_hold();
    shell_run_job(sh, install_step, j);
}

// No pages on the card: offer the download.
static void on_install_answer(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (answer && (!strcasecmp(answer, "y") || !strcasecmp(answer, "yes")))
        install(sh);
    else
        shell_printf(sh, MAN_HOW_TO_INSTALL);
}

static void offer_install(shell_t *sh) {
    char url[128], ref[16];
    bundle_url(ref, sizeof(ref), url, sizeof(url));
    shell_printf(sh, "No manual pages on the card. Download them from GitHub?\n  %s\n", url);
    shell_ask(sh, "Download now? [y/N] ", false, on_install_answer, NULL);
}

// --- the listing ---------------------------------------------------------------------------------

typedef struct {
    DIR dir;
    bool dir_open;
    bool more;                              // more pages than fit the list
    unsigned count, next;
    char names[MAN_PAGES_MAX][MAN_TOPIC_MAX + 1];
    char head[MAN_HEAD_BYTES + 1];
    char desc[MAN_DESC_MAX * 2 + 8];        // filtered: a replaced byte may grow to '.'
    FIL f;
} list_job_t;

static int by_name(const void *a, const void *b) {
    return strcmp(a, b);
}

// The description from the start of a page: the first non-empty line after NAME, without its
// indentation and without the "<topic> - " that opens it. Filtered for the terminal.
static const char *describe(list_job_t *j, const char *name, size_t len) {
    j->head[len] = '\0';
    char *p = j->head;
    if (!strncmp(p, "NAME", 4)) {
        p = strchr(p, '\n');
        p = p ? p + 1 : j->head + len;
    }
    while (*p == ' ' || *p == '\n' || *p == '\r')
        p++;
    size_t nl = strlen(name);
    if (!strncmp(p, name, nl) && !strncmp(p + nl, " - ", 3))
        p += nl + 3;
    char *end = strchr(p, '\n');
    if (end)
        *end = '\0';
    while (*p == ' ')
        p++;
    size_t n = strlen(p);
    if (n > MAN_DESC_MAX)
        n = MAN_DESC_MAX;                   // may cut a UTF-8 character: the filter shows '.'
    termtext_t t = TERMTEXT_INIT;
    size_t o = termtext_filter(&t, (const uint8_t *)p, n, j->desc);
    o += termtext_flush(&t, j->desc + o);
    j->desc[o] = '\0';
    return j->desc;
}

static bool list_done(shell_t *sh, list_job_t *j) {
    (void)sh;
    if (j->dir_open)
        f_closedir(&j->dir);
    heap_release(HEAP_FILES, j);
    files_release();
    return false;
}

static bool list_step(shell_t *sh, void *arg, bool cancel) {
    list_job_t *j = arg;
    if (cancel)
        return list_done(sh, j);
    if (j->dir_open) {                      // phase 1: the names, 16 entries per pass
        for (int i = 0; i < 16; i++) {
            FILINFO fi;
            FRESULT fr = f_readdir(&j->dir, &fi);
            if (fr != FR_OK) {
                shell_printf(sh, "man: %s\n", files_error(fr));
                return list_done(sh, j);
            }
            if (!fi.fname[0]) {
                f_closedir(&j->dir);
                j->dir_open = false;
                if (!j->count) {            // (have_pages() saw some: gone meanwhile)
                    shell_printf(sh, MAN_HOW_TO_INSTALL);
                    return list_done(sh, j);
                }
                qsort(j->names, j->count, sizeof(j->names[0]), by_name);
                shell_printf(sh, "manual pages ('man <topic>' shows one; in it, q quits, h lists the keys):\n");
                return true;
            }
            // The same pages Tab offers: ".txt" in any case (FAT), a topic's name.
            char topic[MAN_TOPIC_MAX + 1];
            if ((fi.fattrib & AM_DIR) || !man_topic_of(fi.fname, topic, sizeof(topic)))
                continue;
            if (j->count == MAN_PAGES_MAX) {
                j->more = true;
                continue;
            }
            strcpy(j->names[j->count++], topic);
        }
        return true;
    }
    // Phase 2: a line per page.
    for (int i = 0; i < MAN_PER_PASS; i++) {
        if (shell_write_space(sh) < MAN_LIST_LINE)
            return true;                    // wait for the client to read
        if (j->next == j->count) {
            if (j->more)
                shell_printf(sh, "... and more: only %u are listed\n", MAN_PAGES_MAX);
            return list_done(sh, j);
        }
        const char *name = j->names[j->next++];
        char path[sizeof(MAN_DIR) + MAN_TOPIC_MAX + sizeof(MAN_EXT)];
        page_path(path, sizeof(path), name, MAN_EXT);
        UINT got = 0;
        if (f_open(&j->f, path, FA_READ) == FR_OK) {
            f_read(&j->f, j->head, MAN_HEAD_BYTES, &got);
            f_close(&j->f);
        }
        shell_printf(sh, "  %-*s %s\n", 12, name, describe(j, name, got));
    }
    return true;
}

static void list_pages(shell_t *sh) {
    if (!sdcard_mount(sh))
        return;
    if (!have_pages()) {
        offer_install(sh);
        return;
    }
    list_job_t *j = heap_zalloc(HEAP_FILES, sizeof(*j));
    if (!j) {
        shell_printf(sh, "man: out of memory\n");
        return;
    }
    FRESULT fr = f_opendir(&j->dir, MAN_DIR);
    if (fr != FR_OK) {
        shell_printf(sh, "man: %s: %s\n", MAN_DIR, files_error(fr));
        heap_release(HEAP_FILES, j);
        return;
    }
    j->dir_open = true;
    files_hold();
    shell_run_job(sh, list_step, j);
}

// --- a page --------------------------------------------------------------------------------------

static void show_page(shell_t *sh, const char *topic) {
    char path[sizeof(MAN_DIR) + MAN_TOPIC_MAX + sizeof(MAN_EXT)];
    page_path(path, sizeof(path), topic, MAN_EXT);
    if (!sdcard_mount(sh))
        return;
    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        if (!have_pages()) {
            offer_install(sh);
            return;
        }
        shell_printf(sh, "man: no page for '%s' ('man' lists the pages, 'help' the commands)\n",
                     topic);
        return;
    }
    if (fr != FR_OK) {
        shell_printf(sh, "man: %s: %s\n", path, files_error(fr));
        return;
    }
    if ((fi.fattrib & AM_DIR) || fi.fsize == 0) {
        shell_printf(sh, "man: %s: not a page\n", path);
        return;
    }
    char name[MAN_TOPIC_MAX + 8];
    snprintf(name, sizeof(name), "man %s", topic);
    edit_view_file(sh, path, name);
}

// Tab after `man`: `install`, and the topics of the pages on the card. They come through the
// file lister (files.c): only while the card is mounted, at most 256 entries.
typedef struct {
    shell_value_fn fn;
    void *arg;
} man_tab_t;

static void man_tab_entry(const char *name, bool is_dir, void *arg) {
    man_tab_t *t = arg;
    char topic[MAN_TOPIC_MAX + 1];
    if (!is_dir && man_topic_of(name, topic, sizeof(topic)))
        t->fn(topic, NULL, t->arg);
}

void man_tab_topics(shell_t *sh, shell_value_fn fn, void *arg) {
    fn("install", NULL, arg);
    man_tab_t t = {fn, arg};
    shell_list_dir(sh, MAN_DIR, man_tab_entry, &t);
}

void man_command(shell_t *sh, char *args) {
    if (!args || !*args) {
        list_pages(sh);
        return;
    }
    if (!strcmp(args, "install")) {
        install(sh);
        return;
    }
    if (strchr(args, ' ') || !man_topic_ok(args)) {
        shell_printf(sh, "usage: man [<topic>]   a topic is a command name, like 'man wifi'\n"
                         "       man install     download the pages from GitHub\n");
        return;
    }
    show_page(sh, args);
}
