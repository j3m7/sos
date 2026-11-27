# Shell pipelines: design note for #182

Decision: **filters run synchronously inside the shell's output path; there is still one job
per shell.** `a | grep x | head 5 | more` runs `a` exactly as today (at once, or as the shell's
job), and every byte `a` writes passes through the filters as it is written, ending in a sink:
the terminal, the pager's capture, or (later) a file. Output is streamed and unbounded through
the filters, nothing is buffered between stages, and the only thing that still has a size cap is
the pager's capture, which now holds the *filtered* output. `head` ends its producer. The pager
learns to show text that is still growing in a later phase, so `cat big.log | more` shows the
first screen while `cat` still runs.

This is neither design A nor B of the issue. A (buffer each stage) caps every stage at 32 KB
and shows nothing until each stage ends. B (one job per stage, rings between them) needs a job
list per shell and a scheduler, and every cost in the issue's list. The filters anyone would
write (`grep`, `head`, `tail`, `wc`) are functions of a byte stream and need no job of their
own; the only consumer that must run *while* the producer runs is the pager, and it is a raw-mode
job already. So: filters are plain functions, and the pager steps the producer itself.

Status (2026-11-26): phases 1 and 2 are built (PR #192 for #184, PR #189 for #185); phase 3
(#186) is merged (PR #196); phase 4 (#187) is `tee` (this PR). Where this note and the code
differ in detail (phase 2 extends the line count rather than resetting it, and has the F key;
phase 3 starts the pager on the producer's first output rather than at once, keeps the producer
in `shell_t.pipe` rather than in `view_t`, and steps it through `shell_pipe_feed()` in
shell_core.c), the code and its header comments are the reference; this note records the
decision and why. Issues: #182 (the investigation).

## 1. What exists today

- `run_line()` in [src/shell/shell_core.c](../../src/shell/shell_core.c) recognises one suffix,
  `| more`. It allocates `sh->pipe_buf` (HEAP_EDIT, grows by 2 KB, capped at `SHELL_PIPE_MAX`
  = 32 KB) and runs the command. `write_out()` appends to the buffer while `pipe_buf` is set. When
  the command has finished (at once, or when its job's step returns false in `poll_job()`),
  `pipe_finish()` hands the block to `shell_page()`, which is `edit_view_take()` in
  [src/edit/view.c](../../src/edit/view.c): a raw-mode job over `pager_t`.
- A command that asks a question (`sh->answer` set) or enters raw mode (`sh->key` set) gets its
  output released to the screen and runs unpaged (`pipe_release()`). A locked console runs the
  line as typed. A command that ended the session (`logout`) has nothing to page.
- Ctrl-C during the job drops the capture and prints `^C`. `shell_end()` cancels the job and
  drops the capture.
- `shell_write_space()` is the only flow control: SSH reports its ring's room capped by a 2 KB
  per-pass budget; the console has none (1024); while capturing it is 1024 (no channel to wait
  for). Producers honour it: `cat` (512-byte chunks), `ls` (one line's worth), `http`, Lua's
  `drain()`, `man list`.
- `shell_screen_held()` is true while capturing: chat messages wait. Console log lines bypass
  the shell (`log_write` → `console_write`) and are held only in raw mode (`log_hold`).
- The pager reads its text through a window (`PAGER_WINDOW` = 16 KB) with `pager_source_t`
  `{read, ctx, size}`; `size` is fixed at `pager_init()`.
- Tests: `tests/test_shell_line.c` (the `| more` group, with a fake `shell_execute()` and
  `shell_page()`), `tests/test_pager.c`.

Sizes of what people pipe (the issue asked):

| Producer | Size | Note |
|---|---|---|
| `dmesg` | ≤ 4 KB | `LOG_HISTORY_SIZE` |
| `help` | ~5 KB | 67 lines |
| `ls` | ≤ 304 B per entry | there is no `ls -R`; the issue's example doesn't exist |
| `man <page>` | paged already | |
| `cat <file>` | unbounded | log files on the card, downloaded pages |
| `http get <url>` | unbounded | tens to hundreds of KB |
| `lua <script>` | unbounded | |
| `ping <host>` | small, but over time | lines arrive for seconds |

Only `cat`, `http`, `lua` and `ping` exceed the cap or take time, and those are exactly the ones
one would filter. With the filters ahead of the capture, `cat big.log | grep error | more` is
bounded by the matches, which is what the issue asked for.

## 2. The model

```
line     := producer ( '|' stage )*
stage    := filter | sink
filter   := 'grep' [-v] [-i] [-n] <text> | 'head' [<n>] | 'tail' [<n>] | 'wc'
sink     := 'more'                      (phase 1: the capture, then the pager)
          | '>' <path> | '>>' <path>    (phase 4, decided separately)
```

- **The producer** is any command, run by `shell_execute()` exactly as now. It may finish at
  once or become the shell's job. It is the only job.
- **A filter** is a pure function over a byte stream with a small state: `write(bytes)` and
  `end()`. It emits to the next stage. Filters run inside `write_out()`, on the producer's
  call stack, so there is nothing to schedule and no buffer between stages.
- **The sink** is where the chain ends. The terminal sink is today's `write_out()` path (CRLF
  translation, `at_line_start`). The `more` sink is today's capture (`pipe_append()`), paged when
  the producer ends (phase 1) or at once, growing (phase 3).
- **Flow control is unchanged.** `shell_write_space()` keeps meaning "what the sink takes now":
  the channel's room for the terminal sink, 1024 for the capture. Filters never grow the stream
  by more than a few bytes a line (`grep -n`), and hold at most one partial line (`grep`) or a
  bounded tail (`tail`), so a producer that paces itself by the sink's room still can't flood it.
  One known exception: `tail`'s flush at `end()` is up to `TAIL_MAX` bytes in one go (see §5).

### Parsing rule

Split from the right. A `|` starts a stage only if the word after it is a stage name (`grep`,
`head`, `tail`, `wc`, `more`); the pipeline is the longest such suffix, and everything before it
is the producer, verbatim. A `|` followed by anything else stays in the producer's arguments,
as today (`ls | morex` runs `ls` with `| morex`; `lua -e "print(1|2)"` is untouched). This
generalises the existing `pipe_suffix()` rule rather than adding quoting to the shell.

Stage arguments are parsed before anything runs. A stage that doesn't parse (`grep` without a
pattern, `head x`) prints its usage and the line does nothing: the producer never starts.

### Stage names are reserved only after `|`

`grep`, `head`, `tail` and `wc` are not commands. Typed first on a line they say
`grep: only after a pipe (<command> | grep <text>)`. (Tab completes them after `| `, and their
options after their name: phase 1 includes this.) Making them commands over files (`grep x
file`) is a possible later step; the filter functions would be reused with `cat`'s job as the
producer.

## 3. Interfaces

New module `src/shell/pipe.c` / `pipe.h`: pure C, host tested, depends on `shell.h` only for
`shell_t`, `shell_printf()` and the heap tag. shell_core.c keeps the glue (parsing the line,
routing `write_out()`, ending the chain when the job ends).

```c
// pipe.h

#define PIPE_STAGES_MAX 4       // filters per line (plus the sink)
#define PIPE_LINE_MAX   512     // a filter's partial-line buffer; longer lines are cut into pieces

typedef struct pipe_stage pipe_stage_t;

typedef struct {
    const char *name;
    // Parse `args` (NULL for none) into a new stage on HEAP_EDIT; NULL after printing why.
    pipe_stage_t *(*open)(shell_t *sh, const char *args);
    // Bytes from the stage before; emit with pipe_emit(st, buf, len).
    void (*write)(pipe_stage_t *st, const char *buf, size_t len);
    // The producer has finished: flush what is held (tail's lines, wc's counts, grep's partial line).
    void (*end)(pipe_stage_t *st);
    // Free the state. Called after end(), or instead of it when cancelled.
    void (*close)(pipe_stage_t *st);
} pipe_filter_t;

struct pipe_stage {
    const pipe_filter_t *f;
    pipe_stage_t *next;         // NULL: the sink
    shell_t *sh;
};

typedef struct {
    pipe_stage_t *first;        // the chain, or NULL when no filter is in the line
    bool done;                  // a stage closed the stream (head): drop what follows, end the producer
    // the sink
    enum { PIPE_SINK_TERMINAL, PIPE_SINK_PAGER } sink;
    // the capture (today's pipe_buf..pipe_name fields move here unchanged)
} pipeline_t;

// Find the stages of `line` from the right (see the rule above): NUL-terminates the producer's
// text in place and returns the stage text from its first '|', or NULL when the line has none.
// Doesn't allocate.
char *pipe_split(char *line);
// Build the chain for the stage text ("| grep x | more"). False (nothing allocated, usage printed)
// if a stage doesn't parse.
bool pipe_open(shell_t *sh, pipeline_t *p, const char *stages);
void pipe_write(pipeline_t *p, const char *buf, size_t len);   // into the first stage
void pipe_end(pipeline_t *p);                                   // end() then close() each stage
void pipe_close(pipeline_t *p);                                 // close() only (cancelled)
// For filters: pass bytes on to the next stage, or to the sink when there is none.
void pipe_emit(pipe_stage_t *st, const char *buf, size_t len);
// For head: nothing more is wanted from upstream.
void pipe_shut(pipe_stage_t *st);
```

`pipe_emit()` on the last stage calls a sink function that shell_core.c supplies (the terminal
path or `pipe_append()`); keep it a function pointer in `pipeline_t` so pipe.c stays host
testable without the shell.

`shell_t` changes: the six `pipe_*` fields become one `pipeline_t pipe;`. `shell_screen_held()`
and `shell_write_space()` test `sh->pipe.sink == PIPE_SINK_PAGER` where they test `pipe_buf`
today.

### Filters

| Filter | State | `write()` | `end()` |
|---|---|---|---|
| `grep [-v] [-i] [-n] <text>` | pattern ≤ 64 B, flags, line counter, partial line ≤ 512 B | complete lines matched (fixed string, `-i` ASCII case-insensitive); emitted whole, `-n` prefixes `<line>:` | a last line without `\n` is matched and emitted with `\n` added |
| `head [<n>]` (10) | lines left | pass bytes through up to and including the n-th `\n`, then `pipe_shut()` | nothing |
| `tail [<n>]` (10) | ring of `TAIL_MAX` = 4 KB, line count | append; drop whole lines from the front past n lines or past the ring | emit the ring |
| `wc` | lines, words, bytes, in-word flag | count | emit `"%lu %lu %lu\n"` (newlines, words, bytes, as Unix wc) |

A line longer than `PIPE_LINE_MAX` is handled as several lines by `grep` (each piece matched on
its own; say so in its man page). `tail` with `n` larger than fits the ring keeps what fits and
says nothing (the ring is the cap). Patterns are fixed strings: no regex, no Lua patterns, no
quoting; the pattern is the stage's text after the options, spaces inside kept.

## 4. Behaviour rules

These are the answers the issue asked for.

- **Ctrl-C** (Enter-mode job): as today. The job is cancelled, `^C` is printed, and the pipeline
  is closed without `end()`: `tail`'s held lines and `wc`'s counts are dropped, the capture is
  dropped. Output already on the terminal stays.
- **Session end** (`shell_end()`): cancel the job, `pipe_close()`, drop the capture. Nothing is
  printed.
- **A stage closes the stream (`head`)**: `pipeline.done` is set; `write_out()` discards further
  producer output for the rest of the pass; `end()` runs down the chain *from the stage after
  head* (so `head 5 | wc` prints `5 ...`); the sink ends (the pager starts). If the producer is a
  job, `poll_job()` cancels it at the end of that pass, **silently**: `step(sh, arg, true)`, no
  `^C`, then the prompt. `cat` closes its file, `ping` stops, a Lua script is stopped as by
  Ctrl-C. A producer that isn't a job (`dmesg`) runs to its end with its output discarded, which
  costs nothing worth avoiding.
- **The capture is full (32 KB)**: as today the text is cut and a note appended, and *in
  addition* the producer is ended as `head` ends it (today it runs on to the end for nothing:
  `cat huge | more` reads the whole file).
- **A stage asks a question** (only the producer can: `wifi set | grep x`, `lua | more`): the
  pipeline is released as today. The filters are closed (no `end()`), the capture goes to the
  screen, the question is asked. Bytes that filters already passed to a terminal sink are on the
  screen and stay. Rule: a command that asks is not a thing to filter; nothing is lost that the
  user wouldn't have seen.
- **A stage enters raw mode** (`edit x | wc`): released the same way; the program draws its own
  screen. Only the producer can be raw; the sink `more` is raw by design and runs after the
  producer (phase 1) or around it (phase 3).
- **A locked console**: the line runs as typed and the gate refuses it, as today.
- **The producer ends the session** (`logout | wc`): nothing to end; `pipe_close()` in
  `shell_end()` already ran.
- **Error messages**: there is no stderr. A producer's `cat: <error>` line goes through the
  filters like any output; `grep` may drop it, `wc` counts it. Today `| more` pages it, so this
  is not a regression. A `shell_eprintf()` that bypasses the pipeline is a possible later step;
  it is a mechanical change at error sites and not part of this design.
- **`at_line_start` and prompts**: `write_out()` keeps updating `at_line_start` at the real
  output, after the filters, so the prompt lands right. Filters emit whole lines, so a producer's
  habit of starting an error with `\n` is harmless.
- **Chat and console log lines**: unchanged. `shell_screen_held()` is true only for the pager
  sink; with a terminal sink chat messages interleave as they do with any job's output.

## 5. Memory

Per pipeline, from HEAP_EDIT (the same tag as the capture; `mem` shows it as "editor"), freed
when the line ends:

| Piece | Bytes |
|---|---|
| `grep` | ~600 (pattern 64, line buffer 512, counters) |
| `head` | ~16 |
| `tail` | ~4 KB + 16 (`TAIL_MAX` ring) |
| `wc` | ~24 |
| `more` sink: capture | 2 KB, growing by 2 KB to 32 KB, plus the 48-byte note |
| `more` sink: pager (existing) | `view_t` ~1 KB + window up to 16 KB |

Typical `cat x | grep y | more`: 600 B while running, then ≤ 32 KB + 16 KB while paging; the
same as today's `| more` plus 600 B. Worst case with four filters: < 6 KB plus the capture. The
image's static use is about 150 KB of the 520 KB SRAM (`arm-none-eabi-size build/sos.elf`);
`mem` on the board shows the heap's headroom with SSH sessions open.

Design B would have cost a 2–4 KB ring per stage boundary plus each stage's job state, and the
same capture for `more`.

## 6. Phases

Each phase is one PR, host tested, then checked on dev-1. Phase 1 stands on its own.

### Phase 1: filters and the parser (#184)

Scope:
- `src/shell/pipe.c`, `pipe.h` as in §3; the four filters.
- shell_core.c: `run_line()` uses `pipe_split()`/`pipe_open()` in place of `pipe_suffix()`;
  `write_out()` routes through `pipe_write()` when a chain exists; `poll_job()` and the
  post-`shell_execute()` logic call `pipe_end()`/`pipe_close()` where they call
  `pipe_finish()`/`pipe_drop()`/`pipe_release()` today; silent cancel on `done`; the cut ends the
  producer.
- shell.c: `help` line for pipes; `grep`/`head`/`tail`/`wc` typed as commands say "only after a
  pipe".
- complete.c: after `| `, Tab offers the stage names; after a stage name, its options. The table
  of stages can be a `shell_word_t` list the filter table provides.
- Man pages `man/grep.txt`, `head.txt`, `tail.txt`, `wc.txt`; `more.txt` and `intro.txt` mention
  pipes; run `tools/man-bundle.sh` so `docs/commands.md` follows.
- `src/shell/README.md`: a "Pipes" bullet under `shell_core.c`.
- Tests: `tests/test_pipe.c` (each filter fed the same text split at every byte boundary gives
  the same output; `head` shuts; `tail` ring eviction; `wc` counts; long lines);
  `test_shell_line.c`: the existing `| more` tests pass unchanged in behaviour; new: `cmd | grep
  x` streams to the terminal, `job | head 2` ends the job without `^C` (`job_cancels == 1`),
  `big | grep fox | more` keeps all matches and isn't cut, `ls | morex` runs as typed, `x | grep`
  prints usage and runs nothing, Ctrl-C drops `tail`'s lines, question and raw release with a
  filter in the line.

Acceptance: `dmesg | grep wifi`, `cat /log/<file> | grep error | more`, `help | wc`, `ls | tail 3`
work on dev-1 over SSH and on the console; `cat <2 MB file> | head 3` returns at once.

### Phase 2: the pager over a growing text (#185)

- `pager_source_t.size` stays, and `pager_grow(pager_t *p, size_t size)` raises it: resets
  `end_top`, `end_top_line`, `lines` (or extends the count if it was finished), marks the frame
  changed when the view showed the end or rows past it.
- `pager_set_growing(pager_t *p, bool)`: the status row shows `...` for the total while true
  and the end key means "the end so far".
- `pager_init()` for a growing source allocates the full `PAGER_WINDOW`, not `min(size, window)`.
- A `F` key (follow, like `less`): stay at the end as text arrives; any movement key leaves it.
  Optional, small.
- Tests in `test_pager.c`: growth while at the end, growth while scrolled back (view doesn't
  move), the random-keys invariant with growth interleaved, line count after growth.

No shell changes. Also wanted for tailing log files later.

### Phase 3: streaming `| more` (#186)

Depends on phases 1 and 2.

- When the producer became a job, start the pager at once on the capture so far instead of
  waiting for the job to end. `view_t` gains the producer: `shell_job_fn prod; void *prod_arg;
  bool prod_done;`. `view_step()` steps it once per pass **with the capture switched on around
  the call** (`sh->pipe.capturing = true` ... `false`): the producer's output goes to the
  capture, the pager's own output (`fsterm_send()`) goes to the terminal. After the step, if the
  capture grew, `pager_grow()`; when it finishes, `pager_set_growing(false)`.
- The capture's block is realloc'd as it grows: the pager's source `ctx` must be re-pointed
  after `pipe_append()` (or `pager_read_mem` reads through a pointer to the pointer).
- `q` with the producer running: cancel it silently (as `head` does), then leave.
- Ctrl-C is a raw key here: stop the producer, keep the text, show "stopped" in the status row.
  Pager keys and the session's end behave as today.
- The cut at 32 KB ends the producer (phase 1 rule); the note is appended; the pager shows it.
- `shell_end()` cancels the pager job; `view_step(cancel)` cancels the nested producer first.
- Synchronous producers (`dmesg`) are unchanged: the pager starts after `shell_execute()` with
  the whole text, as today.
- Tests: `test_shell_line.c` with the fake pager (`shell_page()` gains the producer step in its
  signature, or a second entry point `shell_page_growing()`); the chain `job | more` now pages
  before the job ends; `q` cancels the job silently; Ctrl-C stops it and the pager stays.

### Phase 4: `tee [-a] <path>` as a stage (#187)

Decision (2026-11-26): the file output is a `tee` stage, not a `>` redirect. A `tee` after a `|`
starts a stage like the others and nothing else in the line changes meaning; `echo text > file`
works as before. A `>` on every command would need either a per-command free-text flag or shell
quoting, which is its own change (#199). `tee` is a filter in pipe.c; the card side (resolve,
the `echo` write check, open, write, close, `files_hold()`) is behind three functions that
files.c defines and the host tests stub (`pipe_file_open/write/close`, weak defaults in pipe.c
that refuse). A write error prints `tee: <path>: <error>` once on the terminal and ends the
producer as `head` does. The text below is the design as first written, kept for the reasoning
on `>`.

#### The `>` sink as first considered

Today only `echo` has a redirect, parsed in `cmd_echo()` and known to Tab through
`SHELL_REDIRECT`. The general form is a sink: a `FIL` opened with `files_hold()`, `f_write()`
from `pipe_emit()`, closed at `end()`/`close()` (a cancelled line leaves a partial file, as Unix
does). `shell_write_space()` returns 1024 for it. Card errors end the pipeline with the message
and the producer ended as `head` ends it.

The question to settle before building it is the parse rule. A trailing `> path` on any line
would break commands whose text can hold `>`: `lua -e 'if a>b then'`, `logger`, `http post`,
`chat`. Options: (a) recognise `>` only on commands not flagged `SHELL_FREETEXT` (a new flag
on those four); (b) only after a `|` (`dmesg | > f`), consistent but odd; (c) keep it on `echo`
only. (a) is the recommendation; it needs an audit of which commands take free text
(`strtok(NULL, "")` and the like: `http post`, `logger`, `lua -e`, `chat`, `wall`) and a line
in each man page. Not blocking phases 1 to 3.

## 7. Not in scope, and why

- **A job per stage (design B)**: nothing needs it until a consumer is itself a long-running
  program reading a stream, such as a Lua script reading piped lines. If that comes, the sink
  abstraction above is where it plugs in: a ring the script's core reads, `shell_write_space()`
  returning the ring's room so the producer waits. The filters stay as they are.
- **Quoting and escaping** on the command line: none exists; the stage-name rule avoids needing
  it.
- **Regular expressions** in `grep`: fixed strings; Lua patterns are a plausible later option
  since the Lua library is in the image.
- **`sort`, `uniq`, `cut`**: not asked for; `sort` would need to hold everything and is what
  `| more` is for.
- **Multiple producers or `tee`**: no.
- **stderr**: see §4, error messages.
