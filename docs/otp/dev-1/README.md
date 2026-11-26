# OTP files for dev-1

**Every `picotool otp load` here is permanent. None of these files has been loaded into any
board.** They are for the development board dev-1, serial `F94525FD61D9A1CC`, and for no other
board: the keys in them are development keys that every session on the development machine
can read.

**To run them, follow [`RUNBOOK.md`](RUNBOOK.md).** This page describes the files and how they
were checked.

They belong to Part 2 (dev-1) of [`docs/securing-a-board.md`](../../securing-a-board.md),
which says when each is loaded, what must be true first, and who loads it: the board's owner,
one file at a time, each named. Nothing here replaces that document's gates.

The root secret is not in a file: it is written on the board by the burn image
(`rootsecret burn`), which also locks the root secret's pages (4, 5, 6).

## The files

| Step | File | What it writes | Rows |
|---|---|---|---|
| D4.1 | `1-boot-keys.json` | The SHA-256 hash of each dev signing key's public key: slot 0 and slot 1 | 0x080 to 0x09f (page 2), error-corrected |
| D4.2 | `2-key-valid.json` | `BOOT_FLAGS1.KEY_VALID` = 3: slots 0 and 1 valid. Nothing else in the boot flags | 0x04b and its two copies |
| D4.7 | `3-secure-boot.json` | `CRIT1.SECURE_BOOT_ENABLE`. **The last bit: after it the board runs only firmware signed with one of the two keys** | 0x040 and its seven copies |
| D4.8 | `4-page3-lock.json` | Page 3's lock word: the bootloader may read page 3, not write it | 0xf87 |

Three more files write **nothing**: each has the same form as one of the files above with
every field 0, and is loaded just before it as a trial (see "A trial load that changes
nothing"). They are `0-no-change-key-valid.json`, `0-no-change-secure-boot.json` and
`0-no-change-page3-lock.json`. There is none for the boot keys.

Not written on dev-1, by the owner's decisions: `KEY_INVALID` for slots 2 and 3, the debug
disable bits, `DISABLE_WATCHDOG_SCRATCH`, the glitch detector bits, the sealed-only marker,
and any lock on pages 1 and 2.

The key hashes, as `bootkey` prints them on the board and as `tools/flash.sh` lists them:

```
slot 0  e2f7cff033600f5ab00e33b6f891a0ff6e635ad0f2088613ca8b71b5f56d0d51   sos-dev-sign.pem
slot 1  b97c0fd7efb68937e733c4916b44ce4f940141c4e6ecc90994c941f396d7c910   sos-dev-sign-2.pem
```

In the file they are the same 32 bytes in the same order, as decimal numbers: that is the
order `picotool` itself writes them in when it makes such a file. To check a file against a
key without trusting this page:

```sh
openssl ec -in <key>.pem -pubout -outform DER | tail -c 64 | sha256sum
python3 -c "import json; d=json.load(open('1-boot-keys.json')); print(bytes(d['bootkey0']).hex()); print(bytes(d['bootkey1']).hex())"
```

## The commands

`picotool` 2.3.1. It takes its options in the order of its synopsis: the options, then the
device selection (`-f --ser ...`), then the row selectors; in another order it answers "No
accessible RP2350 devices in BOOTSEL mode were found". `-f` reboots the running board into the
USB bootloader for the command and back afterwards. Wait for the board to be back up between
commands.

Every command names the board. Only dev-1 is connected.

### Before anything: what the board holds now

Read on 2026-11-25, with nothing yet written. These are the read-back commands used after
each step too, with what they print on the blank board.

```sh
picotool otp get -e -n -f --ser F94525FD61D9A1CC BOOTKEY0_0 BOOTKEY0_15 BOOTKEY1_0 BOOTKEY1_15
#   each: VALUE 0x0000
picotool otp get -c 3 -n -f --ser F94525FD61D9A1CC BOOT_FLAGS1
#   VALUE 0x000000, KEY_INVALID = 0, KEY_VALID = 0
picotool otp get -c 8 -n -f --ser F94525FD61D9A1CC CRIT1
#   VALUE 0x000000, DEBUG_DISABLE = 0, SECURE_DEBUG_DISABLE = 0, SECURE_BOOT_ENABLE = 0
picotool otp get -r -n -f --ser F94525FD61D9A1CC PAGE1_LOCK1 PAGE2_LOCK1 PAGE3_LOCK1 PAGE4_LOCK1 PAGE5_LOCK1 PAGE6_LOCK1
#   pages 1 and 2: VALUE 0x040404 (LOCK_NS = 1, as the factory leaves them; LOCK_BL = 0, LOCK_S = 0)
#   pages 3 to 6:  VALUE 0x000000
picotool otp get -r -n -f --ser F94525FD61D9A1CC 0x0c0 0x0c1 0x0c2 0x100 0x112
#   each: VALUE 0x000000 (the sealed-only marker's rows, and two rows of the root secret's first page)
```

Also, once, before the first write: `picotool otp dump -f --ser F94525FD61D9A1CC > dev-1-before.txt`,
kept as the record of the board's state.

### A trial load that changes nothing

`picotool` has no dry run for `otp load`. But OTP bits only go from 0 to 1, so loading a file
whose every field is 0 writes nothing, while it goes through everything else the real load
does: reading the file, finding the row and field names, the redundant copies, and the
bootloader's permission to write the page. So before each of D4.2, D4.7 and D4.8 the owner
loads the matching `0-no-change-...` file, like any other load, and runs the same read-back:
**the row must read exactly as it did before.** If `picotool` refuses the trial file, it would
have refused the real one, and nothing has been spent finding that out.

There is no trial for the boot keys (D4.1). Those rows are written with error correction,
and whether a write of zero leaves a row's correction bits blank is not established; a trial
that might write is not a trial. D4.1 is the first real write, and a slot written wrong can
still be abandoned at that point.

### D4.1: the boot key hashes

```sh
picotool otp load docs/otp/dev-1/1-boot-keys.json -f --ser F94525FD61D9A1CC
```

Then read all 32 rows back and compare with the two hashes above. Row *k* of a slot holds
bytes 2*k* and 2*k*+1, low byte first: the first row of slot 0 must read `0xf7e2`, its last
`0x510d`; the first row of slot 1 `0x7cb9`, its last `0x10c9`.

```sh
picotool otp get -e -n -f --ser F94525FD61D9A1CC 2:
```

On the board: `bootkey` must print the same two hashes, and `NOT YET` for the firmware's own
key.

### D4.2: mark slots 0 and 1 valid

```sh
# trial, changes nothing: BOOT_FLAGS1 must read 0x000000 after it, as before
picotool otp load docs/otp/dev-1/0-no-change-key-valid.json -f --ser F94525FD61D9A1CC
picotool otp get -c 3 -n -f --ser F94525FD61D9A1CC BOOT_FLAGS1

picotool otp load docs/otp/dev-1/2-key-valid.json -f --ser F94525FD61D9A1CC
picotool otp get -c 3 -n -f --ser F94525FD61D9A1CC BOOT_FLAGS1
#   KEY_VALID = 3, KEY_INVALID = 0, everything else 0
```

On the board: `bootkey` says `MATCH`, slot 0. Flash an image signed with the second key:
`MATCH`, slot 1. Flash the first key's image back.

### D4.7: secure boot

Only after the gates in the procedure, the owner's confirmation of the key backup among them.

```sh
# trial, changes nothing: CRIT1 must read 0x000000 after it, as before
picotool otp load docs/otp/dev-1/0-no-change-secure-boot.json -f --ser F94525FD61D9A1CC
picotool otp get -c 8 -n -f --ser F94525FD61D9A1CC CRIT1

picotool otp load docs/otp/dev-1/3-secure-boot.json -f --ser F94525FD61D9A1CC
picotool otp get -c 8 -n -f --ser F94525FD61D9A1CC CRIT1
#   SECURE_BOOT_ENABLE = 1, every other field 0
```

### D4.8: lock page 3 against the bootloader

```sh
# trial, changes nothing: PAGE3_LOCK1 must read 0x000000 after it, as before
picotool otp load docs/otp/dev-1/0-no-change-page3-lock.json -f --ser F94525FD61D9A1CC
picotool otp get -r -n -f --ser F94525FD61D9A1CC PAGE3_LOCK1

picotool otp load docs/otp/dev-1/4-page3-lock.json -f --ser F94525FD61D9A1CC
picotool otp get -r -n -f --ser F94525FD61D9A1CC PAGE3_LOCK1 PAGE4_LOCK1 PAGE5_LOCK1 PAGE1_LOCK1 PAGE2_LOCK1
#   page 3: VALUE 0x101010 (LOCK_BL = 1)
#   pages 4 and 5: as the burn left them, 0x3d3d3d on each page it wrote
#   pages 1 and 2: still 0x040404
```

The check that it took, and that the open pages are open. **The write below is chosen so that
it can change nothing even if the lock did not take:** it is a raw write of the value 0 (no
bit set) to row 0x0ff, the last row of page 3, far from the sealed-only marker's rows
(0x0c0 to 0x0c2), which are in this page and must never be written on dev-1.

```sh
picotool otp set -r 0x0ff 0 -f --ser F94525FD61D9A1CC
#   must be REFUSED. (If it is accepted, the lock did not take; no bit was changed. Stop.)
picotool otp get -r -n -f --ser F94525FD61D9A1CC 0x0ff 0x0c0 0x0c1 0x0c2
#   each: VALUE 0x000000
picotool otp get -c 3 -n -f --ser F94525FD61D9A1CC BOOT_FLAGS1
#   answered: page 1 is open, as intended
```

Do not test the lock with any other value or any other row.

## What has and hasn't been checked

- **Checked:** the two hashes against the key files, with `openssl`, and against what `bootkey`
  prints for an image signed with the first key. The JSON form of the first three files
  against a file `picotool seal` writes (same names, same byte order; such a file was made
  with a throwaway key to compare, and not kept). Every read command above, run on dev-1.
  The selectors with `picotool otp list`.
- **Read in `picotool` 2.3.1's source by the reviewer** (`process_otp_json`), not run: a field
  given by name is read, modified and written back, with a check that the value fits the
  field, so `key_valid` = 3 is accepted; one value is repeated across a row's 3 or 8 redundant
  copies; `page3_lock1` with `lock_bl`, `r1` and `r2` composes to 0x101010 in one row; and
  `bootkey0` as 32 numbers is 16 error-corrected rows, low byte first.
- **Not checked, because it can only be checked by loading:** that `picotool otp load` accepts
  each file as written and does what the source reading says. The trial loads are how that
  is found out without spending anything: for the key-valid, secure-boot and lock files. For
  the boot key file there is no trial.
- The form of `picotool otp set` in D4.8's check is from `picotool help otp set`
  (`[-r] <selector> <value> [device-selection]`); it has not been run.
- The second key has not yet signed an image that ran on the board.
