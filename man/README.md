# man: the manual pages

One plain-text page per command, `man/<topic>.txt`, read on the board with `man <topic>`
(`man` alone lists them), and rendered into [`docs/commands.md`](../docs/commands.md) as the
command reference, so each command is documented once. The firmware holds no page: they live on the SD card in `/man`, put
there by `man install` on the board, which downloads `man/pages.bundle` from this repository
on GitHub (the commit the firmware was built from, or `main`), or by `tools/man-install.sh
<host>` over SFTP.

## Writing a page

```
NAME
    wifi - Wi-Fi networks, state and radio settings

SYNOPSIS
    wifi
    wifi set | add | remove <n> | prefer <n> | connect

DESCRIPTION
    ...

CONSOLE ONLY
    ...

SEE ALSO
    net, setup, config
```

- The topic is the command's name: lower-case letters, digits, `-` and `_`. The file starts
  with `NAME` and then `    <topic> - <one-line description>`; the listing prints that line.
- Plain text, UTF-8 if needed, indented 4 spaces under section headings in capitals, at most 80
  columns, no tabs, and no line starting with `@@` (the bundle's page marker). The pager shows
  the page as it is: no markup is interpreted.
- Say what the command does, its forms, which are console only, what it changes and where that
  is saved, and the pitfalls; point to related pages under SEE ALSO. The pages are the one place
  a command is documented: `docs/commands.md` is generated from them.
- SYNOPSIS lists the command's forms, one per line, each marked `(console only)` or `(change:
  console only)` where that applies; `docs/commands.md` takes its *Where* column from the marks.
- A new page goes into `man/index.txt`, under the section of `docs/commands.md` it belongs to.
- Every line below a heading is indented four spaces; a line that isn't is a heading, in
  capitals (the generator stops on anything else).
- After adding or changing a page, run `tools/man-bundle.sh` (bash and python3): it checks the
  pages and rebuilds
  `man/pages.bundle` and `docs/commands.md` (`tools/man-docs.py`), both committed. The host tests
  (`just test`) fail while either is out of date or a page breaks a rule above.

## On the board

`man install` writes each page to `/man/<topic>.~new` and renames it into place when it is
complete, so a download that fails midway leaves the earlier pages whole; it says how many pages
it installed. `man <topic>` pages the file through `more`'s pager (`src/edit/view.c`), so a page
can't send the terminal a sequence. See `src/man/man.c`.
