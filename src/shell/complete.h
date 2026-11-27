// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_COMPLETE_H
#define SOS_COMPLETE_H

#include <stdbool.h>
#include <stddef.h>

#include "shell/shell.h"

// Tab completion: which words could finish the line being typed. Pure C over the command
// tables (shell.h) and a directory lister, so it builds on the host for unit tests.

// A match: `word`, and what follows it once it's complete: ' ' for a word or a file, '/' for a
// directory, '\0' for nothing (a SHELL_NOSPACE word). `note` (NULL for none) is shown beside it
// in the list only; it comes from outside (a key comment, an SSID), so filter it to show it.
// Both may live in a buffer that is reused once `fn` returns.
typedef void (*complete_fn)(const char *word, char end, const char *note, void *arg);

// A directory lister calls this with every entry.
typedef void (*complete_entry_fn)(const char *name, bool is_dir, void *arg);

typedef enum {
    COMPLETE_DIR_NONE,          // can't list it: no card, no such directory, path too long
    COMPLETE_DIR_ALL,           // every entry given
    COMPLETE_DIR_PART,          // stopped early (a large directory): the entries may be some only
} complete_dir_t;

// List the directory `dir` (as typed: relative to the shell's working directory, or absolute;
// "" for the working directory itself), calling `fn` with each entry except "." and "..".
typedef complete_dir_t (*complete_dir_fn)(shell_t *sh, const char *dir, complete_entry_fn fn,
                                          void *arg);

// The stages a line's output can go through after a '|' (pipe.c), with their options: Tab
// offers these after a '|' (only their names and options, no `run`).
extern const shell_cmd_t pipe_stage_table[];

// The SD card's lister (files.c); the host tests have their own.
complete_dir_t shell_list_dir(shell_t *sh, const char *dir, complete_entry_fn fn, void *arg);

typedef struct {
    const shell_cmd_t *const *tables;   // NULL-terminated (shell_command_tables())
    bool admin;                 // offer SHELL_ADMIN words
    complete_dir_fn list_dir;   // file names; NULL for none
    shell_t *sh;                // passed to list_dir
} complete_src_t;

// Where the text Tab completes is on the line, and how it is spelt there.
typedef struct {
    size_t start;               // where the word being completed begins (with its quote, if any)
    size_t typed;               // bytes of each match already typed: the end of the word's
                                // de-quoted text (after its last '/' for a path)
    char quote;                 // the quote open at the cursor: '\0', '\'' or '"'
    bool escape;                // the line ends in an unquoted, unescaped backslash
} complete_at_t;

// The word at the end of `line` (`len` bytes, the cursor at its end; it may be empty) is the one
// Tab completes, read with the shell's quoting rules (words.h, words_at()): it starts after the
// last unquoted space, '|' or '>', a quote open at the cursor is part of it, and it is matched
// de-quoted. So are the words before it in its segment (after the last unquoted '|'), which
// pick what it can be: the first is a command from the tables (a stage name from
// pipe_stage_table after a '|'), each later one a word that may follow the one before. After a
// command or word marked SHELL_PATH, every word that isn't one of its words is a path, and so is
// the last word: then the entries of the directory it names that start with what follows its
// last '/' match too (only directories with SHELL_DIR). After an unquoted '>' or '>>' (the
// shell's redirect, on any command) the word is a path; after that path, nothing. Words that
// come from a function (SHELL_VALUES_OF, shell.h) are offered like the others, when they pass
// the same check as file names.
//
// Calls `fn` with each match that starts with the de-quoted text (file names ignoring the case
// of ASCII letters, as FAT does; words exactly), and returns how many. The matches are given as
// they are, unquoted: the caller spells them for the line, `at` saying how (shell_core.c: a
// space, quote, backslash, '|' or '>' backslash-escaped outside quotes, raw inside the open
// quote). Words: not SHELL_HIDDEN ones, nor SHELL_ADMIN ones unless `admin`. File names and
// values: only those that can be shown as they are (valid UTF-8; no control characters, C1
// controls, spaces other than U+0020, nor what termtext_hidden() names; U+0020 only between
// other characters and one at a time, so that names in a list can't look alike) and fit on a
// line (SHELL_LINE_MAX bytes at most). None when an earlier word isn't known, is offered only
// to an admin channel and this isn't one, or takes nothing after it. *partial says the
// directory lister stopped early, so there may be more matches than were given.
size_t complete_words(const complete_src_t *src, const char *line, size_t len, complete_at_t *at,
                      complete_fn fn, void *arg, bool *partial);

// Tab at a question with a completer (shell_ask_complete): the whole answer typed so far,
// line[0..len), is the start of each match; spaces are part of it. Calls `fn` with each value
// from `list` that starts with it exactly and passes the file-name check with U+0020 allowed
// (one at a time, not at either end), ending in nothing ('\0'), and returns how many. With
// `words` (shell_ask_complete_words), only the last word is: the values must be single words,
// and end in ' '; with `chars` too (shell_ask_complete_chars), the word is the run of those
// characters before the cursor, and the values end in nothing. *start is where the text Tab
// completes begins. *partial: the list may still grow.
size_t complete_answer(shell_t *sh, shell_ask_complete_fn list, const char *line, size_t len,
                       bool words, const char *chars, size_t *start, complete_fn fn, void *arg,
                       bool *partial);

#endif
