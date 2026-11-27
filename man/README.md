# man: the manual pages

One plain-text page per command, `man/<topic>.txt`, read on the board with `man <topic>`
(`man` alone lists them). The firmware holds no page: they live on the SD card in `/man`, put
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
  is saved, and the pitfalls; point to related pages under SEE ALSO. `docs/commands.md` is the
  reference the pages are written from: a change to a command updates both.
- After adding or changing a page, run `tools/man-bundle.sh`: it checks the pages and rebuilds
  `man/pages.bundle`, which is committed. The host tests (`just test`) fail while the bundle is
  out of date or a page breaks a rule above.

## On the board

`man install` writes each page to `/man/<topic>.~new` and renames it into place when it is
complete, so a download that fails midway leaves the earlier pages whole; it says how many pages
it installed. `man <topic>` pages the file through `more`'s pager (`src/edit/view.c`), so a page
can't send the terminal a sequence. See `src/man/man.c`.
