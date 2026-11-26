# Securing dev-1: runbook

For the person at the board. Do the steps in order. Each has what to type, what you should
see, and when to stop. Why each step exists is in
[`docs/securing-a-board.md`](../../securing-a-board.md); you don't need it open.

- Board: dev-1, serial `F94525FD61D9A1CC`. **No other board connected.**
- Steps marked **PERMANENT** write one-time-programmable memory. They can't be undone.
- **If what you see differs from "You should see", stop.** Don't retry, don't go on. Nothing
  here gets better by running it again.
- Run every command from the repository root, on current main.
- `<path to sos-dev-sign-2.pem>` in step 2 is the one thing to fill in.
- `picotool` 2.3.1. Wait for the board to be back up (its console answers) between commands.
- Nothing in this runbook has been run end to end. Step 5 and step 6 are the first time this
  code writes OTP on any chip.

Commands marked *(not yet run)* have the right form according to `picotool`'s help or source
but have never been executed; the others have been run on dev-1.

## Before you start

- [ ] This runbook has been approved and is on main, and so is the build change it needs
      (`-DSOS_SIGN_KEY` on the command line, PR #90). Your checkout is current main.
- [ ] A copy of both signing keys (`sos-dev-sign.pem`, `sos-dev-sign-2.pem`) exists somewhere
      other than this computer. **Without it, a lost disk means dev-1 can never be reflashed.**
- [ ] The flash guard knows both keys: `cat ~/.config/sos/flash-keys` shows the two hashes
      `e2f7cff0...0d51` and `b97c0fd7...c910`.
- [ ] Everyone else who uses the board has been told, and nobody is using it.
- [ ] The board is on a USB port and power that nobody will touch for an hour.
- [ ] You can reach the BOOTSEL button.
- [ ] A serial terminal is open on the board's USB console.

Every `picotool` command with `-f` restarts the board. From step 9 on the board has a console
password, so **after every restart the console asks for it** before it takes a command.

What will change on the board: its saved configuration, its SSH host key and its SD card's
encryption all belong to the root secret it has now. After step 6 they no longer open. You set
the board up again in step 9, and SSH clients will warn about a changed SSH host key.

## Part 1: preparation (nothing permanent)

### 1. Save the board's OTP as it is

```sh
picotool otp dump -f --ser F94525FD61D9A1CC > dev-1-otp-before.txt     # (not yet run)
```

You should see: a file of a few hundred lines. Keep it.

### 2. Build the four images, and check each one

```sh
cmake --preset debug && cmake --build build                                        # everyday firmware
cmake --preset debug -B build-burn -DSOS_BURN_IMAGE=ON && cmake --build build-burn   # the burn image
cmake --preset debug -B build-key2 -DSOS_SIGN_KEY=<path to sos-dev-sign-2.pem> && cmake --build build-key2
cmake --preset debug -B build-unsigned -DSOS_SIGN_KEY=none && cmake --build build-unsigned
```

Then ask each image which key it is signed with. This flashes nothing:

```sh
SOS_FLASH_SERIAL=F94525FD61D9A1CC tools/flash.sh -n build/sos.bin
SOS_FLASH_SERIAL=F94525FD61D9A1CC tools/flash.sh -n build-burn/sos.bin
SOS_FLASH_SERIAL=F94525FD61D9A1CC tools/flash.sh -n build-key2/sos.bin
SOS_FLASH_SERIAL=F94525FD61D9A1CC tools/flash.sh -n build-unsigned/sos.bin
```

You should see, in this order:

```
... (e2f7cff033600f5ab00e33b6f891a0ff6e635ad0f2088613ca8b71b5f56d0d51)
... (e2f7cff033600f5ab00e33b6f891a0ff6e635ad0f2088613ca8b71b5f56d0d51)
... (b97c0fd7efb68937e733c4916b44ce4f940141c4e6ecc90994c941f396d7c910)
flash guard: REFUSED. build-unsigned/sos.bin is not signed.
```

**Stop if** any of the four differs. In particular the third must show the `b97c...` hash: if
it shows `e2f7...`, the second key was not used and step 12 would prove nothing.

### 3. Walk the recovery path

This is the only way back if a later flash goes wrong, so prove it works now.

1. Unplug the board. Hold BOOTSEL. Plug it in. Let go.
2. ```sh
   picotool info --ser F94525FD61D9A1CC
   ```
   You should see: `Program Information`, then `name: SOS`.
3. ```sh
   picotool load -v -x build/sos.bin -t bin -o 0x10000000 --ser F94525FD61D9A1CC
   ```
   You should see: `OK`, and the board's console comes back.

**Stop if** the board doesn't show up or doesn't run afterwards.

### 4. Flash the burn image and look

```sh
SOS_FLASH_SERIAL=F94525FD61D9A1CC tools/flash.sh build-burn/sos.bin
```

On the console:

```
version
rootsecret burn plan
```

You should see:

```
THIS IS THE BURN IMAGE: ...
It would use OTP page 4 (rows 0x100 to 0x13f).
BOOTSEL button: not held. Nothing was written.
```

Hold BOOTSEL and run `rootsecret burn plan` again. You should see `BOOTSEL button: held`.
**Stop if** it says "not held" while you are holding it: the button isn't being read, and the
next step depends on it.

## Part 2: the root secret

Do steps 5 to 8 in one sitting. Don't leave the board between them.

### 5. Rehearsal: spend one page on purpose — PERMANENT

On the console:

```
rootsecret burn rehearse
```

It asks for the serial: type `F94525FD61D9A1CC`. It asks for a word: type `REHEARSE`. Then hold
BOOTSEL and keep it held until it says you can let go.

You should see:

```
rehearsal: stopped on purpose before the check and the marker; that page is now spent.
OTP page 4: locked against the bootloader
It took ... ms.
```

Write down the time.

**Stop if** it says anything else, in particular "a write failed or didn't read back" or
"STOP HERE". Do not run step 6. This step is the test of whether writing OTP works at all.

If the board restarts while you hold the button it comes up in the USB bootloader. That is the
button, not damage: `picotool reboot --ser F94525FD61D9A1CC` brings it back. Then run
`rootsecret burn plan` and stop.

### 6. The real root secret — PERMANENT

```
rootsecret burn
```

Serial: `F94525FD61D9A1CC`. Word: `BURN`. Hold BOOTSEL until told to let go.

You should see:

```
the root secret is written, verified and its page locked.
OTP page 5: locked against the bootloader
```

**Stop if** it says anything else.

### 7. Put the everyday firmware back

```sh
SOS_FLASH_SERIAL=F94525FD61D9A1CC ninja -C build flash
```

On the console: `rootsecret`

You should see:

```
root secret: present, in OTP page 5
its OTP page is locked until the next reset
```

The board now has no configuration (no network, no SSH, an open console). That is expected.

### 8. Check the secret can't be read over USB

```sh
picotool otp get -r -n -f --ser F94525FD61D9A1CC 0x140 0x100
```

Row 0x140 is the first row of page 5 (the secret), row 0x100 the first of page 4 (the spent
rehearsal page). You should see: both reads refused, or neither showing the row's contents.
**Stop if** it prints a 24-bit value that isn't zero for either row: that page isn't locked.

### 9. Set the board up again

On the console: `setup` (console password, Wi-Fi, your SSH public key). Then `sd format` and
type `yes`; write down the card key it shows. Then `reboot`.

You should see after the reboot: the console asks for the password you just set; then Wi-Fi
connects, and the log line `sshd: listening ... host key ...` shows the board's new SSH host
key: a fingerprint different from before. `sd` shows the card mounted.

From here on, every restart ends at the `Password:` prompt. Type it before the next console
command.

## Part 3: boot keys and secure boot

### 10. The two key hashes — PERMANENT

```sh
picotool otp load docs/otp/dev-1/1-boot-keys.json -f --ser F94525FD61D9A1CC     # (not yet run)
picotool otp get -e -n -f --ser F94525FD61D9A1CC BOOTKEY0_0 BOOTKEY0_15 BOOTKEY1_0 BOOTKEY1_15
```

You should see: `0xf7e2`, `0x510d`, `0x7cb9`, `0x10c9`, in that order.

On the console: `bootkey`. You should see:

```
this firmware: signed; its key's hash is e2f7cff033600f5ab00e33b6f891a0ff6e635ad0f2088613ca8b71b5f56d0d51
secure boot: off
boot key slot 0: e2f7cff033600f5ab00e33b6f891a0ff6e635ad0f2088613ca8b71b5f56d0d51  not marked valid
boot key slot 1: b97c0fd7efb68937e733c4916b44ce4f940141c4e6ecc90994c941f396d7c910  not marked valid
NOT YET: slot 0 holds this firmware's key but isn't marked valid. ...
```

Compare all 64 digits of both slots, not the first few.

**Stop if** a value differs. The slot can still be abandoned at this point; nothing is lost.

### 11. Mark both keys valid — PERMANENT

First a trial that writes nothing:

```sh
picotool otp load docs/otp/dev-1/0-no-change-key-valid.json -f --ser F94525FD61D9A1CC   # (not yet run)
picotool otp get -c 3 -n -f --ser F94525FD61D9A1CC BOOT_FLAGS1
```

You should see: `VALUE 0x000000`, `KEY_VALID ... = 0`. **Stop if** the load is refused or the
value changed.

Then the real one:

```sh
picotool otp load docs/otp/dev-1/2-key-valid.json -f --ser F94525FD61D9A1CC     # (not yet run)
picotool otp get -c 3 -n -f --ser F94525FD61D9A1CC BOOT_FLAGS1
```

You should see: `KEY_VALID ... = 3`, `KEY_INVALID ... = 0`.

On the console: `bootkey`. You should see a last line starting `MATCH: slot 0`.

### 12. Check the second key works

```sh
SOS_FLASH_SERIAL=F94525FD61D9A1CC tools/flash.sh build-key2/sos.bin
```

Console: `bootkey`. You should see `MATCH: slot 1`.

Then put the first key's image back:

```sh
SOS_FLASH_SERIAL=F94525FD61D9A1CC ninja -C build flash
```

Console: `bootkey`. You should see `MATCH: slot 0`.

**Stop if** either says anything but `MATCH`. Do not go on to step 13 with a key that doesn't
match: after step 13 a board whose key doesn't match never runs again.

### 13. Secure boot — PERMANENT, and the one that can't be recovered from

Check again, now:

- [ ] The key backup exists off this computer.
- [ ] `bootkey` on the console says `MATCH: slot 0`.
- [ ] ```sh
      picotool info -a -f --ser F94525FD61D9A1CC          # (not yet run in this form)
      ```
      shows `signature: verified`.

Trial that writes nothing:

```sh
picotool otp load docs/otp/dev-1/0-no-change-secure-boot.json -f --ser F94525FD61D9A1CC   # (not yet run)
picotool otp get -c 8 -n -f --ser F94525FD61D9A1CC CRIT1
```

You should see: `VALUE 0x000000`, `SECURE_BOOT_ENABLE ... = 0`. **Stop if** not.

The real one:

```sh
picotool otp load docs/otp/dev-1/3-secure-boot.json -f --ser F94525FD61D9A1CC   # (not yet run)
picotool otp get -c 8 -n -f --ser F94525FD61D9A1CC CRIT1
```

You should see: `SECURE_BOOT_ENABLE ... = 1`, every other field 0. The board restarts and its
console comes back. Console: `version`. You should see `secure boot: on`.

If the console does **not** come back: use the recovery path from step 2 with
`build/sos.bin`. If that doesn't run either, stop and don't try anything else.

### 14. Check it refuses what it should

A signed image still flashes:

```sh
SOS_FLASH_SERIAL=F94525FD61D9A1CC ninja -C build flash
```

You should see the console come back.

An unsigned image doesn't run. The guard has to be switched off for this one flash, since
refusing such an image is its job:

```sh
SOS_FLASH_UNGUARDED=1 SOS_FLASH_SERIAL=F94525FD61D9A1CC tools/flash.sh build-unsigned/sos.bin
```

You should see: the console does **not** come back. Then recover:

```sh
picotool load -v -x build/sos.bin -t bin -o 0x10000000 --ser F94525FD61D9A1CC
```

You should see: `OK`, and the console back. (If the board isn't found, unplug, hold BOOTSEL,
plug in, and repeat the command.)

### 15. Lock page 3 against the bootloader — PERMANENT

Trial that writes nothing:

```sh
picotool otp load docs/otp/dev-1/0-no-change-page3-lock.json -f --ser F94525FD61D9A1CC   # (not yet run)
picotool otp get -r -n -f --ser F94525FD61D9A1CC PAGE3_LOCK1
```

You should see: `VALUE 0x000000`. **Stop if** not.

The real one:

```sh
picotool otp load docs/otp/dev-1/4-page3-lock.json -f --ser F94525FD61D9A1CC    # (not yet run)
picotool otp get -r -n -f --ser F94525FD61D9A1CC PAGE3_LOCK1
```

You should see: `VALUE 0x101010`.

Check the lock, with a write that changes nothing even if the lock failed. **Use exactly this
command, no other value and no other row:**

```sh
picotool otp set -r 0x0ff 0 -f --ser F94525FD61D9A1CC                            # (not yet run)
```

You should see: refused.

## Afterwards

- Note in the board registry what was written and when: pages 4 and 5 (root secret, page 4
  spent), boot keys in slots 0 and 1, both valid, secure boot on, page 3 locked.
- Record on issue #64: the time from step 5, and anything that didn't match this page.
- From now on dev-1 runs only images signed with one of the two dev keys. Flash it with
  `ninja flash`; the flash guard refuses an image that isn't.

## What is left open on dev-1, on purpose

Debug is still enabled, the board still reboots into the bootloader on command, key slots 2
and 3 are blank, and pages 1 and 2 can still be written over USB. The glitch detector is on,
turned on by the firmware at every boot as before; it is not enabled in OTP. dev-1 shows that the
mechanism works. It does not keep secrets from someone with the board in their hands.
