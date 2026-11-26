#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
"""Tests of tools/secure-board.py against a pretend board (secure_board_fake.py): a pretend
picotool, a pretend console on a pty, and images built here from the block format.

NOTHING HERE TOUCHES A REAL BOARD. The script is pointed at the pretend picotool, a pretend USB
listing and a pretend console through its SOS_SECURE_BOARD_* and SOS_PICOTOOL variables, with
a test profile whose serial is no board's. The "person" at the terminal is this test, typing
on a pty; that is how the steps that need a person are exercised without one.

Usage: tools/test/secure-board-test.py [-v] [TestName ...]
"""

import hashlib
import json
import os
import pty
import re
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import secure_board_fake as fake  # noqa: E402

SERIAL = "AAAAAAAAAAAAAAAA"
KEY1, KEY2 = bytes([0x11]) * 64, bytes([0x22]) * 64
HASH1, HASH2 = hashlib.sha256(KEY1).hexdigest(), hashlib.sha256(KEY2).hexdigest()
PERMANENT_WORDS = ["REHEARSE", "BURN", "KEYS", "VALID", "SECUREBOOT", "LOCK"]


def image(key, burn=False):
    start, imagedef, end = bytes.fromhex("d3deffff"), bytes.fromhex("42012110"), bytes.fromhex("00000000793512ab")
    if key is None:
        body = start + imagedef + bytes.fromhex("ff010000") + end
    else:
        body = start + imagedef + bytes.fromhex("09210001") + key + bytes([0xEE]) * 64 + bytes.fromhex("ff220000") + end
    return body + (b"BURNIMAGE" if burn else b"")


class Board(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="secure-board-test-")
        t = self.tmp
        # the pretend picotool, first on PATH (tools/flash.sh calls `picotool`)
        os.mkdir(os.path.join(t, "bin"))
        with open(os.path.join(t, "bin", "picotool"), "w") as f:
            f.write("#!/bin/sh\nexec %s %s \"$@\"\n" % (sys.executable, os.path.join(HERE, "secure_board_fake.py")))
        os.chmod(os.path.join(t, "bin", "picotool"), 0o755)
        # images
        for name, data in (("build", image(KEY1)), ("build-burn", image(KEY1, burn=True)),
                           ("build-key2", image(KEY2)), ("build-unsigned", image(None))):
            os.makedirs(os.path.join(t, "images", name))
            with open(os.path.join(t, "images", name, "sos.bin"), "wb") as f:
                f.write(data)
        # the profile and its OTP files
        src = os.path.join(ROOT, "docs", "otp", "dev-1")
        dst = os.path.join(t, "profiles", "test-board")
        os.makedirs(dst)
        profile = json.load(open(os.path.join(src, "profile.json")))
        profile.update(name="test-board", serial=SERIAL,
                       keys=[{"slot": 0, "hash": HASH1}, {"slot": 1, "hash": HASH2}])
        json.dump(profile, open(os.path.join(dst, "profile.json"), "w"))
        self.profile = profile
        for name in profile["otp_files"].values():
            shutil.copy(os.path.join(src, name), dst)
        json.dump({"bootkey0": list(bytes.fromhex(HASH1)), "bootkey1": list(bytes.fromhex(HASH2))},
                  open(os.path.join(dst, profile["otp_files"]["boot_keys"]), "w"))
        with open(os.path.join(t, "flash-keys"), "w") as f:
            f.write("%s\n%s\n" % (HASH1, HASH2))
        # the board
        self.world_path = os.path.join(t, "world.json")
        os.environ["SOS_FAKE_WORLD"] = self.world_path
        self.master, slave = pty.openpty()
        self.tty = os.path.join(t, "console")
        fake.save_world({"boards": [SERIAL], "serial": SERIAL, "pages": [4, 5, 6],
                         "usb_file": os.path.join(t, "usb.json"), "tty_link": self.tty,
                         "pty": os.ttyname(slave),
                         "otp": {str(0xF83): 0x040404, str(0xF85): 0x040404},
                         "running": {"key": HASH1, "burn": False}, "mode": "app", "faults": [],
                         "otp_loads": [], "otp_sets": [], "flashed": [], "restarts": 0, "button": False})
        # its console
        self.stopping = False
        self.console = threading.Thread(target=fake.serve_console, args=(self.master, lambda: self.stopping), daemon=True)
        self.console.start()
        self.env = dict(os.environ, PATH=os.path.join(t, "bin") + os.pathsep + os.environ["PATH"],
                        SOS_PICOTOOL=os.path.join(t, "bin", "picotool"), SOS_FAKE_WORLD=self.world_path,
                        SOS_SECURE_BOARD_USB=os.path.join(t, "usb.json"), SOS_SECURE_BOARD_TTY=self.tty,
                        SOS_SECURE_BOARD_IMAGES=os.path.join(t, "images"),
                        SOS_SECURE_BOARD_PROFILES=os.path.join(t, "profiles"),
                        SOS_SECURE_BOARD_TIME_SCALE="0.02", SOS_FLASH_KEYS=os.path.join(t, "flash-keys"),
                        XDG_STATE_HOME=os.path.join(t, "state"), XDG_CONFIG_HOME=os.path.join(t, "config"))
        self.env.pop("SOS_FLASH_UNGUARDED", None)

    def tearDown(self):
        self.stopping = True
        self.console.join(timeout=2)
        os.close(self.master)
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ---- helpers ----

    def world(self):
        return fake.load_world()

    def change(self, **kw):
        with fake.world_lock():
            w = self.world()
            for k, v in kw.items():
                w[k] = v
            fake.save_world(w)

    def fault(self, name):
        with fake.world_lock():
            w = self.world()
            w["faults"].append(name)
            fake.save_world(w)

    def script(self, mode, answers=None, timeout=240):
        """Run the script on a pty, as a person at a terminal would. `answers` maps what a
        prompt asks for (a word, DONE, YES) to what is typed instead; the default types what
        is asked. Returns (exit status, everything it printed)."""
        answers = answers or {}
        pid, fd = pty.fork()
        if pid == 0:
            os.execvpe(sys.executable, [sys.executable, os.path.join(ROOT, "tools", "secure-board.py"), mode, "test-board"], self.env)
        out, handled, deadline = "", 0, time.time() + timeout
        held = unplugged = False
        status = None
        while time.time() < deadline:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(fd, 4096)
                except OSError:
                    data = b""
                if not data:
                    break
                out += data.decode(errors="replace").replace("\r", "")
            new = out[handled:]
            # the person's hands: the button follows the last thing the script said about it,
            # and the board is put in its bootloader when the script asks for that
            hold = max(out.rfind("Hold the BOOTSEL button now, and keep it held until this says"),
                       out.rfind("Hold the BOOTSEL button NOW"))
            let_go = max(out.rfind("Let go."), out.rfind("You can let go"))
            want = hold > let_go and "never_hold" not in answers
            if want != held:
                held = want
                self.change(button=want)
            if "Unplug the board, hold BOOTSEL" in new and not unplugged:
                unplugged = True
                if "stay_running" not in answers:
                    self.change(mode="bootsel")
            if "PRETEND RUN" not in out and len(out) > 200 and "REFUSED" not in out:
                self.fail("a pretend run did not say so at the top:\n" + out[:300])
            m = re.search(r"Type (\w+) to do it \(anything else stops\): $", new) or \
                re.search(r"Type (DONE) when it is \(anything else stops\): $", new) or \
                re.search(r"Type (YES) or NO: $", new)
            if m:
                word = m.group(1)
                reply = answers.get(word, word)
                if isinstance(reply, list):
                    reply = reply.pop(0) if reply else word
                os.write(fd, (reply + "\n").encode())
                handled = len(out)
        else:
            os.kill(pid, 9)
            self.fail("the script did not finish in %d s. Output so far:\n%s" % (timeout, out))
        _, status = os.waitpid(pid, 0)
        return os.waitstatus_to_exitcode(status), out

    def without_terminal(self, mode):
        p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "secure-board.py"), mode, "test-board"],
                           env=self.env, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=240)
        return p.returncode, p.stdout + p.stderr

    def otp_written(self):
        w = self.world()
        return {int(n): v for n, v in w["otp"].items() if v and int(n) not in (0xF83, 0xF85)}

    def assert_secured(self):
        w = self.world()
        self.assertEqual(fake.slot_hash(w, 0), HASH1)
        self.assertEqual(fake.slot_hash(w, 1), HASH2)
        self.assertEqual(fake.slot_hash(w, 2), "00" * 32)
        self.assertEqual(fake.key_valid(w), 3)
        self.assertEqual(fake.secure_boot(w), 1)
        self.assertEqual(fake.row(w, 0xF81 + 2 * 3), 0x101010)
        self.assertEqual(fake.row(w, 0xF81 + 2 * 4), 0x3D3D3D)      # the rehearsal page, locked
        self.assertEqual(fake.row(w, 0xF81 + 2 * 5), 0x3D3D3D)      # the secret's page
        self.assertEqual(fake.root_secret_page(w), 5)
        self.assertFalse(any(fake.row(w, 6 * 64 + i) for i in range(64)))   # page 6 untouched
        self.assertEqual(fake.row(w, 0xF83), 0x040404)              # pages 1 and 2 left open
        self.assertEqual(fake.row(w, 0xF85), 0x040404)
        self.assertEqual(fake.vote(w, 0x40, 8, 3), 1)               # CRIT1: secure boot and nothing else
        self.assertEqual(w["running"], {"key": HASH1, "burn": False})      # the everyday image is on it
        self.assertEqual(w["mode"], "app")
        self.assertEqual(w["otp_sets"], [[0x0FF, 0]])               # the one lock test, value 0
        self.assertEqual(fake.row(w, 0x0FF), 0)


class Plan(Board):
    def test_plan_touches_nothing(self):
        before = open(self.world_path).read()
        rc, out = self.without_terminal("plan")
        self.assertEqual(rc, 0, out)
        self.assertEqual(open(self.world_path).read(), before)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "state")))       # not even a record
        for word in PERMANENT_WORDS + ["BACKUP"]:
            self.assertIn(word, out)
        self.assertEqual(out.count("PERMANENT:"), 6)
        self.assertIn("picotool otp load", out)
        self.assertIn(SERIAL, out)
        # the permanent steps, in the order they must happen
        order = [out.index(t) for t in ("Rehearsal: spend OTP page 4", "Write the root secret into OTP page 5",
                                        "Write the boot key hashes", "Mark the boot keys valid",
                                        "Turn secure boot on", "Lock OTP page 3", "Set the board up")]
        self.assertEqual(order, sorted(order))

    def test_no_mode_is_usage(self):
        p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "secure-board.py"), "test-board"],
                           env=self.env, capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("usage", (p.stdout + p.stderr).lower())
        self.assertEqual(self.world()["restarts"], 0)

    def test_the_real_profile_plans(self):
        env = dict(self.env)
        del env["SOS_SECURE_BOARD_PROFILES"]
        p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "secure-board.py"), "plan", "dev-1"],
                           env=env, capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("F94525FD61D9A1CC", p.stdout)
        self.assertEqual(self.world()["restarts"], 0)


class Check(Board):
    def test_check_changes_nothing(self):
        before = self.otp_written()
        rc, out = self.without_terminal("check")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.otp_written(), before)
        w = self.world()
        self.assertEqual(w["otp_loads"], [])
        self.assertEqual(w["otp_sets"], [])
        self.assertEqual(w["flashed"], [])
        self.assertIn("not yet", out)
        self.assertIn("nothing was changed", out)

    def test_check_on_a_secured_board(self):
        rc, out = self.script("run")
        self.assertEqual(rc, 0, out)
        before = self.otp_written()
        rc, out = self.without_terminal("check")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.otp_written(), before)
        self.assertNotIn("not yet", out)


class NoPerson(Board):
    def test_run_refuses_without_a_terminal(self):
        rc, out = self.without_terminal("run")
        self.assertEqual(rc, 2, out)
        self.assertIn("needs a person at a terminal", out)
        self.assertEqual(self.otp_written(), {})
        self.assertEqual(self.world()["otp_loads"], [])
        self.assertEqual(self.world()["flashed"], [])


class WrongBoard(Board):
    def attached(self, serials):
        self.change(boards=serials)

    def refused(self, text):
        for mode in ("run", "check"):
            rc, out = self.script(mode) if mode == "run" else self.without_terminal(mode)
            self.assertEqual(rc, 2, out)
            self.assertIn(text, out)
            self.assertEqual(self.otp_written(), {})
            self.assertEqual(self.world()["otp_loads"], [])
            self.assertEqual(self.world()["flashed"], [])

    def test_another_board(self):
        self.attached(["BBBBBBBBBBBBBBBB"])
        self.refused("The board attached has serial BBBBBBBBBBBBBBBB")

    def test_two_boards(self):
        self.attached([SERIAL, "BBBBBBBBBBBBBBBB"])
        self.refused("More than one board is attached")

    def test_no_board(self):
        self.attached([])
        self.refused("No board is attached")

    def test_a_second_board_appears_before_a_permanent_step(self):
        # Attached when the rehearsal is about to be confirmed: the check is made again there.
        rc, out = self.script("run", answers={"REHEARSE": "no"})
        self.assertEqual(rc, 2, out)
        self.assertEqual(self.otp_written(), {})
        self.change(boards=[SERIAL, "BBBBBBBBBBBBBBBB"])
        rc, out = self.script("run")
        self.assertEqual(rc, 2, out)
        self.assertIn("More than one board is attached", out)
        self.assertEqual(self.otp_written(), {})


class FullRun(Board):
    def test_a_blank_board_is_secured(self):
        rc, out = self.script("run")
        self.assertEqual(rc, 0, out)
        self.assert_secured()
        w = self.world()
        files = self.profile["otp_files"]
        self.assertEqual(w["otp_loads"], [files["boot_keys"], files["key_valid_trial"], files["key_valid"],
                                          files["secure_boot_trial"], files["secure_boot"],
                                          files["page_lock_trial"], files["page_lock"]])
        for word in PERMANENT_WORDS:
            self.assertIn("Type %s to do it" % word, out)
        self.assertIn("what this changes for good", out)
        # the unsigned image was tried once, did not run, and a signed one was put back
        keys = [f["key"] for f in w["flashed"]]
        self.assertIn(None, keys)
        self.assertIn(HASH2, keys)
        self.assertEqual(keys[-1], HASH1)
        # the record: there, outside the repository, with the OTP before and after
        records = os.path.join(self.tmp, "state", "sos", "boards", SERIAL)
        runs = os.listdir(records)
        self.assertEqual(len(runs), 1)
        got = os.listdir(os.path.join(records, runs[0]))
        self.assertEqual(sorted(got), ["log.txt", "otp-after.txt", "otp-before.txt"])
        log = open(os.path.join(records, runs[0], "log.txt")).read()
        self.assertIn("profile test-board", log)
        self.assertIn("script at commit", log)
        self.assertIn("otp load", log)

    def test_every_permanent_step_can_be_declined(self):
        # Decline each in turn: the script stops there with nothing of that step written, and
        # running it again carries on from where the board is, repeating no permanent step.
        loads_expected = 0
        for n, word in enumerate(PERMANENT_WORDS):
            before = self.otp_written()
            rc, out = self.script("run", answers={word: "yes"})         # "yes" is not the word
            self.assertEqual(rc, 2, out)
            self.assertIn("Not confirmed. Nothing was written.", out)
            self.assertNotIn("Type %s to do it" % PERMANENT_WORDS[n + 1] if n + 1 < len(PERMANENT_WORDS) else "\0", out)
            after = self.otp_written()
            # everything before this step is done, and this step wrote nothing
            # (a trial load may have run; it changes no bit)
            self.assertEqual(before | after, after)
            # do just this step, by declining the next
            if n + 1 < len(PERMANENT_WORDS):
                rc, out = self.script("run", answers={PERMANENT_WORDS[n + 1]: "no"})
                self.assertEqual(rc, 2, out)
                self.assertIn("Type %s to do it" % word, out)
                for earlier in PERMANENT_WORDS[:n]:
                    self.assertNotIn("Type %s to do it" % earlier, out)     # not asked again
        rc, out = self.script("run")
        self.assertEqual(rc, 0, out)
        self.assert_secured()
        # no real OTP file was loaded twice
        w = self.world()
        files = self.profile["otp_files"]
        for name in ("boot_keys", "key_valid", "secure_boot", "page_lock"):
            self.assertEqual(w["otp_loads"].count(files[name]), 1, name)

    def test_a_secured_board_run_again_writes_nothing(self):
        rc, out = self.script("run")
        self.assertEqual(rc, 0, out)
        before, loads = self.otp_written(), list(self.world()["otp_loads"])
        rc, out = self.script("run")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.otp_written(), before)
        self.assertEqual(self.world()["otp_loads"], loads)
        for word in PERMANENT_WORDS:
            self.assertNotIn("Type %s to do it" % word, out)

    def test_backup_not_confirmed(self):
        rc, out = self.script("run", answers={"BACKUP": "not yet"})
        self.assertEqual(rc, 2, out)
        self.assertEqual(self.otp_written(), {})
        self.assertEqual(self.world()["flashed"], [])


class Stops(Board):
    """Each check failing in turn: the script stops, says what it expected and saw, and writes
    nothing further."""

    def stopped(self, expect_text, never_reached, answers=None):
        rc, out = self.script("run", answers=answers)
        self.assertEqual(rc, 2, out)
        self.assertIn("STOPPED.", out)
        self.assertIn(expect_text, out)
        self.assertIn("Nothing further was done", out)
        for word in never_reached:
            self.assertNotIn("Type %s to do it" % word, out)
        return out

    def test_another_picotool(self):
        self.change(picotool="2.4.0")
        out = self.stopped("picotool version", PERMANENT_WORDS)
        self.assertIn("expected: 2.3.1", out)
        self.assertIn("seen:     2.4.0", out)
        self.assertEqual(self.otp_written(), {})

    def test_the_guard_list_lacks_a_key(self):
        with open(os.path.join(self.tmp, "flash-keys"), "w") as f:
            f.write(HASH1 + "\n")
        self.stopped("does not hold the key for slot 1", PERMANENT_WORDS)
        self.assertEqual(self.otp_written(), {})

    def test_the_second_key_image_is_signed_with_the_first(self):
        # What happened for real before the build fix: build-key2 silently signed with key 1.
        shutil.copy(os.path.join(self.tmp, "images", "build", "sos.bin"),
                    os.path.join(self.tmp, "images", "build-key2", "sos.bin"))
        out = self.stopped("build-key2 is signed with", PERMANENT_WORDS)
        self.assertIn("expected: " + HASH2, out)
        self.assertIn("seen:     " + HASH1, out)
        self.assertEqual(self.otp_written(), {})

    def test_the_unsigned_image_is_signed(self):
        shutil.copy(os.path.join(self.tmp, "images", "build", "sos.bin"),
                    os.path.join(self.tmp, "images", "build-unsigned", "sos.bin"))
        self.stopped("build-unsigned is signed with", PERMANENT_WORDS)
        self.assertEqual(self.otp_written(), {})

    def test_the_console_is_locked(self):
        self.fault("console_locked")
        self.stopped("asks for a password", PERMANENT_WORDS)
        self.assertEqual(self.otp_written(), {})

    def test_the_button_is_not_read(self):
        self.fault("button_not_read")
        self.stopped("never reported the BOOTSEL button as held", PERMANENT_WORDS)
        self.assertEqual(self.otp_written(), {})

    def test_the_burn_image_says_stop(self):
        self.fault("burn_fails")
        self.stopped("The burn image said STOP HERE", ["BURN", "KEYS", "VALID", "SECUREBOOT", "LOCK"])
        self.assertEqual(fake.root_secret_page(self.world()), None)

    def test_the_rehearsal_ends_another_way(self):
        self.fault("burn_says_other")
        self.stopped("the burn ended as it should", ["BURN", "KEYS", "VALID", "SECUREBOOT", "LOCK"])

    def test_the_burn_leaves_its_page_unlocked(self):
        self.fault("burn_leaves_unlocked")
        self.stopped("its page is locked", ["BURN", "KEYS", "VALID", "SECUREBOOT", "LOCK"])

    def test_the_button_is_not_held_for_the_burn(self):
        rc, out = self.script("run", answers={"never_hold": True})
        self.assertEqual(rc, 2, out)
        self.assertEqual(self.otp_written(), {})

    def test_the_root_secret_is_not_read_from_otp(self):
        self.fault("root_secret_from_flash")
        self.stopped("page 5 is SPENT WITHOUT A", ["KEYS", "VALID", "SECUREBOOT", "LOCK"])

    def test_the_pages_are_judged_readable_over_usb(self):
        self.stopped("The answer was not YES", ["KEYS", "VALID", "SECUREBOOT", "LOCK"], answers={"YES": ["YES", "NO"]})

    def test_a_boot_key_is_written_wrong(self):
        self.fault("wrong_key_written")
        out = self.stopped("neither blank nor this board's key", ["VALID", "SECUREBOOT", "LOCK"])
        self.assertEqual(fake.key_valid(self.world()), 0)
        self.assertEqual(fake.secure_boot(self.world()), 0)

    def test_bootkey_shows_another_key(self):
        self.fault("bootkey_lies")
        self.stopped("the firmware's own key", ["VALID", "SECUREBOOT", "LOCK"])
        self.assertEqual(fake.key_valid(self.world()), 0)

    def test_a_trial_load_changes_something(self):
        self.fault("trial_writes")
        self.stopped("KEY_VALID is 1", ["VALID", "SECUREBOOT", "LOCK"])
        self.assertEqual(fake.secure_boot(self.world()), 0)

    def test_a_load_is_refused(self):
        self.fault("load_refused")
        self.stopped("failed", ["VALID", "SECUREBOOT", "LOCK"])
        self.assertEqual(fake.slot_hash(self.world(), 0), "00" * 32)

    def test_the_signature_is_judged_not_verified(self):
        # YES for the bootloader and the USB read, then NO for the signature.
        self.stopped("The answer was not YES", ["SECUREBOOT", "LOCK"], answers={"YES": ["YES", "YES", "NO"]})
        self.assertEqual(fake.secure_boot(self.world()), 0)

    def test_an_unsigned_image_runs_under_secure_boot(self):
        self.fault("runs_anything")
        self.stopped("RAN an unsigned image", ["LOCK"])
        self.assertEqual(fake.row(self.world(), 0xF81 + 2 * 3), 0)

    def test_the_page_lock_does_not_take(self):
        self.fault("lock_does_not_take")
        self.stopped("PAGE3_LOCK1", [])
        self.assertEqual(self.world()["otp_sets"], [])          # the test write isn't tried on a failed lock

    def test_a_state_the_procedure_does_not_produce(self):
        # Secure boot on with no keys valid: someone must look; the script writes nothing.
        w = self.world()
        for i in range(8):
            w["otp"][str(0x40 + i)] = 1
        w["faults"].append("runs_anything")
        fake.save_world(w)
        before = self.otp_written()
        self.stopped("not an order this procedure produces", PERMANENT_WORDS)
        self.assertEqual(self.otp_written(), before)

    def test_something_else_set_in_crit1(self):
        w = self.world()
        for i in range(8):
            w["otp"][str(0x40 + i)] = 4                         # DEBUG_DISABLE
        fake.save_world(w)
        self.stopped("CRIT1.DEBUG_DISABLE is set", PERMANENT_WORDS)

    def test_a_key_slot_this_board_should_not_have(self):
        w = self.world()
        w["otp"][str(0x80 + 32)] = 0x1234                       # slot 2
        fake.save_world(w)
        self.stopped("Boot key slot 2 is not blank", PERMANENT_WORDS)


class Bootloader(Board):
    """A board found in its bootloader, with or without something runnable in flash."""

    def test_the_console_is_gone_in_the_bootloader(self):
        # The pretend board itself: no console device while it is in its bootloader.
        self.assertTrue(os.path.exists(self.tty))
        self.change(mode="bootsel")
        self.assertFalse(os.path.exists(self.tty))
        self.assertEqual(json.load(open(os.path.join(self.tmp, "usb.json"))), [[SERIAL, "bootloader"]])

    def test_run_picks_up_a_board_in_its_bootloader(self):
        self.change(mode="bootsel")
        rc, out = self.script("run")
        self.assertEqual(rc, 0, out)
        self.assertIn("The board is in its bootloader, not running.", out)
        self.assertIn("Type FLASH to do it", out)
        self.assert_secured()

    def test_the_flash_from_the_bootloader_can_be_declined(self):
        self.change(mode="bootsel")
        rc, out = self.script("run", answers={"FLASH": "no"})
        self.assertEqual(rc, 2, out)
        self.assertIn("Not flashed", out)
        self.assertEqual(self.world()["flashed"], [])
        self.assertEqual(self.otp_written(), {})

    def test_check_names_the_way_out(self):
        self.change(mode="bootsel")
        rc, out = self.without_terminal("check")
        self.assertEqual(rc, 2, out)
        self.assertIn("SOS_FLASH_SERIAL=%s tools/flash.sh build/sos.bin" % SERIAL, out)
        self.assertEqual(self.world()["flashed"], [])

    def test_stopped_after_the_unsigned_flash_and_started_again(self):
        # Stop the run at the page lock, then leave the board as the unsigned flash leaves it:
        # in its bootloader, with an image it refuses. The next run must get it going again.
        rc, out = self.script("run", answers={"LOCK": "no"})
        self.assertEqual(rc, 2, out)
        w = self.world()
        w["running"] = {"key": None, "burn": False}
        w["mode"] = "bootsel"
        fake.save_world(w)
        rc, out = self.script("run")
        self.assertEqual(rc, 0, out)
        self.assertIn("Type FLASH to do it", out)
        self.assert_secured()

    def test_the_recovery_step_needs_the_bootloader(self):
        # The person types DONE without having put the board in its bootloader.
        rc, out = self.script("run", answers={"stay_running": True})
        self.assertEqual(rc, 2, out)
        self.assertIn("not in its bootloader", out)
        self.assertEqual(self.otp_written(), {})


class Overrides(Board):
    """Test settings must not be able to reach a real run."""

    def refused(self, mode, env):
        p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "secure-board.py"), mode, "test-board"],
                           env=env, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=60)
        out = p.stdout + p.stderr
        self.assertEqual(p.returncode, 2, out)
        self.assertIn("REFUSED", out)
        self.assertEqual(self.world()["restarts"], 0)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "state")))
        return out

    def test_the_usb_listing_alone(self):
        # The dangerous one: the board check answered from a file, everything else real.
        for mode in ("run", "check"):
            env = {k: v for k, v in self.env.items() if not k.startswith("SOS_SECURE_BOARD_") and k != "SOS_PICOTOOL"}
            env["SOS_SECURE_BOARD_USB"] = os.path.join(self.tmp, "usb.json")
            env["SOS_SECURE_BOARD_PROFILES"] = os.path.join(self.tmp, "profiles")
            out = self.refused(mode, env)
            self.assertIn("SOS_PICOTOOL", out)

    def test_each_one_missing(self):
        for name in ("SOS_PICOTOOL", "SOS_SECURE_BOARD_USB", "SOS_SECURE_BOARD_TTY", "SOS_SECURE_BOARD_IMAGES"):
            env = dict(self.env)
            del env[name]
            self.assertIn(name, self.refused("run", env))

    def test_the_real_picotool_on_the_path(self):
        env = dict(self.env)
        other = os.path.join(self.tmp, "otherbin")
        os.mkdir(other)
        with open(os.path.join(other, "picotool"), "w") as f:
            f.write("#!/bin/sh\nexit 1\n")
        os.chmod(os.path.join(other, "picotool"), 0o755)
        env["PATH"] = other + os.pathsep + env["PATH"]
        self.assertIn("flashing would use the real one", self.refused("run", env))

    def test_a_pretend_run_says_so(self):
        rc, out = self.without_terminal("check")
        self.assertEqual(rc, 0, out)
        self.assertTrue(out.startswith("PRETEND RUN"), out[:200])
        records = os.path.join(self.tmp, "state", "sos", "boards", SERIAL)
        log = open(os.path.join(records, os.listdir(records)[0], "log.txt")).read()
        self.assertIn("PRETEND RUN", log.split("\n")[0])


class FailedBurn(Board):
    def test_a_failed_burn_is_not_taken_for_a_finished_one(self):
        self.fault("real_burn_fails")
        rc, out = self.script("run")
        self.assertEqual(rc, 2, out)
        self.assertIn("The burn image said STOP HERE", out)
        w = self.world()
        self.assertEqual(fake.row(w, 0xF81 + 2 * 5), 0x3D3D3D)      # same lock word as a good burn
        self.assertEqual(fake.root_secret_page(w), None)
        before = self.otp_written()
        # Started again: the brake left by the STOP HERE holds it, before anything is run.
        rc, out = self.script("run")
        self.assertEqual(rc, 2, out)
        self.assertIn("An earlier run on this board ended with the burn image saying STOP HERE", out)
        self.assertEqual(self.otp_written(), before)
        # A person removes the brake. Now the board itself is asked, and the stop says what it is.
        os.unlink(os.path.join(self.tmp, "state", "sos", "boards", SERIAL, "BURN-FAILED"))
        for mode in ("run", "check"):
            rc, out = self.script(mode) if mode == "run" else self.without_terminal(mode)
            self.assertEqual(rc, 2, out)
            self.assertIn("page 5 is SPENT WITHOUT A", out)
            self.assertNotIn("Type KEYS to do it", out)
            self.assertEqual(self.otp_written(), before)

    def test_check_on_a_locked_console_says_not_confirmed(self):
        rc, out = self.script("run")
        self.assertEqual(rc, 0, out)
        self.fault("console_locked")
        rc, out = self.without_terminal("check")
        self.assertEqual(rc, 0, out)
        self.assertIn("not confirmed (the console is locked", out)


class Smaller(Board):
    def test_another_chip(self):
        path = os.path.join(self.tmp, "profiles", "test-board", "profile.json")
        profile = json.load(open(path))
        profile["chip"] = "RP2350 A4 (revision 4, ROM 3)"
        json.dump(profile, open(path, "w"))
        rc, out = self.script("run")
        self.assertEqual(rc, 2, out)
        self.assertIn("the chip is the one the profile was written for", out)
        self.assertEqual(self.otp_written(), {})

    def test_a_log_line_after_the_prompt_does_not_fail_a_burn(self):
        self.fault("log_after_prompt")
        rc, out = self.script("run")
        self.assertEqual(rc, 0, out)
        self.assert_secured()

    def test_the_zero_write_is_shown_before_it_runs(self):
        rc, out = self.script("run")
        self.assertEqual(rc, 0, out)
        shown = out.index("command: picotool otp set -r 0x0ff 0 -f --ser " + SERIAL)
        self.assertLess(shown, out.index("Was that write refused?"))


if __name__ == "__main__":
    unittest.main()
