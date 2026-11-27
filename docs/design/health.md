# Health checks: design note for `health`

Decision (the user's, 2026-11-27, on #233): **SOS gets one framework of self-checks, `health`,
covering the board's hardware and its stored state.** Each check reports PASS, WARN or FAIL with
a short reason. A failed check is logged and **refuses only the operations that depend on it**
(a failed random-number test blocks making keys); the board still boots. `health` shows results
in any shell; running checks is console only.

This note sets the rules so that the phases (§10) can be built without deciding them again.
Where this note and the code differ in detail, the code and its header comments are the
reference.

Status (2026-11-27): phase 1 merged (#237); phase 2, the boot tier's hardware and crypto checks,
built (§9.2); its first board run (#242) failed `crypto` and `trng`, fixed in §9.2, and waits for
the second. Phase 3, checks of stored state: designed in §9.3 (approved, #254), built (§9.4),
waiting for its board run.
Tracking issue: #233. Revised after Reviewer's review
of #235: §2, §6, §7, §9 and §10. Stack guards, which #233 planned as a phase, turned out to be in
place already.

## 1. What exists today

These already check or record the board's health. Each one stands alone, and `health` reads
them instead of replacing them:

- **Resets:** `fault_last()` (a hard fault or panic, with the address), `wdog_caused_reset()`,
  and `glitchdet_caused_reset()` with its trigger count, uptime, temperature and supply before
  the reset. Only the first two are logged at boot (`main.c`); `uptime` shows all three.
- **Sensors:** `sensors_temp_stats()` and `sensors_vsys_stats()` (last, min, max and when),
  `sensors_usb_power()`. Read every 10 s by `sensors_poll()`.
- **Memory:** `stackmon_peak()` for core 0 (painted at boot), `lua_core1_stack_peak()` for the
  32 KB Lua stack on core 1 (painted when a script starts). Core 0's stack is also guarded:
  `PICO_USE_STACK_GUARDS` (MSPLIM), and `fault.c` reports a stack overflow. Heap:
  `heap_info()`, `heap_free()`, and use by feature from `heaptag.h`.
- **Stored state:** the config records carry a CRC and are sealed (AES-GCM). `config.c` picks
  the newest valid record of the two but doesn't say whether the other one was valid.
  `rootsource()`, `rootflash_degraded()`, `otpread_status()`, `otpread_bootkeys()`,
  `consoleonly_state()` and `keyprov_locked()` describe the keys and OTP.
- **The card:** `sd check` (#213) checks the file system of an unmounted card.
- **Time:** `timesync_info()` (source, last sync, step).

Before phase 2, nothing checked clocks, SRAM, the running image, the TRNG, or the crypto code
itself. And nothing compares one store with another. Those are the new checks in phases 2 to 6.

## 2. Decisions

1. **Boot FAIL: log it and refuse what depends on it** (§6). No check stops the boot. A board
   that refused to boot would also refuse the console you need to find out why.
2. **`health` shows results in any shell, runs checks only on the console.** `health run` and
   `health full` take the CPU for a while. Lua reads results and doesn't run checks.
   `health full` doesn't unmount the card: its `sd check` step runs only on a card that was
   unmounted first (`sd unmount`, as `sd check` requires, sd-check.md §2.1). On a mounted card
   that step is SKIP with `card mounted: run sd unmount first`.
3. **The latest result counts.** A gate follows the check's latest result: a check that fails
   at boot and passes on `health run` opens its gate again. Every change of status is logged,
   so a FAIL that cleared stays in the record.
4. **Checks read, they don't repair.** A check that finds damage says so. Fixing it belongs to
   the feature's own command (`sd check fix`, `config reset`), never to `health`.
5. **Image hash source and reset history storage** are decided in phases 5 and 4; §11 lists
   the options.

## 3. Shape of the code

As elsewhere in `core/`, the logic is pure C tested on the host, and the hardware is in a
`_board.c` file:

- **`src/core/health.c` / `health.h`: the framework, pure C.** It holds the registry, the
  results, the scheduling, the gates and the text. It knows no hardware and no shell.
- **`src/core/health_board.c`: the check list and the hardware reads.** It has the table of
  checks for the firmware and the functions that read the chip. The functions that only judge
  numbers (thresholds, comparisons) live in `health.c` or a module's own pure file, so the host
  tests can call them.
- **Shell:** `cmd_health()` in `shell.c`. **Lua:** `LH_HEALTH` in `lua_health.c`.

```c
typedef enum {
    HEALTH_NONE,    // not run yet
    HEALTH_SKIP,    // doesn't apply on this board (e.g. OTP checks on an open board)
    HEALTH_PASS,
    HEALTH_WARN,    // worth knowing; nothing is refused
    HEALTH_FAIL,    // refuses the check's gates (§6)
} health_status_t;

typedef enum {
    HEALTH_BOOT,        // once during boot, then on `health run`
    HEALTH_BACKGROUND,  // every period_s from the main loop, and on `health run`
    HEALTH_FULL,        // only on `health full` (console)
} health_tier_t;

// What a check function returns for one call.
typedef enum { HEALTH_DONE, HEALTH_MORE } health_step_t;

typedef struct {
    health_status_t status;
    char text[HEALTH_TEXT_MAX];     // one line, no newline: "clk_sys 150.0 MHz (expected 150)"
} health_result_t;

typedef struct health_check {
    const char *name;               // [a-z0-9-]{1,12}: "clocks", "trng", "stack"
    health_tier_t tier;
    uint32_t period_s;              // HEALTH_BACKGROUND only
    uint32_t first_s;               // HEALTH_BACKGROUND: first run after boot; 0 = period_s
    uint32_t gates;                 // HEALTH_GATE_* bits refused while this check FAILs
    // One step of the check. *state is NULL on the first call and is the check's to use; a
    // check that needs more than one step returns HEALTH_MORE and is called again on a later
    // pass. With cancel set it must free what *state holds and return HEALTH_DONE. Fills *out
    // when it returns HEALTH_DONE without cancel.
    health_step_t (*step)(void **state, bool cancel, health_result_t *out);
} health_check_t;
```

`HEALTH_TEXT_MAX` is 64. The registry is a `const health_check_t[]` given to `health_init()`.
Results are kept in a static array beside it: per check, the latest result, the uptime in
seconds when it was taken, a run count and a fail count. As built, the registry has room for
`HEALTH_MAX` (24) checks, the test-only ones included, and takes about 2.8 KB of RAM with the
scheduling state (§9.1).

## 4. Tiers and scheduling

**Boot.** `health_boot()` runs in `main()` right after `wolfCrypt_Init()`, before
`consoleonly_init()` and `rootsource_init()`. That way the TRNG and crypto tests have run before
the root secret, the owner secret or the host key can be made. Rules for boot checks:

- Each one finishes in one call (HEALTH_MORE isn't allowed at boot). The whole tier finishes in
  **100 ms**, measured and reported by `health`.
- The watchdog is already running (8 s). Even so, a boot check must never wait on something
  that might not come.
- Nothing from later in boot is up yet: no configuration, no Wi-Fi, no card. A check that needs
  the configuration (cross-store, §9) is a background check with a short first delay instead.
- When the tier ends, one log line sums it up (§5).

**Background.** `health_poll()` is called once per main-loop pass, after `sensors_poll()`. It
starts at most **one** check step per pass, picking a check whose period is due (the one most
overdue first). One step must stay under **10 ms**: the loop also serves SSH and the console, and
an SSH work step is measured in the same tens of milliseconds (see `ssh`, "longest"). A check
with longer work splits it with HEALTH_MORE, as the heap walk and RAM test will. A check's first
run is `period_s` after boot, unless it sets a shorter first delay. Background checks run
whether anyone is logged in or not.

**Full.** Run only by `health full`, as a shell job (`shell_run_job()`, like `sd check`). It runs
every check of every tier in table order, stepping each with a budget per job call (the same
budget idea as `CHECK_STEP_US` in `sdcard.c`). Ctrl-C cancels the current check (its step is
called with `cancel`) and stops; checks already done keep their new results. While the job runs,
`health_poll()` starts nothing, so a check is never stepped from two places.

`health run [<name>]` is the same job over the boot and background tiers only, or over one named
check of any tier.

## 5. Results and logging

- **Facility `kern`.** A result is logged when its status **changes** (NONE to anything counts
  as a change, except NONE to PASS or SKIP), so a steady board logs nothing:
  - to FAIL: `err`, `health: trng FAIL: repetition count 41 > 40`
  - to WARN: `warning`, `health: stack WARN: core 0 peak 6.9 of 8 KB`
  - back to PASS from WARN or FAIL: `notice`, `health: trng PASS (was FAIL 3 min ago)`
- **The boot summary**, at `notice` if everything passed or was skipped, otherwise at `warning`:
  `health: boot checks 6 pass, 1 skip in 41 ms` or
  `health: boot checks 5 pass, 1 FAIL (trng) in 44 ms: key making refused`.
- **A refusal is logged** where it happens, by the feature that refuses (§6), with the check's
  name.

## 6. Gates

A gate is one bit for one group of operations that depend on a check:

```c
#define HEALTH_GATE_KEYS    (1u << 0)   // making a key or secret from random numbers
#define HEALTH_GATE_CRYPTO  (1u << 1)   // encrypting new data (sealing, encrypting a card)

// NULL if the gate is open, else the name of the first FAILed check that holds it.
const char *health_gate(uint32_t gate);
```

Each operation that depends on a gate asks first, refuses with the check's name, and logs the
refusal. These are the call sites:

| Operation | Where | Gate |
|---|---|---|
| root secret created (open board) | `rootsecret_ensure()` via `rootflash_init()`: the store's random source, `store_random()` (`rootflash.c`) | KEYS |
| root secret burned to OTP (burn image) | `rootburn_run()`: its random source, `get_random()` (`rootburn_board.c`) | KEYS |
| owner secret created | `ensure_owner_secret()` (`settings.c`) | KEYS |
| SSH host key made | `make_new()` (`hostkey.c`) | KEYS |
| encrypted card format (card id, data key) | `start_encrypted()` (`sdcard.c`), and before the question in `cmd_format()` | KEYS, CRYPTO |
| console password salt | `settings_console_set_password()` | KEYS |
| config record sealed | the sealer `settings.c` gives `config_save()` (`gated_seal()`): every save, and the sealing of a plain record at boot | CRYPTO |

What a refusal means at each site, and why it is safe:

- **Root secret:** this only matters on a fresh open board, one that has no root secret yet. A
  board that already has one is unaffected, because reading it isn't gated. On a fresh board a
  KEYS refusal at first boot leaves it with no root secret, and the console stays locked in its
  rescue state (`console_no_keys()` in `shell.c`), offering only `rootsecret`,
  `rootsecret forget` and `reboot`. Two changes make the real cause visible there:
  - `health` (showing results only) is added to `console_rescue_allowed()`.
  - The refusal records why, and `console_no_keys()` shows that instead of its root-secret
    advice: `key making refused: health check trng failed`, followed by a pointer to `health`.

  Once the check passes, a reboot makes the secret as usual.
- **Owner secret and sealing:** the configuration isn't saved, and the command that wanted to
  save it says `the configuration wasn't saved: health check <name> failed`. What's already in
  flash stays as it was. That promise holds only if every caller of `settings_save()` checks
  its result and says so. Phase 1 audits all four callers: `settings.c`, `shell.c`,
  `tls/certstore.c` (`tls roots approve`) and `files/cardkey.c`. The last two matter most,
  because nobody would expect approving a TLS root or setting up a card key to be refused by a
  crypto self-test. A caller that ignores the result would drop the change silently.
- **Host key:** `sshd` doesn't start. The console still works.
- **Card format:** refused before anything is written to the card.
- **Password:** refused, and the old password stays.

Nothing that only **reads** existing keys is gated: opening a sealed record, unlocking a card,
checking a password and logging in all keep working. A board with a failing check can still be
looked at, logged into and reflashed.

A gate is open while no check that holds it is at FAIL. NONE, SKIP, PASS and WARN don't close
it. So until the boot tier has run, every gate is open, which is the same as today. Phase 1
adds the gate API and the call sites, but none of its checks hold a gate. A test-only command
(§9) forces a FAIL so that the refusals can be tested on the board.

## 7. The `health` command

```
health                      results of every check (any shell)
health run [<name>]         run the boot and background checks now, or one check (console only)
health full                 also the slow checks: RAM, image hash, sd check (console only;
                            sd check is SKIP unless the card was unmounted first)
```

`health` prints one line per check in table order, then the boot tier's time:

```
check       status  when       result
trng        PASS    boot       run 12 (fails at 61), proportion 545/1024 (810), 1.3 ms
clocks      PASS    boot       clk_sys 150.0 MHz; 5 clocks within 0.01%; lposc 32 kHz
stack       WARN    12 s ago   core 0 peak 6.9 of 8 KB
ram         -       never      (health full)
otp         SKIP    boot       open board
boot checks: 41 ms
```

Over SSH, `health run` and `health full` print
`checks can only be run on the serial console` (`require_admin()`). The command goes in
`commands[]` with `run` and `full` marked `SHELL_ADMIN`, and gets a line in `cmd_help()` and a
man page (`man/health.txt`, System section of `man/index.txt`).

## 8. Lua

`sos.health()` returns, like the other board tables (`lua_health.c`, `LH_HEALTH`):

```lua
{
  worst = "warn",           -- the worst status of any check: "pass", "warn" or "fail"
  boot_ms = 41,
  checks = {
    { name = "trng", status = "pass", tier = "boot", text = "...", ago_s = 3605,
      runs = 1, fails = 0 },
    ...
  },
}
```

A check that hasn't run has `status = "none"` and no `ago_s`. The table is read-only, and there
is no way to run a check from Lua.

## 9. Phase 1: framework and seed checks

Phase 1 builds §3 to §8 with checks that only read state SOS already has. These exercise every
tier and status on the board without new hardware code:

| Check | Tier | WARN | FAIL |
|---|---|---|---|
| `resets` | boot | the last reset was a fault, watchdog or glitch detector reset (text says which) | — |
| `glitch` | background, 60 s | the loop guard tripped (detector left off) | — |
| `stack` | background, 60 s | core 0 peak over 75 % of its stack, or core 1 over 75 % of 32 KB | over 90 % |
| `heap` | background, 60 s | free heap under 64 KB (a TLS handshake needs about 64 KB); back to PASS only above 96 KB | under 16 KB |
| `chip-temp` | background, 60 s | over 70 C | over 85 C |
| `supply` | background, 60 s | on USB power, VSYS min since the last check under 4.3 V | under 3.0 V |

None of these holds a gate. `heap` has hysteresis because ordinary use comes near its WARN
line: the heap is about 380 KB, an SSH session with an SFTP transfer takes about 100 KB
(`docs/files.md`) and TLS about 24 KB more, so two sessions transferring at once approach
64 KB free. Without hysteresis it would flip, and log a line, with every transfer. Both figures
are provisional until the phase 1 slot measures free heap with two SFTP sessions running. The
temperature figures are starting points. The sensor reads the
die, a few degrees off when uncalibrated (`temp cal`), and dev-1 idles around 30 C. Phase 1
checks them against the electrical ratings in the RP2350 datasheet before they are fixed. The
thresholds are named constants in `health.h`, and the judging
functions are pure and host tested. `supply` goes by the minimum since its last run, so a dip
between two runs is not missed.

**Test command:** under `SOS_TEST_COMMANDS` only (like `mem hold`), `health inject <name>
fail|clear` sets a check's result by hand, and `health inject gate <keys|crypto>` adds a
test-only check holding that gate at FAIL. That way the board test can see each refusal in §6.

**Host tests** (`tests/test_health.c`, the `health.c` source only):
- status changes and what gets logged (log stubbed)
- gates: open with NONE/SKIP/PASS/WARN, closed by FAIL, reopened by a later PASS
- background scheduling: the most overdue first, one step per poll, HEALTH_MORE resumes,
  nothing starts while a job runs
- cancel frees state
- the boot tier refuses HEALTH_MORE
- each seed check's judging function at and around its thresholds
- the text table and the Lua record

**Board test** (dev-1 slot):
- `health` on the console and over SSH; `run` and `full` refused over SSH
- the boot line in `dmesg`
- `crash fault` then reboot: `resets` WARN
- `health inject` for each gate, then each refused operation where it can be tried without
  touching the stored configuration (the host key refusal is tested on the host; `settings.c`
  has no host test, so the owner secret and sealing refusals are left to the board, on a
  configuration that is backed up first)
- `sos.health()` from Lua
- `mem` before and after, and `ssh` "longest" unchanged

### 9.1 As built (phase 1)

Where the build settled details this note left open, or differs from it:

- **Gates in pure modules** go through the module's existing ops: `rootsecret.c` and
  `rootburn.c` are untouched, and their board backends' random sources refuse. The burn image
  then reports `ROOTBURN_RANDOM`, and the log line before it names the check.
- **The rescue message:** `console_no_keys()` shows
  `key making refused: health check <name> failed` when there is no usable root secret and the
  root secret's store recorded a refusal (`rootflash_refused_by()`). Phase 1 first kept the last
  refusal of any caller (`health_gate_refuse()`, `health_refusal()`); once the rescue message
  blamed only the root secret's refusal, phase 2 removed both, and callers ask `health_gate()`.
- **`settings_save_refused()`** names the check that stopped the last save. What the four
  callers did before: `shell.c`'s `save()` and `tls/certstore.c` (`tls roots approve` and
  `forget`) checked the result but always said `saving to flash failed`; `files/cardkey.c`
  passed the failure on, and `sd format` said `no key to encrypt the card with (see dmesg)`;
  `settings_init()` ignored the result of its save (the owner secret for an older record),
  which only `settings_save()`'s own log line reported. Now the first three print
  `the configuration wasn't saved: health check <name> failed`, and the log line of the
  fourth names the check.
- **Host key:** a refused key isn't final until reboot, as other failures are: `hostkey_get()`
  asks again once the gate is open, so `sshd` starts after a `health run` that passes.
- **Boot checks** that return HEALTH_MORE are cancelled and recorded as FAIL (a bug in the
  check), so the gates they hold close.
- **No reading:** `chip-temp` and `supply` are SKIP without one. `supply` takes the lowest VSYS
  since its last run (`sensors_vsys_min_take()`); run again before a new reading (`health run`
  right after the background run), it takes the latest reading.
- **`health inject <name> clear`** records PASS ("cleared by hand"), so the change is logged
  like any other. The gate checks are full-tier checks named `inj-keys` and
  `inj-crypto`; `health full` runs them and they pass, which opens the gate again.
- **Full tier:** phase 1 has none (`sd check` is phase 5). `health full` is `health run` plus
  any test-only checks.
- **SSH refusal:** `health run` and `health full` check `io->admin` themselves, to print this
  note's message (`require_admin()` speaks of the configuration).
- **Temperature figures:** 70 C and 85 C are as above. The RP2350 datasheet wasn't at hand
  when they were set; 85 C is taken as the top of the chip's rated ambient range, and the die
  runs above ambient. Check them against the datasheet before relying on a FAIL.
- **Cost** (debug build, `arm-none-eabi-size`): text +7.5 KB, bss +2.8 KB.

### 9.2 As built (phase 2): the boot tier

Phase 2 adds four boot checks and one background check. The boot checks run in `health_boot()`
in this order, after `resets`, and `health run` runs them again:

| Check | Tier | Gates | WARN | FAIL |
|---|---|---|---|---|
| `clocks` | boot | — | a clock 1 % to 5 % off its setting; the LPOSC outside 18-54 kHz | a clock over 5 % off |
| `flash` | boot | — | unknown maker or size code; no JEDEC answer | smaller than `PICO_FLASH_SIZE_BYTES` |
| `trng` | boot | KEYS | — | either startup test; more than 8 blocks thrown away by the TRNG's CRNGT; under 16 blocks in 20 ms |
| `crypto` | boot | KEYS, CRYPTO | — | a known answer of the boot subset wrong, a changed input accepted, or an error from wolfCrypt |
| `crypto-ssh` | background, 30 s after boot, then daily | KEYS, CRYPTO | — | the same, for the rest of the known answers |

The judging is pure and host tested: `hwtest.c` (the TRNG statistics, and the judging of
`trng`, `clocks` and `flash`; `tests/test_hwtest.c`) and `selftest.c` (the crypto vectors,
run against the host's wolfCrypt; `tests/test_selftest.c`). `health_board.c` reads the chip.
The boot summary reads as before with five boot checks: `health: boot checks 5 pass in 46 ms`,
or `health: boot checks 4 pass, 1 FAIL (trng) in 47 ms: key making refused`.

**The first board run** (#242, dev-1, RP2350 A2) found three things, fixed here: `crypto`
failed X25519 on every boot, `trng` failed on the TRNG's own autocorrelation test on every
run, and the boot tier took 159 ms of its 100 ms budget (`crypto` alone 150 ms, about six times
the estimate). `clocks` and `flash` passed (`clk_sys 150.0 MHz; 5 clocks within 0.00%; lposc
32 kHz`, 18 ms; `JEDEC ef 40 16 (Winbond, 4 MB)`).

**`crypto` and `crypto-ssh`.** Thirteen known-answer tests (`selftest.c`), each a published
vector:

| Test | Vector | Run by |
|---|---|---|
| SHA-256, SHA-512 | FIPS 180-2 "abc" (Appendix B.1, C.1) | `crypto` |
| HMAC-SHA-256 | RFC 4231 test case 1 | `crypto` |
| AES-256-GCM encrypt, decrypt, and refuse a ciphertext with one bit flipped | GCM specification (McGrew, Viega) test case 16: 256-bit key, 96-bit IV, 20 bytes of AAD | `crypto` |
| XTS-AES-128 encrypt and decrypt, through `wc_AesXts*Sector` as `sdcrypt.c` calls it | IEEE 1619-2007 vector 2 (sequence number 0x3333333333) | `crypto` |
| Ed25519 public key from the secret (how a host key is made) | RFC 8032 §7.1 TEST 2 | `crypto` |
| Ed25519 signature (deterministic, so a known answer), verify, and refuse the message with one bit flipped | RFC 8032 §7.1 TEST 2 | `crypto-ssh` |
| X25519 shared secret, little-endian as wolfSSH calls it | RFC 7748 §6.1 | `crypto-ssh` |

A failing test's phrase is the check's text, and an error from wolfCrypt reads differently from
a wrong answer: `x25519 shared secret failed: error -173` against `x25519 gave a wrong shared
secret`. The vectors were checked against OpenSSL's command line (all but GCM) and wolfSSL's
own test suite (GCM). The host test also makes a fault in each test in turn (a flipped bit in
what it computed, or a refusal test fed the unchanged input) and sees it caught by that test.
Not covered: ChaCha20-Poly1305 and AES-CTR (SSH ciphers: a broken one breaks the session
visibly), AES-SIV (NTS: the server's tag would fail), ML-KEM, ECDSA and RSA (the TLS or SSH
peer checks those). wolfCrypt's Hash-DRBG runs its own health test when it is instantiated.
X25519 has no refusal test: every input gives some secret, so only the comparison applies. The
contexts take about 2 KB of heap (`HEAP_NET`) while a check runs; on a board with no heap to
spare it FAILs with `no memory to run the tests`, the conservative choice for a check holding
gates.

*The X25519 failure (#242).* The firmware has an RNG (Hash-DRBG), and with an RNG and the C
Curve25519 code wolfSSL turns on Curve25519 blinding by itself (`WOLFSSL_CURVE25519_BLINDING`,
`wolfssl/wolfcrypt/settings.h`). The blinded scalar multiplication takes random numbers from an
RNG set on the private key (`wc_curve25519_set_rng()`); wolfSSH's keys get one from
`wc_curve25519_make_key()`, the known-answer test imported a fixed key and had none, so
`wc_curve25519_shared_secret_ex()` returned `BAD_FUNC_ARG` (-173), which the old text reported
as a wrong secret. The byte order was not the problem: every call already used the `_ex` forms
with `EC25519_LITTLE_ENDIAN`. The host build had `WC_NO_RNG`, so it ran the unblinded code
and passed. Now the test gives the key an RNG, and the host's wolfCrypt is built with the
firmware's options for every primitive tested (`tests/wolfcrypt_host/user_settings.h` lists
what still differs and why it can't matter: the RNG's seed source, the AES assembly, the
non-crypto platform settings; it forces the 32-bit field code a 64-bit host would otherwise
replace). With the old test code, that build fails exactly as the board did (error -173), and
`test_selftest` checks that blinding is on and that the field code is the board's.

*Boot subset and time.* The boot subset covers what making keys and sealing at boot rely on:
the hashes and HMAC (the DRBG, HKDF, the password verifier), GCM with its refusal (sealing),
XTS (an encrypted card), and the Ed25519 public key (the host key is made that way). Ed25519
signing and verifying and X25519 are what SSH uses once a client connects, at least 30 s after
boot, so `crypto-ssh` runs them then, as a background check. The figures below are derived
from the one board measurement (150 ms for everything but X25519) and the operations'
relative cost, and are **to be measured** with `health crypto-times`:

| Group | Assumed time |
|---|---|
| sha (3 tests), gcm (3), xts (2) | about 2 ms together |
| ed-pub (base point multiplication) | about 20 ms |
| ed-sign | about 20 ms |
| ed-verify (2 tests: a good signature and a changed message) | about 55 ms each |
| x25519 (blinded, with an RNG made for it) | about 55 to 60 ms |

So `crypto` is about 22 ms at boot and `crypto-ssh` about 190 ms in four steps, the longest
(one verify, or X25519) about 60 ms. That is over the 10 ms a background step should take
(§4); it can't be split further, since each is one wolfCrypt call, and it holds the main loop
less than an SSH handshake does (234 ms measured on the same run). At boot `crypto` runs in one
call, as a boot check must; under `health run`, `health full` and in the background each
check runs one test per step. A passing run's text, and an info line in the log, give the time
per group: `9 known answers in 22 ms: sha 0.5 gcm 1.0 xts 0.4 ed-pub 20` and `4 known answers
in 190 ms: ed-sign 20 ed-verify 110 x25519 60`.

**`trng`.** wolfCrypt is seeded from pico_rand, which mixes the TRNG with other sources, so a
test of its output would test the mixing. The check reads the TRNG block itself:

- **What it samples:** the ring oscillator's raw bits, one sample every 256 clk_sys cycles,
  the shortest inverter chain, the von Neumann corrector bypassed (the datasheet's minimum
  sample period without it is 17 cycles; longer periods give better bits, §12.12.2). The
  block's CRNGT (two equal 16-bit blocks in a row) stays on; its **autocorrelation test is
  bypassed**.
- **Why that setup (#242):** the first board run sampled every 64 cycles with the
  autocorrelation test on, and the test stopped the TRNG on every run (it stops the block until
  a reset after failing four times in a row). The datasheet describes the test only by that
  rule, with no threshold; it belongs to Arm's standard setup (corrector on, a characterised
  sample count), which RP2350's bootrom and SDK don't use either (both bypass every check,
  §12.12.4). On raw bits, without the corrector,
  some autocorrelation between neighbouring samples is expected and is not by itself a fault;
  SP 800-90B's repetition count and adaptive proportion tests are the health tests the standard
  specifies for raw noise samples, with cutoffs from a stated entropy, so they are the check,
  with the CRNGT. A longer sample period (256 instead of 64) lets more jitter accumulate between
  samples, so neighbouring samples are less alike; it costs 5.3 ms instead of 1.3 ms. The
  datasheet suggests 20 to 25 cycles with the corrector on; we keep it off so that the bits
  tested are the oscillator's own (the corrector removes bias, which would hide a biased
  oscillator from the proportion test).
- **To be confirmed on the board:** `health trng-diag` (test builds) runs every combination of
  sample period 17, 64 and 256, autocorrelation test on and off, and corrector on and off, and
  prints for each the blocks collected, the CRNGT and corrector errors, whether the
  autocorrelation test stopped it, the longest run, the largest proportion and the time. The
  chosen setup is marked. It should show 16 blocks, no errors, a run well under 61 and a
  proportion well under 810; if it doesn't, the table shows which setup does.
- **How much:** 16 blocks of 192 bits, 3072 samples: about 5.3 ms at 150 MHz, with interrupts
  off (it holds pico_rand's spin lock, `PICO_SPINLOCK_ID_RAND`, since Lua on core 1 may read
  the TRNG through pico_rand at the same time). 5.3 ms is the expected hold. A timeout of
  20 ms ends a TRNG that stopped, so 20 ms is the stall bound: twice the 10 ms step rule, an
  exception like the 60 ms crypto steps, and reached only if the TRNG stalls. The timeout is not what was wrong in #242 (the
  autocorrelation stop was), and it stays a bound, not a fix.
- **A false FAIL** (one in 2^30 per test at the stated H) on a fresh open board's first boot
  leaves it without a root secret, on the rescue console where `health run` is refused.
  `reboot` runs the boot tier again and makes the secret once `trng` passes, so it is
  recoverable. A board that already has its secret is only refused new keys until the next
  pass.
- **The tests:** SP 800-90B §4.4 as a startup test, assuming H = 0.5 bit of min-entropy per
  sample and alpha = 2^-30 per test. Repetition Count: cutoff C = 1 + ceil(30 / H) = 61, so a
  run of 61 identical samples fails. Adaptive Proportion over non-overlapping windows of
  W = 1024: C = 1 + CRITBINOM(1024, 2^-H, 1 - 2^-30) = 810, so a window whose first sample
  occurs 810 or more times fails. Three windows. The host test computes both cutoffs again.
- **Expected pass rates:** at the assumed H = 0.5, a healthy source fails the repetition count
  about three boots in a million (3072 chances of 2^-30 each) and the proportion test about
  three in a billion (three windows); a source nearer one bit per sample far less. More than 8
  CRNGT errors in a run needs nine equal 16-bit pairs among about 190 comparisons, which at
  2^-8 each (H = 0.5) is about one in ten million, at 2^-16 (H = 1) never in practice.
- **The text says which:** `TRNG threw away 9 blocks (crngt 9, vn 0), 3 of 16 kept`,
  `TRNG autocorrelation error after 3 of 16 blocks` (only possible in `trng-diag` setups now),
  `the TRNG gave 3 of 16 blocks in 20 ms`, or the failing test with its count.
- **Afterwards:** the block is reset through RESETS before and after, so pico_rand finds it as
  after any reset; its next refill sets every register it relies on (sample period 0, all
  checks bypassed, the source on).

What the H = 0.5 assumption means: it isn't an entropy assessment of this oscillator (none is
published for RP2350 at this sample period). It sets how far from ideal the bits may be before
the check fails. A stuck oscillator fails at once (and the CRNGT also throws its blocks away),
a strongly biased one (over about 79 % of one value) fails the proportion test. A short
repeating pattern (0101...) passes both tests; it is the CRNGT's to catch (two equal 16-bit
blocks), and its errors fail the check. The board run should look at the margins in the text
(`run 12 (fails at 61), proportion 545/1024 (810), 5.3 ms`) and in `health trng-diag`.

The sampling differs from pico_rand's: pico_rand takes one sample every clk_sys cycle with
every check bypassed and hashes the result, which the datasheet's 17-cycle minimum rules out
for a test of raw bits (the oscillator is slower than clk_sys, so at one sample a cycle the
bits come in long runs, and the RCT would likely fail a healthy oscillator). The check tests
the same oscillator, not the same sample stream.

**`clocks`.** clk_sys, clk_peri, clk_usb, clk_adc and clk_hstx (those running) through the
frequency counter, FC0 (`frequency_count_khz()`), against `clock_get_hz()`: WARN beyond 1 %,
FAIL beyond 5 %. 18 ms in all on the board (#242). **What it can't see:** FC0 counts against
clk_ref, which comes from the crystal, so a crystal that drifts or is off moves every reading
together and the ratios still match. The check finds a PLL or divider set wrong. clk_ref itself
isn't measured (it would always match itself). The low-power oscillator (LPOSC, an RC
oscillator, 32.768 kHz nominal) measured the same way is the cross-check on the crystal, with a
wide band from the datasheet (§8.4.1: ±20 % untrimmed, ±14 % over temperature, ±20 % over
supply: 18 to 54 kHz): only a crystal off by about 2x shows there, and only as a WARN, since
the LPOSC is too loose to fail on. The ROSC isn't used: after the bootrom's quadrupling it can
be anywhere from 18 to 96 MHz, too wide to show even that. Crystal drift belongs to phase 4's
RTC drift check, against NTS time.

**`flash`.** The JEDEC ID (command 0x9f) through the SDK's `flash_safe_execute()` (interrupts
off, core 1 stopped) and `flash_do_cmd()`, as `flash_get_unique_id()` does: tens of
microseconds. Not through `flashop_run()`, so `uptime`'s count of flash writes stays one. The
text names the ID, the maker and the size (`JEDEC ef 40 16 (Winbond, 4 MB)`). A chip smaller
than `PICO_FLASH_SIZE_BYTES` FAILs: the configuration, the root secret record and the key
sector are at the end of the size the build assumes. Bigger is fine.

**Boot time.** `clocks` 18 ms (measured), `crypto` about 22 ms, `trng` 5.3 ms, `flash` well
under 1 ms: about 46 ms of the 100 ms budget, to be confirmed by the next board run. `health`
prints the tier's time (`boot checks: N ms`); `crypto` and `trng` give their own in their text.

**Test commands** (`-DSOS_TEST_COMMANDS=ON`), for the board run: `health crypto-times` runs
every known answer once and prints each one's time, the boot subset's total and the rest's;
`health trng-diag` prints the TRNG table above. Both hold the main loop while they run (about
200 ms and under half a second).

**Deviations from the plan in §10:**

- `clocks` doesn't measure clk_ref (circular) or the ROSC (its range is too wide), and the
  LPOSC cross-check WARNs instead of FAILing.
- `trng` samples every 256 cycles with the corrector and the block's autocorrelation test
  bypassed and its CRNGT on, not in pico_rand's one-sample-a-cycle mode (above).
- The crypto known answers are split: `crypto` (boot) and `crypto-ssh` (background), since the
  Ed25519 and X25519 ones don't fit the boot budget.
- `crypto` also tests SHA-512 (Ed25519 hashes with it) and the Ed25519 public key; X25519 has
  no refusal test.
- `flash` WARNs on a size code outside 0x10 to 0x1f and on no answer (all ones or zeros).
- `health_gate_refuse()` went with `health_refusal()`; callers ask `health_gate()` (§9.1).

**Cost** (debug build, `arm-none-eabi-size`, against main 37cf0c9): text +8.3 KB (the vectors
are about 600 bytes of it), bss +68 bytes; about 2 KB of heap while `crypto` or `crypto-ssh`
runs, 384 bytes of stack for `trng`.

### 9.3 Phase 3 design: checks of stored state

Status: design approved by Reviewer (#254), with the `config` cases of a plain record split as
below. Built: §9.4 lists where the code settled details, the texts shortened to fit a line
among them. Phase 3 adds six background checks that read
what the board keeps in flash, in OTP and on the card, and compare it with what the running
board uses. §9.4 records what the build settled.

**Common rules.**

- **Read only.** No check writes flash, OTP or the card, mounts a card, or repairs anything
  (§2.4). A finding names the feature's own command where one fixes it.
- **No gates.** None of the six holds KEYS or CRYPTO. They report state that is already stored;
  refusing to make keys or seal would protect nothing that isn't already damaged, and it would
  block the repair: a broken configuration is repaired by `setup` (which makes an owner secret
  and seals), a broken host key by making a new one. The gates stay with the checks that test
  the means of making keys (`trng`, `crypto`, `crypto-ssh`).
- **A FAIL says what the board is doing about it**, which in phase 3 is nothing new: the text
  reads as a finding, never as a refusal ("the record in use doesn't open; running on
  defaults"), because the rescue console shows it next to real refusals. Each FAIL text is
  listed below.
- **Key material stays where it is.** `config` opens sealed records only with the seal key
  `settings.c` already holds in RAM for saves, through the store it saves with; nothing is
  derived again (the key provider is locked after boot, and a second derivation path is what
  the lock prevents). `hostkey` compares a digest of the stored sealed record and the public
  fingerprint; it never opens the private key. `rootsecret` reads no secret: it uses the
  results `rootsource_init()` recorded at boot, plus the in-place record check `rootsecret`
  already runs (open boards only, below).
- **Tier and timing.** All six are HEALTH_BACKGROUND (they need the configuration, the host key
  and the card, none of which exists during the boot tier), and run under `health run`. First
  runs are staggered after boot so that the first minute isn't one long burst, then hourly; the
  card's every 6 hours. Each step stays under 10 ms (§4); the estimates are listed and the board
  run measures them with `health state-dump` (below), which times each read.

| Check | First run | Period | Steps | Estimated longest step |
|---|---|---|---|---|
| `config` | 40 s | 1 h | 2 (one slot each) | about 3.5 ms (CRC-32 over 2.2 KB, about 0.6 ms; AES-GCM open, about 2 to 3 ms) |
| `rootsecret` | 45 s | 1 h | 1 | under 0.1 ms (open board: two 256-byte pages checked in place) |
| `otp` | 50 s | 1 h | 1 | about 0.3 ms (75 raw and 64 corrected row reads, one SHA-256 of 64 bytes) |
| `failcount` | 55 s | 1 h | 1 | under 0.5 ms (one 4 KB sector scanned in place) |
| `hostkey` | 60 s | 1 h | 1 | under 0.2 ms (SHA-256 of one 256-byte page) |
| `card` | 90 s | 6 h | 1, then 1 per 2 KB of `/certs/roots.pem` | about 5 ms (two `f_stat` calls on an encrypted card; 2 KB read, decrypted and hashed) |

#### `config`: both configuration records

**Reads:** the two slots through `settings.c`'s store, with a new pure function in `config.c`,
`config_slot_info(store, slot, &info)`, host tested in `tests/test_config.c`. Its contract:
it reports a slot's **kind and sequence number only, never the record's contents** (the opened
record goes into the scratch buffer `slot_read()` already uses, and is wiped there). `slot_read()`
becomes a mapping of it, so loading and checking can't disagree. `config_in_use(info[2])` (pure)
picks the record in use by `scan()`'s rule (the plain one before a sealed one; else the higher
sequence number), which is what `config_load()` takes at boot. `settings.c` exposes both with
`settings_config_slot()` (the store is static there) and whether flash is usable
(`settings_persistent()`).

Kinds: `empty` (header all 0xff or all zero: erased, or blanked by `config reset`), `junk` (no
magic, not empty), `bad-header` (magic, unknown version or length), `bad-crc`, `refused` (a plain
record, CRC right, on a store that refuses plain), `unopened` (sealed, CRC right, doesn't open),
`invalid` (opens, contents out of bounds), `plain`, `sealed`.

| Status | When | Text (dev-1 or example) |
|---|---|---|
| PASS | in use: sealed; other slot empty or an older sealed record | `rev 28 sealed, in use (slot B); slot A rev 27 sealed` |
| WARN | in use: plain, the other slot not sealed (an open board; `settings_init()` seals a plain record at boot, so one still plain means that sealing failed: its fix is the sealing at the next boot, see dmesg) | `rev 3 plain, in use (slot A): not sealed yet (see dmesg)` |
| WARN | in use: plain, with a sealed record in the other slot. `scan()` takes a plain record before a sealed one whatever the sequence numbers, so this is a plain record written after the sealing: older firmware after a downgrade, provisioning, or a seal interrupted before the plain copy was erased | `rev 3 plain, in use (slot A): written after sealing (downgrade, provisioning or an interrupted seal)` |
| WARN | in use: sealed; other slot `junk`, `bad-header`, `bad-crc` or `invalid` | `rev 28 sealed, in use (slot B); slot A damaged (bad CRC): no fallback` |
| WARN | in use: sealed; other slot `unopened` | `...; slot A sealed under another key (old root secret?)` |
| WARN | in use: sealed; other slot `refused`. Only on a store that refuses plain records (a console-only board): on one that accepts them, the plain record would be the one in use (above). `config_scrub_plain()` erases it at boot, so one still there means that erase failed | `...; slot A: refused plain record left (console-only)` |
| WARN | nothing in use, a `refused` record (console-only board) | `plain record refused (console-only board); running unconfigured` |
| FAIL | nothing in use, a slot `unopened` | `sealed record doesn't open (another root secret?); running unconfigured` |
| FAIL | nothing in use, a slot damaged (any damaged kind) | `slot A damaged (bad CRC), no usable record; running on defaults` |
| FAIL | flash not usable | `firmware overlaps the config slots; settings aren't saved` |
| SKIP | both slots empty | `nothing saved yet (run setup)` |

Two steps, one slot each (each opens at most one sealed record). No retry or repair:
`config reset` and `setup` are the commands that change it. The static scratch buffer is shared
with `config_save()`; both run only on core 0 from the main loop, never interleaved (a check
step runs to completion).

#### `rootsecret`: the root secret's source against the OTP flags

**Reads:** nothing new from OTP. The source and what `rootsource_init()` found at boot
(`rootsource()`, `rootsource_otp_found()`, `rootsource_otp_index()`, a new accessor for its
`locks_ok`, `rootsource_otp_locked()`), `consoleonly_state()`, and on an OTP board the
**soft-lock register** `otp_hw->sw_lock[page]` (a register, not an OTP row: it says whether the
secret's page is still inaccessible). On an open board, `rootflash_state()` and
`rootflash_degraded()`: the same in-place check of the flash record that `rootsecret` runs from
any shell (FNV over each copy, read through XIP; nothing is copied).

| Status | When | Text |
|---|---|---|
| PASS | OTP; page soft-locked; list pages locked against the bootloader | `OTP page 5, locked for this boot; list pages locked` (dev-1) |
| PASS | flash (open board); both copies intact | `in flash, both copies intact (open board)` |
| WARN | OTP, but a list page is still writable from the USB bootloader (an open board uses it anyway, `rootsource.c`) | `OTP page 5; page 4 writable from the bootloader` |
| WARN | OTP, and the page's soft lock reads as not set after boot | `OTP page 5 not locked for this boot (readable by firmware)` |
| WARN | flash, one copy damaged | `in flash, one copy damaged; the other in use` |
| WARN | flash, the OTP list spent | `in flash; every OTP page for it is spent` |
| FAIL | no source, with the reason from boot: OTP secret fails its check, marker damaged, unreadable, console-only with none, locks open on a console-only board | `none: OTP page 5's secret fails its check; there are no keys` |
| FAIL | flash record unusable, none made, or the image overlaps it | `flash record unusable (damaged or newer); there are no keys` |
| FAIL | console-only board on a flash secret (impossible by `roototp_choose()`; checked as an invariant) | `console-only board using a flash secret (a bug); keys in use` |

The texts match `version` (`root secret: in OTP`) and `rootsecret` (`rootsource_describe()`),
which read the same values.

#### `otp`: OTP rows SOS relies on, and the boot keys

**Rows read, and only these** (through `otpread.h`'s read aliases, the read-only path `version`
and `bootkey` already use; the SDK's `otp_access` ROM call can program and is never linked into
everyday firmware, decision 12):

| Rows | What | How |
|---|---|---|
| 0x040-0x047 | CRIT1, eight copies (secure boot, debug, glitch detector) | raw |
| 0x04b-0x04d | BOOT_FLAGS1, three copies (KEY_VALID, KEY_INVALID) | raw |
| 0x080-0x0bf | BOOTKEY0_0 to BOOTKEY3_15, the four boot key slots | raw and error-corrected |
| (register) | OTP CRITICAL, what the chip latched at reset | register |

The root secret's rows (pages 4 to 6, rows 0x100-0x1bf) and their lock words are never read by
this check; every row number above is below 0x0c0. The console-only marker (0x0c0-0x0c2) isn't
read again: `rootsecret` uses its boot-time result.

**ECC without faulting.** The guarded alias faults on an uncorrectable row, which would restart
the board, so it isn't used. For each boot key row the check takes the raw 24 bits, undoes the
bit-repair flip (both BRBP bits set: invert all 24), recomputes the 6 parity bits of the 16 data
bits (the OTP macro's Hamming code, as picotool's `otp_calculate_ecc()` computes it), and
compares. It also compares the 16 data bits with what the unguarded corrected alias
(`otpread_ecc()`, which doesn't fault) returns for the same row: on a clean row they agree, and
a row the hardware corrected shows as a difference. A blank row (all zero) is consistent. Pure
and host tested (`statecheck.c`), with known rows from picotool's code. A row that reads as all
ones (no permission) is reported, never judged.

**Dev keys.** The OTP hashes of `keys/dev-INSECURE-1.pem` and `-2.pem` (SHA-256 of the 64-byte
public key, as `keys/README.md` shows them) are constants in `statecheck.c`, with the command
that computes them; a host test recomputes them from the `.pem` files with `openssl` when it is
installed, and fails if they differ. **Release builds** are those built with `NDEBUG` (CMake's
Release and MinSizeRel; the `release` preset): there a valid dev key is a WARN. In a debug build
it is expected (dev boards are meant to have them) and named in a PASS.

**The running image's key.** As `bootkey` does: the image's public key hashed and compared with
the slots (`bootkey_compare()`).

| Status | When | Text |
|---|---|---|
| PASS | secure boot on, rows consistent, the image's key in a usable slot | `secure boot, debug on; slots 0,1 valid: dev keys 1,2; image key slot 0` (dev-1, debug build; the slot numbers are what it finds) |
| WARN | release build, a usable slot holds a dev key | `valid slot 0 holds public dev key 1: anyone can sign firmware` |
| WARN | a named row unreadable (locked or damaged: the check can't tell) | `row 0x085 unreadable (locked or damaged)` |
| WARN | a boot key row's ECC doesn't match (correctable or not: it can't tell which the boot ROM saw) | `boot key slot 0 row 0x085: ECC mismatch (raw 0x...)` |
| WARN | CRIT1 copies disagree with each other, or their vote with the latched register (`version`'s WARNING) | `CRIT1 rows 0x.. vs latched 0x..: written since reset or damaged` |
| WARN | BOOT_FLAGS1 copies disagree | `BOOT_FLAGS1 copies disagree on bits 0x..` |
| FAIL | secure boot on and the image's key is in no usable slot (revoked, or the slots read differently than at boot) | `image key in no valid slot: it won't start after a reset (running on)` |
| SKIP | secure boot off, every slot blank, no KEY_VALID bit | `open board: secure boot off, no boot keys` |

With secure boot off and keys present (a board being prepared), the rows are still checked and
the image comparison is reported, never FAILed.

#### `failcount`: the wrong-password sector

**Reads:** the sector in place (`failflash.c`'s read), with a new pure function in `failcount.c`,
`failcount_inspect(sector, &info)`, host tested: marker present, erased, or neither; each track's
count; stray cleared bits after a track's count; cleared beyond attempts. And `failflash_count()`
(the count the console uses, RAM included).

| Status | When | Text |
|---|---|---|
| PASS | erased (never counted: no password check has started since it was made) | `no wrong passwords recorded` |
| PASS | marker, count 0 | `0 wrong passwords; 37 of 14336 checks used` (dev-1: some small number; 14336 is `FAILCOUNT_CAPACITY`, 7 pages of 256 bytes of bits, from `failcount.h`, and the code prints that constant) |
| WARN | count above 0: someone typed wrong passwords since the last right one (a check in progress counts too) | `3 wrong passwords since the last right one` |
| WARN | the RAM count is higher than flash's (flash writes didn't take) | `2 counted in RAM only: flash writes failed` |
| WARN | neither marker nor erased: reads as 0, and the next check writes a fresh sector | `no marker: counts as 0 until the next password check` |
| WARN | a track has cleared bits past its count, or cleared beyond attempts | `tracks inconsistent (attempts 5, cleared 7): counts as 0` |
| FAIL | the firmware image overlaps the sector | `firmware overlaps the sector; wrong passwords counted in RAM only` |

Garbage where the marker should be is a WARN, not the FAIL #233 suggested: the board's behaviour
is defined (it counts as zero and starts a fresh sector at the next check), and an erased sector,
which someone with a cable can produce, looks the same as a new one. The count is in the text.

#### `hostkey`: the stored host key against the one sshd serves

**Reads:** the key sector's host key page, and the fingerprint sshd computed at start
(`sshd_host_fingerprint()`). `hostkey.c` keeps, when it opens or stores the key, a SHA-256 of the
256-byte stored page (the sealed record as it is in flash: ciphertext, not key material). The
check hashes the page again and compares. It never opens the record.

| Status | When | Text |
|---|---|---|
| PASS | key in use; page unchanged since it was loaded; sealed for this owner | `sealed, as loaded; SHA256:abcdefghijklmnop...` (dev-1: its fingerprint) |
| WARN | in use unsealed (no root secret on an open board, `HOSTKEY_USE_PLAIN_UNSEALED`) | `stored unsealed (no root secret); SHA256:...` |
| FAIL | the page changed since the key was loaded (nothing but `hostkey.c` writes it) | `stored record changed since boot; sshd serves the key in RAM` |
| FAIL | no key, and `hostkey.c` gave up (no root secret, or the store failed) | `no host key (see dmesg); sshd isn't running` |
| SKIP | the board isn't set up (no owner secret), or no key asked for yet | `board not set up: no host key yet` |

Every path that rewrites the page goes through `hostkey.c`, which updates or clears the digest:
a key made or taken over in `hostkey_get()` (setup, or the first SSH start after it) records the
page after it is stored; `hostkey_owner_gone()` (`config reset`, which does not reboot) and
`hostkey_erase()` clear it, and the check is then SKIP until a new key is in use.

The comparison #233 asked for ("the record opens and its public key matches") would mean opening
the record into a second buffer; the digest of the page as loaded answers the same question (is
what's stored the key being served?) without that.

#### `card`: the protected directories and the approved roots

**Yields to everything that owns the card.** SKIP, without touching the card, when no card is
mounted (it never mounts one), the card was unmounted by hand (`sd unmount`), `sd check` runs, a
format runs, or `files_busy()` (a file command or SFTP has files open: it runs again at the next
period, or on `health run`). `sdcard.c` gets one read-only accessor for those states and
`locked_without_key`. All FatFs calls are on core 0, from the main loop, as every other card
user's (FatFs runs without a reentrancy lock here; one step finishes before anything else runs).
While it hashes, the check holds the files layer (`files_hold()`), so a format or unmount waits
for it as for any open file, and releases it at the end or on cancel; a remount between steps
(`sdcard_generation()` changed) ends it with SKIP.

**What "protected" means here.** The files layer protects `/certs` and `/var/log` (and `/var`)
by path (`path_is_protected()`, called by `files_write_allowed()` before every change), not by
anything stored on the card. So the check sees that each is a directory where expected; there is
no stored protection to read.

**Roots.** If `settings()->certs_hash` is set, `/certs/roots.pem` is hashed (SHA-256, 2 KB a
step, so a 64 KB file is 32 steps, each releasing the main loop) and compared, as
`certstore.c` does before using it.

**Not checked: the man bundle.** `man install` puts the pages of whatever commit is running on the
card, and a card may legitimately hold another commit's; there is no approved reference to
compare against.

| Status | When | Text |
|---|---|---|
| PASS | `/var/log` a directory; no roots approved | `exFAT encrypted; /var/log ok; no /certs: built-in roots only` (dev-1) |
| PASS | roots approved and the file matches | `...; /certs/roots.pem matches its approval` |
| WARN | approved, and the file differs, is missing or is over 64 KB | `roots.pem changed since approval: built-in roots only` |
| WARN | `/var/log` missing or not a directory | `/var/log missing: log files aren't written` |
| WARN | `/certs` exists but isn't a directory | `/certs is a file: remove it with rm on the console` (`/certs` is protected, so only the console's `rm` can) |
| WARN | an encrypted card is in and doesn't open with this board's key | `encrypted card doesn't open with this board's key; not mounted` |
| SKIP | the yielding cases above | `card unmounted by hand`, `sd check running`, `files open; next run` |

The encrypted card's key needs no separate check: an encrypted card that mounted was opened with
it.

#### Diagnostic and tests

**`health state-dump`** (test builds only; reads only; allowed over SSH too, since it prints no
secret): each config slot's kind, version, length, sequence number and the time to read it; the
slot in use; the root secret source and boot-time OTP findings; each boot key slot's first 8
hash bytes, valid/invalid bits, which dev key it is, and per-row ECC results; the failcount
sector's marker and track counts; the host key page's kind and whether its digest matches.
So one board run shows why each check passed or failed.

**Host tests:** every judging function from synthetic inputs, one test per status path:
`config_slot_info()` over made-up slots (each kind, both orders, wrap-around sequence numbers),
`failcount_inspect()` over made-up sectors, the ECC recomputation over rows from picotool's code
and one-bit and two-bit damage, the OTP judging from made-up summaries (debug and release), the
dev key constants against the `.pem` files, and the card and host key judging.

**Board run** (dev-1 slot; no OTP writes of any kind):

1. `health run` on the console: the six new rows as predicted above; `health state-dump` saved
   with the result. `ssh` "longest" and `mem` before and after.
2. FAILs through `health inject <name> fail`, for each new check: logged, no gate closes (making
   a key still works: `health` shows no refusal).
3. **Real damage to the older config slot**, by Reviewer's plan: back up both config sectors
   with picotool (`picotool save -r` of the two sectors below the key sector, files kept until
   the board is back), find the older slot in `health state-dump`, write a copy of that sector
   with one payload byte flipped, boot, `health run config`: expect
   `...; slot X damaged (bad CRC): no fallback`, the board configured as before. Restore the
   saved sector, boot, `health run config`: PASS again. The newer slot is never written.
4. `failcount`: one wrong console password, then the right one; over SSH, `health state-dump`
   between the two shows the count 1 (the background run may or may not catch it; that is fine).
5. `card`: with SFTP holding a file open, `health run card` is SKIP `files open`; after
   `sd unmount`, SKIP `card unmounted by hand`; `sd mount` again, PASS.

### 9.4 As built (phase 3)

Built as §9.3 says, with these details settled by the code. The judging is in `statecheck.c`
(`tests/test_statecheck.c`: 43 tests, one per status path at least), the reads in
`statecheck_board.c`; `config_slot_info()` and `config_in_use()` are tested in
`tests/test_config.c`, `failcount_inspect()` in `tests/test_failcount.c`.

**Texts.** A result is one line of at most 63 characters, so some of §9.3's texts are shorter;
the host test fails any tested text that reaches 63 (it may have been cut). Where they differ:

| Check | §9.3 | As built |
|---|---|---|
| `config` | `rev 3 plain, in use (slot A): written after sealing (downgrade, ...)` | `plain rev 3 (A) in use, written after sealing (downgrade?)` |
| `config` | `rev 3 plain, in use (slot A): not sealed yet (see dmesg)` | `plain rev 3 (A) in use: not sealed yet (see dmesg)` |
| `config` | `...; slot A damaged (bad CRC): no fallback` | `rev 28 sealed, in use (B); A bad CRC: no fallback` |
| `config` | `sealed record doesn't open (another root secret?); running unconfigured` | `sealed record in A doesn't open; running unconfigured` |
| `otp` | `image key in no valid slot: it won't start after a reset (running on)` | `image key in no valid slot: no boot after a reset; running on` |
| `otp` | `secure boot, debug on; slots 0,1 valid: dev keys 1,2; image key slot 0` | `secure boot on, debug on; valid 0=dev1 1=dev2; image key in 0` |
| `otp` | `CRIT1 rows ... written since reset or damaged` | `CRIT1 rows 0x05 vs latched 0x01: written since reset/damaged` |
| `otp` | `valid slot 0 holds public dev key 1: anyone can sign firmware` | `valid slot 0 holds public dev key 1 (release build)` |
| `card` | `...; /certs/roots.pem matches its approval` | `exFAT encrypted; /var/log ok; roots.pem matches its approval` |
| `card` | (`/certs` present, nothing approved) | `exFAT encrypted; /var/log ok; unapproved roots: built-in only` |
| `card` | `/var/log missing: log files aren't written` | `/var/log missing: the log files aren't being written`, and `/var/log is a file: no log files; rm it on the console` |
| `failcount` | `firmware overlaps the sector; wrong passwords counted in RAM only` | `firmware overlaps the sector; wrong passwords in RAM only` |

`hostkey`'s PASS (`sealed, as loaded; SHA256:...`) shows the first 37 characters of the
fingerprint's 43; `ssh` shows all of it. **Release build** means `NDEBUG`, which CMake sets for
the `release` preset (Release) and the size preset (MinSizeRel); the `debug` preset is a debug
build, where a dev key in a valid slot is named in the PASS.

**Also settled:**

- `config` reads slot A in its first step and slot B in its second. A save between the two
  writes only the slot not in use, so the pair it judges is still consistent; a `config reset`
  between them gives one stale result, put right at the next run.
- `config`'s FAIL for "nothing usable" takes `unopened` before a damaged slot (it says more),
  and a damaged slot before a `refused` one.
- `failcount` judges, in order: the firmware overlapping the sector (FAIL), counts in RAM that
  flash didn't take, no marker, inconsistent tracks, a count above 0 (WARN), then PASS. The
  capacity in its text is `FAILCOUNT_CAPACITY` (7 pages of 256 bytes, one bit each: 14336).
- `otp` judges unreadable rows first, then the SKIP of an open board, then each boot key row
  (`ECC mismatch` before `corrected`), then CRIT1 against the latched register, CRIT1's copies,
  BOOT_FLAGS1's copies, the image's key, and dev keys. Copies are compared only on the bits SOS
  relies on (CRIT1 0x7f, BOOT_FLAGS1's KEY_VALID and KEY_INVALID). The dev key hashes are
  constants checked by the host test with `openssl` (it says so and skips the comparison when
  `openssl` is missing). `_Static_assert`s pin the row numbers and bits to the SDK's names, and
  the last row read below the root secret's first page (`ROOTOTP_ROW(0, 0)`).
- `card`'s `locked` WARN is for an encrypted card this board's key doesn't open (another
  board's, or a configuration erased since its format); a card locked for lack of a key (a board
  not set up yet) or of memory is SKIP `no card mounted`, since mounting looks at it again. A
  card unmounted, remounted or taken by `sd check` between hashing steps ends the check with SKIP
  `the card changed during the check; next run`; no memory for the 2.8 KB of hashing state
  (`HEAP_FILES`) is SKIP `no memory to read roots.pem; next run`.
- `hostkey.c` records the page digest at the end of every successful `hostkey_get()` (after any
  store: a new key, or one taken over), and clears it in `hostkey_owner_gone()` (`config reset`,
  which doesn't reboot: the check is then SKIP until setup makes the next key) and
  `hostkey_erase()`. Nothing else writes the key sector.
- `health state-dump` (test builds) is in any shell, as Reviewer allowed; it reads the config
  slots (with their read times), what the root secret check reads, the OTP rows (per slot: the
  hash's first 8 bytes, its valid and invalid bits, which dev key, and how many rows failed the
  ECC comparison), the failcount tracks, the host key's state, and the card's owners. It
  doesn't touch the card.

**Step times** (estimates; `health state-dump` measures the config slots and the OTP reads on
the board): `config` about 3.5 ms per step (one slot: CRC-32 over 2.2 KB bit by bit, about
0.6 ms, and an AES-GCM open, 2 to 3 ms), `otp` about 0.3 ms (139 row reads, one SHA-256 of 64
bytes, the image's block loop), `rootsecret`, `failcount` and `hostkey` under 0.5 ms each,
`card` about 5 ms per step (two `f_stat` calls, or 2 KB read, decrypted and hashed).

**Cost** (debug build, `arm-none-eabi-size`, against main 61ea7d6): text +8.8 KB
(1229096 to 1238152 bytes), bss +536 bytes (the OTP rows gathered, 400 bytes, kept off the
stack; the config check's two slot records; the host key digest). While `card` hashes
`roots.pem`, about 2.8 KB of heap (`HEAP_FILES`: the open file, a SHA-256 context and a 2 KB
buffer).

**Board run:** as §9.3 lists, with the texts above.

## 10. Phases

1. This note, then the framework and seed checks (§9). Host tests, then a dev-1 slot.
2. **Boot tier:** crypto known-answer tests (AES-XTS, AES-GCM, SHA-256, HMAC, Ed25519 and
   Curve25519 as SSH uses them; run on core 0, KEYS + CRYPTO), TRNG startup test (repetition
   count and adaptive proportion on raw TRNG output, SP 800-90B style; KEYS), clocks (FC0
   against the configured frequencies), and the flash JEDEC ID and size. As built: §9.2.
3. **Stored state:** both config records (needs `config.c` to report each slot), root secret
   source against the OTP flags, the host key against its fingerprint, the card key unwraps,
   the failcount marker, `/var/log` and `/certs` present and protected, the man bundle and cert
   store hashes. Also the OTP ECC rows, the boot key hashes (dev or production set), and the CRIT
   flags against `version`. Reads only, never writes OTP. Design: §9.3; as built: §9.4.
4. **Background tier:** RTC drift as ppm after each NTS sync, the brownout detector and
   `last reset: brownout`, a heap free-list walk in steps, a core 1 heartbeat while a script
   runs, and reset history.
5. **Full tier:** a RAM march test over free heap and SCRATCH_Y in steps, the running-image
   hash, and `sd check` as a check.
6. **Counters:** CYW43 firmware version and bus errors, SD SPI CRC errors and retries.

## 11. Left open for later phases

- **Image hash (phase 5):** either a post-build step that hashes the image and writes the hash
  into a known section, or the signature block of a signed build (only signed builds would
  have one).
- **Reset history (phase 4):** either a flash sector written like the failcount sector (bits
  cleared, rarely erased), or a POWMAN scratch register for the count plus the `--- boot`
  lines already in `/var/log/kern.log`.
