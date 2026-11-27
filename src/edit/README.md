# edit: the full-screen programs

`edit <file>` (the editor) and `more <file>` (the pager) run full screen on the console or over
SSH; see [docs/commands.md](../../docs/commands.md#editor) for the keys. The editor edits a file
of up to 128 KB (less while the heap has less to spare); the pager reads a file of any size, or a
text in RAM (`edit_view()`, for the manual pages).

## `screen.c`: what they share of the screen (pure C, host tested)

- **Layout:** each character takes the columns `charwidth()` gives (two for CJK and most emoji,
  none for combining marks), a tab up to the next multiple of 8; a letter and its combining marks
  (up to `SCREEN_MAX_MARKS`, 2) are one unit, and a mark beyond that or with no letter to go on
  shows as `?`. A wide character cut by either edge of the screen shows as spaces. All of this is
  one function, `screen_unit()`, so layout, cursor and drawing agree; and since each column holds
  one unit of at most 12 bytes, a row's output is bounded (`SCREEN_ROW_BYTES`, 4816 bytes at the
  400-column limit) whatever the text holds. Controls, invalid UTF-8 and the code points
  `termtext_hidden()` names show as `?`, so nothing in a file can send the terminal a sequence or
  hide what its bytes are. Lines don't wrap; the view scrolls sideways.
- **Frames** (`screen_draw()`): the program gives three callbacks (start of a frame, render row
  r, where the cursor goes); the driver sends only rows that changed (a hash of what each row
  shows), a row at a time, as many as the caller has room for, in pieces when the caller has less
  room than a row (SSH takes 2048 bytes a pass, a 400-column row of emoji can be more), and
  restarts the frame if something changes in between (a row cut short goes out to its end first,
  so the terminal never sees a cut sequence). Typing a character in the editor sends one text
  row, the status bar and the cursor.

## `editor.c`: the editor's text and keys (pure C, host tested)

- **Text** is bytes in a gap buffer, allocated whole at the editor's limit: what edit.c found the
  heap could spare when the file was opened, at most `EDITOR_MAX_TEXT`. The cursor moves a UTF-8
  character at a time (a letter and its marks as one). A file with CRLF line ends is edited with
  LF and saved with CRLF again (`editor_export()`).
- **Keys** come from `keys.h`; `editor_key()` returns what the caller must do: save, quit, or ask
  the terminal for its size. Ctrl/Alt+arrows and Alt+B/F move by word, Ctrl+Home/End to the ends.

## `pager.c`: the pager's window and keys (pure C, host tested)

- **Source:** a `pager_source_t` is a size and a `read(off, buf, n)`; a window of it
  (`PAGER_WINDOW`, 16 KB, or the whole text if smaller) is held in RAM, loaded around whatever is
  needed (forward from an offset, or ending at it for a scan backwards), so a file larger than
  memory reads the same as a small one. Finding line starts and ends, searching and counting all
  go through the window.
- **Lines** are separated by `\n`; a text ending in one has no empty line after it. Line numbers
  are counted as the view moves from the start; the total (and so the number of the last
  screenful) is counted in one pass over the text the first time it is needed: the end key, or a
  search that lands in the last screenful. The end of the text is found by walking back from it,
  so paging forward never reads the whole file.
- **Keys** (less-like): Space/b, j/k, d/u, g/G, Left/Right, `/` and `n`, q; Ctrl-F, Ctrl-Q, Ctrl-C
  and the arrows as in the editor. One status row: name, `lines a-b[/total]` and a percentage or
  `END`; it shows the search prompt or a message instead.

## `fsterm.c`: the terminal side of a full-screen job

Shared by `edit.c` and `view.c`: the alternate screen with autowrap off (so a wide character
can't push a row onto the next one; rows are erased before they're drawn, so a full row keeps its
last character); the size query on start and Ctrl-L (the cursor is moved to row and column 999
and asked where it is, `ESC [ 6 n`; the answer is the size, and the first frame waits up to 0.5 s
for it); sending each frame as the channel's free space allows (`shell_write_space()`); on the
console, counting the log messages held while the job runs (`shell_raw_mode()`, `log_pending()`),
once a second, for the status bar; and giving the screen back at the end.

## `edit.c`: the `edit` command

- Reads the file whole, straight into the editor's buffer (`editor_open()`, so there's never a
  second copy), then runs the editor as a raw-mode shell job (`shell_run_raw()`).
- **Memory:** the buffer is allocated whole when the file is opened, for the file plus 32 KB of
  typing room (`GROWTH`), so it never has to grow while typing (a realloc would hold the old and
  the new block together) and a small file costs little while an editor sits open. The room is
  less when the heap has less to spare: 64 KB stays free (`HEAP_RESERVE`, what a TLS handshake
  needs), and if no block of that size can be had the room is halved, down to 1 KB. Typing past
  the room says so; save and open again for more. A file that leaves less than 1 KB of room, or
  more than `EDITOR_MAX_TEXT`, is refused with the sizes.
- **Saving:** through `files_write_allowed()` (so `/certs` is console-only), into `<file>.~edit`,
  then the old file is removed and the new one renamed into place. The file isn't held open
  between saves. If the rename fails the text stays "modified", so quitting warns and Ctrl-S
  tries again. Power lost between the two leaves only `<file>.~edit`: `edit <file>` then opens
  that, and says it may be incomplete (it is if the power went while a new file's first save was
  being written); if both exist it says the leftover is incomplete and will be replaced.

## `view.c`: the `more` command and `edit_view()`

- `more <file>` keeps the file open for as long as the pager runs (`files_hold()`, so the card
  can't be unmounted under it) and reads it through `f_lseek()`/`f_read()` as the window moves.
  Reads happen inside the job's step, so a search through a large file holds the main loop for
  as long as the card takes (about a second per megabyte).
- `edit_view(sh, name, text, len)` copies the text to the heap and pages it: for the manual pages
  (`man`) and any command with more to show than a screen.
