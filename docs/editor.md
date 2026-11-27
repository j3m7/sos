# Editor, pager and manual pages

## Editor

`edit <file>` opens a file of up to 128 KB (less while memory is short) in a full-screen editor,
on the console or over SSH
(a VT100/xterm-compatible terminal; it uses the alternate screen, so the shell's output is back
afterwards). Ctrl-S saves, Ctrl-Q quits (twice with unsaved changes), Ctrl-F finds, Ctrl-L
redraws and asks the terminal for its size again. UTF-8 is fine, wide characters take two
columns, and CRLF line ends are kept.

- **Saving** writes `<file>.~edit` and then replaces the file, so an interrupted save never
  leaves half a file; after a power cut, `edit <file>` picks the text up from the `.~edit` copy.
- While the editor runs on the console, log messages wait (the status bar counts them) and appear
  when it quits; `wall` and `chat` messages wait too.
- `/certs` can be opened from SSH but saved only from the console.

The keys and details are in the manual page [man/edit.txt](../man/edit.txt) and
[`src/edit/`](../src/edit/README.md).

## Pager

`more <file>` reads a file of any size a screen at a time (a 16 KB window of it is in memory),
with less-like keys: Space and b page, j and k scroll, g and G go to the ends, `/` searches, q
quits. It draws text the way the editor does, so a file can't send the terminal a sequence, and
it is the viewer for any command with more text to show than a screen, the manual pages first
of all. The keys are in [man/more.txt](../man/more.txt).
Any command's output can be read the same way: end the line with `| more` (`dmesg | more`,
`wifi stats chip | more`). The output is kept instead of printed, up to 32 KB, and paged once
the command has finished; see [man/more.txt](../man/more.txt).

## Manual pages

`man` lists the manual pages and `man <topic>` reads one in the pager: one page per command,
written in [`man/`](../man/README.md) and kept on the SD card as `/man/<topic>.txt`. A board without
them is offered a download: `man install` fetches them from this repository on GitHub over HTTPS,
matching the firmware's commit; `tools/man-install.sh <host>` copies them over SFTP instead. See
[man/README.md](../man/README.md) and [man/man.txt](../man/man.txt).
