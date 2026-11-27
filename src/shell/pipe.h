// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_PIPE_H
#define SOS_PIPE_H

#include <stdbool.h>
#include <stddef.h>

// Pipelines: `<command> | grep x | head 5 | more` (docs/design/shell-pipes.md). The command is
// the producer, run by shell_execute() as any other line; every byte it writes passes through
// the filters as it is written (on the producer's call stack, inside shell_core.c's
// write_out()), and the last stage hands it to the sink: the terminal, or the capture that
// `more` pages. Nothing is buffered between stages. Pure C over the heap and shell_printf(), so
// it builds on the host for unit tests.

#define PIPE_STAGES_MAX 4       // filters in one line (the sink `more` not counted)
#define PIPE_LINE_MAX   512     // grep's partial-line buffer; a longer line is cut into pieces
#define PIPE_PATTERN_MAX 64     // grep's text
#define TAIL_MAX        4096    // tail's ring: it keeps at most this many bytes

typedef struct shell shell_t;   // shell.h
typedef struct pipe_stage pipe_stage_t;
typedef struct pipeline pipeline_t;

typedef struct {
    const char *name;
    // Parse `args` (NULL for none) into a new stage on HEAP_EDIT; NULL after printing why.
    pipe_stage_t *(*open)(shell_t *sh, const char *args);
    // Bytes from the stage before; pass on what it keeps with pipe_emit(st, buf, len).
    void (*write)(pipe_stage_t *st, const char *buf, size_t len);
    // The producer has finished: flush what is held (tail's lines, wc's counts, grep's partial
    // line).
    void (*end)(pipe_stage_t *st);
    // Free the state. Called after end(), or instead of it when the line is cancelled.
    void (*close)(pipe_stage_t *st);
} pipe_filter_t;

// The start of every stage's state (each filter's struct begins with one).
struct pipe_stage {
    const pipe_filter_t *f;
    pipe_stage_t *next;         // NULL: the sink comes next
    pipeline_t *pipe;           // the line it belongs to
};

typedef enum { PIPE_SINK_TERMINAL, PIPE_SINK_PAGER } pipe_sink_t;

// Where the last stage's bytes go (shell_core.c: the terminal, or the pager's capture).
typedef void (*pipe_out_fn)(shell_t *sh, const char *buf, size_t len);

// One shell's pipeline (shell_t.pipe). Idle when `first` is NULL and `sink` is the terminal.
struct pipeline {
    pipe_stage_t *first;        // the filters, or NULL when the line has none
    bool done;                  // a stage closed the stream (head) or the capture is full: drop
                                // what follows, end the producer
    pipe_sink_t sink;           // PIPE_SINK_PAGER only while capturing for `more`
    pipe_out_fn out;            // the sink's function
    shell_t *sh;
    bool pending;               // the producer is a job: the line ends when it does
    bool aside;                 // output from outside the line (a chat message, between
                                // shell_hide_input() and shell_show_input()): straight to the
                                // terminal
    // `| more` with a producer that is a job: the pager starts on the first output and steps
    // the producer itself (shell_pipe_feed()); the producer's job is kept here meanwhile.
    bool paging;                // the pager is on screen while the producer runs: the shell's
                                // output goes to the terminal (the pager's frames) ...
    bool capturing;             // ... except during the producer's steps, which go to the line
    bool (*prod)(shell_t *sh, void *arg, bool cancel);  // the producer (a shell_job_fn)
    void *prod_arg;
    bool released;              // it asked a question while paged: the pager has left, and the
                                // shell shows the text and the question (and resumes `prod`)
    bool page_later;            // the pager couldn't start early: page when the producer ends
    // `| more`: the output is kept (a HEAP_EDIT block) instead of sent, up to SHELL_PIPE_MAX
    // bytes, then handed to the pager (shell_page()).
    char *buf;                  // NULL when not capturing
    size_t len, cap;
    bool cut;                   // output past SHELL_PIPE_MAX was dropped
    bool oom;                   // ... or because memory ran out
    char name[64];              // the command line, for the pager's status row
};

// The card side of `tee` (files.c on the firmware; the host tests provide their own). pipe.c
// holds no FatFs: it calls these three, whose strong definitions replace the weak ones in
// pipe.c that refuse for want of a card. A handle is opaque to pipe.c.
typedef struct pipe_file pipe_file_t;

// Resolve `path` against the shell's working directory, check that the shell may write it
// (the check and message of `echo`) and open it: replaced, or appended to. Holds the card's
// files (files_hold()) until pipe_file_close(). NULL after printing why ("tee: ...").
pipe_file_t *pipe_file_open(shell_t *sh, const char *path, bool append);

// Write `len` bytes (all of them or an error). NULL when written, else the error as text.
const char *pipe_file_write(pipe_file_t *f, const char *buf, size_t len);

// Close and release the handle (the card's files too). NULL, or the error as text (the file may
// not have been written out).
const char *pipe_file_close(pipe_file_t *f);

// The stages after `|` for Tab, with each one's options, are pipe_stage_table (declared in
// complete.h, which knows shell_cmd_t).

// Is `word` (up to a space or the end) the name of a stage: grep, head, tail, wc, tee or more?
bool pipe_is_stage(const char *word);

// Find the stages of `line`: the first '|' followed by a stage name (spaces between allowed)
// starts them, and a '|' followed by anything else is ordinary text (`ls | morex` and
// `lua -e "print(1|2)"` have none; in `x | grep a|b`, "a|b" is grep's text). NUL-terminates the
// producer's text in place (spaces before the '|' dropped) and returns the text after that
// first '|', or NULL when the line has no stage. Doesn't allocate.
char *pipe_split(char *line);

// Build the chain for the stage text pipe_split() returned ("grep x | more"): the filters on
// HEAP_EDIT, and the sink (`more` last: the pager; otherwise the terminal). False if a stage
// doesn't parse (its usage printed) or memory ran out: then nothing is allocated. Sets
// `first`, `sink`, `sh`, and clears `done`; the caller sets `out`.
bool pipe_open(shell_t *sh, pipeline_t *p, char *stages);

// Bytes from the producer, into the first stage (or the sink when there is none). Dropped once
// `done`.
void pipe_write(pipeline_t *p, const char *buf, size_t len);

// The producer has finished: end() each filter in order (what they hold goes on to the sink),
// then close() them. `first` is NULL and `done` false afterwards; the sink is the caller's.
void pipe_end(pipeline_t *p);

// The line is cancelled: close() each filter without end(). As pipe_end() afterwards.
void pipe_close(pipeline_t *p);

// For filters: pass bytes on to the next stage, or to the sink when there is none.
void pipe_emit(pipe_stage_t *st, const char *buf, size_t len);

// For head: nothing more is wanted from upstream.
void pipe_shut(pipe_stage_t *st);

#endif
