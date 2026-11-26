#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
"""Secure a board, step by step, checking each result.

    tools/secure-board.py plan <board>     print every step and command; touch nothing
    tools/secure-board.py check <board>    run only the steps that read; change nothing
    tools/secure-board.py run <board>      do it: the steps that write OTP, for good

The mode comes first and there is no default. (`run` is spelled out at the start of the
command so that it can be told apart from the other two by anything that looks only at how a
command begins, such as a rule that keeps automated sessions from starting it.)

<board> names a profile, docs/otp/<board>/profile.json: the board's serial, its key hashes,
its OTP files and which steps apply. Nothing that identifies the board is typed by hand.

What it does (docs/securing-a-board.md has the reasons):

- Before anything, and again before every permanent step: exactly one board is attached and
  its serial is the profile's.
- It runs the checks itself and compares whole values. On any mismatch it stops, says what it
  expected and what it saw, and does not offer to continue.
- Before each PERMANENT step (a write to one-time-programmable memory) it shows the exact
  command and what it changes for good, and waits for one typed word naming the step. Those
  steps need a person at a terminal: without one they refuse. They cannot be undone.
- It works out where the board is from the board itself (its OTP, and its console), so it can
  be run again after a stop.
- It keeps a record per board outside the repository: every command, its output, the time.
  Passwords and the SD card's key never pass through it: `setup` and `sd format` are done by
  the person on the board's console, last, with the script paused and the console released.

Environment, for tests only: SOS_PICOTOOL (the picotool to run), SOS_SECURE_BOARD_USB (a JSON
file listing attached boards in place of /sys), SOS_SECURE_BOARD_TTY (the console's device),
SOS_SECURE_BOARD_IMAGES (a directory holding build/, build-burn/ ... already built),
SOS_SECURE_BOARD_PROFILES (where the profiles are), SOS_SECURE_BOARD_TIME_SCALE. Together they
make a PRETEND run against a pretend board. `check` and `run` refuse to start with only some
of them set: one alone could answer "which board is attached?" from a file while the real
picotool wrote to a real board.
"""

import argparse
import datetime
import glob
import json
import os
import re
import select
import shutil
import subprocess
import sys
import termios
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RPI_VENDOR = "2e8a"
BOARD_PRODUCTS = {"0003", "0009", "000a", "000f"}   # RP2 boot ROMs and the SDK's USB serial
PROMPT = "sos> "
LOCK_BURNED = 0x3D3D3D                              # core/rootburn.h: ROOTBURN_LOCK_WORD
RESULT_REHEARSED = "rehearsal: stopped on purpose before the check and the marker; that page is now spent."
RESULT_BURNED = "the root secret is written, verified and its page locked."

# How long each wait may take, in seconds. (SOS_SECURE_BOARD_TIME_SCALE shortens them all, for
# the tests, which run against a pretend board that answers at once.)
SCALE = float(os.environ.get("SOS_SECURE_BOARD_TIME_SCALE", "1"))
WAIT_BOARD = 60 * SCALE          # the board back on USB after a command that restarts it
WAIT_PROMPT = 30 * SCALE         # a console prompt
WAIT_BUTTON = 60 * SCALE         # the person holding the BOOTSEL button
WAIT_BURN = 120 * SCALE          # a burn, once the button is held
WAIT_NOT_RUNNING = 25 * SCALE    # how long an image that must not start is given to not start


def pause(seconds):
    time.sleep(seconds * SCALE)


class Stop(Exception):
    """The run stops here. Nothing offers to continue."""


PRETEND_ALL = ["SOS_PICOTOOL", "SOS_SECURE_BOARD_USB", "SOS_SECURE_BOARD_TTY",
               "SOS_SECURE_BOARD_IMAGES", "SOS_SECURE_BOARD_PROFILES"]


def pretend_run():
    """True if this is a pretend run (every test override set), False if it is a real one
    (none set). Anything in between is refused: a mixture is how a test setting could reach a
    real board."""
    have = [v for v in PRETEND_ALL + ["SOS_SECURE_BOARD_TIME_SCALE"] if os.environ.get(v)]
    if not have:
        return False
    missing = [v for v in PRETEND_ALL if not os.environ.get(v)]
    if missing:
        raise Stop("Test settings are in this shell's environment: %s.\nThey are only allowed all "
                   "together (a pretend run); missing: %s.\nFor a real run, unset them and start again."
                   % (", ".join(have), ", ".join(missing)))
    # tools/flash.sh runs `picotool` from PATH: in a pretend run that must be the pretend one too.
    on_path = shutil.which("picotool")
    if not on_path or not os.path.samefile(on_path, os.environ["SOS_PICOTOOL"]):
        raise Stop("Test settings are set, but `picotool` on PATH (%s) is not SOS_PICOTOOL (%s): "
                   "flashing would use the real one. Refused."
                   % (on_path or "none", os.environ["SOS_PICOTOOL"]))
    return True


def checkout_problem():
    """Why this checkout must not be used for a real run, or None. A run from a modified or
    stale tree would do something other than what was reviewed."""
    def git(*args):
        return subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True, timeout=60)
    try:
        # Tracked files only: untracked ones (editor and tool directories) are not the code.
        st = git("status", "--porcelain", "--untracked-files=no")
        if st.returncode != 0:
            return "`git status` failed here: %s" % st.stderr.strip()
        if st.stdout.strip():
            return "this checkout has changes that are not committed:\n" + indent(tail(st.stdout, 10))
        if git("fetch", "--quiet", "origin", "main").returncode != 0:
            return "`git fetch origin main` failed, so it can't be told whether this checkout is current"
        head, main = git("rev-parse", "HEAD").stdout.strip(), git("rev-parse", "origin/main").stdout.strip()
        if not head or head != main:
            return "this checkout is at %s, and origin/main is at %s" % (head[:12], main[:12])
    except (OSError, subprocess.SubprocessError) as e:
        return "git could not be run: %s" % e
    return None


# --- the record ------------------------------------------------------------------------------------

class Record:
    def __init__(self, profile, mode):
        self.dir = None
        self.log_file = None
        if mode == "plan":
            return
        base = os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state")
        stamp = datetime.datetime.now().strftime("%Y-%m-%d-%H%M%S")
        self.dir = os.path.join(base, "sos", "boards", profile["serial"], stamp + "-" + mode)
        os.makedirs(self.dir, exist_ok=True)
        self.board_dir = os.path.dirname(self.dir)
        self.log_file = open(os.path.join(self.dir, "log.txt"), "w")
        if os.environ.get("SOS_SECURE_BOARD_USB"):
            self.note("PRETEND RUN against a pretend board: no real board is involved")
        self.note("profile %s, mode %s, script at commit %s" % (profile["name"], mode, git_commit()))

    def note(self, text):
        if self.log_file:
            now = datetime.datetime.now().strftime("%H:%M:%S")
            self.log_file.write("[%s] %s\n" % (now, text))
            self.log_file.flush()

    def save(self, name, text):
        if self.dir:
            with open(os.path.join(self.dir, name), "w") as f:
                f.write(text)


def git_commit():
    try:
        out = subprocess.run(["git", "-C", ROOT, "describe", "--always", "--dirty"],
                             capture_output=True, text=True, timeout=10).stdout.strip()
        return out or "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def say(text=""):
    print(text, flush=True)


# --- the person --------------------------------------------------------------------------------------

def need_person(what):
    """Permanent steps and judgements need someone at a terminal. No terminal: refuse."""
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        raise Stop("%s needs a person at a terminal, and there is none. Refused." % what)
    try:
        return Tty()
    except OSError:
        raise Stop("%s needs a person at a terminal, and there is none. Refused." % what)


class Tty:
    """The person's terminal itself (/dev/tty), not whatever stdin has been pointed at."""

    def __init__(self):
        self.fd = os.open("/dev/tty", os.O_RDWR | os.O_NOCTTY)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        os.close(self.fd)

    def write(self, text):
        # Nothing typed before the question was shown counts as an answer to it.
        termios.tcflush(self.fd, termios.TCIFLUSH)
        os.write(self.fd, text.encode())

    def flush(self):
        pass

    def readline(self):
        line = b""
        while not line.endswith(b"\n"):
            ch = os.read(self.fd, 1)
            if not ch:
                break
            line += ch
        return line.decode(errors="replace")


def ask_word(what, word, record):
    """One confirmation: the word that names the step, typed in full. Anything else stops."""
    tty = need_person(what)
    with tty:
        tty.write("Type %s to do it (anything else stops): " % word)
        tty.flush()
        answer = tty.readline().strip()
    record.note("confirmation asked for %s: %s" % (word, "given" if answer == word else "NOT given"))
    if answer != word:
        raise Stop("Not confirmed. Nothing was written.")


def ask_ready(question, record):
    """A pause for something only the person can do. They say when it is done."""
    tty = need_person("This step")
    with tty:
        tty.write(question + " Type DONE when it is (anything else stops): ")
        tty.flush()
        answer = tty.readline().strip()
    record.note("asked: %s -> %s" % (question, answer))
    if answer != "DONE":
        raise Stop("Stopped at: " + question)


def judge(question, output, record):
    """An answer whose exact text isn't known yet: the person reads it and decides."""
    say(output.rstrip())
    tty = need_person("Judging this output")
    with tty:
        tty.write("\n%s Type YES or NO: " % question)
        tty.flush()
        answer = tty.readline().strip()
    record.note("judged by the person: %s -> %s" % (question, answer))
    if answer != "YES":
        raise Stop("%s The answer was not YES.\nSeen:\n%s" % (question, output.rstrip()))


# --- the board on USB --------------------------------------------------------------------------------

BOOTLOADER_PRODUCTS = {"0003", "000f"}


def usb_listing():
    """The RP2 boards on USB: a list of (serial in upper case, "bootloader" or "running")."""
    fake = os.environ.get("SOS_SECURE_BOARD_USB")
    if fake:
        for _ in range(20):                      # the pretend board rewrites this file
            try:
                with open(fake) as f:
                    return [(s.upper(), m) for s, m in json.load(f)]
            except (OSError, ValueError):
                time.sleep(0.01)
        raise Stop("The pretend USB listing (%s) could not be read." % fake)
    found = []
    for dev in glob.glob("/sys/bus/usb/devices/*"):
        try:
            with open(os.path.join(dev, "idVendor")) as f:
                vendor = f.read().strip()
            with open(os.path.join(dev, "idProduct")) as f:
                product = f.read().strip()
        except OSError:
            continue
        if vendor != RPI_VENDOR or product not in BOARD_PRODUCTS:
            continue
        mode = "bootloader" if product in BOOTLOADER_PRODUCTS else "running"
        try:
            with open(os.path.join(dev, "serial")) as f:
                found.append((f.read().strip().upper(), mode))
        except OSError:
            found.append(("(no serial)", mode))
    return found


def attached_boards():
    return [serial for serial, _ in usb_listing()]


def board_mode(profile):
    """"bootloader" or "running" for the profile's board; None if it isn't (alone) on USB."""
    listing = usb_listing()
    if len(listing) == 1 and listing[0][0] == profile["serial"]:
        return listing[0][1]
    return None


def only_this_board(profile, record, wait=0):
    """Exactly one board attached, and it is the profile's. Checked before anything permanent."""
    deadline = time.time() + wait
    while True:
        boards = attached_boards()
        if boards == [profile["serial"]]:
            record.note("one board attached: %s" % boards[0])
            return
        if time.time() >= deadline:
            break
        pause(1)
    if not boards:
        raise Stop("No board is attached. Expected %s (serial %s)." % (profile["name"], profile["serial"]))
    if len(boards) > 1:
        raise Stop("More than one board is attached: %s. Leave only %s (serial %s) connected."
                   % (", ".join(boards), profile["name"], profile["serial"]))
    raise Stop("The board attached has serial %s. This run is for %s, serial %s."
               % (boards[0], profile["name"], profile["serial"]))


# --- picotool ----------------------------------------------------------------------------------------

class Picotool:
    def __init__(self, profile, record):
        self.exe = os.environ.get("SOS_PICOTOOL", "picotool")
        self.profile = profile
        self.record = record
        self.sel = ["-f", "--ser", profile["serial"]]

    def run(self, args, timeout=180):
        cmd = [self.exe] + args
        self.record.note("$ " + " ".join(cmd))
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        except FileNotFoundError:
            raise Stop("picotool was not found.")
        except subprocess.TimeoutExpired:
            raise Stop("`%s` did not finish in %d s." % (" ".join(cmd), timeout))
        out = (p.stdout + p.stderr).replace("\r", "")
        self.record.note("exit %d\n%s" % (p.returncode, out.rstrip()))
        return p.returncode, out

    def version(self):
        rc, out = self.run(["version"])
        m = re.search(r"picotool v(\S+)", out)
        return m.group(1) if m else out.strip()

    def get(self, options, selectors):
        """`picotool otp get`: options first, then the device, then the selectors. Returns
        {row number: value} and {(row, field): value}."""
        rc, out = self.run(["otp", "get"] + options + ["-n"] + self.sel + selectors)
        if rc != 0:
            raise Stop("Reading OTP failed.\nCommand: %s\nSeen:\n%s"
                       % (self.text(["otp", "get"] + options + ["-n"] + self.sel + selectors), out.rstrip()))
        rows, fields, row = {}, {}, None
        for line in out.split("\n"):
            m = re.match(r"ROW (0x[0-9a-fA-F]+)", line)
            if m:
                row = int(m.group(1), 16)
                continue
            m = re.match(r"\s+VALUE (0x[0-9a-fA-F]+)", line)
            if m and row is not None:
                rows[row] = int(m.group(1), 16)
                continue
            m = re.match(r"\s+field (\w+) \(bits? [\d-]+\) = (\d+)", line)
            if m and row is not None:
                fields[(row, m.group(1))] = int(m.group(2))
        return rows, fields

    def text(self, args):
        return " ".join(["picotool"] + args)


# --- the console -------------------------------------------------------------------------------------

class Console:
    """The board's USB console, found by the board's serial, never "the first port"."""

    def __init__(self, profile, record):
        self.profile = profile
        self.record = record
        self.fd = None

    def path(self):
        return (os.environ.get("SOS_SECURE_BOARD_TTY")
                or "/dev/serial/by-id/usb-Raspberry_Pi_Pico_%s-if00" % self.profile["serial"])

    def open(self, wait=WAIT_BOARD):
        self.close()
        deadline = time.time() + wait
        while not os.path.exists(self.path()):
            if time.time() >= deadline:
                raise Stop("The board's console (%s) did not appear within %d s." % (self.path(), wait))
            pause(0.5)
        pause(0.5)
        self.fd = os.open(self.path(), os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            a = termios.tcgetattr(self.fd)
            a[0] = a[1] = a[3] = 0
            a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            termios.tcsetattr(self.fd, termios.TCSANOW, a)
        except termios.error:
            pass

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None

    def read_until(self, endings, timeout, containing=()):
        """Read until the text ends with one of `endings`, or holds one of `containing` (then a
        moment more, for the rest of that answer). Returns the text; None on timeout."""
        out, deadline, seen_at = "", time.time() + timeout, None
        while time.time() < deadline:
            if seen_at and time.time() - seen_at > 1.0:
                return out
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(self.fd, 4096)
                except BlockingIOError:
                    continue
                except OSError:
                    return None
                out += chunk.decode(errors="replace").replace("\r", "")
                out = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", out)
                if any(out.endswith(e) for e in endings):
                    return out
                if not seen_at and any(c in out for c in containing):
                    seen_at = time.time()
        self.record.note("console, no ending seen in %d s; got:\n%s" % (timeout, out.rstrip()))
        return None

    def send(self, text):
        os.write(self.fd, text.encode())

    def reach_prompt(self, wait=WAIT_PROMPT):
        """Get to the shell's prompt. An open console may be in a first-use question (Ctrl-C
        leaves it). A console asking for its password is locked: this script never types one."""
        deadline = time.time() + wait
        while time.time() < deadline:
            self.send("\x03")
            self.read_until([PROMPT, "Password: "], 1.0)
            self.send("\r")
            out = self.read_until([PROMPT, "Password: "], 3.0)
            if out is None:
                continue
            if out.endswith("Password: "):
                return False
            return True
        raise Stop("The board's console did not answer with its prompt within %d s." % wait)

    def command(self, text, timeout=WAIT_PROMPT):
        self.record.note("console> " + text)
        self.send(text + "\r")
        out = self.read_until([PROMPT], timeout)
        if out is None:
            raise Stop("The console did not finish `%s` within %d s." % (text, timeout))
        body = out[: -len(PROMPT)]
        if body.startswith(text):
            body = body[len(text):]
        # Log lines the firmware prints between commands are not part of the answer.
        lines = [l for l in body.split("\n") if not re.match(r"^\[\s*\d+\.\d+\] ", l)]
        body = "\n".join(lines).strip("\n")
        self.record.note(body)
        return body


# --- the run ------------------------------------------------------------------------------------------

class Run:
    def __init__(self, profile, profile_dir, mode):
        self.p = profile
        self.dir = profile_dir
        self.mode = mode                       # "run", "plan" or "check"
        self.record = Record(profile, mode)
        self.pt = Picotool(profile, self.record)
        self.con = Console(profile, self.record)
        self.images = os.environ.get("SOS_SECURE_BOARD_IMAGES") or ROOT
        self.first = bool(profile.get("first_run"))
        pages = profile["root_secret_pages"]
        self.rehearsal_page = pages[0] if self.first else None
        self.secret_page = pages[1] if self.first else pages[0]
        self.key = {k["slot"]: k["hash"] for k in profile["keys"]}
        self.state = {}
        self.chip_checked = False
        self.secret_seen = None                # what the firmware said about the secret's page
        self.recover_cmd = "SOS_FLASH_SERIAL=%s tools/flash.sh build/sos.bin" % profile["serial"]

    # ---- small helpers ----

    def otp_file(self, which):
        return os.path.join(self.dir, self.p["otp_files"][which])

    def image(self, directory):
        return os.path.join(self.images, directory, "sos.bin")

    def expect(self, what, expected, seen):
        self.record.note("check %s: expected %r, seen %r" % (what, expected, seen))
        if expected != seen:
            raise Stop("%s\n  expected: %s\n  seen:     %s" % (what, expected, seen))
        say("  ok: %s" % what)

    def expect_in(self, what, expected, text):
        self.record.note("check %s: looking for %r" % (what, expected))
        if expected not in text:
            raise Stop("%s\n  expected to find: %s\n  seen:\n%s" % (what, expected, indent(text)))
        say("  ok: %s" % what)

    def console_open(self):
        """Open the console and get to its prompt. Stops if it is locked: this run keeps the
        board without a console password until the very end."""
        self.con.open()
        if not self.con.reach_prompt():
            raise Stop("The board's console asks for a password. This script never types one, and its "
                       "checks need an open console.")
        if not self.chip_checked and self.p.get("chip"):
            self.expect_in("the chip is the one the profile was written for", "chip: %s\n" % self.p["chip"],
                           self.con.command("version") + "\n")
            self.chip_checked = True

    def board_back(self):
        """After a command that restarts the board: it is on USB again, alone, AND running (its
        console is there and stays there). A board seen on USB a moment after a restart is
        often still on its way down or up, and the next command then fails."""
        only_this_board(self.p, self.record, wait=WAIT_BOARD)
        deadline, since = time.time() + WAIT_BOARD, None
        while time.time() < deadline:
            if os.path.exists(self.con.path()) and board_mode(self.p) == "running":
                since = since or time.time()
                if time.time() - since >= 3 * SCALE:
                    return
            else:
                since = None
            time.sleep(min(0.1, SCALE))
        if board_mode(self.p) == "bootloader":
            raise Stop(self.in_bootloader_text("The board did not start its firmware within %d s of a "
                                               "restart: it sits in its bootloader." % WAIT_BOARD))
        raise Stop("The board did not come back up within %d s of a restart (no console at %s)."
                   % (WAIT_BOARD, self.con.path()))

    def in_bootloader_text(self, first):
        return ("%s\nThat is where a board waits when nothing in its flash can start (an image it "
                "refuses, or a flash that failed).\nThe way out is to flash the everyday image, through "
                "the guard:\n    %s\n`run` offers to do exactly that when it is started with the board "
                "in this state." % (first, self.recover_cmd))

    def wait_bootloader(self):
        """The person has put the board in its bootloader: it is alone on USB, as a bootloader."""
        deadline = time.time() + WAIT_BOARD
        while time.time() < deadline:
            if board_mode(self.p) == "bootloader":
                self.record.note("the board is in its bootloader")
                return
            pause(1)
        only_this_board(self.p, self.record)
        raise Stop("The board is attached but not in its bootloader (%d s). Unplug it, hold BOOTSEL "
                   "while plugging it in, and start again." % WAIT_BOARD)

    def start_from_bootloader(self):
        """Started with the board already in its bootloader: the one safe thing is offered."""
        say("\nThe board is in its bootloader, not running.")
        if self.mode != "run":
            raise Stop(self.in_bootloader_text("`check` only reads, and a board in its bootloader has "
                                               "no firmware running to read through."))
        if not os.path.exists(self.image("build")):
            raise Stop("The board is in its bootloader and %s isn't built yet.\nBuild it "
                       "(`cmake --preset debug && cmake --build build`), then start again."
                       % self.image("build"))
        self.expect("build is signed with", self.key[0], self.image_key("build") or "(not signed)")
        say("The one safe way on is to flash the everyday image through the guard. It writes flash,")
        say("not OTP, and is what the recovery step does anyway.")
        say("  command: %s" % self.recover_cmd)
        tty = need_person("Flashing from the bootloader")
        with tty:
            tty.write("Type FLASH to do it (anything else stops): ")
            answer = tty.readline().strip()
        self.record.note("offered the flash from the bootloader: %s" % ("taken" if answer == "FLASH" else "declined"))
        if answer != "FLASH":
            raise Stop("Not flashed. The board is still in its bootloader.")
        self.flash("build")
        say("  ok: the board runs again")

    def confirm_secret(self):
        """The lock word of the secret's page is the same after a burn that worked and one that
        failed part way: only the firmware can tell them apart. Ask it, where it will answer."""
        page = self.secret_page
        if self.state["lock"][page] != LOCK_BURNED:
            return
        self.con.open()
        if not self.con.reach_prompt():
            self.con.close()
            if self.mode == "run":
                raise Stop("The board's console asks for a password. This script never types one, and its "
                           "checks need an open console.")
            say("  the root secret in page %d: not confirmed (the console is locked, as on a set-up board)" % page)
            return
        out = self.con.command("rootsecret")
        self.secret_seen = out
        if "root secret: present, in OTP page %d" % page in out:
            say("  ok: the firmware reads the root secret from OTP page %d" % page)
            return
        raise Stop(self.spent_text(out))

    def spent_text(self, seen):
        page = self.secret_page
        return ("OTP page %d is locked the way a burn leaves it, but the firmware does not find a root "
                "secret in it.\nThat is what a burn that failed part way leaves: page %d is SPENT WITHOUT A "
                "SECRET, and this board's profile expects the secret there.\nThis is not something to "
                "retry. Whether to use another page is a decision, and a change to the profile.\n"
                "The firmware said:\n%s" % (page, page, indent(seen)))

    def flash(self, directory, unguarded=False, runs=True):
        env = dict(os.environ, SOS_FLASH_SERIAL=self.p["serial"])
        env.pop("SOS_FLASH_UNGUARDED", None)
        if unguarded:
            env["SOS_FLASH_UNGUARDED"] = "1"
        cmd = [os.path.join(ROOT, "tools", "flash.sh"), self.image(directory)]
        self.record.note("$ %s%s" % ("SOS_FLASH_UNGUARDED=1 " if unguarded else "", " ".join(cmd)))
        self.con.close()
        p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=300)
        out = (p.stdout + p.stderr).replace("\r", "")
        self.record.note("exit %d\n%s" % (p.returncode, tail(out)))
        if p.returncode != 0:
            raise Stop("Flashing %s failed.\nSeen:\n%s" % (self.image(directory), indent(tail(out))))
        if runs:
            self.board_back()
        else:
            only_this_board(self.p, self.record, wait=WAIT_BOARD)

    def image_key(self, directory):
        """Which key an image is signed with, as tools/flash.sh -n says. None: not signed."""
        env = dict(os.environ, SOS_FLASH_SERIAL=self.p["serial"])
        env.pop("SOS_FLASH_UNGUARDED", None)
        cmd = [os.path.join(ROOT, "tools", "flash.sh"), "-n", self.image(directory)]
        p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=120)
        out = (p.stdout + p.stderr)
        self.record.note("$ %s\nexit %d\n%s" % (" ".join(cmd), p.returncode, out.rstrip()))
        m = re.search(r"\(([0-9a-f]{64})\)", out)
        if p.returncode == 0 and m:
            return m.group(1)
        if p.returncode == 3 and "is not signed" in out:
            return None
        raise Stop("Could not tell which key %s is signed with.\nSeen:\n%s"
                   % (self.image(directory), indent(out)))

    def permanent(self, step_name, word, changes, command_text):
        """Everything that must be true and said right before a write to OTP."""
        only_this_board(self.p, self.record)
        say()
        say("PERMANENT: %s" % step_name)
        say("  command: %s" % command_text)
        say("  what this changes for good: %s" % changes)
        ask_word(step_name, word, self.record)

    def load_otp(self, which):
        args = ["otp", "load", self.otp_file(which)] + self.pt.sel
        rc, out = self.pt.run(args)
        if rc != 0:
            raise Stop("`%s` failed.\nSeen:\n%s" % (self.pt.text(args), indent(out)))
        self.board_back()

    # ---- where the board is ----

    def read_state(self):
        """What the board's OTP says: which permanent steps have been done."""
        say("Reading the board's OTP...")
        s = {}
        pages = self.p["root_secret_pages"] + [self.p["page_lock"]["page"]]
        rows, _ = self.pt.get(["-r"], ["PAGE%d_LOCK1" % pg for pg in sorted(pages)])
        self.board_back()
        s["lock"] = {pg: rows.get(0xF81 + 2 * pg) for pg in pages}
        if any(v is None for v in s["lock"].values()):
            raise Stop("The lock words could not be read: %r" % s["lock"])
        rows, _ = self.pt.get(["-e"], ["2:"])
        self.board_back()
        s["bootkey"] = {}
        for slot in range(4):
            vals = [rows.get(0x80 + 16 * slot + k) for k in range(16)]
            if any(v is None for v in vals):
                raise Stop("Boot key slot %d could not be read." % slot)
            s["bootkey"][slot] = b"".join(bytes([v & 0xFF, v >> 8]) for v in vals).hex()
        _, fields = self.pt.get(["-c", "3"], ["BOOT_FLAGS1"])
        self.board_back()
        s["key_valid"] = fields.get((0x4B, "KEY_VALID"))
        s["key_invalid"] = fields.get((0x4B, "KEY_INVALID"))
        _, fields = self.pt.get(["-c", "8"], ["CRIT1"])
        self.board_back()
        s["crit1"] = {k[1]: v for k, v in fields.items() if k[0] == 0x40}
        if s["key_valid"] is None or "SECURE_BOOT_ENABLE" not in s["crit1"]:
            raise Stop("The boot flags could not be read.")
        self.state = s
        self.record.note("state: %s" % json.dumps(s, sort_keys=True))
        self.judge_state()
        return s

    def judge_state(self):
        """Is this a state this procedure produces? If not, stop: someone must look."""
        s, blank = self.state, "00" * 32
        for slot, want in self.key.items():
            if s["bootkey"][slot] not in (blank, want):
                raise Stop("Boot key slot %d holds something that is neither blank nor this board's key.\n"
                           "  expected: %s\n  seen:     %s" % (slot, want, s["bootkey"][slot]))
        for slot in range(4):
            if slot not in self.key and s["bootkey"][slot] != blank:
                raise Stop("Boot key slot %d is not blank, and the profile has no key for it: %s"
                           % (slot, s["bootkey"][slot]))
        if s["key_valid"] not in (0, self.p["key_valid"]):
            raise Stop("KEY_VALID is %d: neither 0 nor the %d this board gets." % (s["key_valid"], self.p["key_valid"]))
        if s["key_invalid"]:
            raise Stop("KEY_INVALID is %d: a key slot is marked invalid, which this procedure never does here."
                       % s["key_invalid"])
        allowed = {"SECURE_BOOT_ENABLE"}
        for name, value in s["crit1"].items():
            if value and name not in allowed:
                raise Stop("CRIT1.%s is set, which this procedure does not do on this board." % name)
        for pg in self.p["root_secret_pages"]:
            if s["lock"][pg] not in (0, LOCK_BURNED):
                raise Stop("Page %d's lock word is 0x%06x: neither blank nor what a burn writes." % (pg, s["lock"][pg]))
        lp = self.p["page_lock"]
        if s["lock"][lp["page"]] not in (0, int(lp["value"], 16)):
            raise Stop("Page %d's lock word is 0x%06x: neither blank nor %s." % (lp["page"], s["lock"][lp["page"]], lp["value"]))
        # Order: nothing later may be done while something earlier isn't.
        done = self.done()
        order = ["rehearsal", "burn", "boot_keys", "key_valid", "secure_boot", "page_lock"]
        if not self.first:
            order.remove("rehearsal")
        seen_undone = None
        for name in order:
            if not done[name]:
                seen_undone = seen_undone or name
            elif seen_undone:
                raise Stop("The board has `%s` done but not `%s`, which comes before it. "
                           "That is not an order this procedure produces." % (name, seen_undone))

    def done(self):
        s, blank = self.state, "00" * 32
        d = {}
        if self.first:
            d["rehearsal"] = s["lock"][self.rehearsal_page] == LOCK_BURNED
        d["burn"] = s["lock"][self.secret_page] == LOCK_BURNED
        d["boot_keys"] = all(s["bootkey"][slot] == h for slot, h in self.key.items())
        if not d["boot_keys"] and any(s["bootkey"][slot] != blank for slot in self.key):
            raise Stop("Only some of the boot key slots are written. Someone must look at this.")
        d["key_valid"] = s["key_valid"] == self.p["key_valid"]
        d["secure_boot"] = bool(s["crit1"].get("SECURE_BOOT_ENABLE"))
        d["page_lock"] = s["lock"][self.p["page_lock"]["page"]] == int(self.p["page_lock"]["value"], 16)
        return d

    # ---- the steps ----

    def steps(self):
        """The whole procedure for this profile, in order. `plan` prints it; `run` does it.
        Each entry: (id, title, kind, lines for the plan, function, "already done" key)."""
        p, S = self.p, self.pt.sel
        sel = " ".join(S)
        key0, pages = self.key[0], p["root_secret_pages"]
        lp = p["page_lock"]
        steps = []

        def add(ident, title, kind, plan, fn, done=None):
            steps.append({"id": ident, "title": title, "kind": kind, "plan": plan, "fn": fn, "done": done})

        add("preflight", "Check the tools, the board and the keys", "check",
            ["picotool version is %s" % p["picotool"],
             "exactly one board attached, serial %s" % p["serial"],
             "the flash guard's key list holds this board's key hashes",
             "no earlier run on this board ended with the burn image saying STOP HERE",
             "run: the checkout is clean and is origin/main",
             "only if the board is found in its bootloader: offer to flash build/sos.bin through the guard (FLASH)",
             "the person confirms a backup of the signing keys exists off this computer (BACKUP)"],
            self.step_preflight)
        add("otp-before", "Save the board's OTP as it is", "check",
            ["picotool otp dump %s   -> the record" % sel], self.step_dump_before)
        build = ["cmake --preset debug && cmake --build build   (everyday firmware)",
                 "cmake --preset debug -B build-burn -DSOS_BURN_IMAGE=ON && cmake --build build-burn   (burn image)"]
        if self.first:
            build += ["cmake --preset debug -B build-key2 -DSOS_SIGN_KEY=<the second key> && cmake --build build-key2",
                      "cmake --preset debug -B build-unsigned -DSOS_SIGN_KEY=none && cmake --build build-unsigned"]
        build += ["tools/flash.sh -n on each: build and build-burn signed with %s" % key0]
        if self.first:
            build += ["build-key2 signed with %s; build-unsigned not signed" % self.key[1]]
        add("images", "Build the images and check which key each is signed with", "build", build, self.step_images)
        add("state", "Read where the board is", "check",
            ["picotool otp get: the lock words, the boot key rows, BOOT_FLAGS1, CRIT1",
             "if the secret's page is locked: console: rootsecret -> present, in OTP page %d" % self.secret_page],
            self.step_state)
        add("recovery", "Walk the recovery path", "manual",
            ["the person: unplug, hold BOOTSEL, plug in, let go",
             "picotool info --ser %s   (shown; the person says whether it is this board in the bootloader)" % p["serial"],
             "tools/flash.sh build/sos.bin, and the console answers"], self.step_recovery, done="burn")
        add("burn-image", "Flash the burn image and look", "flash",
            ["tools/flash.sh build-burn/sos.bin",
             "console: version                -> THIS IS THE BURN IMAGE",
             "console: rootsecret burn plan   -> It would use OTP page %d" % (self.rehearsal_page or self.secret_page),
             "the person holds BOOTSEL; rootsecret burn plan -> BOOTSEL button: held"],
            self.step_burn_image, done="burn")
        if self.first:
            add("rehearsal", "Rehearsal: spend OTP page %d on purpose" % self.rehearsal_page, "permanent",
                ["console: rootsecret burn rehearse   (confirm: REHEARSE; the person holds BOOTSEL)",
                 "-> %s" % RESULT_REHEARSED, "-> OTP page %d: locked against the bootloader" % self.rehearsal_page],
                self.step_rehearsal, done="rehearsal")
        add("burn", "Write the root secret into OTP page %d" % self.secret_page, "permanent",
            ["console: rootsecret burn   (confirm: BURN; the person holds BOOTSEL)",
             "-> %s" % RESULT_BURNED, "-> OTP page %d: locked against the bootloader" % self.secret_page],
            self.step_burn, done="burn")
        add("everyday", "Put the everyday firmware back and check the root secret", "flash",
            ["tools/flash.sh build/sos.bin",
             "console: rootsecret   -> root secret: present, in OTP page %d" % self.secret_page],
            self.step_everyday)
        rows = " ".join("0x%03x" % (pg * 64) for pg in ([self.rehearsal_page] if self.first else []) + [self.secret_page])
        add("usb-read", "Check the root secret's pages can't be read over USB", "check",
            ["picotool otp get -r -n %s %s   (shown; the person says whether every read is refused)" % (sel, rows)],
            self.step_usb_read)
        add("boot-keys", "Write the boot key hashes", "permanent",
            ["picotool otp load %s %s   (confirm: KEYS)" % (rel(self.otp_file("boot_keys")), sel),
             "picotool otp get -e -n %s 2:   -> slot 0 %s, slot 1 %s" % (sel, self.key[0], self.key.get(1, "(none)")),
             "console: bootkey   -> NOT YET"], self.step_boot_keys, done="boot_keys")
        kv = ["picotool otp load %s %s   (confirm: VALID)" % (rel(self.otp_file("key_valid")), sel),
              "picotool otp get -c 3 -n %s BOOT_FLAGS1   -> KEY_VALID = %d" % (sel, p["key_valid"]),
              "console: bootkey   -> MATCH: slot 0"]
        if self.first:
            kv.insert(0, "trial that writes nothing: picotool otp load %s %s; BOOT_FLAGS1 unchanged"
                      % (rel(self.otp_file("key_valid_trial")), sel))
        add("key-valid", "Mark the boot keys valid", "permanent", kv, self.step_key_valid, done="key_valid")
        if self.first:
            add("second-key", "Check the second key works", "flash",
                ["tools/flash.sh build-key2/sos.bin; console: bootkey -> MATCH: slot 1",
                 "tools/flash.sh build/sos.bin; console: bootkey -> MATCH: slot 0"], self.step_second_key,
                done="secure_boot")
        sb = ["the person confirms the key backup again (BACKUP)",
              "console: bootkey -> MATCH: slot 0",
              "picotool info -a %s   (shown; the person says whether the signature is verified)" % sel,
              "picotool otp load %s %s   (confirm: SECUREBOOT)" % (rel(self.otp_file("secure_boot")), sel),
              "picotool otp get -c 8 -n %s CRIT1   -> SECURE_BOOT_ENABLE = 1" % sel,
              "console: version   -> secure boot: on"]
        if self.first:
            sb.insert(3, "trial that writes nothing: picotool otp load %s %s; CRIT1 unchanged"
                      % (rel(self.otp_file("secure_boot_trial")), sel))
        add("secure-boot", "Turn secure boot on: THE STEP WITH NO WAY BACK", "permanent", sb,
            self.step_secure_boot, done="secure_boot")
        if self.first:
            add("refuses", "Check the board refuses what it should", "flash",
                ["tools/flash.sh build/sos.bin; the console answers (a signed image still flashes)",
                 "SOS_FLASH_UNGUARDED=1 tools/flash.sh build-unsigned/sos.bin; the console must NOT answer",
                 "tools/flash.sh build/sos.bin; the console answers again"], self.step_refuses, done="page_lock")
        pl = ["picotool otp load %s %s   (confirm: LOCK)" % (rel(self.otp_file("page_lock")), sel),
              "picotool otp get -r -n %s PAGE%d_LOCK1   -> %s" % (sel, lp["page"], lp["value"]),
              "picotool otp set -r %s 0 %s   (a write of nothing; shown; the person says whether it was refused)" % (lp["test_row"], sel),
              "picotool otp get -r -n %s %s   -> 0x000000" % (sel, lp["test_row"])]
        if self.first:
            pl.insert(0, "trial that writes nothing: picotool otp load %s %s; lock word unchanged"
                      % (rel(self.otp_file("page_lock_trial")), sel))
        add("page-lock", "Lock OTP page %d against the bootloader" % lp["page"], "permanent", pl,
            self.step_page_lock, done="page_lock")
        add("otp-after", "Save the board's OTP as it is now", "check",
            ["picotool otp dump %s   -> the record" % sel], self.step_dump_after)
        add("setup", "Set the board up (the person, on the console)", "manual",
            ["the script lets go of the console",
             "the person, on the board's console: setup (console password, Wi-Fi, SSH key), then sd format",
             "nothing typed there passes through this script or its record"], self.step_setup)
        return steps

    # ---- each step ----

    def step_preflight(self):
        self.expect("picotool version", self.p["picotool"], self.pt.version())
        only_this_board(self.p, self.record)
        say("  ok: one board attached, serial %s" % self.p["serial"])
        keys = os.environ.get("SOS_FLASH_KEYS") or os.path.join(
            os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "sos", "flash-keys")
        try:
            with open(keys) as f:
                listed = f.read().lower()
        except OSError:
            raise Stop("The flash guard's key list (%s) is missing. Without it nothing checks which key an "
                       "image is signed with." % keys)
        for slot, h in self.key.items():
            if h not in listed:
                raise Stop("The flash guard's key list (%s) does not hold the key for slot %d:\n  %s" % (keys, slot, h))
        say("  ok: the flash guard's key list holds this board's key hashes")
        for name, what in (("BURN-FAILED", "ended with the burn image saying STOP HERE"),
                           ("BURN-UNFINISHED", "started a burn and never got to the end of checking it "
                                               "(it timed out, was killed, or the board reset)")):
            brake = os.path.join(self.record.board_dir, name)
            if os.path.exists(brake):
                with open(brake) as f:
                    raise Stop("An earlier run on this board %s:\n%s\nA page may be spent and locked "
                               "without being what the next step assumes; the board can't say.\n"
                               "Nothing is run on this board until that is understood. When it is, a "
                               "person removes %s." % (what, indent(f.read()), brake))
        if self.mode == "run" and not self.pretend:
            problem = checkout_problem()
            if problem:
                raise Stop("A real run is only made from a clean checkout of origin/main: %s." % problem)
            say("  ok: the checkout is clean and is origin/main")
        if board_mode(self.p) == "bootloader":
            self.start_from_bootloader()
        if self.mode == "run":
            say("\nA lost signing key means this board can never take new firmware once secure boot is on.")
            say("Is there a copy of the signing keys somewhere other than this computer?")
            ask_word("Confirming the key backup", "BACKUP", self.record)

    def step_dump_before(self):
        rc, out = self.pt.run(["otp", "dump"] + self.pt.sel)
        if rc != 0:
            raise Stop("`picotool otp dump` failed.\nSeen:\n%s" % indent(out))
        self.record.save("otp-before.txt", out)
        self.board_back()
        say("  saved: otp-before.txt")

    def step_dump_after(self):
        rc, out = self.pt.run(["otp", "dump"] + self.pt.sel)
        if rc != 0:
            raise Stop("`picotool otp dump` failed.\nSeen:\n%s" % indent(out))
        self.record.save("otp-after.txt", out)
        self.board_back()
        say("  saved: otp-after.txt")

    def step_images(self):
        wanted = [("build", self.key[0]), ("build-burn", self.key[0])]
        if self.first:
            wanted += [("build-key2", self.key[1]), ("build-unsigned", None)]
        if not os.environ.get("SOS_SECURE_BOARD_IMAGES") and self.mode == "run":
            key2 = signing_key_file(self.key[1]) if self.first else None
            cmds = [["cmake", "--preset", "debug"], ["cmake", "--build", "build"],
                    ["cmake", "--preset", "debug", "-B", "build-burn", "-DSOS_BURN_IMAGE=ON"],
                    ["cmake", "--build", "build-burn"]]
            if self.first:
                cmds += [["cmake", "--preset", "debug", "-B", "build-key2", "-DSOS_SIGN_KEY=" + key2],
                         ["cmake", "--build", "build-key2"],
                         ["cmake", "--preset", "debug", "-B", "build-unsigned", "-DSOS_SIGN_KEY=none"],
                         ["cmake", "--build", "build-unsigned"]]
            say("  Building %d images. This takes several minutes and prints nothing while it works."
                % (len(cmds) // 2))
            for cmd in cmds:
                say("  $ " + " ".join(cmd))
                self.record.note("$ " + " ".join(cmd))
                p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
                if p.returncode != 0:
                    self.record.note(tail(p.stdout + p.stderr, 60))
                    raise Stop("`%s` failed.\nSeen:\n%s" % (" ".join(cmd), indent(tail(p.stdout + p.stderr))))
        for directory, want in wanted:
            if not os.path.exists(self.image(directory)):
                raise Stop("%s does not exist. (`check` does not build; `run` does.)" % self.image(directory))
            self.expect("%s is signed with" % directory, want or "(not signed)",
                        self.image_key(directory) or "(not signed)")

    def step_state(self):
        self.read_state()
        d = self.done()
        for name in ["rehearsal", "burn", "boot_keys", "key_valid", "secure_boot", "page_lock"]:
            if name in d:
                say("  %-12s %s" % (name, "done" if d[name] else "not yet"))
        self.confirm_secret()

    def step_recovery(self):
        say("This is the only way back if a later flash goes wrong, so it is tried now.")
        self.con.close()
        ask_ready("Unplug the board, hold BOOTSEL, plug it in, and let go.", self.record)
        self.wait_bootloader()
        rc, out = self.pt.run(["info", "--ser", self.p["serial"]])
        if rc != 0:
            raise Stop("The board did not answer from its bootloader.\nSeen:\n%s" % indent(out))
        judge("Is this the board, answering from its bootloader?", out, self.record)
        self.flash("build")
        self.console_open()
        self.expect_in("the board runs again after a load from the bootloader", "SOS ", self.con.command("version"))

    def step_burn_image(self):
        self.flash("build-burn")
        self.console_open()
        self.expect_in("the burn image is on the board", "THIS IS THE BURN IMAGE", self.con.command("version"))
        page = self.rehearsal_page if (self.first and not self.done()["rehearsal"]) else self.secret_page
        plan = self.con.command("rootsecret burn plan")
        self.expect_in("the burn would use the expected page", "It would use OTP page %d " % page, plan)
        self.expect_in("the button reads as not held", "BOOTSEL button: not held", plan)
        say("\nThe burn only starts while the BOOTSEL button is held, so the button is tried first.")
        say("Hold the BOOTSEL button now, and keep it held until this says it has seen it...")
        deadline = time.time() + WAIT_BUTTON
        while True:
            if "BOOTSEL button: held" in self.con.command("rootsecret burn plan"):
                break
            if time.time() >= deadline:
                raise Stop("The board never reported the BOOTSEL button as held (%d s). The burn depends on "
                           "that button being read." % WAIT_BUTTON)
            pause(1)
        say("  ok: the board sees the button. Let go.")
        pause(3)

    def burn(self, rehearse):
        page = self.rehearsal_page if rehearse else self.secret_page
        word = "REHEARSE" if rehearse else "BURN"
        command = "rootsecret burn rehearse" if rehearse else "rootsecret burn"
        changes = ("OTP page %d is written with 32 random bytes and locked. It is spent for good. No root "
                   "secret yet." % page if rehearse else
                   "OTP page %d gets this board's root secret and is locked. Everything the board has "
                   "sealed so far stops opening." % page)
        self.console_open()
        self.expect_in("the burn image is on the board", "THIS IS THE BURN IMAGE", self.con.command("version"))
        self.expect_in("the burn would use the expected page", "It would use OTP page %d " % page,
                       self.con.command("rootsecret burn plan"))
        self.permanent("%s (on the board's console: `%s`)" % ("the rehearsal" if rehearse else "the root secret", command),
                       word, changes, "console: " + command)
        # From here until every check of the result has passed, a file says a burn is under
        # way. If this script dies, times out or the board resets in between, the page may be
        # locked just as after a good burn, and only this file knows the result was never seen.
        unfinished = os.path.join(self.record.board_dir, "BURN-UNFINISHED")
        with open(unfinished, "w") as f:
            f.write("%s, `%s` on OTP page %d, record %s\n"
                    % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), command, page, self.record.dir))
            f.flush()
            os.fsync(f.fileno())
        say("\nHold the BOOTSEL button NOW and keep it held until this says to let go.")
        self.record.note("console> " + command)
        self.con.send(command + "\r")
        if self.con.read_until(["go on: "], WAIT_PROMPT) is None:
            raise Stop("The burn image did not ask for the serial. Nothing was written.")
        self.con.send(self.p["serial"] + "\r")
        if self.con.read_until(["on purpose: ", "OTP: "], WAIT_PROMPT) is None:
            raise Stop("The burn image did not accept the serial. Nothing was written.")
        self.con.send(word + "\r")
        started = time.time()
        # The end is the image's own last line, not "the prompt came back": a log line printed
        # after the prompt must not turn a finished burn into "did not finish".
        out = self.con.read_until([PROMPT], WAIT_BUTTON + WAIT_BURN,
                                  containing=["You can let go of the button.", "nothing was written"])
        if out is None:
            raise Stop("The burn did not finish within %d s. Do not run anything else: look at the board."
                       % (WAIT_BUTTON + WAIT_BURN))
        self.record.note(out)
        say(indent(out.rstrip()))
        say("\nYou can let go of the button.")
        if "STOP HERE" in out:
            with open(os.path.join(self.record.board_dir, "BURN-FAILED"), "w") as f:
                f.write("%s, `%s`, record %s\n" % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
                                                    command, self.record.dir))
            os.unlink(unfinished)            # its result was seen; BURN-FAILED now holds the board
            raise Stop("The burn image said STOP HERE. Do not run this again, on this board or another, "
                       "until the cause is understood.")
        if "nothing was written" in out and "writing OTP" not in out:
            os.unlink(unfinished)            # the image itself says it wrote nothing
            raise Stop("The burn did not start (was the button held in time?). Nothing was written.")
        want = RESULT_REHEARSED if rehearse else RESULT_BURNED
        self.expect_in("the burn ended as it should", want, out)
        self.expect_in("its page is locked", "OTP page %d: locked against the bootloader" % page, out)
        m = re.search(r"It took (\d+) ms", out)
        say("  the burn took %s ms (%.0f s from the confirmation)" % (m.group(1) if m else "?", time.time() - started))
        self.record.note("burn time: %s ms" % (m.group(1) if m else "?"))
        self.con.close()
        self.read_state()
        self.expect("page %d's lock word" % page, "0x%06x" % LOCK_BURNED, "0x%06x" % self.state["lock"][page])
        os.unlink(unfinished)                # result, lock line and read-back all checked

    def step_rehearsal(self):
        self.burn(True)

    def step_burn(self):
        if self.first and not self.done()["rehearsal"]:
            raise Stop("The rehearsal has not been done. The root secret is not written before it.")
        self.burn(False)

    def step_everyday(self):
        self.flash("build")
        self.console_open()
        version = self.con.command("version")
        if "THIS IS THE BURN IMAGE" in version:
            raise Stop("The burn image is still on the board after flashing the everyday firmware.")
        say("  ok: the everyday firmware is on the board")
        root = self.con.command("rootsecret")
        if self.done()["burn"]:
            if "root secret: present, in OTP page" not in root:
                raise Stop(self.spent_text(root))
            self.expect_in("the root secret is read from OTP", "root secret: present, in OTP page %d" % self.secret_page, root)

    def step_usb_read(self):
        if not self.done()["burn"]:
            say("  (nothing to check yet: no root secret in OTP)")
            return
        pages = ([self.rehearsal_page] if self.first else []) + [self.secret_page]
        args = ["otp", "get", "-r", "-n"] + self.pt.sel + ["0x%03x" % (pg * 64) for pg in pages]
        rc, out = self.pt.run(args)
        self.board_back()
        if self.mode == "check":
            say(indent(out.rstrip()))
            say("  (shown, not judged: `check` asks nobody)")
            return
        judge("Is every one of these reads refused (no row contents shown)?", out, self.record)

    def step_boot_keys(self):
        args = ["otp", "load", self.otp_file("boot_keys")] + self.pt.sel
        on_file = json.load(open(self.otp_file("boot_keys")))
        for slot, h in self.key.items():
            self.expect("the file's hash for slot %d" % slot, h, bytes(on_file["bootkey%d" % slot]).hex())
        self.permanent("the boot key hashes", "KEYS",
                       "Boot key slots %s are written. A slot written wrong can be abandoned, not rewritten."
                       % ", ".join(str(s) for s in sorted(self.key)), self.pt.text(args))
        self.load_otp("boot_keys")
        self.read_state()
        for slot, h in self.key.items():
            self.expect("boot key slot %d, all 64 digits" % slot, h, self.state["bootkey"][slot])
        self.check_bootkey("NOT YET: slot 0 ")

    def check_bootkey(self, verdict, slot_hash=None):
        self.console_open()
        out = self.con.command("bootkey")
        self.expect_in("the firmware's own key", "its key's hash is %s" % (slot_hash or self.key[0]), out)
        last = [l for l in out.split("\n") if l.strip()][-1] if out.strip() else ""
        if not last.startswith(verdict) and verdict not in out:
            raise Stop("`bootkey` did not say what it should.\n  expected a line starting: %s\n  seen:\n%s"
                       % (verdict, indent(out)))
        say("  ok: bootkey says %s" % verdict.strip())

    def step_key_valid(self):
        if self.first:
            say("A trial first: a file of the same form that writes nothing.")
            before = self.state["key_valid"]
            self.load_otp("key_valid_trial")
            self.read_state()
            self.expect("KEY_VALID after the trial (unchanged)", before, self.state["key_valid"])
        args = ["otp", "load", self.otp_file("key_valid")] + self.pt.sel
        self.permanent("marking the boot keys valid", "VALID",
                       "The boot keys in the written slots count from now on. KEY_VALID becomes %d." % self.p["key_valid"],
                       self.pt.text(args))
        self.load_otp("key_valid")
        self.read_state()
        self.expect("KEY_VALID", self.p["key_valid"], self.state["key_valid"])
        self.check_bootkey("MATCH: slot 0 ")

    def step_second_key(self):
        self.flash("build-key2")
        self.check_bootkey("MATCH: slot 1 ", self.key[1])
        self.flash("build")
        self.check_bootkey("MATCH: slot 0 ")

    def step_secure_boot(self):
        say("After this step the board runs only firmware signed with one of its keys, for ever.")
        say("If the key on the board did not match, the board would never run again.")
        ask_word("Confirming the key backup again", "BACKUP", self.record)
        self.flash("build")
        self.expect("the image on the board is signed with", self.key[0], self.image_key("build"))
        self.check_bootkey("MATCH: slot 0 ")
        rc, out = self.pt.run(["info", "-a"] + self.pt.sel)
        if rc != 0:
            raise Stop("`picotool info -a` failed.\nSeen:\n%s" % indent(out))
        self.board_back()
        judge("Does it say the signature is verified?", out, self.record)
        if self.first:
            say("A trial first: a file of the same form that writes nothing.")
            self.load_otp("secure_boot_trial")
            self.read_state()
            self.expect("SECURE_BOOT_ENABLE after the trial (unchanged)", 0, self.state["crit1"]["SECURE_BOOT_ENABLE"])
        self.check_bootkey("MATCH: slot 0 ")
        args = ["otp", "load", self.otp_file("secure_boot")] + self.pt.sel
        self.permanent("secure boot", "SECUREBOOT",
                       "The board refuses, for ever, any firmware not signed with a key in its valid slots.",
                       self.pt.text(args))
        self.load_otp("secure_boot")
        self.read_state()
        self.expect("SECURE_BOOT_ENABLE", 1, self.state["crit1"]["SECURE_BOOT_ENABLE"])
        self.console_open()
        self.expect_in("the firmware starts with secure boot on", "secure boot: on", self.con.command("version"))

    def step_refuses(self):
        self.flash("build")
        self.console_open()
        self.expect_in("a signed image still flashes and runs", "secure boot: on", self.con.command("version"))
        say("Now an image that is not signed. The flash guard is switched off for this one flash.")
        self.flash("build-unsigned", unguarded=True, runs=False)
        deadline = time.time() + WAIT_NOT_RUNNING
        answered = False
        while time.time() < deadline and not answered:
            if os.path.exists(self.con.path()):
                try:
                    self.con.open(wait=1)
                    answered = self.con.reach_prompt(wait=3) is not None
                except Stop:
                    answered = False
            pause(1)
        if answered:
            raise Stop("The board RAN an unsigned image with secure boot on. Secure boot is not doing its job.")
        say("  ok: the unsigned image did not start (%d s)" % WAIT_NOT_RUNNING)
        self.flash("build")
        self.console_open()
        self.expect_in("the board runs a signed image again", "secure boot: on", self.con.command("version"))

    def step_page_lock(self):
        lp = self.p["page_lock"]
        word_row = "PAGE%d_LOCK1" % lp["page"]
        if self.first:
            say("A trial first: a file of the same form that writes nothing.")
            self.load_otp("page_lock_trial")
            self.read_state()
            self.expect("page %d's lock word after the trial (unchanged)" % lp["page"], "0x000000",
                        "0x%06x" % self.state["lock"][lp["page"]])
        args = ["otp", "load", self.otp_file("page_lock")] + self.pt.sel
        self.permanent("locking OTP page %d against the bootloader" % lp["page"], "LOCK",
                       "Nothing in OTP page %d can be written over USB again, by anyone." % lp["page"],
                       self.pt.text(args))
        self.load_otp("page_lock")
        self.read_state()
        self.expect("%s" % word_row, lp["value"], "0x%06x" % self.state["lock"][lp["page"]])
        say("The lock is tried with a write that changes nothing even if the lock failed: the value 0.")
        only_this_board(self.p, self.record)
        need_person("Trying the lock").__exit__()
        set_args = ["otp", "set", "-r", lp["test_row"], "0"] + self.pt.sel
        say("  command: %s" % self.pt.text(set_args))
        rc, out = self.pt.run(set_args)
        self.board_back()
        judge("Was that write refused?", out, self.record)
        rows, _ = self.pt.get(["-r"], [lp["test_row"]])
        self.board_back()
        self.expect("row %s is still blank" % lp["test_row"], "0x000000", "0x%06x" % rows.get(int(lp["test_row"], 16), -1))

    def step_setup(self):
        self.con.close()
        say("The OTP steps are done. What is left only you can do, on the board's own console,")
        say("with your terminal program (this script has let go of the console):")
        say("  1. setup        the console password, the Wi-Fi network, your SSH public key")
        say("  2. sd format    type yes; WRITE DOWN the card key it shows")
        say("  3. reboot")
        say("Nothing you type there passes through this script or its record.")
        ask_ready("Set the board up as above.", self.record)
        say("\nDone. Record in the board registry what was written, with today's date.")
        say("The record of this run: %s" % self.record.dir)

    # ---- running ----

    def plan(self):
        say("Plan for %s (serial %s, %s board)%s" % (self.p["name"], self.p["serial"], self.p["role"],
                                                      ", first run on hardware" if self.first else ""))
        say("Nothing is touched by `plan`.\n")
        n = 0
        for st in self.steps():
            n += 1
            say("%2d. %s%s" % (n, "PERMANENT: " if st["kind"] == "permanent" else "", st["title"]))
            for line in st["plan"]:
                say("      " + line)
        say("\nNot done on this board:")
        for line in self.p.get("not_on_this_board", []):
            say("  - " + line)

    def run(self):
        steps = self.steps()
        check_only = self.mode == "check"
        if self.pretend:
            say("PRETEND RUN against a pretend board (test settings are set). No real board is involved.")
        say("%s %s (serial %s). Record: %s\n" % ("Checking" if check_only else "Securing", self.p["name"],
                                                 self.p["serial"], self.record.dir))
        n = 0
        for st in steps:
            n += 1
            if check_only and st["kind"] != "check":
                continue
            if st["done"] and self.state and self.done().get(st["done"]):
                say("%2d. %s: already done, skipped" % (n, st["title"]))
                self.record.note("step %s: already done" % st["id"])
                continue
            say("%2d. %s" % (n, st["title"]))
            self.record.note("step %s: start" % st["id"])
            st["fn"]()
            self.record.note("step %s: ok" % st["id"])
        self.con.close()
        say("\nAll steps %s." % ("that only read are done; nothing was changed" if check_only else "are done"))


def signing_key_file(key_hash):
    """Where the private key with this public hash is, from the machine's list (never typed):
    ${XDG_CONFIG_HOME:-~/.config}/sos/sign-keys, lines of `<hash> <path>`."""
    path = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "sos", "sign-keys")
    try:
        with open(path) as f:
            for line in f:
                parts = line.split("#")[0].split(None, 1)
                if len(parts) == 2 and parts[0].lower() == key_hash:
                    return os.path.expanduser(parts[1].strip())
    except OSError:
        pass
    raise Stop("The signing key with hash %s is not in %s.\nAdd a line `<hash> <path to the .pem>` for it." % (key_hash, path))


def rel(path):
    return os.path.relpath(path, ROOT) if path.startswith(ROOT) else path


def indent(text):
    return "\n".join("    " + l for l in text.rstrip().split("\n"))


def tail(text, lines=15):
    return "\n".join(text.rstrip().split("\n")[-lines:])


def main():
    ap = argparse.ArgumentParser(
        description="Secure a board, step by step, checking each result.",
        usage="%(prog)s {plan,check,run} <board>")
    ap.add_argument("mode", choices=["plan", "check", "run"],
                    help="plan: print the steps, touch nothing. check: only read. run: do it, for good")
    ap.add_argument("board", help="the board's profile name (docs/otp/<board>/profile.json)")
    if len(sys.argv) < 3:
        ap.print_usage()
        return 1
    args = ap.parse_args()
    profiles = os.environ.get("SOS_SECURE_BOARD_PROFILES") or os.path.join(ROOT, "docs", "otp")
    profile_dir = os.path.join(profiles, args.board)
    try:
        with open(os.path.join(profile_dir, "profile.json")) as f:
            profile = json.load(f)
    except OSError:
        sys.exit("No profile for a board called %r (%s/profile.json)." % (args.board, rel(profile_dir)))
    profile["serial"] = profile["serial"].upper()
    mode = args.mode
    pretend = False
    if mode != "plan":
        try:
            pretend = pretend_run()
        except Stop as stop:
            say("REFUSED.\n%s" % stop)
            return 2
    run = Run(profile, profile_dir, mode)
    run.pretend = pretend
    try:
        if mode == "plan":
            run.plan()
            return 0
        if mode == "run":
            need_person("Securing a board").__exit__()
        run.run()
        return 0
    except Stop as stop:
        run.record.note("STOP: %s" % stop)
        run.con.close()
        say("\nSTOPPED.\n%s" % stop)
        if run.record.dir:
            say("\nThe record of this run: %s" % run.record.dir)
        say("Nothing further was done. Find out why before running anything again.")
        return 2


if __name__ == "__main__":
    sys.exit(main())
