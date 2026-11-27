# Shell quoting: design note for #199

Decision (the user's, 2026-11-27): **the shell gets POSIX-style quoting, at both levels
described in #199.** One tokenizer splits a typed line into words before anything else looks at
it; `|`, `>` and `>>` outside quotes are operators and nothing else is special. Commands move to
an argument vector one at a time, starting with those that take paths; a command that has not
moved gets the words joined with single spaces, as today's flat string. `>` becomes a redirect
for every command, spelled as a quiet `tee`, and `echo` loses its private one.

#199 holds the measurements (232 bytes of flash for the tokenizer, ~170 bytes of stack while a
line is dispatched, one pass over at most 256 bytes per command) and the counts of what moves.
This note fixes the rules so that three agents can build the phases without re-deciding them.
Where this note and the code differ in detail, the code and its header comments are the
reference.

Status (2026-11-27): all three phases are built and merged (#205 in PR #211, #206 in #215, #207 in
#216), board-tested on dev-1. Where this note and the code differ in detail, the code and its
header comments are the reference; this note records the decisions and why.

## 1. What exists today

- `run_line()` in [src/shell/shell_core.c](../../src/shell/shell_core.c) trims the line, adds it
  to the history, and asks `pipe_split()` for the first `|` that *starts a stage*: a `|` followed
  by one of `grep`, `head`, `tail`, `wc`, `tee`, `more` (the stage-name rule of
  [shell-pipes.md](shell-pipes.md) §2). Any other `|` is text. `pipe_open()` splits the stage
  text at the next such `|`, and each filter's `open()` parses its own argument string.
- `shell_execute()` in [src/shell/shell.c](../../src/shell/shell.c) splits the command name at
  the first space and hands the rest, trimmed, to `run(sh, args)`; `args` is NULL when there is
  none. 81 table entries (72 in shell.c, 9 in files.c) have that signature. 22 of them split
  `args` with `strtok(.., " ")`, 7 with `strchr(.., ' ')`, and files.c resolves 11 path arguments
  through `two_args()` and `resolve()`, whose comment says names with spaces aren't supported.
- `echo` finds the line's last `>` itself (`cmd_echo`, [src/files/files.c](../../src/files/files.c))
  and the table marks it `SHELL_REDIRECT` so that Tab completes a path after `>`
  (`complete_redirect()` in [src/shell/complete.c](../../src/shell/complete.c)).
- Quotes and backslashes are ordinary characters everywhere: `lua -e print("hi")` works because
  the quotes reach Lua; `cat 'a b'` looks for `'a` and `b'`.
- complete.c treats every space as a word boundary (14 places) and never offers a file name that
  contains a space (complete.h says so).

## 2. Lexical rules

The tokenizer (`words_split()`, new file [src/shell/words.c](../../src/shell/words.c)) works on
the line buffer in place, after the line has been copied to the history and to the pipeline's
name, and before anything else reads it. It writes de-quoted words over the input (the output is
never longer), each NUL-terminated, and fills:

```c
#define SHELL_WORDS_MAX 32                      // words in a line, operators included

typedef enum { WORD_PLAIN, WORD_PIPE, WORD_REDIRECT, WORD_APPEND } word_kind_t;

typedef struct {
    char *v[SHELL_WORDS_MAX + 1];               // the words, NULL after the last and for operators
    unsigned char kind[SHELL_WORDS_MAX];        // word_kind_t
    int n;
} words_t;

// Split `line` in place. Returns NULL, or why not: "unmatched quote", "line ends in \",
// "too many words". `line` is unusable after an error.
const char *words_split(char *line, words_t *w);
```

Rules, a subset of POSIX sh word splitting with nothing that SOS has no use for:

- Words are separated by one or more spaces (the line editor never puts a tab in a line).
- `'...'`: everything up to the next `'` is literal, including `"`, `\`, `|`, `>` and spaces.
- `"..."`: literal except that `\"` is `"` and `\\` is `\`. Any other backslash is kept as is.
- `\x` outside quotes: `x` is literal, whatever it is (space, quote, `|`, `>`, `\`).
- `|`, `>` and `>>` outside quotes are operators. They end the word before them and need no
  spaces: `a|b` is the three words `a`, `|`, `b`. `>>` is one operator; `>>>` is `>>` then `>`.
  An operator's `v[i]` is NULL and its `kind` says which it is, so that every command and
  stage segment is a NULL-terminated slice of `v` that can be passed as `argv` without copying.
- Quotes may open and close inside a word: `a"b c"d` is the one word `ab cd`. `""` and `''` are
  a word that is the empty string.
- No variables, globbing, tilde, `#` comments, `;`, `&`, `<`, `2>`, backquotes or `$(...)`.
  Those characters are ordinary.
- An unmatched quote or a line ending in an unescaped backslash is an error and nothing runs
  (there is no multi-line input to continue into). The 33rd word is an error.

Visible costs, accepted: quotes disappear from free text (`echo "a"` prints `a`; `lua -e
print("hi")` must be typed `lua -e 'print("hi")'`); an apostrophe in a `wall`, `chat` or
`logger` message must be escaped or the text quoted (`wall don't` is "unmatched quote", `wall
"don't"` works), and this is the case people will hit most; `|` is always a pipe and `>` always
a redirect, so Lua code with `|`, `>` or `>=`, an `->` in a message, or a JSON body for `http
post` are quoted too (`http post <url> '{"a":1}'`). The user has said compatibility is not a
constraint while one board runs SOS.

## 3. The line

```
line     := command [ '|' stage ]* [ redirect ]
command  := word+                      (the first word is the command name)
stage    := filter word*               (grep, head, tail, wc, tee: their options and arguments)
          | 'more'                     (only as the last stage, and not before a redirect)
redirect := ( '>' | '>>' ) word        (exactly one word, and the end of the line)
```

- `|` is always a pipe. The word after it must be a stage name, else
  `<word>: not a filter (grep, head, tail, wc, tee, more)` and nothing runs. The stage-name rule
  of shell-pipes.md §2 is retired: quoting now says whether a `|` is text.
- `> path` and `>> path` are a quiet `tee`: the stage is `tee` with a `quiet` flag, so output
  goes only to the file and nothing reaches the terminal. It is appended after the last filter
  and is the only sink that is not the terminal or the pager, so `| more` and a redirect
  exclude each other (`more: only at the end of the line`). A redirect not at the end
  (`a > f | grep x`), without a path (`a >`), with a second redirect, or on a line with no
  command (`> f`) is an error and nothing runs. The tee rules hold: the path is checked when the
  line is parsed, the file is opened on the first byte of output or at the end for an empty one,
  and an error from the file ends the pipeline once, said on the terminal.
- Errors that run nothing, each with its own short message: a line that starts with an
  operator (`| grep x`, `> f`), a trailing `|` (`a |`), a redirect without a path (`a >`), a
  redirect not at the end (`a > f | grep x`), a second redirect (`a > f > g`), `| more` with a
  redirect, a word after `|` that is not a stage name, and the tokenizer's three errors.
- Order with the console lock: the lock is checked first, on the raw line, as `run_line()` does
  today through `shell_command_tables()` before it splits anything; a locked console gets its
  refusal (or the rescue command, which `rescue_allowed()` recognises by the first word) and
  never the tokenizer's messages. Then the line is tokenized.
- A command that asks a question or enters raw mode while piped is released, as today.
- `echo` is an ordinary command: `echo <text>` prints it, and `echo <text> > file` is the shell's
  redirect. `SHELL_REDIRECT` is retired; Tab completes a path after an unquoted `>` or `>>` on
  any line (phase 3).

## 4. Dispatch: two kinds of handler

`shell_cmd_t` gains a second handler:

```c
typedef struct {
    const char *name;
    void (*run)(shell_t *sh, char *args);                  // the flat string, NULL without any
    void (*runv)(shell_t *sh, int argc, char **argv);      // or the words: argv[0] is the name
    const shell_word_t *words;
    unsigned flags;
} shell_cmd_t;
```

Exactly one of `run` and `runv` is set; a host test walks every table and checks. Existing
entries `{"name", fn, words, flags}` become `{"name", fn, NULL, words, flags}` in one mechanical
edit; migrated ones are `{"name", NULL, fn, words, flags}`. 81 entries × 4 bytes is the flash
cost. `shell_execute()` takes the command segment as words:

```c
void shell_execute(shell_t *sh, int argc, char **argv);
```

For a `run` handler it joins `argv[1..]` with single spaces, in place (the words lie in the
line buffer in order, so the join only moves bytes left), through one helper (`words_join()`,
in words.c) that migrated handlers with free text after their options use too. The handler gets
NULL when `argc == 1` or when the joined text is empty (`echo ""`), otherwise the text as it is,
spaces included: `echo "" ""` gets one space. Handlers that test only `!args` behave as today
for every line that was valid before. Quoted spaces survive the join because they are inside a
word: `echo "a   b"` prints `a   b`, `echo a   b` prints `a b`, as in sh. That is the join rule
for the free-text commands (`echo`, `logger`, `wall`, `chat`, `http post`), which stay on `run`
and need no change. `key add <line>` stays on `run` too; its man page notes that a quote or
backslash in the key's comment must be escaped (the base64 body has neither).

Which commands migrate to `runv` in phase 2: **every entry flagged `SHELL_PATH`** (so that
`cat 'my file'` works and Tab's new quoting of inserted names, phase 3, lands on a command that
can read it) **and the pipe filters** (`grep 'a b'`, `tee 'my file'`). On main at e7174c4 the
`SHELL_PATH` entries are `ls`, `cd`, `cat`, `cp`, `mv`, `rm`, `mkdir` (files.c) and `lua`,
`edit`, `more` (shell.c): ten, and a host test checks that each of them has `runv`. `lua` is
both a path command (`lua <file>`, `lua -b <file>`) and a free-text one (`lua -e <code>`): it
moves to `runv`, and for `-e` its handler joins `argv[2..]` with `words_join()`, so `lua -e
print(1 +  1)` still works and `lua -e 'print("hi")'` is one word. Everything else migrates when
it is next touched: its flat string is correct for the unquoted words those commands take.

Filters: `pipe_filter_t.open` takes `(shell_t *sh, int argc, char **argv)` with `argv[0]` the
stage name, from phase 1, because `pipe_open()` already has the words then and there is no
reason to join and re-split them. `grep` takes **one** pattern word after its options, as in
sh: `grep 'no route'`, and `grep no route` is a usage error (today the text runs to the end of
the stage, spaces kept; man/grep.txt changes in phase 1). `tee` takes one path word, which may
now contain spaces.

## 5. Tab

complete.c learns the lexical rules, from the line's start up to the cursor:

- The word being completed starts after the last unquoted space or operator, and a quote that
  is open at the cursor is part of it. The word's text is de-quoted for matching.
- A completion that contains a space, a quote, a backslash, `|` or `>` is inserted with each
  such character backslash-escaped when no quote is open, and raw when the cursor is inside
  quotes (the user closes the quote). File names with spaces are offered; complete.h's note that
  they never are goes away.
- After an unquoted `>` or `>>` the word is a path, for any command. After an unquoted `|` the
  word is a stage name, as today. The 14 space-boundary sites move to one helper that reports
  the current word's start and quoting state.

## 6. Phases

Three PRs, each host-tested, reviewed, run on dev-1 in a slot, then merged. Phases 2 and 3
depend on phase 1 and not on each other.

1. **Tokenizer, grammar, redirect** (#205): `words.c/.h` with its tests and a fuzz target;
   `run_line()` tokenizes and builds the pipeline from words; `pipe_split()` and `next_stage()`
   go; filters open from words; `tee` gets `quiet` and the shell appends it for `>`/`>>`;
   `shell_execute()` takes words and joins for `run`; `cmd_echo` loses its redirect and
   `SHELL_REDIRECT` goes (complete.c's `complete_redirect()` is left for phase 3 but no longer
   finds the flag: make it apply to every command, as the simplest step; it keeps scanning for
   the line's last `>`, so until phase 3 a `>` inside quotes misleads Tab, accepted). Docs:
   man/intro.txt gets a QUOTING paragraph; echo.txt (the redirect is the shell's; the file is
   still written when the text is empty, because a quiet tee opens at the end for empty output),
   grep.txt (one pattern word; its NOTES paragraph about `grep a|b` goes), more.txt, tee.txt,
   lua.txt, key.txt, and one sentence each in wall.txt, logger.txt, chat.txt and http.txt
   pointing at intro's QUOTING; docs/lua.md's examples; shell-pipes.md's status line records that
   §2's rule is retired here.
2. **Argument vectors** (#206): `runv` in `shell_cmd_t`, the table test, migration of the ten
   `SHELL_PATH` commands listed in §4 (`lua -e` joining with `words_join()`) and of
   `two_args()`/`resolve()`; the files.c comment about spaces goes.
3. **Tab with quotes** (#207): §5.

Not in scope: `<`, `2>`, `;`, variables, globbing. A `>` sugar for `tee` was the reason this
came up (#187) and is phase 1's redirect.

Context: #199 (the findings and measurements), #187 and shell-pipes.md (where `>` was first
considered), #182.
