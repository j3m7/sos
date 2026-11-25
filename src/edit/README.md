# edit: the full-screen text editor

`edit <file>` (see [docs/commands.md](../../docs/commands.md#editor) for the keys) edits a file
of up to 32 KB on the console or over SSH.

## `editor.c`: text, keys and screen (pure C, host tested)

- **Text** is bytes in a gap buffer that grows as needed up to `EDITOR_MAX_TEXT`. The cursor
  moves a UTF-8 character at a time. A file with CRLF line ends is edited with LF and saved with
  CRLF again (`editor_export()`).
- **Keys** come from `keys.h`; `editor_key()` returns what the caller must do: save, quit, or ask
  the terminal for its size.
- **Screen:** each character takes the columns `charwidth()` gives (two for CJK and most emoji,
  none for combining marks), a tab up to the next multiple of 8; a letter and its combining marks
  (up to `EDITOR_MAX_MARKS`, 2) move and delete as one, and a mark beyond that or with no letter
  to go on shows as `?`. A wide character cut by either edge of the screen shows as spaces. All of
  this is one function, `unit_bytes()`, so layout, cursor and drawing agree; and since each column
  holds one unit of at most 12 bytes, a row's output is bounded (`EDITOR_ROW_BYTES`, 1936 bytes at
  the 160-column limit) whatever the file holds, and fits one SSH write. Controls, invalid
  UTF-8 and the code points `termtext_hidden()` names show as `?`, so nothing in a file can send
  the terminal a sequence or hide what its bytes are. Lines don't wrap; the view scrolls
  sideways.
- **Drawing** (`editor_draw()`) only sends rows that changed (a hash of what each row shows), in
  whole rows, as many as the caller has room for, and restarts the frame if something changes
  in between. Typing a character sends one text row, the status bar and the cursor.

## `edit.c`: the command

- Reads the file whole, then runs the editor as a raw-mode shell job (`shell_run_raw()`), sending
  screen updates as the channel's free space allows (`shell_write_space()`).
- **Screen size:** on start and Ctrl-L it moves the cursor to row and column 999 and asks where it
  is (`ESC [ 6 n`); the answer is the size. The first frame waits up to 0.5 s for it.
- **Terminal:** the alternate screen, with autowrap off so a wide character can't push a row onto
  the next one; rows are erased before they're drawn, so a full row keeps its last character.
- **Saving:** through `files_write_allowed()` (so `/certs` is console-only), into `<file>.~edit`,
  then the old file is removed and the new one renamed into place. The file isn't held open
  between saves. If the rename fails the text stays "modified", so quitting warns and Ctrl-S
  tries again. Power lost between the two leaves only `<file>.~edit`: `edit <file>` then opens
  that, and says it may be incomplete (it is if the power went while a new file's first save was
  being written); if both exist it says the leftover is incomplete and will be replaced.
- **Log:** on the console, the log is held while the editor runs (`shell_raw_mode()`), and the
  status bar shows `log_pending()`, counted once a second.
