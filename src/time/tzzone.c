// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "time/tzzone.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "core/heaptag.h"
#include "core/log.h"
#include "core/settings.h"
#include "files/files.h"
#include "files/sdcard.h"
#include "net/http.h"
#include "time/timeconv.h"
#include "time/timesync.h"
#include "time/tzbundle.h"
#include "time/tzrule.h"

#define TZZONE_NEW      TZZONE_DIR "/zones.~new"    // a bundle being downloaded
#define TZZONE_OLD      TZZONE_DIR "/zones.~old"    // the one it replaces, until it has
// Always main's: a tzdata update doesn't depend on the firmware, so a board takes it without
// being reflashed.
#define TZZONE_URL      "https://raw.githubusercontent.com/j3m7/sos/main/tz/zones.bundle"
#define TZZONE_SIZE_MAX (64 * 1024)                 // the bundle is about 17 KB
#define TZZONE_STEP_US  20000                       // most of a main-loop pass a job takes
#define TZZONE_LIST_LINE (TZBUNDLE_LINE_MAX + 8)    // a listing line: "  name  rule\r\n"

static bool read_fil(void *ctx, char *buf, size_t cap) {
    return f_gets(buf, (int)cap, ctx) != NULL;
}

// Look name up in the bundle at path (the card mounted). FR_OK unless the file can't be opened.
static FRESULT look_up(const char *path, const char *name, tzbundle_result_t *res,
                       tzbundle_find_t *out) {
    FIL f;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK)
        return fr;
    *res = tzbundle_find(read_fil, &f, name, out);
    f_close(&f);
    return FR_OK;
}

static int64_t now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

void tzzone_print_local(shell_t *sh, int64_t t) {
    const sos_config_t *c = settings();
    if (!c->tz_name[0])
        return;
    tzrule_t r;
    const char *why = "";
    if (!tzrule_parse(c->tz_rule, &r, &why)) {
        shell_printf(sh, "local:  the saved rule for %s isn't usable (%s): 'time zone %s' again\n",
                     c->tz_name, why, c->tz_name);
        return;
    }
    tzrule_local_t l = tzrule_at(&r, t);
    char when[24], off[10];
    timeconv_format(t + l.offset, when, sizeof(when));
    tzrule_format_offset(l.offset, off, sizeof(off));
    shell_printf(sh, "local:  %s %s (UTC%s, %s)\n", when, l.abbr, off, c->tz_name);
}

static void print_local_now(shell_t *sh) {
    if (timesync_clock_set())
        tzzone_print_local(sh, now_s());
    else
        shell_printf(sh, "local:  the clock isn't set ('time sync' sets it)\n");
}

// --- showing and setting ------------------------------------------------------------------------

static void show(shell_t *sh) {
    const sos_config_t *c = settings();
    if (c->tz_name[0]) {
        shell_printf(sh, "zone:   %s\nrule:   %s\n", c->tz_name, c->tz_rule);
        print_local_now(sh);
    } else {
        shell_printf(sh, "zone:   UTC (none set; 'time zone list' lists them, "
                         "'time zone <name>' sets one)\n");
    }
    tzbundle_find_t f;
    tzbundle_result_t res = TZBUNDLE_NOT_A_BUNDLE;
    if (!sdcard_mount(NULL)) {
        shell_printf(sh, "bundle: no card to read it from\n");
        return;
    }
    if (look_up(TZZONE_BUNDLE, c->tz_name[0] ? c->tz_name : "UTC", &res, &f) != FR_OK) {
        shell_printf(sh, "bundle: none on the card ('time zone install' downloads it)\n");
        return;
    }
    if (res == TZBUNDLE_NOT_A_BUNDLE) {
        shell_printf(sh, "bundle: " TZZONE_BUNDLE " isn't one ('time zone install' replaces it)\n");
        return;
    }
    shell_printf(sh, "bundle: tzdata %s on the card\n", f.version);
    if (!c->tz_name[0])
        return;
    if (res == TZBUNDLE_NOT_FOUND)
        shell_printf(sh, "        it has no zone %s; the saved rule stays in use\n", c->tz_name);
    else if (strcmp(f.rule, c->tz_rule))
        shell_printf(sh, "        its rule for %s is %s: the saved one may be stale, and "
                         "'time zone %s' takes the card's\n", c->tz_name, f.rule, c->tz_name);
}

static void set(shell_t *sh, const char *name) {
    if (!shell_require_admin(sh))
        return;
    if (!sdcard_mount(sh)) {
        shell_printf(sh, "not changed: the zones are read from the card\n");
        return;
    }
    tzbundle_find_t f;
    tzbundle_result_t res = TZBUNDLE_NOT_A_BUNDLE;
    if (look_up(TZZONE_BUNDLE, name, &res, &f) != FR_OK) {
        shell_printf(sh, "not changed: no zone bundle on the card ('time zone install' downloads "
                         "it, or copy it over SFTP: tools/tz-install.sh <host>)\n");
        return;
    }
    if (res == TZBUNDLE_NOT_A_BUNDLE) {
        shell_printf(sh, "not changed: " TZZONE_BUNDLE " isn't a zone bundle "
                         "('time zone install' replaces it)\n");
        return;
    }
    if (res == TZBUNDLE_NOT_FOUND) {
        shell_printf(sh, "not changed: no zone %s in the card's bundle (tzdata %s)\n", name,
                     f.version);
        if (f.near_count) {
            shell_printf(sh, "did you mean");
            for (unsigned i = 0; i < f.near_count; i++)
                shell_printf(sh, "%s %s", i ? "," : "", f.near[i]);
            shell_printf(sh, "?\n");
        } else {
            shell_printf(sh, "'time zone list [<prefix>]' lists them\n");
        }
        return;
    }
    sos_config_t *c = settings();
    snprintf(c->tz_name, sizeof(c->tz_name), "%s", f.name);
    snprintf(c->tz_rule, sizeof(c->tz_rule), "%s", f.rule);
    shell_save_settings(sh);
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: time zone %s (%s, tzdata %s)", c->tz_name,
            c->tz_rule, f.version);
    shell_printf(sh, "zone:   %s\nrule:   %s (tzdata %s)\n", c->tz_name, c->tz_rule, f.version);
    print_local_now(sh);
}

static void clear(shell_t *sh) {
    if (!shell_require_admin(sh))
        return;
    sos_config_t *c = settings();
    c->tz_name[0] = '\0';
    c->tz_rule[0] = '\0';
    shell_save_settings(sh);
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: time zone UTC");
    shell_printf(sh, "zone:   UTC\n");
}

// --- the listing --------------------------------------------------------------------------------

typedef struct {
    FIL f;
    tzbundle_lines_t lines;
    char prefix[TZBUNDLE_NAME_MAX + 1];
    char line[TZBUNDLE_READ_MAX];
    unsigned shown;
} list_job_t;

static bool list_end(list_job_t *j) {
    f_close(&j->f);
    heap_release(HEAP_FILES, j);
    files_release();
    return false;
}

static bool list_step(shell_t *sh, void *arg, bool cancel) {
    list_job_t *j = arg;
    if (cancel)
        return list_end(j);
    for (int i = 0; i < 16; i++) {
        if (shell_write_space(sh) < TZZONE_LIST_LINE)
            return true;                    // wait for the client to read
        bool too_long;
        if (!tzbundle_next_line(&j->lines, j->line, &too_long)) {
            if (!j->shown)
                shell_printf(sh, "no zone starts with %s\n", j->prefix);
            return list_end(j);
        }
        char *name, *rule;
        if (!too_long && tzbundle_split(j->line, &name, &rule) &&
            tzbundle_has_prefix(name, j->prefix)) {
            shell_printf(sh, "  %-32s %s\n", name, rule);
            j->shown++;
        }
    }
    return true;
}

static void list(shell_t *sh, const char *prefix) {
    if (strlen(prefix) > TZBUNDLE_NAME_MAX) {
        shell_printf(sh, "no zone starts with that\n");
        return;
    }
    if (!sdcard_mount(sh))
        return;
    list_job_t *j = heap_zalloc(HEAP_FILES, sizeof(*j));
    if (!j) {
        shell_printf(sh, "time zone: out of memory\n");
        return;
    }
    strcpy(j->prefix, prefix);
    FRESULT fr = f_open(&j->f, TZZONE_BUNDLE, FA_READ);
    char version[TZBUNDLE_VERSION_MAX + 1];
    bool too_long = false;
    j->lines = (tzbundle_lines_t){.read = read_fil, .ctx = &j->f};
    if (fr != FR_OK) {
        shell_printf(sh, "no zone bundle on the card ('time zone install' downloads it)\n");
    } else if (!tzbundle_next_line(&j->lines, j->line, &too_long) || too_long ||
               !tzbundle_version(j->line, version)) {
        shell_printf(sh, TZZONE_BUNDLE " isn't a zone bundle ('time zone install' replaces it)\n");
        f_close(&j->f);
    } else {
        shell_printf(sh, "zones in tzdata %s ('time zone <name>' sets one):\n", version);
        files_hold();
        shell_run_job(sh, list_step, j);
        return;
    }
    heap_release(HEAP_FILES, j);
}

// --- installing ---------------------------------------------------------------------------------

typedef struct {
    http_req_t *req;
    bool shown;                             // the status line was handled
    FIL f;
    bool f_open;
    size_t got;
    char buf[1024];
} install_job_t;

static bool install_end(install_job_t *j) {
    if (j->f_open) {
        f_close(&j->f);
        f_unlink(TZZONE_NEW);               // cut short: the bundle before it stays
    }
    http_close(j->req);
    heap_release(HEAP_FILES, j);
    files_release();
    return false;
}

// The download is complete in TZZONE_NEW: check it is a bundle, every line of it a zone, and put
// it in place. Then say what it means for the zone set.
static bool install_done(shell_t *sh, install_job_t *j) {
    FRESULT fr = f_close(&j->f);
    j->f_open = false;
    if (fr != FR_OK) {
        shell_printf(sh, "time zone install: " TZZONE_NEW ": %s\n", files_error(fr));
        f_unlink(TZZONE_NEW);
        return install_end(j);
    }
    tzbundle_find_t f;
    tzbundle_result_t res = TZBUNDLE_NOT_A_BUNDLE;
    fr = look_up(TZZONE_NEW, "", &res, &f);         // no zone is called "": reads it all
    if (fr != FR_OK || res == TZBUNDLE_NOT_A_BUNDLE || f.skipped || !f.zones) {
        if (fr != FR_OK)
            shell_printf(sh, "time zone install: " TZZONE_NEW ": %s\n", files_error(fr));
        else
            shell_printf(sh, "time zone install: what came isn't a zone bundle (%u zones, %u "
                             "other lines); the card's is unchanged\n", f.zones, f.skipped);
        f_unlink(TZZONE_NEW);
        return install_end(j);
    }
    // The old bundle is moved aside, not removed, until the new one is in its place: a failure
    // in between puts it back, so the card always has one.
    f_unlink(TZZONE_OLD);                           // left by an install cut short
    fr = f_rename(TZZONE_BUNDLE, TZZONE_OLD);
    bool had_old = fr == FR_OK;
    if (fr == FR_OK || fr == FR_NO_FILE)
        fr = f_rename(TZZONE_NEW, TZZONE_BUNDLE);
    if (fr != FR_OK) {
        shell_printf(sh, "time zone install: " TZZONE_BUNDLE ": %s\n", files_error(fr));
        f_unlink(TZZONE_NEW);
        if (had_old && f_rename(TZZONE_OLD, TZZONE_BUNDLE) != FR_OK)
            shell_printf(sh, "the old bundle is in " TZZONE_OLD "; run install again\n");
        return install_end(j);
    }
    if (had_old)
        f_unlink(TZZONE_OLD);
    shell_printf(sh, "time zone install: tzdata %s, %u zones, in " TZZONE_BUNDLE "\n", f.version,
                 f.zones);
    const sos_config_t *c = settings();
    if (!c->tz_name[0]) {
        shell_printf(sh, "'time zone list' lists them, 'time zone <name>' sets one\n");
    } else if (look_up(TZZONE_BUNDLE, c->tz_name, &res, &f) != FR_OK ||
               res == TZBUNDLE_NOT_A_BUNDLE) {
        shell_printf(sh, "couldn't read it back to check %s's rule\n", c->tz_name);
    } else if (res == TZBUNDLE_NOT_FOUND) {
        shell_printf(sh, "it has no zone %s; the saved rule stays in use\n", c->tz_name);
    } else if (!strcmp(f.rule, c->tz_rule)) {
        shell_printf(sh, "the rule for %s is unchanged\n", c->tz_name);
    } else {
        shell_printf(sh, "the rule for %s is now %s (saved: %s); 'time zone %s' on the console "
                         "takes it\n", c->tz_name, f.rule, c->tz_rule, c->tz_name);
    }
    return install_end(j);
}

static bool install_step(shell_t *sh, void *arg, bool cancel) {
    install_job_t *j = arg;
    if (cancel)
        return install_end(j);
    uint64_t t0 = time_us_64();
    while (time_us_64() - t0 < TZZONE_STEP_US) {
        http_state_t s = http_poll(j->req);
        if (s == HTTP_FAILED) {
            shell_printf(sh, "time zone install: %s\n", http_error(j->req));
            return install_end(j);
        }
        if (s < HTTP_BODY)
            return true;
        if (!j->shown) {
            j->shown = true;
            int status = http_response(j->req)->status;
            if (status != 200) {
                shell_printf(sh, "time zone install: " TZZONE_URL ": HTTP %d\n", status);
                return install_end(j);
            }
            FRESULT fr = f_mkdir(TZZONE_DIR);
            if (fr == FR_OK || fr == FR_EXIST)
                fr = f_open(&j->f, TZZONE_NEW, FA_WRITE | FA_CREATE_ALWAYS);
            if (fr != FR_OK) {
                shell_printf(sh, "time zone install: " TZZONE_NEW ": %s\n", files_error(fr));
                return install_end(j);
            }
            j->f_open = true;
            shell_printf(sh, "downloading " TZZONE_URL "\n");
        }
        int n = http_read(j->req, j->buf, sizeof(j->buf));
        if (n < 0) {
            if (http_poll(j->req) != HTTP_DONE) {
                shell_printf(sh, "time zone install: %s\n", http_error(j->req));
                return install_end(j);
            }
            return install_done(sh, j);
        }
        if (n == 0)
            return true;                    // nothing more yet
        j->got += (size_t)n;
        if (j->got > TZZONE_SIZE_MAX) {
            shell_printf(sh, "time zone install: over %u KB: not a zone bundle\n",
                         TZZONE_SIZE_MAX / 1024);
            return install_end(j);
        }
        UINT wrote = 0;
        FRESULT fr = f_write(&j->f, j->buf, (UINT)n, &wrote);
        if (fr != FR_OK || wrote != (UINT)n) {
            shell_printf(sh, "time zone install: " TZZONE_NEW ": %s\n",
                         fr != FR_OK ? files_error(fr) : "card full");
            return install_end(j);
        }
    }
    return true;
}

static void install(shell_t *sh) {
    if (!sdcard_mount(sh))
        return;
    install_job_t *j = heap_zalloc(HEAP_FILES, sizeof(*j));
    if (!j) {
        shell_printf(sh, "time zone install: out of memory\n");
        return;
    }
    http_opts_t o = {.method = "GET", .url = TZZONE_URL};
    const char *err = "";
    j->req = http_start(&o, &err);
    if (!j->req) {
        shell_printf(sh, "time zone install: %s\n", err);
        heap_release(HEAP_FILES, j);
        return;
    }
    files_hold();
    shell_run_job(sh, install_step, j);
}

// --- Tab ----------------------------------------------------------------------------------------

typedef struct {
    shell_value_fn fn;
    void *arg;
} tab_t;

static void tab_name(const char *name, void *arg) {
    tab_t *t = arg;
    t->fn(name, NULL, t->arg);
}

void tzzone_tab(shell_t *sh, shell_value_fn fn, void *arg) {
    fn("list", NULL, arg);
    fn("install", NULL, arg);
    if (!sh->io->admin)                     // setting a zone is console only
        return;
    fn("utc", NULL, arg);
    FIL f;
    if (!sdcard_mounted() || f_open(&f, TZZONE_BUNDLE, FA_READ) != FR_OK)
        return;
    tab_t t = {fn, arg};
    tzbundle_names(read_fil, &f, tab_name, &t);
    f_close(&f);
}

// --- the command --------------------------------------------------------------------------------

// Is s the word w, alone or followed by a space?
static const char *word(const char *s, const char *w) {
    size_t n = strlen(w);
    if (strncmp(s, w, n) || (s[n] && s[n] != ' '))
        return NULL;
    s += n;
    while (*s == ' ')
        s++;
    return s;
}

void tzzone_command(shell_t *sh, const char *args) {
    while (*args == ' ')
        args++;
    const char *list_prefix = word(args, "list");
    const char *after_install = word(args, "install");
    if (!*args)
        show(sh);
    else if (list_prefix)
        list(sh, list_prefix);
    else if (after_install && !*after_install)
        install(sh);
    else if (!strcasecmp(args, "utc"))
        clear(sh);
    else if (strlen(args) > TZBUNDLE_NAME_MAX)
        shell_printf(sh, "not changed: no zone has a name that long\n");
    else
        set(sh, args);
}
