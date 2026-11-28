// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Reading tz/zones.bundle (#240): the time zones `time zone` offers, one per line as
// "<name> <POSIX TZ rule>", after a header line "# tzdata <version>: ...". tools/tz-bundle.sh
// builds it from IANA's signed release; the board reads it from the card (/tz/zones.bundle),
// a line at a time, through a reader the caller gives, so this runs in the host tests too.
//
// The card is anyone's to write (over SFTP, say), so nothing here trusts it: a line too long,
// or one that isn't "<name> <rule>", is skipped, and a rule is only used once tzrule_parse()
// takes it.
//
// Pure C: no Pico SDK dependency.

#ifndef SOS_TZBUNDLE_H
#define SOS_TZBUNDLE_H

#include <stdbool.h>
#include <stddef.h>

#define TZBUNDLE_NAME_MAX    39     // as config's tz_name holds (tools/tz-bundle.sh checks)
#define TZBUNDLE_RULE_MAX    63     // as config's tz_rule holds
#define TZBUNDLE_VERSION_MAX 15
#define TZBUNDLE_LINE_MAX    (TZBUNDLE_NAME_MAX + 1 + TZBUNDLE_RULE_MAX)   // a zone line's text
#define TZBUNDLE_NEAR        3      // closest names offered for one that isn't found
// The buffer lines are read into: room for the longest zone line and its newline, with some to
// spare, because FatFs's f_gets() fills a few bytes less than it is given (4 less with the UTF-8
// API this build has), and for the header.
#define TZBUNDLE_READ_MAX    160

// The next line into buf (cap bytes, NUL-terminated, the newline kept if it fits), as fgets()
// and FatFs's f_gets() do; false at the end. A line may come in several pieces, each shorter
// than cap: only a newline, or the end of the file, ends one.
typedef bool (*tzbundle_read_fn)(void *ctx, char *buf, size_t cap);

// Whole lines from a reader, whatever pieces it gives them in.
typedef struct {
    tzbundle_read_fn read;
    void *ctx;
    char next[TZBUNDLE_READ_MAX];   // a piece read ahead, to tell the last line from a long one
    bool have_next;
} tzbundle_lines_t;

// The next line into line, its newline dropped; false at the end. A line the reader doesn't
// give in one piece (fgets: up to TZBUNDLE_READ_MAX - 1 bytes; f_gets on the board: 4 fewer) is
// read to its end and dropped: true with *too_long set, and line holding only its start.
bool tzbundle_next_line(tzbundle_lines_t *l, char line[TZBUNDLE_READ_MAX], bool *too_long);

// The version a header line names ("# tzdata 2026f: ..." gives "2026f"); false if it isn't one.
bool tzbundle_version(const char *line, char out[TZBUNDLE_VERSION_MAX + 1]);

// A zone line "<name> <rule>", split in place: newline dropped, *name and *rule pointing into
// line. False for anything else (the header, a blank line, a line with other characters).
bool tzbundle_split(char *line, char **name, char **rule);

typedef enum {
    TZBUNDLE_FOUND,
    TZBUNDLE_NOT_FOUND,         // near[] holds the closest names, if any are close
    TZBUNDLE_NOT_A_BUNDLE,      // no header line first
} tzbundle_result_t;

typedef struct {
    char version[TZBUNDLE_VERSION_MAX + 1];
    char name[TZBUNDLE_NAME_MAX + 1];       // found: as the bundle spells it
    char rule[TZBUNDLE_RULE_MAX + 1];       // found: the rule, which tzrule_parse() takes
    char near[TZBUNDLE_NEAR][TZBUNDLE_NAME_MAX + 1];    // not found: closest first
    unsigned near_count;
    unsigned zones;                         // usable lines read
    unsigned skipped;                       // lines read that aren't usable zones
} tzbundle_find_t;

// Look `name` up: an exact match, case aside (and with ' ' taken as '_': "america/new york").
// A line whose rule doesn't parse isn't a match. Reads the whole bundle when it isn't found.
tzbundle_result_t tzbundle_find(tzbundle_read_fn read, void *ctx, const char *name,
                                tzbundle_find_t *out);

// Each zone's name, in the bundle's order, to fn (the name is only valid during the call);
// lines that aren't zones are skipped, rules aren't parsed. False, with no call, if it isn't a
// bundle. For Tab after `time zone`.
bool tzbundle_names(tzbundle_read_fn read, void *ctx, void (*fn)(const char *name, void *arg),
                    void *arg);

// How far `query` is from `name`, for suggestions: the fewest letters to add, drop or change
// (case aside, ' ' as '_') to make query into the whole name or into its last part ("madrid"
// is 0 from "Europe/Madrid"), or 1 if query is inside the name. Capped at 99.
unsigned tzbundle_distance(const char *query, const char *name);

// Does name start with prefix, case aside? (An empty prefix: every name.)
bool tzbundle_has_prefix(const char *name, const char *prefix);

#endif
