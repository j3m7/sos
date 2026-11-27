// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Shell line editing and output. Depends only on shell.h, sos_version and the C library, so it
// builds on the host for unit tests (which define sos_version); the commands live in shell.c
// (shell_execute).

#include "shell/shell.h"
#include "core/version.h"
#include "shell/charwidth.h"
#include "shell/complete.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// --- output --------------------------------------------------------------------

static void write_out(shell_t *sh, const char *buf, size_t len) {
    if (len)
        sh->at_line_start = buf[len - 1] == '\n';
    if (!sh->io->crlf) {
        sh->io->write(sh->ctx, buf, (int)len);
        return;
    }
    // Translate "\n" to "\r\n" through a buffer, so the channel gets a few large writes (on
    // SSH, each write can become an encrypted packet) rather than one per line and per "\r\n".
    char out[256];
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (n + 2 > sizeof(out)) {
            sh->io->write(sh->ctx, out, (int)n);
            n = 0;
        }
        if (buf[i] == '\n')
            out[n++] = '\r';
        out[n++] = buf[i];
    }
    if (n)
        sh->io->write(sh->ctx, out, (int)n);
}

void shell_write(shell_t *sh, const char *buf, size_t len) {
    write_out(sh, buf, len);
}

void shell_printf(shell_t *sh, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = sizeof(buf) - 1;    // truncated; callers keep individual lines short
    write_out(sh, buf, (size_t)n);
}

static void prompt(shell_t *sh) {
    shell_printf(sh, "%s", sh->answer ? sh->ask_prompt : SHELL_PROMPT);
}

void shell_ask(shell_t *sh, const char *ask_prompt, bool secret, shell_answer_fn answer, void *arg) {
    sh->ask_prompt = ask_prompt;
    sh->secret = secret;
    sh->ask_eof = false;
    sh->answer = answer;
    sh->answer_arg = arg;
}

char shell_eof[] = "";

void shell_ask_eof(shell_t *sh, const char *ask_prompt, shell_answer_fn answer, void *arg) {
    shell_ask(sh, ask_prompt, false, answer, arg);
    sh->ask_eof = true;
}

static void wipe(void *p, size_t n) {
    volatile char *v = p;
    for (size_t i = 0; i < n; i++)
        v[i] = 0;
}

void shell_clear_history(shell_t *sh) {
    wipe(sh->history, sizeof(sh->history));
    wipe(sh->draft, sizeof(sh->draft));
    sh->history_len = 0;
    sh->history_pos = -1;
    sh->draft_len = 0;
}

void shell_discard_input(shell_t *sh) {
    wipe(sh->line, sizeof(sh->line));
    wipe(sh->ahead, sizeof(sh->ahead));
    shell_clear_history(sh);
    sh->line_len = 0;
    sh->ahead_len = 0;
    sh->last_was_cr = false;
    sh->keys = (keys_t)KEYS_INIT;
    sh->tab_pressed = false;
}

void shell_cancel_question(shell_t *sh) {
    if (sh->secret) {                   // a half-typed secret must not become a command
        wipe(sh->line, sizeof(sh->line));
        sh->line_len = 0;
    }
    sh->answer = NULL;
    sh->secret = false;
    sh->ask_eof = false;
}

// Take the pending question, if any, and hand it the answer (NULL = cancelled).
static bool answer_question(shell_t *sh, char *answer) {
    shell_answer_fn fn = sh->answer;
    if (!fn)
        return false;
    void *arg = sh->answer_arg;
    sh->answer = NULL;                  // cleared first: the callback may ask the next question
    sh->secret = false;
    sh->ask_eof = false;
    fn(sh, answer, arg);
    return true;
}

void shell_hide_input(shell_t *sh) {
    if (!sh->io || shell_raw_mode(sh))          // full-screen program: leave its screen alone
        return;
    if (sh->job) {                              // job output on screen: don't erase it
        if (!sh->at_line_start)
            write_out(sh, "\n", 1);
        return;
    }
    sh->io->write(sh->ctx, "\r\x1b[K", 4);    // to column 0, erase the line
}

void shell_show_input(shell_t *sh) {
    if (!sh->io || sh->job)                     // no prompt while a job runs
        return;
    prompt(sh);
    if (!sh->secret && sh->line_len)
        sh->io->write(sh->ctx, sh->line, (int)sh->line_len);
}

void shell_logout(shell_t *sh) {
    if (sh->io->logout)
        sh->io->logout(sh->ctx);
    else
        shell_printf(sh, "logout: the %s session can't be ended\n", sh->io->name);
}

// --- command history (Up/Down) ----------------------------------------------------------------

// Remember a command, unless it repeats the newest one. The oldest entries make room.
static void history_add(shell_t *sh, const char *cmd) {
    size_t n = strlen(cmd) + 1;
    if (n > sizeof(sh->history))
        return;                         // can't happen: a line is shorter than the history
    if (sh->history_len) {              // the newest entry starts after the '\0' before its own
        size_t start = sh->history_len - 1;
        while (start > 0 && sh->history[start - 1] != '\0')
            start--;
        if (!strcmp(sh->history + start, cmd))
            return;
    }
    size_t drop = 0;
    while (sh->history_len - drop + n > sizeof(sh->history))
        drop += strlen(sh->history + drop) + 1;
    if (drop) {
        memmove(sh->history, sh->history + drop, sh->history_len - drop);
        sh->history_len -= drop;
        wipe(sh->history + sh->history_len, drop);
    }
    memcpy(sh->history + sh->history_len, cmd, n);
    sh->history_len += n;
}

static int history_count(const shell_t *sh) {
    int count = 0;
    for (size_t i = 0; i < sh->history_len; i++)
        count += sh->history[i] == '\0';
    return count;
}

// The entry `back` places before the newest (0 = the newest).
static const char *history_entry(const shell_t *sh, int back) {
    int index = history_count(sh) - 1 - back;
    const char *p = sh->history;
    while (index-- > 0)
        p += strlen(p) + 1;
    return p;
}

// The line is being changed by hand (or entered): a recalled line becomes ordinary input, and
// the next Up starts again from the newest command.
static void history_edited(shell_t *sh) {
    if (sh->history_pos < 0)
        return;
    sh->history_pos = -1;
    wipe(sh->draft, sh->draft_len);
    sh->draft_len = 0;
}

// Put text in place of the line being typed, and redraw it.
static void line_replace(shell_t *sh, const char *text, size_t len) {
    shell_hide_input(sh);
    memcpy(sh->line, text, len);
    sh->line_len = len;
    shell_show_input(sh);
}

// Up: the previous command (the line being typed is kept for Down). Down: the next one, and
// after the newest, the line that was being typed.
static void history_key(shell_t *sh, int key) {
    if (sh->answer)                     // a question: its answers aren't commands
        return;
    if (key == KEY_UP) {
        if (sh->history_pos + 1 >= history_count(sh))
            return;
        if (sh->history_pos < 0) {
            memcpy(sh->draft, sh->line, sh->line_len);
            sh->draft_len = sh->line_len;
        }
        sh->history_pos++;
        const char *e = history_entry(sh, sh->history_pos);
        line_replace(sh, e, strlen(e));
    } else if (key == KEY_DOWN) {
        if (sh->history_pos < 0)
            return;
        sh->history_pos--;
        if (sh->history_pos < 0) {
            line_replace(sh, sh->draft, sh->draft_len);
            wipe(sh->draft, sh->draft_len);
            sh->draft_len = 0;
        } else {
            const char *e = history_entry(sh, sh->history_pos);
            line_replace(sh, e, strlen(e));
        }
    }
}

// Trim spaces and hand non-empty commands to the command module.
static void run_line(shell_t *sh, char *cmd) {
    while (*cmd == ' ')
        cmd++;
    char *end = cmd + strlen(cmd);
    while (end > cmd && end[-1] == ' ')
        *--end = '\0';
    if (*cmd) {
        history_add(sh, cmd);
        shell_execute(sh, cmd);
    }
}

// --- input ---------------------------------------------------------------------

void shell_bind(shell_t *sh, const shell_io_t *io, void *ctx) {
    sh->io = io;
    sh->ctx = ctx;
    sh->line_len = 0;
    sh->last_was_cr = false;
    sh->keys = (keys_t)KEYS_INIT;
    sh->tab_pressed = false;
    sh->logger_quota = (log_quota_t){0};    // a reused SSH slot starts with its own count
    sh->answer = NULL;
    sh->secret = false;
    sh->ask_eof = false;
    sh->job = NULL;             // a job of an earlier session was cancelled by shell_end()
    sh->job_arg = NULL;
    sh->key = NULL;
    sh->ahead_len = 0;
    strcpy(sh->cwd, "/");
    shell_clear_history(sh);    // a new session doesn't see the last one's commands
}

void shell_greet(shell_t *sh) {
    shell_printf(sh, "\nSOS %s\nType 'help' for commands.\n", sos_version);
}

void shell_init(shell_t *sh, const shell_io_t *io, void *ctx) {
    shell_bind(sh, io, ctx);
    shell_greet(sh);
    prompt(sh);
}

void shell_run_job(shell_t *sh, shell_job_fn step, void *arg) {
    sh->job = step;
    sh->job_arg = arg;
    sh->key = NULL;
}

void shell_run_raw(shell_t *sh, shell_job_fn step, shell_key_fn key, void *arg) {
    shell_run_job(sh, step, arg);
    sh->key = key;
}

bool shell_job_running(const shell_t *sh) {
    return sh->job != NULL;
}

bool shell_raw_mode(const shell_t *sh) {
    return sh->job != NULL && sh->key != NULL;
}

bool shell_busy(const shell_t *sh) {
    return sh->job != NULL || sh->ahead_len > 0;
}

int shell_write_space(shell_t *sh) {
    if (!sh->io->write_space)
        return 1024;
    int n = sh->io->write_space(sh->ctx);
    return n > 0 ? n : 0;
}

void shell_end(shell_t *sh) {
    if (sh->job) {
        shell_job_fn step = sh->job;
        sh->job = NULL;
        sh->key = NULL;
        step(sh, sh->job_arg, true);
    }
    sh->answer = NULL;
    shell_clear_history(sh);    // the session's commands don't wait in RAM for the next one
}

// While a job runs: only Ctrl-C counts; then one step.
// Keep input that arrives while a job runs, to process once it's done. Beyond the buffer, input
// stays unread in the channel (an SSH client then waits).
static void stash(shell_t *sh, const char *buf, size_t n) {
    size_t room = sizeof(sh->ahead) - sh->ahead_len;
    if (n > room)
        n = room;               // only when a single read brought more than the buffer holds
    memcpy(sh->ahead + sh->ahead_len, buf, n);
    sh->ahead_len += n;
}

// --- UTF-8 in the line ----------------------------------------------------------------------------
// The line holds UTF-8 (a Wi-Fi network named "Mjölnir", say). A character's bytes arrive one by
// one; a byte that can't continue or start a character is dropped, as are control bytes.

// How many bytes a character with this lead byte has: 1 for ASCII, 2 to 4, 0 if it can't lead.
static unsigned utf8_len(unsigned char c) {
    if (c < 0x80)
        return 1;
    if (c >= 0xc2 && c <= 0xdf)
        return 2;
    if (c >= 0xe0 && c <= 0xef)
        return 3;
    if (c >= 0xf0 && c <= 0xf4)
        return 4;
    return 0;                           // a continuation byte, or never in UTF-8
}

// Where the line's last character starts; how many bytes it should have goes to *want.
static size_t last_char(const shell_t *sh, unsigned *want) {
    size_t i = sh->line_len;
    while (i > 0 && ((unsigned char)sh->line[i - 1] & 0xc0) == 0x80 && sh->line_len - i < 3)
        i--;
    if (i == 0) {                       // only continuation bytes: can't happen, but be safe
        *want = 0;
        return 0;
    }
    i--;
    *want = utf8_len((unsigned char)sh->line[i]);
    return i;
}

// Is the line's last character still missing bytes?
static bool last_char_partial(const shell_t *sh) {
    if (!sh->line_len)
        return false;
    unsigned want;
    size_t start = last_char(sh, &want);
    return want > sh->line_len - start;
}

// Take a byte of a printable character into the line, echoing it unless secret.
static void line_add(shell_t *sh, unsigned char c) {
    unsigned need = utf8_len(c);
    if ((c & 0xc0) == 0x80) {           // continuation: only of a character under way
        if (!last_char_partial(sh))
            return;
    } else if (need == 0) {             // 0xc0, 0xc1, 0xf5-0xff: never in UTF-8
        return;
    } else {
        if (last_char_partial(sh)) {    // a new character before the last one ended: drop that
            unsigned want;
            sh->line_len = last_char(sh, &want);
        }
        if (sh->line_len + need > SHELL_LINE_MAX)   // the whole character must fit
            return;
    }
    history_edited(sh);
    sh->line[sh->line_len++] = (char)c;
    if (!sh->secret)
        write_out(sh, (const char *)&c, 1);
}

// Backspace: remove the line's last character, and as many columns of it on screen.
static void line_backspace(shell_t *sh) {
    if (!sh->line_len)
        return;
    history_edited(sh);
    unsigned want;
    size_t start = last_char(sh, &want);
    size_t len = sh->line_len - start;
    const unsigned char *p = (const unsigned char *)sh->line + start;
    int width = 1;
    if (want >= 2 && len < want)        // cut short: the terminal showed nothing yet
        width = 0;
    else if (want >= 2) {
        uint32_t cp = p[0] & (0x7f >> want);
        for (size_t i = 1; i < len; i++)
            cp = cp << 6 | (p[i] & 0x3f);
        width = charwidth(cp);
    }
    sh->line_len = start;
    if (!sh->secret)
        for (int i = 0; i < width; i++)
            shell_printf(sh, "\b \b");
}

// --- Tab completion ------------------------------------------------------------------------------
// Tab completes the word at the end of the line: a command, a word that may follow it, or a
// file name on the SD card (complete.h). One match: the rest of it, then a space, or '/' for a
// directory. Several: as far as they agree; when they agree no further, the terminal's bell,
// and a second Tab lists them.

#define TAB_LIST_COLUMNS 80     // the terminal's width isn't known: list for the narrowest

// Columns `text` takes on the terminal (it's valid UTF-8: complete.h).
static size_t text_columns(const char *text) {
    size_t cols = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned n = utf8_len(*p);
        if (n <= 1) {
            cols++;
            p++;
            continue;
        }
        uint32_t cp = p[0] & (0x7f >> n);
        for (unsigned i = 1; i < n && p[i]; i++)
            cp = cp << 6 | (p[i] & 0x3f);
        cols += (size_t)charwidth(cp);
        p += n;
    }
    return cols;
}

typedef struct {
    char first[SHELL_LINE_MAX + 1];     // the first match: a copy (a file name's buffer is reused)
    bool have;
    char first_end;             // what follows it: ' ' or '/'
    size_t common;              // bytes all matches have in common
    size_t widest;              // columns of the widest, with its '/'
} tab_matches_t;

static void tab_collect(const char *word, char end, void *arg) {
    tab_matches_t *m = arg;
    size_t n = strlen(word);
    if (!m->have) {
        memcpy(m->first, word, n + 1);  // fits: complete.h offers no longer word
        m->have = true;
        m->first_end = end;
        m->common = n;
    } else {
        size_t i = 0;
        while (i < m->common && word[i] == m->first[i])
            i++;
        m->common = i;
    }
    size_t cols = text_columns(word) + (end == '/');
    if (cols > m->widest)
        m->widest = cols;
}

typedef struct {
    shell_t *sh;
    size_t width, per_row, in_row, last;   // `last`: columns of the match printed before
} tab_list_t;

// One match of the list, in columns `width` wide; no spaces at the end of a row.
static void tab_print(const char *word, char end, void *arg) {
    tab_list_t *l = arg;
    if (l->in_row == l->per_row) {
        write_out(l->sh, "\n", 1);
        l->in_row = 0;
    } else if (l->in_row) {
        shell_printf(l->sh, "%*s", (int)(l->width - l->last), "");
    }
    l->in_row++;
    write_out(l->sh, word, strlen(word));
    if (end == '/')
        write_out(l->sh, "/", 1);
    l->last = text_columns(word) + (end == '/');
}

static void line_tab(shell_t *sh, bool again) {
    const shell_cmd_t *const *tables = shell_command_tables(sh);
    if (!tables)
        return;
    complete_src_t src = {tables, sh->io->admin, shell_list_dir, sh};
    size_t start;
    bool partial;
    tab_matches_t m = {0};
    size_t n = complete_words(&src, sh->line, sh->line_len, &start, tab_collect, &m, &partial);
    size_t typed = sh->line_len - start;
    if (n == 0) {
        write_out(sh, "\a", 1);
        return;
    }
    // Two names can share the first bytes of different characters: stop before those.
    while (m.common > typed && ((unsigned char)m.first[m.common] & 0xc0) == 0x80)
        m.common--;
    bool whole = n == 1 && !partial;    // a directory cut short may hold more matches
    if (m.common > typed || whole) {
        size_t add = m.common - typed + whole;       // a single match ends with ' ' or '/'
        if (sh->line_len + add > SHELL_LINE_MAX) {
            write_out(sh, "\a", 1);
            return;
        }
        history_edited(sh);
        memcpy(sh->line + sh->line_len, m.first + typed, m.common - typed);
        if (whole)
            sh->line[sh->line_len + add - 1] = m.first_end;
        write_out(sh, sh->line + sh->line_len, add);
        sh->line_len += add;
        return;
    }
    if (!again) {
        sh->tab_pressed = true;
        write_out(sh, "\a", 1);
        return;
    }
    tab_list_t l = {.sh = sh, .width = m.widest + 2};
    l.per_row = TAB_LIST_COLUMNS / l.width;
    if (!l.per_row)
        l.per_row = 1;
    write_out(sh, "\n", 1);
    complete_words(&src, sh->line, sh->line_len, &start, tab_print, &l, &partial);
    if (partial)
        shell_printf(sh, "\n(more: the directory is too large to list here)");
    write_out(sh, "\n", 1);
    prompt(sh);
    write_out(sh, sh->line, sh->line_len);
}

// Line editing for `n` input bytes. Stops early (stashing the rest) if a command starts a job.
static void process_input(shell_t *sh, const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n' && sh->last_was_cr) {      // swallow LF of a CRLF pair
            sh->last_was_cr = false;
            continue;
        }
        sh->last_was_cr = (c == '\r');

        int k = keys_feed(&sh->keys, (unsigned char)c);
        bool tab_again = sh->tab_pressed;       // any other key in between starts over
        sh->tab_pressed = false;
        if (k == '\t') {
            if (!sh->answer)                    // questions (passwords) have nothing to complete
                line_tab(sh, tab_again);
            continue;
        }
        if (k == KEY_UP || k == KEY_DOWN) {
            history_key(sh, k);
            continue;
        }
        if (k == KEYS_NONE || k > 0xff)         // other special keys: no editing with them (yet)
            continue;
        if (c == '\r' || c == '\n') {
            history_edited(sh);
            shell_printf(sh, "\n");
            if (last_char_partial(sh)) {        // a character cut short: not part of the line
                unsigned want;
                sh->line_len = last_char(sh, &want);
            }
            sh->line[sh->line_len] = '\0';
            sh->line_len = 0;
            if (!answer_question(sh, sh->line))
                run_line(sh, sh->line);
            if (sh->job) {  // the command started a job: keep the rest for afterwards
                stash(sh, buf + i + 1, n - (i + 1));
                return;
            }
            prompt(sh);
        } else if (c == 0x7f || c == '\b') {
            line_backspace(sh);
        } else if (c == 0x03) {                  // Ctrl-C: discard line, cancel a question
            history_edited(sh);
            shell_printf(sh, "^C\n");
            sh->line_len = 0;
            answer_question(sh, NULL);
            prompt(sh);
        } else if (c == 0x04) {                  // Ctrl-D on an empty line: logout
            if (sh->line_len == 0) {
                shell_printf(sh, "\n");
                if (sh->answer && sh->ask_eof) {  // ...or end of input for the question
                    answer_question(sh, SHELL_EOF);
                    if (sh->job) {
                        stash(sh, buf + i + 1, n - (i + 1));
                        return;
                    }
                } else {
                    shell_logout(sh);
                }
                prompt(sh);
            }
        } else if ((unsigned char)c >= 0x20 && c != 0x7f) {
            line_add(sh, (unsigned char)c);
        }
    }
}

// Raw input mode: decode the bytes into keys for the job. Enter is '\r' whichever of CR, LF or
// CRLF the terminal sends.
static void raw_keys(shell_t *sh, const char *buf, size_t n) {
    for (size_t i = 0; i < n && sh->key; i++) {
        char c = buf[i];
        if (c == '\n' && sh->last_was_cr) {
            sh->last_was_cr = false;
            continue;
        }
        sh->last_was_cr = (c == '\r');
        int k = keys_feed(&sh->keys, (unsigned char)c);
        if (k == KEYS_NONE)
            continue;
        sh->key(sh, k == '\n' ? '\r' : k, sh->job_arg);
    }
}

// While a raw-mode job runs: up to RAW_PASS_MAX input bytes go to the job as keys, then one step.
#define RAW_PASS_MAX 256

static void poll_raw(shell_t *sh) {
    if (sh->ahead_len) {        // typed after the command that started the job, in the same read
        size_t len = sh->ahead_len;
        sh->ahead_len = 0;
        raw_keys(sh, sh->ahead, len);
    }
    char buf[32];
    int n;
    size_t total = 0;
    while (total < RAW_PASS_MAX && (n = sh->io->read(sh->ctx, buf, sizeof(buf))) > 0) {
        raw_keys(sh, buf, (size_t)n);
        total += (size_t)n;
    }
    if (!sh->job)               // key() must not end the job, but don't call NULL if it did
        return;
    if (!sh->job(sh, sh->job_arg, false)) {
        sh->job = NULL;
        sh->job_arg = NULL;
        sh->key = NULL;
        if (!sh->at_line_start)
            write_out(sh, "\n", 1);
        prompt(sh);
    }
}

// While a job runs: Ctrl-C cancels it (and drops input typed meanwhile); other input is kept.
static void poll_job(shell_t *sh) {
    char buf[32];
    int n;
    bool cancel = false;
    while (!cancel && sh->ahead_len < sizeof(sh->ahead)) {
        size_t room = sizeof(sh->ahead) - sh->ahead_len;
        if ((n = sh->io->read(sh->ctx, buf, room < sizeof(buf) ? (int)room : (int)sizeof(buf))) <= 0)
            break;
        for (int i = 0; i < n && !cancel; i++) {
            if (buf[i] == 0x03)
                cancel = true;
            else
                stash(sh, &buf[i], 1);
        }
    }
    if (cancel) {
        sh->ahead_len = 0;
        shell_printf(sh, "%s^C\n", sh->at_line_start ? "" : "\n");
    }
    if (!sh->job(sh, sh->job_arg, cancel) || cancel) {
        sh->job = NULL;
        sh->job_arg = NULL;
        if (!sh->at_line_start)
            write_out(sh, "\n", 1);
        prompt(sh);
    }
}

void shell_poll(shell_t *sh) {
    char buf[32];
    int n;
    if (sh->job) {
        if (sh->key)
            poll_raw(sh);
        else
            poll_job(sh);
        return;
    }
    if (sh->ahead_len) {        // input typed during the last job
        char ahead[sizeof(sh->ahead)];
        size_t len = sh->ahead_len;
        memcpy(ahead, sh->ahead, len);
        sh->ahead_len = 0;
        // Typed-ahead input can be a password (typed while a check was running): don't leave
        // copies of it behind.
        memset(sh->ahead, 0, len);
        process_input(sh, ahead, len);
        volatile char *v = ahead;
        for (size_t i = 0; i < len; i++)
            v[i] = 0;
        if (sh->job)
            return;
    }
    while (!sh->job && (n = sh->io->read(sh->ctx, buf, sizeof(buf))) > 0)
        process_input(sh, buf, (size_t)n);
}
