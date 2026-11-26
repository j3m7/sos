#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
"""A pretend board for tools/test/secure-board-test.py: a pretend `picotool`, and the pretend
board's console. NOT a tool: it talks to no hardware.

The board is a JSON file (the "world", $SOS_FAKE_WORLD): its OTP rows, what firmware it runs,
whether it sits in the bootloader, and the faults a test asked for. OTP behaves as on the chip:
a bit once set stays set; a page whose lock word says the bootloader may not write refuses a
write from picotool; with secure boot on, firmware not signed with a valid key doesn't start.

Run as `picotool ...` it answers the picotool commands tools/secure-board.py and tools/flash.sh
use, in picotool 2.3.1's output format. serve_console() is the board's console on a pty.
"""

import hashlib
import json
import os
import re
import sys

NAMES = {"BOOT_FLAGS1": 0x4B, "CRIT1": 0x40}
for _page in range(64):
    NAMES["PAGE%d_LOCK1" % _page] = 0xF81 + 2 * _page
for _slot in range(4):
    for _k in range(16):
        NAMES["BOOTKEY%d_%d" % (_slot, _k)] = 0x80 + 16 * _slot + _k


def load_world():
    with open(os.environ["SOS_FAKE_WORLD"]) as f:
        return json.load(f)


class world_lock:
    """One writer of the world at a time: the pretend picotool, the pretend console and the
    test (the person's hands) all change it."""

    def __enter__(self):
        import fcntl
        self.f = open(os.environ["SOS_FAKE_WORLD"] + ".lock", "w")
        fcntl.flock(self.f, fcntl.LOCK_EX)

    def __exit__(self, *exc):
        self.f.close()


def unique():
    import threading
    return "%d.%d" % (os.getpid(), threading.get_ident())


def save_world(w):
    path = os.environ["SOS_FAKE_WORLD"]
    tmp = "%s.%s.tmp" % (path, unique())
    with open(tmp, "w") as f:
        json.dump(w, f)
    os.replace(tmp, path)
    show_on_usb(w)


def show_on_usb(w):
    """What the computer sees of the board, as on the real thing: the USB listing says whether
    each board runs or sits in its bootloader, and the console device exists only while the
    board runs. (A console that was always there hid a real fault once.)"""
    if w.get("usb_file"):
        mode = "running" if w["mode"] == "app" else "bootloader"
        tmp = "%s.%s.tmp" % (w["usb_file"], unique())
        with open(tmp, "w") as f:
            json.dump([[s, mode if s == w["serial"] else "running"] for s in w["boards"]], f)
        os.replace(tmp, w["usb_file"])
    link = w.get("tty_link")
    if link:
        there = os.path.islink(link)
        if w["mode"] == "app" and w["serial"] in w["boards"] and not there:
            os.symlink(w["pty"], link)
        elif (w["mode"] != "app" or w["serial"] not in w["boards"]) and there:
            os.unlink(link)


def row(w, n):
    return w["otp"].get(str(n), 0)


def lock_byte(w, page):
    v = row(w, 0xF81 + 2 * page)
    a, b, c = v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF
    return (a & b) | (a & c) | (b & c)


def bl_lock(w, page):
    return lock_byte(w, page) >> 4 & 3      # 0 read-write, 1 read-only, 3 inaccessible


def vote(w, first, copies, need):
    out = 0
    for bit in range(24):
        if sum(row(w, first + i) >> bit & 1 for i in range(copies)) >= need:
            out |= 1 << bit
    return out


def key_valid(w):
    return vote(w, 0x4B, 3, 2) & 0xF


def secure_boot(w):
    return vote(w, 0x40, 8, 3) & 1


def slot_hash(w, slot):
    return b"".join(bytes([row(w, 0x80 + 16 * slot + k) & 0xFF, row(w, 0x80 + 16 * slot + k) >> 8 & 0xFF])
                    for k in range(16)).hex()


def accepted(w, key):
    """Would the boot ROM start an image signed with `key` (None: unsigned)?"""
    if "runs_anything" in w["faults"] or not secure_boot(w):
        return True
    return key is not None and any(key_valid(w) >> s & 1 and slot_hash(w, s) == key for s in range(4))


def write_row(w, n, value, raw):
    """Program a row from the bootloader. Returns an error text, or None."""
    if bl_lock(w, n // 64 if n < 0xF80 else (n - 0xF80) // 2) != 0:
        return "ERROR: Failed to write OTP row 0x%04x: permission failure" % n
    have = row(w, n)
    if have & ~value & 0xFFFFFF:
        return "ERROR: Failed to write OTP row 0x%04x: cannot clear bits" % n
    w["otp"][str(n)] = (have | value) & 0xFFFFFF
    return None


def image_info(path):
    data = open(path, "rb").read()
    key = None
    i = data.find(b"\x09\x21\x00\x01")
    if i >= 0:
        key = hashlib.sha256(data[i + 4:i + 68]).hexdigest()
    return {"key": key, "burn": b"BURNIMAGE" in data}


def restart(w):
    """The board restarts: it runs its firmware if the boot ROM accepts it, else the bootloader."""
    w["restarts"] = w.get("restarts", 0) + 1
    w["mode"] = "app" if accepted(w, w["running"]["key"]) else "bootsel"
    w["button"] = w.get("button", False)


# --- picotool ----------------------------------------------------------------------------------------

def device_ok(w, args):
    """The device selection: --ser must name an attached board; a running board needs -f."""
    ser = args[args.index("--ser") + 1] if "--ser" in args else None
    if not w["boards"]:
        return "No accessible RP2350 devices in BOOTSEL mode were found."
    if ser and ser not in w["boards"]:
        return "No accessible RP-series devices in BOOTSEL mode were found with serial number\n%s." % ser
    if len(w["boards"]) > 1 and not ser:
        return "Multiple RP-series devices found."
    if w["mode"] == "app" and "-f" not in args and "-F" not in args:
        return "No accessible RP2350 devices in BOOTSEL mode were found.\n\nbut:\n\nconsider -f"
    return None


def selectors(args):
    """What is left of an `otp get` after its options and the device selection."""
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-c", "--ser"):
            skip = True
            continue
        if a.startswith("-"):
            continue
        out.append(a)
    return out


def rows_for(sel):
    if sel in NAMES:
        return [(NAMES[sel], sel)]
    m = re.fullmatch(r"(\d+):", sel)
    if m:
        page = int(m.group(1))
        back = {v: k for k, v in NAMES.items()}
        return [(page * 64 + i, back.get(page * 64 + i, "")) for i in range(64)]
    return [(int(sel, 16), "")]


def otp_get(w, args):
    # picotool wants its options, then the device, then the selectors: in another order it
    # fails to find the device. The pretend one is as strict, so the script can't get lazy.
    seen_device = False
    for a in args:
        if a in ("-f", "--ser"):
            seen_device = True
        elif a in ("-r", "-e", "-n", "-c") and seen_device:
            return 1, "No accessible RP2350 devices in BOOTSEL mode were found."
    err = device_ok(w, args)
    if err:
        return 1, err
    out = []
    copies = int(args[args.index("-c") + 1]) if "-c" in args else 1
    for sel in selectors(args):
        for n, name in rows_for(sel):
            page = n // 64
            if n < 0xF80 and bl_lock(w, page) == 3:
                out.append("ROW 0x%04x%s\n    ERROR: no permission to read this row" % (n, ": OTP_DATA_" + name if name else ""))
                continue
            if "-e" in args:
                out.append("ROW 0x%04x: OTP_DATA_%s (ECC)\n    VALUE 0x%04x" % (n, name, row(w, n) & 0xFFFF))
            elif copies > 1:
                v = vote(w, n, copies, 3 if copies == 8 else 2)
                out.append("ROW 0x%04x: OTP_DATA_%s\n    VALUE 0x%06x" % (n, name, v))
                if name == "BOOT_FLAGS1":
                    out.append("    field DOUBLE_TAP (bit 19) = %d" % (v >> 19 & 1))
                    out.append("    field KEY_INVALID (bits 8-11) = %d" % (v >> 8 & 0xF))
                    out.append("    field KEY_VALID (bits 0-3) = %d" % (v & 0xF))
                if name == "CRIT1":
                    for fname, bit in (("GLITCH_DETECTOR_ENABLE", 4), ("BOOT_ARCH", 3), ("DEBUG_DISABLE", 2),
                                       ("SECURE_DEBUG_DISABLE", 1), ("SECURE_BOOT_ENABLE", 0)):
                        out.append("    field %s (bit %d) = %d" % (fname, bit, v >> bit & 1))
            else:
                out.append("ROW 0x%04x%s\n    VALUE 0x%06x" % (n, ": OTP_DATA_" + name if name else "", row(w, n)))
    restart(w)
    return 0, "\n".join(out) + "\n\nThe device was asked to reboot back into application mode."


def otp_load(w, args):
    err = device_ok(w, args)
    if err:
        return 1, err
    data = json.load(open(args[0]))
    w["otp_loads"].append(os.path.basename(args[0]))
    if "load_refused" in w["faults"]:
        return 1, "ERROR: could not parse OTP JSON"
    writes = []
    for name, value in data.items():
        m = re.fullmatch(r"bootkey(\d)", name)
        if m:
            slot = int(m.group(1))
            if "wrong_key_written" in w["faults"]:
                value = [value[0] ^ 1] + value[1:]
            for k in range(16):
                writes.append((0x80 + 16 * slot + k, value[2 * k] | value[2 * k + 1] << 8))
        elif name == "boot_flags1":
            v = value.get("key_valid", 0)
            if "trial_writes" in w["faults"] and v == 0:
                v = 1
            for i in range(3):
                writes.append((0x4B + i, v))
        elif name == "crit1":
            v = value.get("secure_boot_enable", 0)
            for i in range(8):
                writes.append((0x40 + i, v))
        else:
            m = re.fullmatch(r"page(\d+)_lock1", name)
            if not m:
                return 1, "ERROR: unknown row %s" % name
            v = value.get("lock_bl", 0) << 4 | value.get("r1", 0) << 8 | value.get("r2", 0) << 16
            if "lock_does_not_take" in w["faults"]:
                v = 0
            writes.append((0xF81 + 2 * int(m.group(1)), v))
    for n, v in writes:
        err = write_row(w, n, v, True)
        if err:
            return 1, err
    restart(w)
    return 0, "  OK\n\nThe device was asked to reboot back into application mode."


def otp_set(w, args):
    err = device_ok(w, args)
    if err:
        return 1, err
    pos = [a for a in selectors(args)]
    n, value = int(pos[0], 16), int(pos[1], 0)
    w["otp_sets"].append([n, value])
    err = write_row(w, n, value, True)
    restart(w)
    if err:
        return 1, err
    return 0, "  OK"


def flash(w, args):
    err = device_ok(w, args)
    if err:
        return 1, err
    info = image_info(args[[i for i, a in enumerate(args) if a.endswith(".bin")][0]])
    w["running"] = info
    w["flashed"].append(info)
    restart(w)
    return 0, "  OK\n\nThe device was rebooted to start the application."


def picotool(argv):
    with world_lock():
        return picotool_locked(argv)


def picotool_locked(argv):
    w = load_world()
    if argv[:1] == ["version"]:
        print("picotool v%s (Linux, pretend)" % w.get("picotool", "2.3.1"))
        return 0
    if argv[:1] == ["info"]:
        err = device_ok(w, argv[1:])
        if err:
            print(err)
            return 1
        print("Program Information\n name:          SOS\n description:   Small/Secure OS")
        if "-a" in argv:
            print(" signature:           %s" % ("verified" if w["running"]["key"] else "not present"))
        if "-f" in argv:
            restart(w)
            save_world(w)
        return 0
    if argv[:2] == ["otp", "get"]:
        rc, out = otp_get(w, argv[2:])
    elif argv[:2] == ["otp", "dump"]:
        err = device_ok(w, argv[2:])
        rc, out = (1, err) if err else (0, "\n".join("0x%04x: %06x" % (int(n), v) for n, v in sorted(w["otp"].items())))
        if not err:
            restart(w)
    elif argv[:2] == ["otp", "load"]:
        rc, out = otp_load(w, argv[2:])
    elif argv[:2] == ["otp", "set"]:
        rc, out = otp_set(w, argv[2:])
    elif argv[:1] == ["load"]:
        rc, out = flash(w, argv[1:])
    else:
        print("pretend picotool: unknown command %r" % argv)
        return 9
    save_world(w)
    print(out)
    return rc


# --- the board's console -----------------------------------------------------------------------------

BLANK = "00" * 32


def root_secret_page(w):
    """As core/roototp.c finds it: the first list page with the marker."""
    for page in w["pages"]:
        if row(w, page * 64 + 18) & 0xFFFF == 0xA55A:
            return page
    return None


def first_blank_page(w):
    for page in w["pages"]:
        if not any(row(w, page * 64 + i) for i in range(64)):
            return page
    return None


def console_answer(w, line, state):
    """One command line -> (text, world changed). `state` holds a burn dialogue in progress."""
    fw = w["running"]
    if state.get("ask") == "serial":
        if line.upper() != w["serial"]:
            state.clear()
            return "that isn't this board's serial: nothing was written\n", False
        state["ask"] = "word"
        return ("Type REHEARSE to spend one OTP page on purpose: " if state["rehearse"]
                else "Type BURN to write to this board's OTP: "), False
    if state.get("ask") == "word":
        rehearse = state["rehearse"]
        state.clear()
        if line != ("REHEARSE" if rehearse else "BURN"):
            return "not confirmed: nothing was written\n", False
        out = "Hold the BOOTSEL button now, and keep it held until told to let go (30 s to start; Ctrl-C cancels).\n"
        # As the image does, give the person a moment to get a finger on the button.
        import time
        waited = 0
        while not load_world().get("button") and waited < 40:
            time.sleep(0.05)
            waited += 1
        if not load_world().get("button"):
            return out + "\nthe button wasn't held in time: nothing was written\n", False
        out += "\nwriting OTP: keep the button held and don't unplug the board...\n"
        page = first_blank_page(w)
        if "burn_fails" in w["faults"] or ("real_burn_fails" in w["faults"] and not rehearse):
            w["otp"][str(page * 64)] = 0x212345
            w["otp"][str(0xF81 + 2 * page)] = 0x3D3D3D
            return (out + "\na write failed or didn't read back.\nOTP page %d: locked against the bootloader\n"
                    "It stopped at row 0x%03x.\n\nSTOP HERE. Do not run `rootsecret burn` again.\nIt took 12 ms.\n"
                    "You can let go of the button.\n" % (page, page * 64 + 1)), True
        for k in range(16):
            w["otp"][str(page * 64 + k)] = 0x210000 | (k * 257 + page)
        if not rehearse:
            w["otp"][str(page * 64 + 18)] = 0x21A55A
        if "burn_leaves_unlocked" not in w["faults"]:
            w["otp"][str(0xF81 + 2 * page)] = 0x3D3D3D
        text = ("rehearsal: stopped on purpose before the check and the marker; that page is now spent"
                if rehearse else "the root secret is written, verified and its page locked")
        if "burn_silent" in w["faults"]:
            w["faults"].remove("burn_silent")
            state["mute"] = True             # the page is written and locked; nothing is said
            return "", True
        if "burn_says_other" in w["faults"]:
            text = "what was written didn't verify: that page is spent"
        locked = "locked against the bootloader" if "burn_leaves_unlocked" not in w["faults"] else "NOT LOCKED"
        return (out + "\n%s.\nOTP page %d: %s\nIt took 345 ms.\nYou can let go of the button.\n"
                % (text, page, locked)), True
    if line == "version":
        out = "SOS pretend\n"
        if fw["burn"]:
            out += "THIS IS THE BURN IMAGE: it can write OTP (`rootsecret burn`). Not for everyday use.\n"
        out += "board: serial %s\nchip: RP2350 A2 (revision 2, ROM 2)\n" % w["serial"]
        out += "secure boot: %s\ndebug port: enabled\n" % ("on (only firmware signed with a boot key in OTP starts)"
                                                          if secure_boot(w) else "off (open board: any firmware starts)")
        return out, False
    if line == "rootsecret":
        page = root_secret_page(w)
        if page is None:
            return "root secret: present, in flash (open board: it isn't protected from someone who has the board)\n", False
        if "root_secret_from_flash" in w["faults"]:
            return "root secret: present, in flash (open board: it isn't protected from someone who has the board)\n", False
        return "root secret: present, in OTP page %d\nits OTP page is locked until the next reset\n" % page, False
    if line == "rootsecret burn plan" and fw["burn"]:
        page = first_blank_page(w)
        if root_secret_page(w) is not None:
            return "this board already has a root secret in OTP, locked: nothing was written.\n", False
        held = w.get("button") and "button_not_read" not in w["faults"]
        return ("It would use OTP page %d (rows 0x%03x to 0x%03x).\nBOOTSEL button: %s. Nothing was written.\n"
                % (page, page * 64, page * 64 + 63, "held" if held else "not held")), False
    if line in ("rootsecret burn", "rootsecret burn rehearse") and fw["burn"]:
        state["ask"], state["rehearse"] = "serial", line.endswith("rehearse")
        return ("It would use OTP page %d.\n\nTHIS WRITES ONE-TIME-PROGRAMMABLE MEMORY. IT CANNOT BE UNDONE.\n"
                "Type this board's serial number to go on: " % first_blank_page(w)), False
    if line == "bootkey":
        key = fw["key"]
        if "bootkey_lies" in w["faults"]:
            key = "ab" * 32
        out = ("this firmware: signed; its key's hash is %s\n" % key) if key else "this firmware: not signed\n"
        out += "secure boot: %s\n" % ("on" if secure_boot(w) else "off")
        verdict = "NO MATCH: no boot key slot holds this firmware's key."
        any_slot = False
        for s in range(4):
            h = slot_hash(w, s)
            if h == BLANK:
                continue
            any_slot = True
            valid = key_valid(w) >> s & 1
            out += "boot key slot %d: %s  %s\n" % (s, h, "valid" if valid else "not marked valid")
            if h == fw["key"]:
                verdict = ("MATCH: slot %d holds the hash of this firmware's key and is valid." % s if valid else
                           "NOT YET: slot %d holds this firmware's key but isn't marked valid." % s)
        if not any_slot:
            out += "boot key slots in OTP: all four blank\n"
        return out + verdict + "\n", False
    return "unknown command: %s\n" % line, False


def serve_console(master_fd, stop):
    """The board's console on a pty, until stop() is true. Reads the world for every line."""
    import select
    buf, state, last_restart = b"", {}, None
    while not stop():
        r, _, _ = select.select([master_fd], [], [], 0.05)
        try:
            w = load_world()
        except (OSError, ValueError):
            continue
        if w.get("restarts") != last_restart:
            last_restart, state, buf = w.get("restarts"), {}, b""
        if not r:
            continue
        try:
            data = os.read(master_fd, 4096)
        except OSError:
            return
        if w["mode"] != "app":
            continue                     # in the bootloader: no console
        for ch in data:
            if ch == 3:                  # Ctrl-C
                buf, state = b"", {}
                os.write(master_fd, b"^C\r\nsos> ")
            elif ch in (13, 10):
                line = buf.decode(errors="replace").strip()
                buf = b""
                if "console_locked" in w["faults"]:
                    os.write(master_fd, b"\r\nPassword: ")
                    continue
                if not line and not state:
                    os.write(master_fd, b"\r\nsos> ")
                    continue
                text, changed = console_answer(w, line, state)
                if changed:                  # a burn: only OTP changed; keep what others changed
                    with world_lock():
                        now = load_world()
                        now["otp"] = w["otp"]
                        now["faults"] = w["faults"]
                        save_world(now)
                if state.pop("mute", None):
                    continue
                text = text.replace("\n", "\r\n")
                if not state:
                    text += "sos> "
                    if changed and "log_after_prompt" in w["faults"]:
                        text += "\r\n[   12.345] wifi: no settings\r\n"
                os.write(master_fd, (line + "\r\n" + text).encode())
            else:
                buf += bytes([ch])


if __name__ == "__main__":
    sys.exit(picotool(sys.argv[1:]))
