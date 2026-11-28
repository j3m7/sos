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
waiting for its board run. Phase 4, the background tier: designed in §9.5 (approved with
changes, #255), built (§9.6), waiting for its board run. Phase 5, the full tier: designed in
§9.7 (approved, #262, with the image hash source decided by the user), built (§9.8), waiting
for its board run. Phase 6, the counters: designed in §9.9 (approved with changes, #268),
built (§9.10), waiting for its board run.
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
(`otpread_ecc()`, which doesn't fault) returns for the same row: on a clean row they agree.
A 1-bit error in the raw row shows as `ECC mismatch` (its parity no longer fits);
`corrected` is reported only when the raw row is self-consistent but the alias returns other
data. The check can't tell a correctable error from an uncorrectable one, and doesn't try.
A blank row (all zero) is consistent. Pure
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
- `hostkey` after `hostkey_erase()`: WARN `erased: new key at next boot; sshd serves the old
  one`. The FAIL `stored record changed since loaded` is kept for a page that changed without
  hostkey.c writing it.
- `failcount` judges, in order: the firmware overlapping the sector (FAIL), counts in RAM that
  flash didn't take, no marker, inconsistent tracks, a count above 0 (WARN), then PASS. The
  capacity in its text is `FAILCOUNT_CAPACITY` (7 pages of 256 bytes, one bit each: 14336).
- `otp` judges unreadable rows first, then the SKIP of an open board, then the image's key
  (the one FAIL, before every WARN, so a damaged row or split copies can't hide that the board
  won't start after a reset; it uses the corrected hashes), then each boot key row
  (`ECC mismatch` before `corrected`), CRIT1 against the latched register, CRIT1's copies,
  BOOT_FLAGS1's copies, and dev keys. Copies are compared only on the bits SOS
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

### 9.5 Phase 4 design: the background tier

Status: approved with changes by Reviewer (#255), and the open points decided by the user:
the history sector at -6, and `crash brownout` in test builds only. This section reads as
decided; the decisions are listed at its end. Phase 4 adds three background checks
(`rtc-drift`, `heap-sum`, `core1`), widens two existing ones (`resets`, with every reset reason
the chip can name and a reset history; `supply`, with the brownout detector's setting), adds one
flash sector (the history sector), adds a test-build diagnostic that walks newlib's heap
(`health heap-walk`), and wraps `health state-dump`'s long lines. §10 item 4 listed five things;
where this design does them differently it says so, and the list is at the end of this
section.

**Common rules.**

- **Read only, as in phase 3.** No check writes flash, the RTC or the heap. The writes this phase
  adds (a record in the history sector at each boot and at each time sync) belong to the boot
  path and to `timesync.c`, not to a check, so §2.4 still holds.
- **No gates.** None of the new checks holds KEYS or CRYPTO. A drifting crystal, a past
  brownout, a damaged heap or a stuck core 1 doesn't make the means of making keys less
  trustworthy: `trng` and `crypto` test those directly. A damaged heap threatens everything, but
  refusing to seal or make keys wouldn't protect anything, and it would stop the repair
  (`reboot` is the repair, and it needs nothing). Each FAIL text says what the board does.
- **Texts are ASCII and under 63 characters**, as §9.4 settled (the tests fail a text that
  reaches 63). Uncertainties are written `+-`.
- **Judging is pure and host tested**, in a new `src/core/bgcheck.c` (`tests/test_bgcheck.c`),
  as `statecheck.c` is for phase 3; the reads are in `bgcheck_board.c`. The heap-walk
  diagnostic's logic is pure too (`heapwalk.c`, run over synthetic arenas on the host). The
  history sector's
  format is pure (`history.c`, `tests/test_history.c`), with its flash access in
  `history_board.c`, the way `failcount.c` and `failflash.c` are split.

| Check | Tier | First run | Period | Steps | Estimated longest step |
|---|---|---|---|---|---|
| `resets` (widened) | boot | — | — | 1 | about 0.3 ms (the history sector scanned in place, 4 KB) |
| `supply` (widened) | background | 60 s | 60 s | 1 | one more register read |
| `core1` | background | 10 s | 10 s | 1 | under 10 us (three words read) |
| `heap-sum` | background | 70 s | 5 min | 1 | tens of microseconds (one `mallinfo()`, which `heap` already calls) |
| `rtc-drift` | background | 100 s | 10 min | 1 | under 10 us (reads what `timesync.c` recorded) |

First runs avoid phase 3's (40 to 90 s). The registry then holds 22 checks with the two
test-only gate checks; `HEALTH_MAX` goes from 24 to 32, room for phase 5's full-tier checks and
phase 6's counters.

#### Reset reasons: what the chip can say

Today three places work out the last reset's reason, each a little differently: `main.c`'s boot
log line (fault or watchdog only), `uptime` (fault, glitch detector, watchdog) and the log
files' boot marker (`logfile_board.c`: those three or `power-on or reboot`); `resets` judges the
same three. Phase 4 puts the decoding in one pure function, `resetwhy_decode()` (new
`src/core/resetwhy.c`, host tested), and all four use its result.

**What the registers hold** (RP2350 datasheet §6.4 CHIP_RESET, §7.3 table 560, §12.9 watchdog):

- **POWMAN CHIP_RESET** latches which chip-level reset happened last: HAD_POR (power-on),
  HAD_BOR (brownout), HAD_RUN_LOW (the RUN pin), HAD_DP_RESET_REQ and HAD_RESCUE (the debugger),
  HAD_GLITCH_DETECT, HAD_HZD_SYS_RESET_REQ, HAD_SWCORE_PD and the HAD_WATCHDOG_RESET_* variants.
  The bits are read-only and stay until the next chip-level reset that resets POWMAN. POR, BOR,
  RUN and the debugger's requests hard-reset POWMAN, so after one of those only its own bit is
  set. A glitch detector reset doesn't reset POWMAN, so an older HAD_POR may still be set beside
  HAD_GLITCH_DETECT.
- **The SDK's watchdog reset** (every `reboot`, a picotool reboot or flash, `fault.c`'s reset and
  a real timeout) resets the PSM but not POWMAN: CHIP_RESET keeps the bits of the reset before
  it. That is why `glitchdet_boot()` already requires `watchdog_hw->reason == 0` before it
  believes HAD_GLITCH_DETECT. The watchdog's REASON register (TIMER, FORCE) is cleared by every
  chip-level reset, so a non-zero REASON means this boot came from the watchdog.
- **A past brownout is latched**, in HAD_BOR, and readable at the next boot. That answers
  Reviewer's question: the BOD register (POWMAN offset 0x1c) is only the detector's setting
  (EN and the threshold VSEL); there is no live status bit, and while DVDD is under the
  threshold the chip is held in reset, so there is nothing to read until it boots again.
- **A brownout clears the POWMAN scratch registers** (table 560: "AON scratch: reset" for POR,
  BOR and RUN) as well as the watchdog's. So after a brownout the glitch detector's counts, the
  uptime before the reset and the last temperature and supply readings (`glitchdet.h`, scratch 0
  to 4) are gone, as after a power cycle. Only the history sector (below) keeps anything across
  it.

**The order of decoding** (pure, so each case is a host test with register values):

1. A fault record in the watchdog scratch (`fault_last()`): `fault` with its text, as today.
2. Watchdog REASON non-zero: `watchdog` if `wdog_caused_reset()` (a real timeout), else
   `reboot` (asked for: `reboot`, picotool, the boot ROM).
3. Otherwise CHIP_RESET, first the bits of resets that leave older bits standing:
   HAD_GLITCH_DETECT `glitch detector`; HAD_HZD_SYS_RESET_REQ, HAD_DP_RESET_REQ or HAD_RESCUE
   `debugger`; HAD_WATCHDOG_RESET_* or HAD_SWCORE_PD `watchdog` (SOS sets none of the modes that
   cause them); then HAD_POR `power-on` (with or without HAD_BOR beside it, below), HAD_BOR
   alone `brownout`, HAD_RUN_LOW `reset pin`.
4. None of them: `unknown (CHIP_RESET 0x...)`.

**HAD_BOR with HAD_POR is a power-on** (Reviewer): on a cold power-up the brownout detector
trips as the core rail rises, after the power-on reset has let go, so an ordinary plug-in can
leave both bits set. Only HAD_BOR without HAD_POR is reported as a brownout: the supply dipped
under the threshold without falling to nothing. The cost is that a brownout during the first
moments of a power-up reads as a power-on, which is what it nearly was. `resetwhy_decode()`'s
comment says so, and a host test pins it.

`main.c` logs `last reset: brownout (core supply under 0.95 V)` at warning (a fault and the
glitch detector stay at err, a watchdog timeout at warning), `uptime` adds
`(last reset: brownout)`, and the log files' marker names any reason, so `power-on or reboot`
becomes one or the other. `power-on`, `reboot` and `reset pin` are not logged as a line of their
own (they are ordinary).

**Is the brownout detector on?** Yes, and SOS doesn't need to turn it on: the BOD register's
reset value is EN = 1 with VSEL = 0b01011 (0.946 V; the datasheet's VSEL table marks 0.860 V as
"default", but the reset value column, the §7.6.2.2 text and the SDK's header agree on 0x0b), it
is re-enabled by every reset, and neither SOS nor the SDK writes it (no use of `powman_hw->bod`
or `POWMAN_BOD` outside the SDK's register headers). So
"turn on the brownout detector" in §10 item 4 is already true, and **nothing changes on USB
power**. Raising VSEL nearer DVDD's 1.1 V would only make resets more likely on the dips a
Wi-Fi transmission causes; the datasheet warns against going above 1.1 V. The threshold stays.

**What the detector watches is DVDD, the 1.1 V core supply**, made from 3.3 V by the on-chip
regulator, not VSYS. DVDD holds while the board's buck-boost regulator keeps 3.3 V, which it
does well below the VSYS figures `supply` judges. So no VSYS threshold predicts a brownout, and
a dip short enough to cause one (the detector asserts after 3 to 10 us) is invisible to
`sensors_poll()`, which reads VSYS every 10 s. The `supply` thresholds of phase 1 stay as they
are; a brownout is reported by its reset reason.

**No separate `brownout` check.** A past brownout is a reset reason (`resets`), and how many
there were is reset history (below); the detector's setting is one register, which `supply`
reads:

| Check | Change | Status | Text |
|---|---|---|---|
| `resets` | a brownout, the reset pin, the debugger and unknown join fault, watchdog and glitch detector | WARN for brownout, debugger and unknown, as for fault, watchdog and glitch; PASS for power-on, reboot and reset pin | `last reset: brownout (core supply under 0.95 V)` |
| `supply` | the BOD register: EN = 1 and VSEL = 0x0b | WARN otherwise (nothing in SOS writes it, so this is an invariant, like `rootsecret`'s console-only FAIL) | `brownout detector off or moved (BOD 0x000000b0)` |
| `supply` PASS | the BOD state named | — | `on USB, VSYS min 4.61 V; BOD on at 0.946 V` |

#### Reset history and boot loops

**Decided (the user, on #255): a new flash sector, the history sector**, written the failcount
way (records programmed into erased flash, the sector erased only when full). §11's other
option, a POWMAN scratch register for counts plus the `--- boot` lines in
`/var/log/kern.log`, can't see the resets that matter most here: a brownout, a power cycle and
the reset pin all clear the POWMAN scratch registers (table 560), so a board browning out over
and over would show no count and no loop. The log lines exist only with a card mounted, are
written lazily (a run that crashes before the card is mounted, or before the next write, leaves
none), and reading them back would mean parsing files on the card from a check. The scratch
option's cost is low (no flash, no format), but it answers the wrong question.

**Where it goes.** The sixth sector from the end of flash, `PICO_FLASH_SIZE_BYTES -
6 * FLASH_SECTOR_SIZE` (0x3fa000 on dev-1's 4 MB), directly below the failcount sector. The end
of flash then holds, from the top: keys (-1), configuration B and A (-2, -3), root secret (-4),
failcount (-5), history (-6). (`docs/system.md`'s layout table lacks the failcount sector; phase 4
adds both rows.) **Why it can't collide:**

- **The image.** SOS's image is about 1.24 MB (debug build) and grows from the start of flash;
  it would have to grow by about 2.7 MB to reach the sector. As `failflash.c` does,
  `history_board.c` checks `__flash_binary_end <= XIP_BASE + HISTORY_OFFSET` at boot and, if
  not, never reads or writes the sector (`resets` then says `history off: the image overlaps
  its sector`), so even an image that big damages nothing it didn't already overwrite.
- **The root secret.** Its sector is -4, two sectors up; the offsets are each a constant times
  the sector size from the end, and a `_Static_assert` in `history_board.c` checks that
  `HISTORY_OFFSET` is below `FAIL_OFFSET`, which is below `ROOT_OFFSET` (the offsets move to a
  shared header, `flashlayout.h`, so the assertion sees all five).
- **Other writers.** SOS has no partition table; picotool's `load` erases and writes only the
  image's sectors; the boot ROM writes no flash. Only a whole-chip erase clears it, and then it
  reads as erased, which is a valid empty history.

**What older firmware does with it: nothing.** No earlier image reads or writes the sixth sector
from the end, so a downgrade can't damage it and it can't confuse older firmware (the "config
downgrade hazard" was about records older firmware reads). Two effects remain, both gaps rather
than damage: boots under older firmware aren't recorded, and an older image's time sync rewrites
the RTC without a record, which spoils the next drift measurement (see `rtc-drift`). For the
future, the sector's header carries a format version: firmware that finds a version newer than
its own never writes the sector and says `history sector of a newer format: not written`, so a
later phase can change the format without the downgrade problem the config records had.

**Format** (pure, `history.c`):

- Page 0: a header, 16 bytes: a magic word, the format version (1), the rest 0xff.
- Pages 1 to 15: records of 16 bytes, 240 in all, appended in order into erased slots:

| Bytes | Boot record (kind 1) | RTC record (kind 2) |
|---|---|---|
| 0 | kind; 0xff = free | kind |
| 1 | reason (`resetwhy` code) | calibration steps in force (int8) |
| 2-3 | boot number (wraps at 65536) | 0 |
| 4-7 | uptime before the reset, s; 0xffffffff unknown | 0 |
| 8-11 | wall time at boot (Unix s), 0xffffffff if the RTC had none | the time written to the RTC (Unix s) |
| 12 | glitch detectors fired, or the fault kind | 0 |
| 13 | 0xff | 0xff when written; programmed to 0x00 once the RTC took the time |
| 14-15 | CRC-16 (CCITT) over bytes 0-12 | the same |

- **The uptime before a reset** comes from `glitchdet.c`'s uptime register (POWMAN scratch 2,
  kept to the second), which survives a watchdog reset, a fault and a glitch detector reset; a
  new accessor `glitchdet_prev_uptime_s()` returns it for any reason, where today it is kept only
  after a detector reset. After a power-on, brownout or reset pin it is unknown.
- **The boot record is written** once per boot, right after `timesync_boot()` (so the RTC's time
  is known) and before `health_boot()` (so `resets` sees it): one page program, about 1 ms with
  interrupts off, core 1 not started yet. It goes through `flashop_run()`, so `uptime`'s count of
  flash writes counts it.
- **Full:** the sector is erased (about 45 ms typical for this chip, 400 ms at most, well inside
  the 8 s watchdog), the header written again, and the newest 16 boot records and the newest
  confirmed RTC record copied back from RAM. A power cut between the erase and the copy loses the
  history (not the board's ability to keep one): the next boot finds an erased or header-only
  sector and starts again.
- **Damage:** a record with a wrong CRC or an unknown kind is skipped and counted; a record past
  the first free slot is stray (counted). Neither is used. A sector with no magic and not erased
  (old image data, say) is erased before its first record, as the failcount sector is.
- **Wear.** One erase per 240 boots and syncs. A boot loop of one boot every 3 s (SOS waits up to
  2 s for USB at boot) erases it about 120 times a day, so 100 000 cycles last over two years of
  continuous looping; a loop nobody notices for that long has worse problems. So there is no
  loop throttling, which would need a counter somewhere else.

**Boot loops.** A run is **short** when it ended within `GLITCHDET_STABLE_S` (180 s): by its
uptime before the reset when that is known, else by the wall times of its boot record and the
next one when both are set, else it doesn't count as short. A loop is the last
`GLITCHDET_GUARD_RESETS` (3) resets in a row, none of them a `reboot` (one asked for), each
ending a short run. Any reason counts, the brownout first among them; the glitch detector's own
guard stays as it is (it decides whether to arm the detector; this only reports).

**Shown where.** The judging joins `resets` (boot tier: it reads the history in place, about
0.3 ms), whose text stays one line:

| Status | When | Text |
|---|---|---|
| WARN | a loop | `3 short runs in a row (brownout, brownout, power-on)` |
| WARN | the last reset was a fault, watchdog, glitch detector, brownout, debugger or unknown (as above) | `last reset: brownout (core supply under 0.95 V)` |
| WARN | the history has damaged or stray records | `history: 2 damaged records skipped; last reset: reboot` |
| WARN | the image overlaps the sector, or the sector is of a newer format | `reboot; history off: the image overlaps its sector` |
| PASS | otherwise | `power-on; 41 boots recorded, no loop` |

The list itself is a new subcommand, `uptime history` (any shell; reads only): one line per
recorded boot, newest first, with its time, reason, the uptime before it and the boot number, and
the RTC records marked. `man/uptime.txt` gets it.

#### `rtc-drift`: the RTC's rate and the crystal's

**Two clocks, two measurements.** Between syncs the system clock (`gettimeofday()`, the always-on
timer) counts the RP2350's 12 MHz crystal (XOSC); the RTC is read only at boot. So the step a
sync makes (`timesync_info().step_ms`) measures the XOSC when the last sync was in this uptime,
and a mix of the RTC (until the boot) and the XOSC (since) when the board restarted in between.
That is why `docs/time.md`'s calibration procedure restarts the board before the second sync.
The check measures each clock separately:

- **The RTC:** at every accepted sync, before `apply_time()` writes the RTC, it reads the RTC
  (`rtc_read()`, one I2C read, about 1 ms) at the NTS second boundary that `apply_time()`
  already waits for. The RTC's error is its reading minus that second; the interval is that
  second minus the time written at the previous sync, from the newest confirmed RTC record in
  the history sector. Rate in ppm = error / interval, positive when the RTC runs fast (as
  `docs/time.md` defines it).
- **The XOSC:** when the previous accepted sync was in this uptime, its rate is minus the step
  over the time between the two syncs (a negative step means the clock had run ahead).

This also covers what phase 2's `clocks` can't see (§9.2): FC0 measures every clock against
clk_ref, which comes from the same crystal, so a crystal that is off moves everything together.
The XOSC measurement is the first that compares the crystal with an outside reference.

**How exact.** The RTC counts whole seconds, so one reading is good to 1 s. Its calibration adds
a sawtooth: in its default mode the PCF8523 applies the whole correction once every two hours
(a pulse a minute until done), so between corrections it runs at the uncorrected rate, and its
error swings by up to |steps| x 4.34 ppm x 7200 s, 31 ms a step (0.66 s for dev-1's 21 steps).
The uncertainty is taken as 1000 ms + 32 ms x |steps|, plus half of each sync's round trip
(tens of ms). Polling the seconds register for its tick during `apply_time()`'s wait would take
the 1 s down to a millisecond only when the tick falls in that wait, and the sawtooth would stay,
so it isn't done. The XOSC's uncertainty is half the sum of the two syncs' round trips.

**The record.** `timesync.c` keeps the latest measurement in RAM for `timesync_drift()`:
whether each part is valid, the error and its uncertainty (ms), the interval (s) and the
calibration steps in force. It is computed in `apply_time()` because only there are the NTS
time and the RTC's reading taken at the same moment; the check reads it (a callback from
`timesync.c` into `health` would need a second way of starting a check, for no gain: a
measurement waits at most 10 minutes to be judged, and `health run rtc-drift` judges it at once).
The pure arithmetic (ppm, uncertainty, the suggested `time cal` value) goes in `timeconv.c`
beside the calibration steps; the judging in `bgcheck.c`.

**The baseline across restarts** is the RTC record in the history sector, written before
`rtc_write()` and confirmed (byte 13 programmed to 0) after it read back. A record left
unconfirmed (the write failed, or power went in between) ends the baseline: the next sync only
starts a new one. The baseline is also unusable, and the next sync starts a new one, when the RTC
reported its oscillator stopped (`RTC_NOT_SET`) or was absent, or when the calibration steps
differ from the record's (`time cal` changed the rate in between).

**Thresholds.**

- **RTC: WARN beyond 20 ppm** (about 1.7 s a day). A 32.768 kHz tuning-fork crystal is typically
  specified to +-20 ppm at 25 C, and its temperature curve (about -0.034 ppm/C^2) costs at most
  about 3.4 ppm between 15 and 35 C, so a healthy RTC, calibrated or within its crystal's
  tolerance, stays inside it; beyond it the crystal's load doesn't match (the PCF8523's CAP_SEL,
  which `rtc.c` leaves as it finds it) or the calibration is wrong. The Adalogger's crystal part
  isn't documented in this repository; the board run's figure checks the assumption. The WARN
  text suggests the `time cal` value that would correct it: the calibration in force plus the
  measured rate.
- **RTC beyond `time cal`'s range** (-277.76 to 273.42 ppm, `timeconv_cal_steps()`): still WARN,
  saying so. No FAIL: nothing on the board depends on the RTC's accuracy beyond what a sync
  repairs.
- **XOSC: WARN beyond 65 ppm.** The Pico 2 W's crystal is the ABM8-272-T3 (the hardware design
  guide; datasheet §8.2.1.1): +-30 ppm tolerance, +-30 ppm stability
  over temperature, +-5 ppm aging in the first year.
- **XOSC: FAIL beyond 1000 ppm**: the RP2350's clocks are designed with margin for up to 1000 ppm
  of XOSC error (datasheet §8.1.2.3); clk_sys runs at its 150 MHz maximum, so beyond that it runs
  out of specification. Nothing is gated.
- **Judged only when sure.** A part is WARN only when |rate| minus its uncertainty is over the
  threshold, PASS only when |rate| plus its uncertainty is within it, and otherwise "too short to
  judge". With a 1 s reading, an uncalibrated RTC 92 ppm fast shows as WARN after about 4 hours
  between syncs; a calibrated one passes after about a day (1.7 s over 20 ppm is 85 000 s).

| Status | When | Text (dev-1 or example) |
|---|---|---|
| PASS | the RTC within 20 ppm, sure; the XOSC within 65 ppm or not measured | `RTC +0.9+-19 ppm in 24 h, cal 91.1; XOSC -1.8+-0.6 ppm` |
| WARN | the RTC surely beyond 20 ppm | `RTC +92.1+-13 ppm in 22 h, no cal: try time cal 92` |
| WARN | the same, calibrated | `RTC +23.4+-19 ppm in 24 h, cal 91.1: try time cal 114.5` |
| WARN | beyond `time cal`'s range | `RTC beyond time cal's range: +310+-12 ppm in 22 h` |
| WARN | the XOSC surely beyond 65 ppm | `XOSC +71.2+-0.9 ppm in 26 h (crystal: 30+30 ppm)` |
| FAIL | the XOSC surely beyond 1000 ppm | `XOSC +1210+-3 ppm: beyond the chip's 1000 ppm clock margin` |
| SKIP | the latest measurement too short to judge | `RTC +5+-45 ppm in 6 h, no cal: too short to judge` |
| SKIP | one sync since the baseline began (or none) | `one sync since the RTC was set: drift after the next` |
| SKIP | the calibration changed since the baseline | `calibration changed since the RTC was set; next sync` |
| SKIP | no RTC; the XOSC part alone, judged as above when measured | `no RTC (Adalogger not answering): XOSC -1.8+-0.6 ppm` |

The calibration in force is always in the text, as Reviewer asked: `no cal`, or the value as
`time cal` takes it (positive slows the RTC). The XOSC part is dropped from a text that wouldn't
fit, and `health state-dump` prints both parts in full. `time` gains a `drift:` line with the
same figures, so the procedure in `docs/time.md` no longer needs the arithmetic by hand (the doc
is updated with it).

**Expected on dev-1.** Its configuration was reset on 2026-11-25, which cleared the calibration
(`time` shows `cal: off`), and the RTC was 92 ppm fast before. So after a sync, a wait of four
hours or more (a restart in between is fine) and a second sync: WARN
`RTC +92+-13 ppm in 22 h, no cal: try time cal 92`, if the crystal is as it was. After
`time cal 92` and a day between two more syncs: PASS with `cal 91.1`. That is the verification of
`time cal` that the earlier work couldn't do. Before two syncs it is SKIP.

**How long between syncs.** The figure after `+-` is the measurement's uncertainty, and the
check never WARNs or PASSes while that interval straddles a threshold. Uncalibrated, the
uncertainty is about 1.05 s (the whole second plus the round trips), so a sure PASS within 20 ppm
needs at least 1.05 s / 20 ppm, about **14.6 hours** between the baseline and the measuring sync;
calibrated at dev-1's 21 steps (about 1.7 s) about 24 hours. A shorter gap can still show a
large error for sure (92 ppm surely over 20 after about 4 hours), but it can't show that the RTC
is within 20 ppm: until then the result is SKIP `too short to judge`, with the figure. The XOSC's
uncertainty is only the round trips (tens of ms), so an hour between two syncs in one uptime
judges it.

**A known limit, from the dev-1 queue.** An older image that syncs rewrites the RTC with no
record. Nothing can detect it (older firmware leaves no trace in the sector or the RTC). On its
next sync the phase 4 image measures against the stale baseline: the RTC was set right part of
the way through, so the error read is only what it gathered since then, divided by the whole
interval, and the rate reads too small once (nearer zero; for a fast RTC it can be a false PASS).
That sync writes a new baseline, so the measurement after it is right again. The board run starts
its baseline with a `time sync` on the phase 4 image, and `docs/time.md` says to sync twice after
running older firmware.

**The crystal's load (CAP_SEL) is not in this phase.** dev-1's 92 ppm suggests the PCF8523's
CAP_SEL (7 pF from reset, which `rtc.c` leaves as it finds it) doesn't match the Adalogger's
crystal; a crystal loaded with less capacitance than it is cut for runs fast. That goes to a
separate issue, fed by this check's first dev-1 measurements.

#### `heap-sum`: the heap's totals, and a heap walk by hand

**What the allocator is.** The firmware links Arch's arm-none-eabi newlib 4.6.0 (full newlib, not
nano: `nm` shows `__malloc_av_`, `__malloc_sbrk_base` and `_mallinfo_r` in `sos.elf`), wrapped
by the SDK's `pico_malloc` (its own mutex around malloc, calloc, realloc and free). Inside,
newlib's `mallocr.c` (Doug Lea's malloc 2.6.4) takes `__malloc_lock()`, which `heap.c` makes a
recursive mutex. Chunks lie end to end from the start of the arena to the top chunk; each starts
with a size word whose bit 0 says whether the chunk before it is in use; a free chunk also holds
the doubly linked list pointers of its bin and repeats its size at its end.

**The options** weighed for review were: (1) the totals that `heaptag` and `mallinfo()` give,
(2) canaries on `heaptag` blocks, (3) a validated walk of newlib's chunks and bins under its
lock. **Decided (Reviewer): the background check uses only the totals; the walk is a test-build
diagnostic run by hand.** The walk's own failure mode, tripping over an allocator change and
FAILing a healthy board after a toolchain update, is worse than what it finds, and holding the
allocator's lock for up to 8 ms blocks core 1's allocations meanwhile. Canaries were never
the choice: they change `heaptag`'s design (blocks carry no header, and freeing needs no size),
cost bytes on each of the thousands of small blocks a script makes, and see only tagged blocks.

**`heap-sum` is its own check, not part of `heap`.** `heap` judges how much is free, with
hysteresis, because ordinary use comes near its WARN line; its status follows load. `heap-sum`
judges invariants whose failure means a bug. Folding them into one status would let a
hysteresis WARN hide a broken invariant, or an invariant WARN hold `heap` away from PASS when
memory is plentiful; two rows say two different things.

**What it reads**, all public: `mallinfo()` (in use `uordblks`, free `fordblks`, the arena
`arena`), `sbrk(0)` (the heap's current end), the linker's `__end__`, `__HeapLimit` and
`__StackLimit`, core 0's stack limit register MSPLIM (the stack guard, `PICO_USE_STACK_GUARDS`),
and the sum of `heaptag`'s tags. The `mallinfo()` and `sbrk(0)` reads are taken under
`__malloc_lock()` so they describe the same moment; nothing allocates while the lock is held, and
the step allocates nothing.

- **The tags' sum is at most the bytes in use.** Each tag counts `malloc_usable_size()` of its
  blocks, which is less than the chunk `mallinfo()` counts, and untagged blocks only add to the
  in-use side. A sum over it means a block released under the wrong tag, or released twice (the
  counter underflows and the sum becomes huge). The tags are updated outside the lock, just after
  malloc returns or just before free; only a shrinking realloc briefly counts more than the heap,
  so a violation is read twice, 1 ms apart, and counts only if both reads show it.
- **In use plus free equals the arena.** In newlib 4 this holds by construction (`mallinfo()`
  computes in use as arena minus free), so it can fail only if the allocator or its `mallinfo()`
  changed; then it says so instead of being trusted.
- **The arena's end against the stack guard.** The arena ends at `sbrk(0)`, which must be at or
  above `__end__ + arena`, within `__HeapLimit` and `__StackLimit`, and below core 0's MSPLIM. The
  SDK's `_sbrk()` refuses to pass `__StackLimit`, so a FAIL here means the layout and the guard
  disagree: the stack could then grow into the heap without the guard catching it.

| Status | When | Text (example) |
|---|---|---|
| PASS | all hold | `in use 96 KB (tags 81 KB), arena 210 KB of 380 KB` |
| WARN | the tags' sum over the bytes in use, twice | `tags hold 312 KB, heap in use 120 KB: a double release?` |
| WARN | in use plus free isn't the arena | `mallinfo: in use + free 210 KB, arena 212 KB` |
| FAIL | the arena's end beyond the stack guard or the heap's limit | `heap end 0x2007e010 beyond the stack guard 0x2007e000` |

**Expected on dev-1:** PASS, `in use` about 100 KB idle.

**`health heap-walk`, the diagnostic** (test builds, console, run by hand, holds the main loop
while it runs): the walk of option 3, with every guard of the design kept, since a person reads
its output:

- **Build time:** newlib 4 with `MALLOC_ALIGNMENT` 8, or the command says it isn't built for this
  C library. Another minor version than 4.6 gives a compiler `#warning`, in this test-only code
  and nowhere in the production build.
- **A known answer first:** outside the lock, it allocates a block of a known size; the walk must
  find that block's chunk, in use, with the size newlib's rounding gives, or it prints
  `newlib's heap layout not recognised` and stops before judging anything.
- **Two views cross-checked:** every chunk from the arena's start to the top (each size a
  multiple of 8, at least 16, ending inside the arena; a free chunk's size repeated at its end;
  no two free chunks neighbours; the top chunk ending at `sbrk(0)`), then each of the 128 bins'
  lists, every pointer checked before it is followed (inside the arena, aligned, naming a chunk
  the walk saw as free, the back pointer naming the node before, no more nodes than free chunks,
  so a cycle ends). The two views must agree on the free chunks' count and bytes.
- **Lock order:** the walk holds `__malloc_lock()`'s mutex alone and never calls malloc or free
  while holding it. `pico_malloc` takes its own mutex first and then newlib's, so core 1 may hold
  the SDK's and wait for newlib's while core 0 walks; a malloc inside the walk would then wait for
  the SDK's: a deadlock. The known-answer block is allocated before the lock and freed after.
- **Bounded:** it checks the time every 256 chunks and stops at 8 ms (about 24 000 chunks would
  take some 3 ms), printing how far it got.

It prints the chunks, the free chunks and their bytes, the largest free block, the walk's time,
and the first rule broken with the chunk's address and the four words around it. It changes
nothing. The pure part (`heapwalk.c`) is host tested over synthetic arenas.

#### `core1`: a heartbeat while a script runs

**What a stuck core 1 does to the board today** (Reviewer's question), from `lua_core1.c` and
`lua_host.c`:

- **Nothing on core 0 waits for core 1.** Core 0 answers a script's requests from the main loop
  and never blocks on it (`lua_host_poll()` polls `lua_req`; the shell job polls the job's state).
- **Ctrl-C and `lua stop`** only set `lua_job.cancel`; the instruction-count hook (every 1000
  Lua instructions, `HOOK_EVERY`) raises the error. A shell job stops waiting at once on Ctrl-C
  (the prompt comes back; the script is then "stopping" in `lua ps`), but a script inside a long
  C call (a pathological `string.find` pattern, `string.rep` of megabytes) runs no Lua
  instruction, so the hook doesn't run and it doesn't stop until the call returns.
- **What stays blocked:** no other script can start (`a script is already running`) until it
  ends, and its memory (the Lua state and the 32 KB stack) stays held. Flash writes still work:
  core 0 pauses core 1 with the lockout interrupt, which core 1 takes even inside a C loop (Lua
  never turns interrupts off).
- **The watchdog doesn't cover core 1, and shouldn't:** it is fed by core 0's main loop, which
  keeps running, and a stuck script is no reason to restart a board that serves SSH. If core 1
  ever held the heap's lock for good, core 0's next malloc would block, the loop would stop, and
  the watchdog would reset the board with `last reset: watchdog timeout`. A hard fault on core 1
  already resets the chip with `fault.c`'s record (`hard fault on core 1 at ...`).

So the check's FAIL says what the user loses: scripts, until the call returns or a reboot.

**The heartbeat.** A new shared word pair in `lua_bridge.h`: `beats`, which core 1 increments in
the hook (every 1000 instructions, or every instruction after a stop) and in its own wait loops
(`sos.sleep()`'s 5 ms loop, the timers' wait), and `waiting`, which core 1 sets while it waits
for core 0 (`request()`: an HTTP request, a ping of up to 100 s, a file read), for room in the
output buffer, or for the prompt's next line, and clears after. One increment per 1000
instructions costs nothing measurable. Core 0's check reads `lua_job.state`, `lua_job.cancel`,
`beats` and `waiting`, and keeps in its own state the last `beats` seen and when it last moved.

**Rules.** A long C call (`sos.http` waiting on core 0) reads as waiting, not as a hang; a silent
core 1 not waiting is a long C call or stuck, which the check can't tell apart until a stop is
asked for. The 30 s WARN line is three runs with no beat; a normal C call in a script (sorting,
JSON, hashing a few hundred KB) is well under a second.

| Status | When | Text (example) |
|---|---|---|
| PASS | a script runs and `beats` moved since the last run | `loop.lua running 42 s, 9800 hooks/s` |
| PASS | core 1 is waiting for core 0, the output or a line | `loop.lua waiting for core 0 (http) for 12 s` |
| WARN | no beat for 30 s, not waiting, no stop asked | `loop.lua in one C call for 31 s: Ctrl-C waits for it` |
| FAIL | a stop was asked 10 s ago or more, and no beat since and still running | `core 1 ignored stop for 40 s: no new script until it returns` (seen on dev-1: the stuck call returning frees core 1 again, and the check says so; a call that never returns needs a reboot) |
| PASS | the script ended after this check had WARNed or FAILed (logged as the change back) | `core 1 free again: script ended after 52 s` |
| SKIP | no script running (the usual case) | `no script running` |

**Expected on dev-1:** SKIP, unless a script runs (`/autorun.lua` would show as running).

#### `health state-dump`'s long lines

`dump()` in `statecheck_board.c` formats into a 112-byte buffer, so the root secret, failcount,
host key and boot-key-slot lines lose their tails. Phase 4 formats into 256 bytes and prints
through a pure `statecheck_wrap()` that breaks a line at the last `; ` or `, ` before column 79,
continuing with a two-space indent (a word longer than the line is cut hard), host tested. Phase
4's own dump lines (CHIP_RESET and the watchdog's REASON as read, the reason decoded, the BOD
register, the last 8 history records, the drift record, `heap-sum`'s figures and `core1`'s
counters) go through the same function.

#### Test commands and tests

**Test commands** (`SOS_TEST_COMMANDS`):

- **`crash brownout`** (approved by the user: test builds only, never in the normal image, to
  be run once on dev-1): a real brownout reset, by writing the BOD register's VSEL to 0b10001
  (1.204 V, above DVDD's 1.1 V), EN kept, with the POWMAN password in the top half
  (`powman_hw->bod = POWMAN_PASSWORD_BITS | 0x11 << 4 | 1`). The detector resets the chip within
  about 40 us (programming and assertion delays, datasheet §7.6.2.3), and the reset puts the
  register back to its default, so the next boot is ordinary. That is exactly why the datasheet
  says not to set it above the supply. It is the same reset as a power dip and clears what one
  clears (the POWMAN and watchdog scratch registers). **It refuses** unless nothing is writing
  flash or the card: no file open (`files_busy()`: SFTP, `cp`, `tee`, a `>` redirection), no
  `sd check` or format running, no Lua script running (a script writes through core 0), and no
  log lines waiting for the card; flash writes are synchronous on core 0 (`flashop_run()`), so
  none can be in progress while a console command runs. It then flushes the log files, as
  `reboot` does, waits 100 ms, and writes the register. Console only.
- **`sos.spin(ms)`** in Lua, test builds only: a C loop on core 1 that runs no Lua instruction,
  to make a long C call on purpose.
- **`health heap-walk`**: the newlib heap walk described under `heap-sum`, run by hand.
- **`health history-fill`**: appends test records (kind 0x7f, shown as `test`, never judged)
  until one slot is left, so the next boots exercise the erase and the copy back.

**Host tests:** `resetwhy_decode()` for every CHIP_RESET bit alone, with a stale HAD_POR beside
HAD_GLITCH_DETECT, and under each watchdog REASON; the history format (append, full and the copy
back, damaged and stray records, a newer version left unwritten, an unconfirmed RTC record); the
boot-loop rule (known and unknown uptimes, a `reboot` breaking the row); the drift arithmetic
(signs, the sawtooth term, "too short to judge" at its edges, the suggested `time cal` value, the
range); `heap-sum`'s totals; the heap walk over synthetic arenas built in newlib's layout (a
clean one, then each rule broken in turn: a size not a multiple of 8, a missing footer,
neighbouring free chunks, a bin pointer outside, a cycle, a back pointer wrong, a top chunk
short of the heap's end); `core1`'s rules with made-up counters and times; the wrapping.

**Board run:** the plan, with the commands and the texts to expect, is in the phase 4 PR
(#255); in short, after backing up the config and history sectors: the new rows in `health run`;
each reset reason (`reboot`, `crash fault`, `crash hang`, `crash brownout`) and `uptime history`
across them; a boot loop of three `crash fault`s; `health history-fill` and the erase;
`health heap-walk` idle and with a script allocating; `core1` with a running script, one waiting
for core 0 and one in a long C call (`sos.spin()`), stopped; `rtc-drift` over two syncs, at least
14.6 hours apart for a PASS (the slot may end first: the second sync can be a later slot's, or the
periodic sync if `time auto` is on); the wrapped `health state-dump`.

**Cost estimate:** text about +6 KB, bss under 600 bytes (the drift record, the heartbeat words,
the history's RAM copy of the newest records), one 4 KB flash sector.

#### Decisions on review, and changes to §10 item 4

**Decided** (Reviewer and the user, on #255):

1. **The history sector at -6** (0x3fa000), with `flashlayout.h`'s ordering assertions, the
   image-overlap guard and a version byte (the user).
2. **No heap walk in the background:** `heap-sum` checks the totals; the walk is the test-build
   diagnostic `health heap-walk` (Reviewer). The minor-version `#warning` is in the diagnostic
   only.
3. **`crash brownout`** is approved for test builds only, refusing while anything writes flash or
   the card (the user).
4. **HAD_BOR with HAD_POR decodes as power-on** (Reviewer).
5. **`uptime history`** is the command's name; `resets` stays the check's.
6. **Thresholds:** RTC WARN 20 ppm, XOSC WARN 65 and FAIL 1000 ppm, judged only when sure.
7. **CAP_SEL** goes to a separate issue.

**Changes from §10 item 4:**

- **"The brownout detector" needs no turning on:** it is on from reset at 0.946 V and nothing
  writes it. The phase reports brownout resets and checks the setting instead, and adds no
  `brownout` check: reasons go to `resets`, the setting to `supply`, the count to the history.
  No new VSYS threshold, since the detector watches DVDD.
- **No background heap walk:** `heap-sum` checks the totals in the background, and the walk is
  a diagnostic run by hand.
- **RTC drift measures two clocks**: the RTC against NTS, and the RP2350's crystal from the steps
  between syncs in one uptime, which is the measurement §9.2 left to this phase.
- **Reset history is not a check of its own:** it is judged by `resets`, listed by
  `uptime history`, and stored in a new sector that also holds the RTC's baseline.

### 9.6 As built (phase 4)

Built as §9.5 decides, with these details settled by the code. The judging is in `bgcheck.c`
(`tests/test_bgcheck.c`, 21 tests), the reset decoding in `resetwhy.c` (`tests/test_resetwhy.c`),
the history format in `history.c` (`tests/test_history.c`), the heap walk in `heapwalk.c`
(`tests/test_heapwalk.c`, 13 tests over arenas in newlib's layout); the reads are in
`bgcheck_board.c`, `resetwhy_board.c` and `history_board.c`. The offsets of the sectors at the end
of flash moved to `flashlayout.h`, which asserts their order.

**Texts.** As in §9.5, but where they differ:

| Check | §9.5 | As built |
|---|---|---|
| `resets` | `3 short runs in a row (brownout, brownout, power-on)` | `3 short runs in a row; last reset: brownout` (three reason names don't fit a line) |
| `rtc-drift` | `RTC +92.1+-13 ppm in 22 h, no cal: try time cal 92` | the same; a rate of 100 ppm or more has no decimal (`RTC +310+-13 ppm in 22 h: beyond time cal's range`) |
| `rtc-drift` | `one sync since the RTC was set: drift after the next` | `RTC set 3 h ago (no cal): drift at the next sync`, and before any baseline `RTC: no baseline; drift after two syncs (no cal)` |
| `rtc-drift` | `XOSC +1210+-3 ppm: beyond the chip's 1000 ppm clock margin` | `XOSC +1210+-3.0 ppm: beyond the chip's 1000 ppm margin` |
| `core1` | `loop.lua waiting for core 0 (http) for 12 s` | `loop.lua waiting for core 0, running 82 s` (the request's kind isn't kept) |
| `supply` | `on USB, VSYS min 4.61 V; BOD on at 0.946 V` | phase 1's text plus `; BOD on` (`VSYS min 4.61 V since the last check, USB power; BOD on`) |
| `heap-sum` | `heap end 0x2007e010 beyond the stack guard 0x2007e000` | the same, and `heap end ... beyond its limit ...` for the lower of `__HeapLimit` and `__StackLimit` |

A result joins its second part (the XOSC after the RTC) only if the whole stays under 63
characters; `health state-dump` prints both in full.

**Also settled:**

- **The RTC record is written after the RTC, not before.** §9.5 had it appended before
  `rtc_write()` and confirmed after. But the RTC must be written on the second boundary, and a
  flash write takes a millisecond (an erase, when the sector is full, up to 400 ms), so the record
  is appended just after `rtc_write()`, confirmed at once if the RTC took the time and left
  unconfirmed if not (an unconfirmed record ends the baseline). A power cut in the millisecond
  between the two leaves the old baseline, and the next measurement reads too small once, as after
  an older image's sync.
- **The RTC's uncertainty** is 1000 ms + 32 ms per calibration step + the measuring sync's round
  trip (the baseline's round trip isn't recorded; one round trip covers half of each). SNTP syncs,
  which give no round trip, count 50 ms.
- **The measurement is read at the boundary** with one `rtc_read()` (about 1 ms at 100 kHz), so the
  RTC is written about 1 ms after the boundary: well inside the second it can resolve.
- **The first sync with no baseline** becomes the baseline: the check then says
  `RTC set 5 min ago (no cal): drift at the next sync`, the age counted from that sync.
- **`time cal`** marks a measurement taken under the old calibration as `calibration changed`, and
  before any sync since boot the check reads the baseline's calibration from the history sector.
- **`uptime`** now always names the reason (`(last reset: power-on)`); a fault keeps its own line.
  `sos.health()`'s `reset` table gains `reason`, the same names (`cause` is unchanged).
- **`resets`' history** is read in place: the newest 16 boot records, judged with
  `history_short_runs()`. Damaged and stray records are counted together.
- **`core1`'s run time** is the script's own (`lua_host_job_seconds()`); its name is the file's base
  name, `lua -e` or `lua prompt`. `waiting` is set in `request()`, while the output buffer is full,
  and while the prompt waits for a line; `beats` counts in the hook, `sos.sleep()` and the timers'
  wait.
- **`heap-sum`** reads `mallinfo()` and `sbrk(0)` under the heap's lock (`heap_snapshot()`),
  `__end__`, `__HeapLimit`, `__StackLimit` and MSPLIM; a tags-over-use reading is taken again after
  1 ms.
- **`health heap-walk`** starts at `__malloc_sbrk_base` rounded up to 8 and takes the top chunk
  from `__malloc_av_[2]`; the known block is a `malloc(100)` (a 104-byte chunk). It is built only
  with newlib 4 and 8-byte alignment (`_Static_assert` on 32-bit pointers); another minor version
  than 4.6 gives a `#warning` in that test-only block. A broken rule is also logged at err.
- **`crash brownout`** puts the register back and says so if no reset came within 100 ms.
- **`health history-fill`** fills the sector to the end, so the next boot's record erases it.
- **`health state-dump`** prints every line through `statecheck_wrap()` at 79 columns, phase 3's
  lines formatted into 256 bytes instead of 112.
- **`HEALTH_MAX`** is 32.
- **A script's wall error** (`other shells haven't read the last messages yet: try again`) was 66
  characters and the 64-byte error field cut it: shortened, found by the strict warnings.

**Step times** (estimates): `resets` reads at most 4 KB in place at boot, under 0.5 ms; `core1`,
`rtc-drift` and `supply` read a few words; `heap-sum` one `mallinfo()` (which `heap` already
calls). The boot record adds one page program (about 1 ms) to every boot; a sync adds an RTC read
and a page program or two.

**Cost** (`arm-none-eabi-size`, against main 06e0722): debug text +10.8 KB (1238280 to 1249296),
bss +1.3 KB (most of it `HEALTH_MAX` 24 to 32; the history's 16-record read buffer, 320 bytes,
is static); release text +16.1 KB, bss +1.3 KB; the test build text +15.6 KB. One 4 KB flash
sector.

### 9.7 Phase 5 design: the full tier

Status: approved by Reviewer (#262), with the image hash source decided by the user; the
decisions are listed at the end of this section. Phase 5 adds the three full-tier checks that
§7 promised, `ram`, `image` and `sd-check`, run only by `health full` (and one at a time by
`health run <name>`, which §4 allows for a check of any tier). It settles §11's image hash
question (decided: the HASH_VALUE item of the image's own block), and changes one build rule:
the image is hashed by picotool on every build, signed or not.

**Common rules.**

- **Full tier only.** The background scheduler never starts them (`period_s` 0, tier
  HEALTH_FULL), and `health run` without a name skips them, as it skips the test-only gate
  checks today. Results live in RAM like every other, so after a reset the three read `-` /
  `never` until `health full` runs again, and any gate they held is open again (§6: NONE doesn't
  close a gate).
- **Console only,** as `health full` already is. Lua sees their results, as for every check,
  and runs nothing.
- **Steps of 10 ms or less** (§4), in the order of the table, after every boot and background
  check. One documented exception, accepted on review as `crypto-ssh`'s 60 ms steps were: the
  first step of `sd-check` opens the card as a mount does (below). The job's budget per call
  stays `HEALTH_JOB_STEP_US` (20 ms), so one call may run two or three short steps back to back,
  as it does for the other tiers.
- **Read only, as in phases 3 and 4**, except the RAM test's writes into memory it owns (heap
  blocks it allocated, and SCRATCH_Y, which nothing uses; each SCRATCH_Y piece is put back
  within the step that tests it). No check writes flash or the card.
- **Ctrl-C** cancels the check that is running (§4). What each one leaves behind is in its
  part below; in short, nothing: the RAM test frees its blocks, the hash frees its context, and
  `sd check` ends as `sd check` ends.
- **Texts ASCII and under 63 characters** (§9.4), judged by pure functions, host tested: the
  march test in a new `src/core/memtest.c`, the judging of all three in `src/core/fullcheck.c`
  (`tests/test_fullcheck.c`), the image's hash items in `bootkey.c` beside the block loop it
  already walks; the reads in `fullcheck_board.c`.
- **A new heap tag, `HEAP_HEALTH`** (`health` in `mem`), for the RAM test's blocks and the
  hash's context, so `mem` and `heap-sum` see them for what they are.

| Check | Gates | Steps | Longest step (estimate) | Total on dev-1 (estimate) |
|---|---|---|---|---|
| `ram` | KEYS, CRYPTO (proposed) | one 4 KB heap block, or one 1 KB piece of SCRATCH_Y, a step: about 50 | about 2 ms (a 4 KB block); 0.5 ms with interrupts off (a 1 KB piece of SCRATCH_Y) | about 0.1 to 0.2 s |
| `image` | KEYS, CRYPTO (proposed) | the block, then 16 KB of flash a step: 78 | about 5 to 7 ms (16 KB through wolfCrypt's SHA-256) | about 0.4 to 0.5 s |
| `sd-check` | — | as `sd check`, with a 6 ms target per step | the card's opening, then under 10 ms | SKIP (card mounted); 0.5 to 0.7 s on an unmounted card |

`health full` on dev-1 then takes about 0.9 s with the card mounted (about 0.3 s for the boot
and background checks, `crypto-ssh`'s 190 ms the most of it) and about 1.5 s with it
unmounted. Each figure is to be measured: the PASS texts carry their times.

**The registry:** 20 checks today, 23 with phase 5, 25 with the two test-only gate checks, of
`HEALTH_MAX` 32. Phase 6's counters (§10 item 6: three or so) still fit.

#### `ram`: a march test over memory nobody is using

**What can be tested, and what can't.** RP2350 has 520 KB of SRAM (datasheet §4.2: eight
64 KB banks, 32 bits wide, word-striped four to each 256 KB, and the two 4 KB scratch banks),
with no ECC or parity. In the debug build at main 11112a8:

| Memory | Where (dev-1 build) | Tested | Why |
|---|---|---|---|
| Free heap | the arena from `__end__` 0x2001e9d4 to `__HeapLimit` 0x2007e000 (381 KB), less what is in use (about 100 KB idle) | yes, in blocks the check allocates | heap blocks the check owns are memory nobody else can reach: `pico_malloc`'s mutex keeps core 1 out, interrupt handlers don't allocate (lwIP has its own pools, `MEM_LIBC_MALLOC` 0), and no DMA channel is pointed at memory its owner freed |
| SCRATCH_Y | 0x20081000-0x20082000 (4 KB) | yes, in place, a 1 KB piece at a time, put back | `.scratch_y` is empty in the build (`__scratch_y_start__` = `__scratch_y_end__`), no source names it, and `config/ld/` moved the core 0 stack out of it; checked again at run time (below) |
| Heap in use | blocks other code holds, core 1's 32 KB Lua stack among them (allocated at the first script and kept) | no | in use; its owners read and write it at any time, core 1 from the other core |
| Small free pieces | free chunks under 4 KB between blocks in use | no | the check allocates 4 KB blocks; smaller pieces would cost a step each for little memory |
| Statics | `.data` and `.bss`, 125 KB: lwIP's heap (`MEM_SIZE` 32000) and pools, the CYW43 driver's buffers (written by its PIO and DMA from interrupts), FatFs's windows, the health registry, everything else | no | always in use, much of it written from interrupts or by DMA at any moment |
| Core 0 stack | the top 8 KB of main RAM (`STACK0`) | no | in use by the code running and by every interrupt handler; its unused part is painted for `stackmon_peak()`, which a test would wipe |
| SCRATCH_X | 0x20080000-0x20081000 (4 KB) | no (decided on review) | the linker reserves it as core 1's stack (`.stack1_dummy`). SOS starts core 1 on its own heap stack (`multicore_launch_core1_with_stack()`), so nothing seems to use it today, but a later `multicore_launch_core1()` would, silently; the code asserts at build time that the range it tests in place doesn't reach it |
| USB RAM, XIP cache, boot RAM | — | no | not main memory: TinyUSB's buffers, the flash cache, the boot ROM's state |

So on an idle dev-1 the check reaches about 180 KB of free heap and SCRATCH_Y's 4 KB: about a
third of the SRAM. Memory in use is tested only by being used.

**Which test.** March C- (van de Goor): `{w0}; up(r0,w1); up(r1,w0); down(r0,w1); down(r1,w0);
{r0}`, ten accesses a word. It is the march test IEC 60730's class B RAM tests are usually built
on, and finds every stuck-at, transition and address decoder fault, and the unlinked coupling
faults (inversion, idempotent and state) between two words. A cheaper one, MATS+ (five
accesses a word), finds stuck-at and address decoder faults but no coupling faults; the
difference is 1 ms a block, so the stronger one is taken. Because the memory is read and
written a word at a time, coupling between bits of one word needs more than 0 and 1: each block
runs March C- over six data backgrounds, `00000000`, `55555555`, `33333333`, `0f0f0f0f`,
`00ff00ff`, `0000ffff` (with their complements as the "1"), which is the usual extension for
word-wide memory (log2(32) + 1 backgrounds). That is 60 accesses a word.

**What it can't find.** Retention faults (a cell that loses its value after a while: the test
reads back within microseconds, and a retention test would hold the memory for seconds per
pattern); faults that depend on temperature or supply at another moment; soft errors except by
chance; and faults in memory it doesn't test (above). An address decoder fault shows only
between two addresses that are both in tested memory; the striping spreads each 4 KB block over
four banks, so each bank's decoder is exercised, but not over its whole range. A decoder fault
that maps an address in use onto a tested block would let the test's writes reach memory in use;
on such a board every write already does that, so the test adds no new harm.

**Steps.**

1. **Plan** (first step): note the free heap (`heap_free()`) and check SCRATCH_Y is still free:
   the linker symbols say `.scratch_y` is empty, MSPLIM (core 0's stack guard) and the stack
   (`__StackBottom`) are outside it. If either fails, SCRATCH_Y is left out and the text says
   so.
2. **Heap blocks**, one a step, **with interrupts on** (nothing else can reach a block the test
   owns, so there is nothing to keep out): allocate 4 KB under `HEAP_HEALTH`, run the six March
   C- passes over the 4 KB asked for (not over the allocator's extra bytes), keep the block (so
   the next allocation is other memory), and go on while the free heap stays above **96 KB**
   (`HEALTH_HEAP_CLEAR`, the `heap` check's line back to PASS: a TLS handshake and an SSH login
   still fit while the blocks are held). About 2 ms a block: 60 accesses a word, about 5 cycles
   each with volatile accesses in C, 1024 words, at 150 MHz.
3. **SCRATCH_Y**, a 1 KB piece a step: before each piece, check that no DMA channel that is busy
   has its write address in SCRATCH_Y (all 16 of RP2350's channels, 0 to 15: each one's
   CTRL_TRIG.BUSY and WRITE_ADDR, a few microseconds; a busy one there is named in the text and
   SCRATCH_Y is left out); copy the piece into a block the test already passed, run the passes
   on it with interrupts off on core 0, copy it back, interrupts on. About 0.5 ms with
   interrupts off. Core 1 isn't stopped: nothing it runs knows the address, and stopping it
   would cost more than the test.
4. **Release** every block (last step, or on Ctrl-C).

Blocks are held only while the test runs, about 0.1 to 0.2 s in all on an idle board. They are
allocated as the test goes, so the free heap comes down to 96 KB only near the end. What the
march leaves in the blocks is a pattern; freeing them as they are wipes whatever freed data
was in that memory before, which does no harm.

**A mismatch** records the address, what was written and what was read, the march element and
the background. The same block (or piece) is tested again in the next step: a fault that comes
back at the same address is a FAIL, one that doesn't is a WARN (a soft error, or something else
wrote into memory it had freed). At that moment the check also looks at the 16 DMA channels'
busy bits and write addresses, and the log line names a busy channel writing into the block,
since that would be a use-after-free elsewhere in SOS, not a bad cell. **Every mismatch is
logged** (at warning for a WARN, at err for a FAIL) with the address, what was written and what
was read, the element and the background, so two WARNs a week apart can be compared: the same
address twice is a weak cell, two addresses are more likely soft errors or a stray write.

| Status | When | Text (example) |
|---|---|---|
| PASS | every pass over every block and piece read back as written | `184 KB of free heap + 4 KB SCRATCH_Y in 0.12 s: no faults` |
| PASS | the same, SCRATCH_Y in use (the linker put something there) or a DMA channel pointed into it | `184 KB of free heap in 0.11 s; SCRATCH_Y in use: not tested` |
| WARN | a mismatch that the second test of that block didn't repeat | `0x2004a1f0 read 00000400 for 00000000 once, not again` |
| FAIL | the same address wrong in both tests | `RAM 0x2004a1f0 bad: power-cycle; keys, seals off until reset` |
| SKIP | free heap under 96 KB and SCRATCH_Y not testable | `free heap 80 KB, under the 96 KB kept free: not tested` |

With the free heap under 96 KB but SCRATCH_Y free, it tests SCRATCH_Y alone and says so.

**What a FAIL does (decided on review): KEYS and CRYPTO, on the FAIL only.** A WARN holds no
gate. A stuck or coupled cell confirmed twice is a hardware fault in memory the heap hands out.
The next key made, or the next record sealed, may be built in that memory, and what is then
written to flash carries the flipped bit for good: a host key or a sealed configuration that is
consistent with itself and wrong, which nothing would notice until it is used. Refusing those
until the next reset costs little (the person is at the console, reading the result) and
protects what is stored. Reads aren't gated, so the board can still be looked at, logged into
and reflashed. The gate lasts until a reset (the result is then NONE) or a later run that
passes; a reset doesn't repair the cell, so the text says the address, that keys and seals are
off until a reset, and what to do: power-cycle (a cold start, which also clears what the bad
cell may have spoiled), then `health full` again; the log line at err adds what was written and
read, and `if it fails again, the board's RAM is faulty`. `health` and the rescue console stay
reachable: they read results and make nothing. The phase 3 and 4 argument against gates
(refusing blocks the repair) doesn't apply: there is no repair on the board for a bad cell.

**Ctrl-C** frees every block held; a SCRATCH_Y piece is always put back within its step, so it
is never left with a pattern in it.

**Expected on dev-1:** PASS, about 180 KB of heap and SCRATCH_Y, in about 0.1 to 0.2 s.

#### `image`: the running image against its own hash

**Decided (Reviewer, and the user for the build rule, on #262): the expected hash is the image's
HASH_VALUE item, and every build is hashed.** Today `CMakeLists.txt` calls
`pico_hash_binary(sos)` and `pico_sign_binary()` only when `SOS_SIGN_KEY` is set. Phase 5 calls
`pico_hash_binary(sos)` on every build (`picotool seal --hash`; picotool already post-processes
every RP2350 build, so nothing new is needed on the build machine), and signs as before when
there is a key.

**What is in the block** (datasheet §5.1.8, §5.9.2.2-§5.9.2.4, §5.9.3.2), as on dev-1's image
(`picotool info -a build/sos.elf`): the image runs from 0x10000000 to 0x10130e1c, and the last
block of the loop, at 0x10130e1c, is the image definition the boot ROM uses. Its LOAD_MAP has
two entries, `0x10000000-0x1012d6c0` hashed in place (code and read-only data) and
`0x1012d6c0-0x10130e1c`, the initial values of `.data`, copied to 0x20000140. Its HASH_DEF
says SHA-256 over the load map's data followed by the block's first 16 words; HASH_VALUE holds
the result, and SIGNATURE signs it. Recomputed on the host from `build/sos.bin`, the SHA-256 of
the two ranges (read from flash, where they are stored) and the 16 words is
`43fe8126...0924`, picotool's `hash value`: 1 248 860 bytes in all. So the check needs no
signature code (wolfCrypt has no secp256k1 here): the boot ROM verified the signature over that
hash at boot, and the check compares the flash with the hash.

**Why this source.**

- **The boot ROM checks the same hash.** With secure boot it checks the signature over it; on
  any board, a hashed image whose hash doesn't match isn't valid and isn't started (§5.1.8). So
  the check and the boot ROM agree by construction about what "the image" is, and a FAIL also
  means the board won't start this image again after a reset.
- **Nothing new to build.** `bootkey.c` already walks the block loop to find the signature's
  key (for `bootkey`, the `otp` check and the flash guard in `tools/flash.sh`); finding the
  LOAD_MAP, HASH_DEF and HASH_VALUE items is a few more cases in the same walk, host tested the
  same way. picotool writes the hash; nothing in SOS's build computes one.
- **Unsigned builds stop mattering.** SOS does build unsigned images: `SOS_SIGN_KEY` unset (a
  fresh checkout, the host tests' machine) or `-DSOS_SIGN_KEY=none`. With `pico_hash_binary` on
  every build they carry a HASH_VALUE too, so the check works the same on them, and an open
  board gets the boot ROM's hash check it doesn't get today. A signed image is unchanged by
  this (it was already hashed); an unsigned one gains a block of about 50 bytes. The flash
  guard sees a hashed, unsigned image as unsigned, as before (`bootkey_image_pubkey()`).
- **The alternative's cost.** A post-build step that hashes the image and writes the result into
  a known section has to run between linking and picotool's own post-processing (which rewrites
  the ELF and appends the blocks), has to leave its own bytes out of the hash (so what it hashes
  isn't quite the image flashed), and duplicates what HASH_VALUE already holds. And the boot ROM
  would never look at it: a damaged image would still start and run damaged code until someone
  ran `health full`. What it would buy, independence from picotool's block format, SOS already
  gave up in `bootkey.c` and the flash guard.

**What the check finds that the boot ROM doesn't.** The boot ROM hashes the image once, at
reset, and the code then runs from flash through the XIP cache (the load map's first entry is
hashed in place, not copied). So the check covers what happens after boot:

- **Flash disturbed after boot:** a bit that changed in the flash (retention or read disturb,
  over a long uptime), or a write that went astray. A step can't overlap a flash write:
  `flashop_run()` runs from the main loop like the check, with XIP stalled, interrupts off on
  core 0 and core 1 paused, so the flash read between two steps is the flash as it is. SOS's
  only flash writer is `flashop_run()` for the stores at the end of flash; a store bug that
  reached the image would show here.
- **The block itself changed after boot.** The block's HASH_VALUE isn't covered by the hash, so
  flash rewritten with a consistent hash would pass a check that trusted the flash's own copy.
  At boot, `health_boot()` copies the HASH_VALUE (32 bytes, read just after the boot ROM
  verified it) and the block's address into RAM; the check compares the hash it computes with
  that copy, and the flash's HASH_VALUE with it too.
- **Not the XIP cache's contents.** The check reads the flash through the uncached, no-allocate
  alias (`XIP_NOCACHE_NOALLOC_BASE`, 0x14000000), for two reasons: what it hashes is the flash
  itself, not a cached copy, and 1.25 MB streamed through the 16 KB cache would evict the code
  the main loop runs (SSH, the console) and slow it for the whole check. No cache flush is done,
  for the same reason. A cache line that differs from the flash would need a flash write without
  the flush the SDK's `flash_range_erase()` and `flash_range_program()` always do (the boot
  ROM's `flash_flush_cache`), so it isn't checked; a reset would clear it anyway.
- **Not the RAM copies.** Code and data that run from SRAM (the functions crt0 copies with
  `.data`, `__not_in_flash` code, SHA-256's table, `FLASH_QUALIFIER`) are covered in their flash
  form only; a change to the running copy isn't seen. SHA-256's table is also what `crypto`'s
  known answers test at boot.

**The steps.**

1. **The block** (first step): find the last image definition in the loop, as `bootkey.c` does,
   and its LOAD_MAP, HASH_DEF and HASH_VALUE. Each load map entry must be word aligned and its
   storage range inside the image's flash (0x10000000 up to the block), and HASH_DEF's word
   count inside the block; a zero-fill entry (storage address 0) is hashed as its 32-bit size,
   as the datasheet says. Under 0.1 ms.
2. **The flash**, 16 KB a step, through the uncached alias: 77 steps for dev-1's 1 248 796
   bytes of load map data, then the block's 16 words and the comparison in the last. SHA-256 is
   wolfCrypt's (`wc_Sha256Update()`), on core 0 in the main loop, as wolfCrypt must be.
3. **Time:** not measured yet. wolfCrypt's C SHA-256 runs at about 2.5 to 3.5 MB/s on a
   Cortex-M at this clock (its table in SRAM, the debug build at `-Og`), so 16 KB is about 5 to
   6.5 ms; the flash is read faster than that (quad SPI at clk_sys / 2, sequential reads
   chained by the QMI). About 0.4 to 0.5 s in all. If the board measures more than 10 ms a
   step, the step becomes 8 KB. The hardware SHA-256 block (§12.13, about 79 MB/s with DMA)
   would be about ten times faster, but it is an engine SOS doesn't use or test today, and the
   boot ROM uses it (for its random numbers at least); hashing with wolfCrypt, whose SHA-256
   `crypto` tests at every boot, keeps the check independent of what the boot ROM used.

| Status | When | Text (dev-1 or example) |
|---|---|---|
| PASS | the hash matches the HASH_VALUE copied at boot and the one in flash | `signed, sha256 43fe8126fb07af18 as booted, 1.2 MB in 0.45 s` (`hashed` for an unsigned image) |
| FAIL | the computed hash differs from the copy taken at boot: the body changed | `flash image changed: reflash; keys, seals off until reset` |
| FAIL | the flash's HASH_VALUE differs from the copy taken at boot: the block changed (judged first, a different finding from a changed body) | `image hash item changed: reflash; keys, seals off until reset` |
| WARN | the block loop, the load map or HASH_DEF isn't one this code understands (the boot ROM accepted it, so the parser is behind, not the image) | `image's hash block not understood: not checked` |
| SKIP | the image has no HASH_VALUE (built without `pico_hash_binary`: an image from before phase 5, or built another way) | `image has no hash: not checked` (a text that won't age once every build is hashed) |

**What a FAIL does (decided on review): KEYS and CRYPTO, on the two FAILs only.** The WARN and
the SKIP hold no gate. The code in flash isn't what was
flashed (and, on a board with secure boot, signed). The code running is read from that flash,
16 KB of cache at a time, so the board can't trust itself: the code that makes keys and seals
may be what changed. That is the reason `crypto` holds both gates, and it applies here with
more force. Reads stay open, so the person can still log in, read `health`, back up and
reflash; `health` and the rescue console stay reachable. The text says what to do (reflash)
and that keys and seals are off until a reset; the log line at err adds the hashes (both
prefixes) and that the boot ROM won't start this image again after a reset (the board would come
up in the USB boot loader, from which a reflash works).

**Ctrl-C** frees the hash context; nothing else is held.

**Expected on dev-1:** PASS with the prefix picotool printed for the image flashed (for the
build above, `43fe8126fb07af18`), `signed`, in about 0.45 s.

#### `sd-check`: `sd check` as the last step

**Only on a card unmounted by hand** (§2 decision 2; sd-check.md §2.1): it runs when
`sdcard_health_state()` reports `card unmounted by hand` (after `sd unmount`, or after an
earlier `sd check`). It never unmounts, and it doesn't start on a card that is simply not
mounted (no card, or a mount that failed): `sd check`'s end sets `unmounted_by_hand`, which
stops the log writer's quiet remount, and a health check mustn't change that. A card being
formatted or already checked is SKIP too.

**The same job, not a copy.** `sdcard.c`'s check today is tied to the shell: `cmd_check()`
opens the card and allocates the job, `check_step()` prints the report lines and the progress,
and both wait for room in the shell's output. Phase 5 splits it:

- `sdcard_check_begin(for_health, why)`: what `cmd_check()` does after its argument and console
  tests (the `checking` flag, `card_open()`, the memory, `fsck_new()`). `cmd_check()` calls it.
- `sdcard_check_step(cancel, out)`: one `fsck_step()`, with the budget adapted toward a target:
  `CHECK_STEP_US` (20 ms) for `sd check`, **6 ms** for the health check, so that its steps stay
  under 10 ms (the adaptation halves the budget only past 1.5 times the target). Every end goes
  through `check_end()` as now, with no shell: the card is left as `sd unmount` leaves it, its
  key forgotten, and the summary logged (`sd: check: ...`).
- The health check passes no shell: the report lines (one per problem) aren't printed in
  `health full`'s table, only counted, and the header and the summary are logged as for
  `sd check`. The text points at `sd check` for the list.

`fsck.c` checks FAT32 and exFAT (phases 1 and 2 of sd-check.md; repair since #238 and #249), so
dev-1's encrypted exFAT card is checked as `sd check` checks it, through the decrypting layer.

**The first step opens the card** (`card_open()`: `disk_initialize()`, the encrypted card's
header and key), as every mount and `sd check` already do in the main loop: up to hundreds of
milliseconds once, an exception to the 10 ms rule for the same reason as theirs (it is one call
into the driver), accepted on review like `crypto-ssh`'s 60 ms steps.

**Mapping the result.** `fsck_result_t` counts problems, lost clusters, cross-linked clusters
and, on exFAT, clusters reached but free in the allocation bitmap:

| Status | When | Text (dev-1 or example) |
|---|---|---|
| PASS | finished, no problems (notes don't count, as in `sd check`) | `exFAT encrypted 7.5 GB: no problems, 2 passes in 0.5 s` |
| WARN | problems, none of them cross-linked or reached-but-free clusters (lost clusters, sizes, names, a dirty flag): the card is safe to write, space or a file's tail is lost | `3 problems, 512 KB lost: sd check lists, sd check fix repairs` |
| FAIL | cross-linked clusters, or exFAT clusters in use but free in the bitmap: the next write to the card can overwrite a file | `2 problems, 4 cross-linked: sd check fix before writing to it` |
| WARN | the check stopped (a read error, the work bound) after it began | `check stopped in pass 2: read error at sector 81920` |
| SKIP | the card is mounted | `card mounted: sd unmount, then health run sd-check` |
| SKIP | not unmounted by hand: no card, or a mount that failed | `not unmounted by hand: sd unmount, then health run sd-check` |
| SKIP | the card couldn't be opened (locked, refused, no memory), with `sd check`'s reason | `not checked: encrypted with another board's key` |

**WARN or FAIL.** Most problems lose space or data that is already lost, and writing to the
card doesn't make them worse: WARN. Two make the next write destroy data that is still good, a
cluster that two files share, or one that a file uses while the bitmap says it is free (FatFs
would hand it to the next file written): those are FAIL, so the log line is at err and the text
says to repair first. The card is never what the board relies on to run (the configuration, the
keys and the image are in flash), so **no gate**: refusing to make keys or seal wouldn't protect
the card, and the repair (`sd check fix`) is the person's to choose.

**Ctrl-C** ends the check as Ctrl-C ends `sd check`: `check_end()`, the job's memory wiped and
freed, the card left unmounted (as it was), its key forgotten. The check's old result stays.

**Expected on dev-1:** SKIP `card mounted: sd unmount, then health run sd-check` in an ordinary
`health full`; after `sd unmount`, PASS with the same summary `sd check` gives (about 0.5 s on
the nearly empty card measured in sd-check.md §6.2), and the card still unmounted afterwards.

#### `health run` and the rest

- `health run` (no name) runs no full-tier check, as now. `health run ram`, `health run image`
  and `health run sd-check` run one, with the same rules (console only).
- `card` (phase 3) is SKIP `card unmounted by hand` in the same `health full` that runs
  `sd-check`, since it yields to a card unmounted by hand; `sd-check` covers the card then.
- `heap` and `heap-sum` run before `ram` in table order, so neither sees the RAM test's blocks.
- `health state-dump` (test builds) adds: the RAM test's last mismatch in full (address,
  written, read, element, background, a DMA channel writing there), the HASH_VALUE copied at
  boot and the block's address, and the load map's entries.

#### Test commands and tests

**Test commands** (`SOS_TEST_COMMANDS`): `health fault ram once|twice|image` arms a fault for
the next run: `once` flips a bit in the block under test between a write and its read in the
first test only (a WARN), `twice` in both (a FAIL), `image` flips one byte of what the next
`image` run hashes (a FAIL). Nothing is written to flash or to real memory in use; each fault
is used once.

**Host tests:**

- `memtest.c`'s march over a simulated memory (the test builds it with its reads and writes
  redirected), one fault at a time: stuck-at 0 and 1 in each bit position, transition faults,
  inversion and idempotent coupling between two words and between two bits of one word, and an
  address decoder fault that maps two addresses to one cell; each must be found, with the
  address, and a fault-free memory must pass.
- The block parsing in `bootkey.c`: synthetic blocks with each item, relative and absolute load
  maps, a zero-fill entry, entries outside the image, a HASH_DEF count past the block, no
  HASH_VALUE. The hash over a small synthetic image is checked against one computed in the test
  the datasheet's way.
- `fullcheck.c`'s judging: each status path of each check, `fsck_result_t` values for the
  `sd-check` mapping, every text under 63 characters.

**Board run** (dev-1 slot, config untouched, no flashing beyond the slot's image):

1. `picotool info -a build/sos.elf` on the host: note `hash value`. On the console, `mem`,
   then `health full`: `ram` PASS, `image` PASS with that prefix, `sd-check` SKIP (card
   mounted); the time of each and the total. `mem` again (`health` 0 bytes held). Over SSH:
   refused.
2. `sd unmount`, `health full`: `sd-check` PASS, its summary the same as `sd check`'s right
   after; the card still unmounted; `sd mount`, files as before.
3. `health run ram`, `health run image`, `health run sd-check` one at a time.
4. Ctrl-C during `health full` several times, so that each of the three is stopped (the image
   hash, 0.45 s, is the easiest): afterwards `mem` shows nothing held under `health` and the
   card is as it was.
5. `health fault ram once` then `health run ram`: WARN, nothing refused. `health fault ram
   twice`: FAIL, logged at err, and an encrypted `sd format` refused before its question
   (`format refused: health check ram failed`; nothing is written); `health run ram`: PASS,
   the gates open again. `health fault image` then `health run image`: FAIL with the same
   refusal; `health run image`: PASS.
6. `ssh` "longest" before and after a `health full` run while an SSH session is open, to see
   that no step held the loop more than 10 ms longer than usual.

**Cost estimate:** text about +5 KB (the march, the hash steps, the split of `sd check`), bss
under 100 bytes (the boot-time hash copy, the armed test faults). While `health full` runs: the
RAM test's blocks (free heap down to 96 KB, for a fraction of a second), about 200 bytes for
the hash, and `sd check`'s 7 KB plus its 4 to 16 KB window and the card's 7.5 KB layer.

#### Decisions on review

Decided (Reviewer, and the user for the build rule, on #262):

1. **The image hash source** (§11): the HASH_VALUE item. **Every build is hashed** (the user):
   `pico_hash_binary` is unconditional, signed builds keep their signature. An unsigned image
   now carries a hash, and the boot ROM refuses to start one whose flash is corrupted;
   `docs/development.md` says so.
2. **Gates:** KEYS and CRYPTO on the FAIL grades only (a mismatch repeating at the same
   address; a hash mismatch), never on a WARN. The FAIL texts say "until reset" and what to do:
   power-cycle for `ram`, reflash for `image`.
3. **The reserve** stays at 96 KB (`HEALTH_HEAP_CLEAR`).
4. **SCRATCH_X is excluded**, as the linker's reserved core 1 stack, with a build-time
   assertion that the in-place range doesn't reach it.
5. **The 6 ms target** for `sd-check`'s steps and the card's opening are accepted as a
   documented exception, like `crypto-ssh`'s 60 ms steps. The SKIP texts say how to run it.
6. **Registry:** 25 of 32 is fine.

### 9.8 As built (phase 5)

Built as §9.7 decides, with these details settled by the code. The march test is in `memtest.c`
(`tests/test_memtest.c`: a simulated memory, one fault at a time: stuck-at in every bit,
transition faults, inversion and idempotent coupling between two words and between bits of one
word, an address decoder fault), the judging in `fullcheck.c` (`tests/test_fullcheck.c`, 13
tests), the hash items in `bootkey.c` (`bootkey_image_hash()`, three more tests in
`tests/test_bootkey.c`); the work on the board in `fullcheck_board.c`, and `sd check`'s job
split in `sdcard.c` (`check_begin()`, `check_work()`, used by `sd check` and by
`sdcard_check_health_begin()` and `sdcard_check_health_step()`).

**The hash, checked against real images.** A host program built from `bootkey.c` and
`fullcheck.c` (the walk `fullcheck_hash_next()` the board uses, with OpenSSL's SHA-256) found
the items in `build/sos.bin` and computed picotool's `hash value` for it: 1 256 156 bytes hashed
for the phase 5 debug image, signed. An unsigned build (`-DSOS_SIGN_KEY=none`) now carries a
hash too (`picotool info -a`: `hash: verified`, no signature), whose block hashes 11 of its
words, and the same program matched it.

**Texts.** As in §9.7, but where they differ:

| Check | §9.7 | As built |
|---|---|---|
| `ram` | `184 KB of free heap in 0.11 s; SCRATCH_Y in use: not tested` | `184 KB free heap in 0.11 s; SCRATCH_Y in use: not tested` (room for the longer reasons `SCRATCH_Y DMA 15 busy`, `SCRATCH_Y holds the stack`, `SCRATCH_Y no memory`) |
| `ram` | (heap under 96 KB, SCRATCH_Y tested) | `free heap 80 KB, under 96 KB: SCRATCH_Y only, no faults` |
| `ram`, `image`, `sd-check` | (no memory for the check's own state) | SKIP `no memory to run the test`, `no memory to hash the image`, `no memory to check the card` |
| `sd-check` | `3 problems, 512 KB lost: sd check lists, sd check fix repairs` | `3 problems, 512 KB lost: sd check fix repairs them`, and without lost clusters `1 problem: sd check lists it, sd check fix repairs` |
| `sd-check` | `2 problems, 4 cross-linked: sd check fix before writing to it` | `4 cross-linked clusters: sd check fix before writing`, and `5 clusters used but free: sd check fix before writing` (counts of up to ten digits fit) |
| `sd-check` | `check stopped in pass 2: read error at sector 81920` | the same, the reason cut to fit; `not checked: <why>` (WARN) when fsck found no volume to check |

**Also settled:**

- **`ram`'s steps:** the first only plans (the free heap, SCRATCH_Y's state); then one 4 KB
  block a step while `heap_free()` stays above 96 KB plus the block, then the four 1 KB pieces
  of SCRATCH_Y, then the release. The blocks' pointers are kept in an array sized from the free
  heap at the start (about 300 bytes). A piece of SCRATCH_Y is saved into the first heap block
  tested (or a 1 KB buffer when none was), tested and put back with interrupts off on core 0.
  SCRATCH_Y counts as free when `__scratch_y_start__` equals `__scratch_y_end__` and neither the
  core 0 stack (`__StackBottom` to `__StackTop`) nor MSPLIM is inside it.
- **A mismatch** is tested again in the next step, over the same region; the log line (warning,
  or err when it repeated) gives the address, what was written and read, the element, the
  background, a busy DMA channel writing into the region if there was one, and, for a FAIL,
  `keys and seals refused until reset; power-cycle, and if it fails again the board's RAM is
  faulty`. A FAIL ends the test at once (the blocks are freed). `health state-dump` keeps the
  last one.
- **SCRATCH_X:** `fullcheck_board.c` asserts at build time that the in-place range
  (`SRAM_SCRATCH_Y_BASE`, 4 KB) doesn't reach `SRAM_SCRATCH_X_BASE`'s 4 KB.
- **`image`:** `fullcheck_boot()` reads the items through the cached alias in `health_boot()`,
  before the boot tier; the check reads the items again, and the flash, through
  `XIP_NOCACHE_NOALLOC_BASE`. The image is taken, as `bootkey` takes it, from
  `__flash_binary_start` to 4 KB past `__flash_binary_end`, where picotool's block lies. The
  step that hashes the last bytes also finishes (one more step only when they end exactly on a
  16 KB step): 78 steps for the phase 5 debug image's 1 256 156 bytes. A FAIL's log line gives
  the first 8 bytes of the computed hash and the first two words of the one booted, and says the
  boot ROM won't start the image after a reset.
- **`sd-check`:** the job without a shell skips the report lines (fsck still counts them) and
  the progress lines, but logs the header and the summary as `sd check` does. A `config reset`
  during it ends it as `sd check` ends (WARN `check stopped in pass N: the configuration was
  erased ...`). Its start refuses a card that `sd check` or a format owns (SKIP with which).
- **`health fault ram once|twice`, `health fault image`** (test builds, console): the
  disturbance is a flip of bit 10 in the middle word of the region, between the first element's
  writes and the second element's reads (`memtest_march()`'s `disturb` hook); for `image`, byte
  17 of a copy of the first 64 bytes hashed. Each is used by one run.
- **`HEAP_HEALTH`** is the tag `health` in `mem` and `sos.health()`'s heap table.
- **The registry:** 23 checks, 25 with the test-only gate checks, of 32.

**Step times** (estimates; the PASS texts carry the measured totals): `ram` about 2 ms a block
and 0.5 ms a piece; `image` 16 KB of SHA-256 a step, about 5 to 6.5 ms; `sd-check` aimed at 6
ms, its first step the card's opening.

**Cost** (`arm-none-eabi-size -A`, debug build, against main 11112a8): `.text` +5.2 KB,
`.rodata` +1.9 KB, `.bss` +160 bytes (the hash items copied at boot, the last mismatch, the test
faults). An unsigned image gains picotool's hashed block.

**Board run:** as §9.7 lists.

### 9.9 Phase 6 design: the counters

Status: approved by Reviewer (#268) with changes, which this section now reads as: the SD
library's debug lines counted by function name, the driver's warnings logged one line per site
per 10 s, the clock alone no WARN, the figures provisional; the decisions are listed at its end.
Phase 6 adds three background checks that judge counters:
`wifi` (the Wi-Fi chip's driver errors, link drops and firmware version), `sd-bus` (the SD
card's SPI errors and clock) and `net-pools` (lwIP's pool and heap failures). Nothing in the Pico
SDK or the SD card library is edited: the counters are taken through the hooks both libraries
offer for configuration, from SOS's own code and build files. §10 item 6 named the first two;
the third, and what was considered and dropped, are argued below.

**Common rules.**

- **Counters, judged as rates.** Each check reads counters kept in RAM since boot by the code
  that sees the events, and judges how many fell in a window, not the total: a board up for a
  month mustn't WARN for errors spread over it. The windows: the last hour for driver and pool
  errors, the last 24 hours for link drops, and since the card's last initialisation for the
  SD card. Totals since boot stay in `health state-dump` and Lua.
- **Windows measured, not assumed.** A check stores a sample (the uptime and the totals) at a
  run at most every 10 minutes, keeps the last 8, and takes the window from the newest sample at
  least an hour old. With 8 samples at least 10 minutes apart one always is, once the ring is
  full; before that the window starts at boot (all counters are zero then), and the text says
  `in 42 min` while it is shorter than an hour. So an extra `health run` between two periodic
  runs changes nothing. A WARN clears by itself once the window holds no more errors, and the
  change back is logged (§5).
- **Read only, and cheap.** The steps read RAM, and `net-pools` copies a few lwIP counters under
  the stack's lock. No step sends an ioctl to the Wi-Fi chip or touches the card: the one ioctl
  (the firmware version) is sent once, by `wifi_start()` at boot. So each check is one step of
  well under 0.1 ms, and the 10 ms rule (§4) holds without an exception.
- **No gates.** Wi-Fi and the card have nothing to do with making keys or sealing: the keys,
  the configuration and the image are in flash, and `trng` and `crypto` test the means of making
  keys. A failing card or radio shows itself by failing; refusing keys would add nothing.
- **Texts ASCII and under 63 characters** (§9.4). The judging is pure and host tested, in a new
  `src/core/countcheck.c` (`tests/test_countcheck.c`), the reads in `countcheck_board.c`, as
  for phases 3 to 5.

| Check | First run | Period | Steps | Longest step (estimate) |
|---|---|---|---|---|
| `wifi` | 110 s | 10 min | 1 | under 20 us (counters, the state, the drop times) |
| `sd-bus` | 115 s | 10 min | 1 | under 20 us (counters and two SPI registers) |
| `net-pools` | 120 s | 10 min | 1 | under 50 us (three lwIP counters copied under the lock) |

First runs come after phase 4's last (100 s) and after Wi-Fi usually connects. **The registry:**
23 checks today, 26 with phase 6, 28 with the two test-only gate checks, of `HEALTH_MAX` 32.

#### `wifi`: the chip's driver errors, link drops and firmware

**What the driver can say.** The CYW43 driver (Pico SDK 2.3.1, `lib/cyw43-driver`, version
1.1.0) talks to the chip over a gSPI bus that a PIO program drives
(`pico_cyw43_driver/cyw43_bus_pio_spi.c`). It counts nothing by default, and reports trouble in
three ways:

- **Error returns**, each wrapped at its origin in the macro `CYW43_FAIL_FAST_CHECK(res)`
  (`cyw43_config.h` defines it as `(res)` only if the port hasn't): an ioctl's reply not
  arriving within `CYW43_IOCTL_TIMEOUT_US` (1 s, `cyw43_do_ioctl()`), the chip giving no
  transmit credit within 1 s (`STALL ... timeout` in `cyw43_sdpcm_send_common()`), the WLAN
  FIFO not ready after 1000 polls (`F2 not ready`, in the PIO bus code's write), and a few
  argument and firmware-load errors.
- **Warnings**, through `CYW43_WARN()`, likewise the port's to define: a bus overflow or
  underflow flagged in the chip's SPI interrupt register (`Bus error condition detected`), a
  frame whose header fails its check (`hdr mismatch`), a bus that won't wake (`could not bring
  bus up`), and a frame the driver couldn't send (`send_ethernet failed`, in `cyw43_lwip.c`).
  Today these go to `printf`, so they appear raw on the serial console, even while it is locked
  and outside the log.
- **Statistics** under `CYW43_USE_STATS`, off by default. They include a bus error count, but
  `cyw43_ll.c` has `assert(CYW43_STAT_GET(BUS_ERROR) < 100)` beside it: in a build without
  `NDEBUG` (the debug preset) the hundredth bus error would panic and reset the board. **Not
  used**: the warning hook counts the same event without the assert.

An ioctl the chip answers with an error status isn't reported at all
(`sdpcm_process_rx_packet()` has a TODO for it); only failures to get an answer are. The
driver's failures while initialising the chip at boot (`F2 not ready`, `HT not ready`,
backplane timeouts during `cyw43_arch_init()`) can't be judged by a check: if it fails, `main()`
stops there (`cyw43 init failed`).

**Where the counters live, with no driver change.** The driver includes `CYW43_CONFIG_FILE` in
place of the SDK's `cyw43_configport.h` when it is defined. SOS defines it in `CMakeLists.txt`
(`CYW43_CONFIG_FILE="cyw43_sos.h"`, for the `sos` target, which compiles the driver's sources
as the SDK's libraries are interface libraries), and `config/cyw43_sos.h` includes the SDK's
`cyw43_configport.h`, then defines:

- `CYW43_FAIL_FAST_CHECK(res)` as `wifistat_fail((res), __func__)`, which returns `res`
  unchanged and counts it: `timeout` (`-CYW43_ETIMEDOUT`), `io` (`-CYW43_EIO`) or `other`. It
  keeps the last error's code, function and time for `health state-dump`.
- `CYW43_WARN(...)` as `wifistat_warn(__func__, __LINE__, ...)`, which counts by the
  function the warning comes from, never by its wording (decided on review):
  `cyw43_ll_sdpcm_poll_device` (the bus error and the header mismatch), `cyw43_kso_set` and
  `cyw43_ll_bus_sleep_helper` (the bus that won't wake) are `bus`; `cyw43_netif_output` (a
  frame not sent) is `tx`; `cyw43_do_ioctl` and `cyw43_sdpcm_send_common`, whose timeouts are
  already counted by their error returns, count nothing more; any other function is `other`.
  It logs the line at warning (facility daemon, `wifi: driver: ...`) through `log_msg()`, which
  only stores a line logged from interrupt context and prints it from the main loop, at most
  one line per site (function and line) every 10 s; the lines held back are counted all the
  same, and the next line logged from that site says how many there were. Never `printf`.

Both live in a new `src/net/wifistat.c`. They run where the driver runs, in the async context
on core 0 or in a caller holding the driver's lock, so plain 32-bit counters suffice; the log
already takes lines from interrupt context (`log_in_irq`). A build guard keeps the hooks honest:
`wifistat.c` refuses to compile unless `cyw43_sos.h` was the configuration file, and
`CMakeLists.txt` checks at configure time that the driver's sources still define the functions
the classifier names and still leave both macros to the port (`file(READ)`), so a driver update
that renames them fails the build instead of counting them as `other`.

**Link drops** are counted in `wifi.c`, where `W_UP` sees the link go (`wifi: link lost (status
%d), reconnecting`). A disconnect that was asked for doesn't pass there: `wifi connect`, `wifi
set` and `wifi remove` go through `wifi_reconnect()`, and the move to a stronger access point
happens before `W_UP`. `wifi.c` keeps the uptimes of the last 8 drops, and since when the link
has been down (from `wifi_start()`, or from the last time it left `W_UP`).

**The firmware version** is read once by `wifi_start()`: the `ver` iovar (one
`CYW43_IOCTL_GET_VAR`, as `wifi stats chip` reads `counters`), kept as a string in `wifi.c`.
The chip answers with its build line, for the blob linked today
`wl0: <date> version 7.95.49 (2271bb6 CY) FWID 01-...` (the blob's own trailer says
`Version: 7.95.49 (2271bb6 CY) ... Date: Mon 2021-11-29`). The check shows the version number
and the driver's (`CYW43_VERSION`). A version that can't be read is a WARN: the chip answered
nothing at the one moment asked, which is a bus finding of its own. The answer isn't compared
with the linked blob: the chip runs what the driver loaded at this boot, so the two can't
differ.

**Rules.** The judged errors are `timeout + io + bus`: each one is a reply lost, a frame lost or
corrupt on the gSPI bus, or the chip not answering. `tx` follows from them and `other` is
programming errors (an argument refused), so both are shown, not judged.

| Status | When | Text (dev-1 or example) |
|---|---|---|
| PASS | fewer than 3 judged errors in the hour, fewer than 4 drops in 24 h, connected | `fw 7.95.49, driver 1.1.0; no errors in 1 h; no drops in 24 h` |
| PASS | the same, with some | `fw 7.95.49; 2 errors in 1 h; 1 drop in 24 h` |
| WARN | 3 or more judged errors in the hour | `7 driver errors in 1 h (5 timeouts, 2 bus); fw 7.95.49` |
| FAIL | 30 or more judged errors in the hour | `41 driver errors in 1 h: the chip isn't answering` |
| WARN | 4 or more link drops in 24 h | `4 link drops in 24 h, the last 2 h ago; fw 7.95.49` |
| WARN | set up, not connected for 30 min or more | `no connection for 42 min: check the network and 'wifi'` |
| WARN | the firmware version wasn't read at boot | `the chip didn't give its firmware version` |
| SKIP | no network saved, or Wi-Fi stopped | `Wi-Fi not set up`, `Wi-Fi stopped` |

The figures, and why:

- **3 errors an hour, WARN.** A sound board's bus should have none; a single timeout can follow
  a burst of traffic held up by the chip's flow control, or a scan, so one is not a finding.
  Three in an hour is a pattern. Provisional, like every figure in this section: built as
  given, and fixed later from a 24-hour baseline on dev-1 (a follow-up, below).
- **30 an hour, FAIL.** A chip that stops answering times out at every frame it is given: the
  keepalive alone sends one a minute (`KEEPALIVE_MS`), and each send waits 1 s for credit, so a
  hung chip passes 60 an hour while holding the main loop a second each time. FAIL logs at err;
  it refuses nothing (no gate).
- **4 drops in 24 hours, WARN; never FAIL.** A router restart or an access point's firmware
  update costs one or two; a nightly channel change, one a day. Four in a day is more than
  maintenance. A drop can be the network's doing, not the board's, so it doesn't FAIL.
- **30 minutes not connected, WARN.** A router restart takes one to three minutes, and the
  retry delay is 15 s (`RETRY_DELAY_MS`). Half an hour without a link is not a blip, and without
  this rule a board stuck "connecting" for days would read PASS. The text says where to look:
  the network itself, and `wifi` for the saved networks and the state.

**RSSI** isn't judged. Reading it is an ioctl (an answer within 1 s, or a timeout counted
above), it depends on where the board stands, and nothing in the board fixes it; `wifi stats`
and `net` show it live, and `connected()` already logs it at each join. The join's RSSI goes to
`health state-dump`.

**Expected on dev-1:** PASS `fw 7.95.49, driver 1.1.0; no errors in 1 h; no drops in 24 h`.

#### `sd-bus`: the card's SPI errors and clock

**What the driver does with errors.** The SD card library (no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
3.6.2, the submodule `lib/sdcard`) checks CRCs both ways (`SD_CRC_ENABLED`, on by default):
- **Reads:** a data block's CRC16 is checked (`chk_crc16()`), and the R1 response's CRC error
  bit (`sd_cmd()`, `CRC error CMD:%d`). `sd_read_blocks()` retries a failed read up to
  `sd_timeouts.sd_command_retries` times (3), and only the last failure is returned.
- **Writes:** the card answers each data block with a token; a CRC or write error is reported
  through `EMSG_PRINTF` (`Block Write not accepted`), which SOS already logs
  (`put_out_error_message()`), and a multi-block write is retried (`Retrying`).
- **Timeouts:** waiting for a data token or for the card to be ready, each up to
  `sd_timeouts.sd_command` (2 s), and a command without a reply, retried (`No response CMD:%d`).

The library counts none of these. Everything retried is visible only as `DBG_PRINTF` lines,
compiled to nothing unless `USE_DBG_PRINTF` is set. What reaches SOS is the final result of a
read or write.

**Two levels.** SOS doesn't patch its submodules: wolfSSH's bugs, for one, are worked around
in SOS's own files (THIRD_PARTY.md), and the submodules are checked out unchanged. Both levels
keep to that.

- **Level A, no change below SOS.** SOS already wraps the driver's calls: reads in
  `read_blocks_logged()`, writes in `write_blocks_fed()` and initialisations in `init_layer()`
  (`sdcard.c`). New counters there: sectors read and written; reads and writes that failed
  after the driver's retries, by error (`CRC`, `NO_RESPONSE`, `WRITE`, other); initialisations
  attempted and failed. Write rejections are counted where SOS already receives them: SOS
  defines the library's weak `error_message_printf()` (it gets the function's name), counts the
  ones from `send_block()`, and logs the line as `put_out_error_message()` does today.
- **Level B, the retried errors, by a build flag** (approved). `CMakeLists.txt` compiles one
  library source, `sd_driver/SPI/sd_card_spi.c`, with `USE_DBG_PRINTF=1`, through
  `set_source_files_properties()` on that file alone, so no other library file starts
  printing; SOS defines the weak `debug_message_printf(func, line, fmt, ...)`. It formats
  nothing and prints nothing: it counts by `func`, the name of the function the line comes
  from, never by the line's text (decided on review):

  | Function | Counted as | What its lines say |
  |---|---|---|
  | `chk_crc16` | `crc` | a data block's CRC16 doesn't match (a read, or a register read at initialisation) |
  | `sd_wait_token` | `timeout` | no data token within `sd_command` (2 s) |
  | `sd_wait_ready` | `timeout` | the card still busy after `sd_command` (2 s) |
  | `in_sd_spi_sectors` | `cmd` | the card's CSD register unreadable |
  | `sd_cmd`, outside an initialisation | `cmd` | a command unanswered (retried), refused for its CRC, or illegal |
  | `send_block`, through `error_message_printf()` | `write` | a data block refused by the card's token (CRC or write error) |

  `sd_cmd`'s lines during an initialisation are mostly the card's type (`V2-Version Card`,
  `R3/R7`), so they count only outside one: `sdcard.c` knows, since `init_layer()` wraps the
  driver's `init`. A failed initialisation is counted by level A. The callers' repeats of the
  same event (`read_bytes()` and `in_sd_read_blocks()` after `chk_crc16`, `send_block()` and
  `stop_wr_tran()` after `sd_wait_ready`, `sd_write_blocks()`'s retry lines after
  `send_block`) count nothing, so each event counts once. Measured on that file alone (debug
  build, `arm-none-eabi-size`): text and rodata 4.9 KB without, 7.3 KB with the flag, so about
  2.4 KB of format strings and calls. A configure-time check fails the build if the counted
  functions are gone from `sd_card_spi.c` or its count of `DBG_PRINTF(` calls changes (51 in
  3.6.2), so an update of the library is looked at before it is counted. The classifier is pure
  and host tested.
- **Not proposed:** a copy of `sd_card_spi.c` in SOS's tree with counters added (1731 lines to
  keep in step by hand), or a patch to the submodule. Either is the user's decision.

Without level B, the rate and timeout rules below would go, and `sd-bus` would judge failed
reads and writes and the initialisation.

**Where the counters live:** `sdcard.c`, beside `io_stats` (which a format resets, so it isn't
reused), read with a new `sdcard_counters()` that copies them and reads the SPI clock
(`spi.baud_rate` as set, `spi_get_baudrate()` from the SPI block's two divider registers). It
never calls the driver, mounts nothing and doesn't wait: so it is safe while `sd check` or a
format runs, and both count like any other traffic.

**The window is since the card's last initialisation** (each real one, in `init_layer()`: a
mount after `sd unmount`, `sd speed`, a format, `sd check`'s opening, a quiet remount). A new
card, or the same card at a new clock, is a new situation; errors from before would only blur
it. Totals since boot go to `health state-dump`.

| Status | When | Text (dev-1 or example) |
|---|---|---|
| PASS | no failed read or write, rates under the line | `25.0 MHz; 37 MB since the card's init, no errors` |
| PASS | the same above 25 MHz | `at 37.5 MHz; 3 MB since the card's init, no errors` |
| PASS | retried errors under the line | `25.0 MHz; 2 retried in 412 MB (under 1 per 10 MB)` |
| FAIL | 10 or more failed reads and writes since the init | `12 failed reads/writes since init: card or bus failing` |
| WARN | a failed read or write since the init | `2 failed writes, 1 failed read since init: sd check` |
| WARN | the last initialisation failed, after one had worked since boot | `card no longer initialises at 75.0 MHz (removed?)` |
| WARN | a timeout since the init (level B) | `card timed out 2x since init (2 s each); 25.0 MHz` |
| WARN | above 25 MHz, any retried error (level B) | `2 retried at 37.5 MHz, over SD's 25 MHz: sd speed 25` |
| WARN | 3 or more retried errors, and 1 or more per 10 MB moved (level B) | `25.0 MHz; 14 retried in 31 MB: over 1 per 10 MB` |
| SKIP | no initialisation has worked since boot | `no card initialised since boot` |

The first matching row wins, in this order (FAIL, then the WARNs as listed). The figures:

- **A failed read or write is counted, not rated.** Each one already got past three tries and
  reached FatFs as a failed file operation, so one is worth knowing, and a failed write can
  leave the file system half updated: the text points at `sd check`. Ten mean the card or the
  bus is failing (or the card was pulled without `sd unmount`): FAIL, at err. It clears at the
  next initialisation (a remount, a reboot).
- **1 retried error per 10 MB, with at least 3.** At 25 MHz a clean short bus with a bit error
  rate of 1e-9 gives one CRC error per 119 MB (1e9 bits). One per 10 MB is ten times worse: the
  margin is gone, though the retries still hide it. The floor of three keeps one error after the
  first megabyte from reading as a rate.
- **Any timeout.** Each one holds the main loop for up to 2 s (`sd_timeouts.sd_command`), and
  three in a row (one read's retries) come near the 8 s watchdog. Rare on a working card.
- **Above 25 MHz, no rate.** `sdcard.c` notes 25 MHz as the SD spec's maximum in SPI mode; the
  SPI block divides 150 MHz by even numbers, so `sd speed` can give 75, 37.5 or 25 MHz and less.
  37.5 MHz worked on one card. Running there is the person's choice and not a finding by itself
  (decided on review): the PASS text names the clock (`at 37.5 MHz`). But an error there is put
  down to the clock at once, without waiting for a rate, and the text says how to go back.
  `sd speed` isn't saved, so a reboot goes back to 25 MHz.
- **No card is SKIP**, not a finding: there is no card-detect pin, so a missing card and a dead
  one look the same until one has worked. The WARN for a card that stopped initialising covers
  the second.

**Expected on dev-1:** PASS `25.0 MHz; N MB since the card's init, no errors` (the card is
mounted at boot for the log files).

#### `net-pools`: lwIP's pools and heap

**Reads,** under `cyw43_arch_lwip_begin()`, as `wifi stats` does:
`lwip_stats.memp[MEMP_PBUF_POOL]->err`, `lwip_stats.memp[MEMP_TCP_SEG]->err` and
`lwip_stats.mem.err` (`MEMP_STATS` and `MEM_STATS` are on for `wifi stats`), with each one's
`max` and `avail`.

**Why it is a finding.** A received frame that finds the packet pool empty
(`PBUF_POOL_SIZE` 12) is dropped by the driver without a word (`src/net/README.md`, "Packet loss
and `wifi stats`"); a full segment pool or lwIP heap (`MEM_SIZE` 32000) refuses a send, which
TCP retries a second later (`LWIP_TCP_RTO_TIME`). In November 2026's measurements none of the
three failed, SFTP included, so a failure means the load outgrew SOS's sizing: something to
change in `lwipopts.h`, which no other check sees (`heap` and `heap-sum` judge newlib's heap, not
lwIP's). The connection pool (`MEMP_TCP_PCB`) is left out: a port scan can empty it, which says
nothing about the board; it goes to `health state-dump`.

| Status | When | Text (example) |
|---|---|---|
| PASS | no failure in the hour | `no failures in 1 h; packets most 5 of 12` |
| WARN | the packet pool failed in the hour | `packet pool empty 3x in 1 h: received frames dropped` |
| WARN | the segment pool or the lwIP heap failed in the hour | `lwIP heap full 2x in 1 h: sends refused` |

**One failure an hour, WARN; never FAIL.** Each failure costs a frame or a second, recovered by
TCP; nothing breaks for good. But since none happen in measured use, one is the sign. Whether
the segment pool and the heap stay at zero during heavy SSH output (SOS checks `tcp_sndbuf()`
before writing, so lwIP's own refusals shouldn't be its flow control) is the baseline's
question; if they don't, only the packet pool is judged.

**Expected on dev-1:** PASS.

#### Where the counters show

- **`health state-dump`** (test builds): one line per check with every counter since boot and
  in its window: Wi-Fi's per-kind counts, the last error's code, line and age, the full `ver`
  line, the drop times, the join's RSSI and the time down; the card's sectors, failures by
  error, level B's counts, initialisations attempted and failed, the clock set and actual; the
  pools' failures, `max` and `avail`, the connection pool among them.
- **Lua:** `sos.wifi.status()` gains `firmware` (the version), `errors = {timeout=, io=, bus=,
  tx=, other=}` and `drops` (since boot); `sos.sd.info()` gains `errors` with the same counts
  as the dump, read from RAM (`sd_table()` already mounts for its other fields, as `sd` does;
  the counters add no access). The pools stay in `sos.mem()`'s `lwip` table, which gains
  `failures`. `sos.health()` keeps one row per check, as for every phase.
- **Commands:** `wifi stats` (the packet-loss diagnostic, any shell) gains one line, `driver:
  N timeouts, N bus, N io, N tx, N other; N link drops; firmware 7.95.49` since boot; its pools
  line already shows the failures `net-pools` judges. `sd` (card information) gains one line of
  errors since the init and since boot. `mem` and `net` don't change: `health` gives the
  judgement, and the two diagnostics that already show the neighbouring numbers give the
  counts.

#### Considered and dropped

- **`CYW43_USE_STATS`:** the assert that resets a debug build at the hundredth bus error;
  the warning hook counts the same event.
- **RSSI and its trend:** an ioctl per reading, noisy, decided by where the board stands; shown
  by `wifi stats` and `net`, logged at each join.
- **The chip's own counters** (`wifi stats chip`): their layout isn't published for this
  firmware (`shell.c`'s comment), and reading them is a 1 KB ioctl.
- **Comparing the chip's version with the linked blob:** the chip runs what was loaded at this
  boot, so they can't differ.
- **TLS handshake failures, SSH protocol errors:** set by the other end (scanners, wrong keys,
  a server's new certificate), already logged (facility auth); counting them measures the
  peers, not the board.
- **lwIP's receive checksum errors** (`chkerr` per protocol): a TCP or IP checksum failing after
  the chip checked the frame's FCS could mean corruption on the gSPI bus, but senders on the
  internet produce them too and their base rate isn't known. Shown in `health state-dump`, not
  judged; a later phase can judge them once a baseline exists.
- **USB CDC:** TinyUSB keeps no error counters; output dropped because the host isn't reading
  isn't a fault.
- **SD re-initialisations as a count:** without card detect, the board initialises the card
  only when SOS asks (a mount, `sd speed`, a format, `sd check`, the quiet remount), so the count
  measures commands. Only the failed one after a working one is judged.

#### Test commands and tests

**Test commands** (`SOS_TEST_COMMANDS`, console):

- **`wifi fault drop`:** sends the chip `CYW43_IOCTL_SET_DISASSOC` directly, as
  `cyw43_wifi_leave()` does, without telling `wifi.c`, and only while connected. The chip
  answers with its `DISASSOC` event, on which the driver (`cyw43_ctrl.c`) sets the link down and
  clears its join state, so `cyw43_tcpip_link_status()` stops reading `CYW43_LINK_UP`.
  `wifi_poll()` then takes its ordinary `W_UP` path: it logs `link lost`, counts a drop, and
  starts a new attempt (`begin_attempt()`), as after a real drop. Nothing else is touched.
- **`health fault wifi timeout|bus <n>`:** calls `wifistat_fail()` or `wifistat_warn()` n times,
  as the driver would, to take the rules through WARN and FAIL. It tests counting and judging on
  the board; the driver's own calls are checked at build time (the guard above) and by the
  symbols (board run, step 1).
- **`sd fault read <n>`** (an addition to `sd fault`, `files/sdfault.h`): the nth read request
  from now returns `SD_BLOCK_DEVICE_ERROR_CRC` without reaching the card. A read changes
  nothing, so this moves the failed-read counter with nothing to repair afterwards.

**Host tests:** `countcheck.c`'s rules at and around each figure; the windows (samples at
irregular times, an extra run, uptime under an hour, the oldest sample dropping out, counters
that only grow); the drop times over 24 h; the rule order for `sd-bus`; `sdstat_classify()` and
`wifistat`'s classifier over the libraries' own texts; every text under 63 characters.

**Board run** (dev-1 slot, config untouched, debug preset with test commands):

1. On the host: `arm-none-eabi-nm` on the driver objects (`cyw43_ll.c.obj`,
   `cyw43_bus_pio_spi.c.obj`, `sd_card_spi.c.obj`) shows them calling `wifistat_fail`,
   `wifistat_warn` and `debug_message_printf`, and `nm build/sos.elf` shows SOS's strong
   `debug_message_printf` and `error_message_printf`.
2. Two minutes after boot, `health`: the three rows PASS with the texts above; `health
   state-dump`, `wifi stats` and `sd` show the counters; `ssh` "longest" as before.
3. `wifi connect`: the link goes and comes back, and the drop count doesn't move (asked for).
   `wifi fault drop` four times, a minute apart: four reconnects; `health run wifi` WARN
   `4 link drops in 24 h`. (They age out after 24 h; the host tests cover that.)
4. `health fault wifi timeout 3`, `health run wifi`: WARN; `health fault wifi bus 30`: FAIL,
   logged at err; the warning lines appear in `dmesg`, at most one per site every 10 s with
   the count of those held back, and nothing raw on the console. The reboot at the slot's end
   clears them.
5. `sd speed 75` (the card fails to initialise at 75 MHz, as `sdcard.c` notes, so nothing is
   mounted or written), `health run sd-bus`: WARN `card no longer initialises at 75.0 MHz`, and
   level B's counts in the dump; `sd speed 25`: mounted again; `health run sd-bus`: PASS, the
   window started again. `sd speed 37.5`, `cat` a large file, `health run sd-bus`: PASS
   `at 37.5 MHz`, or WARN if a retried error came; `sd speed 25`.
6. `sd fault read 1`, `cat` a file: the read error; `health run sd-bus`: WARN `1 failed read
   since init`; `sd unmount`, `sd mount`, `health run sd-bus`: PASS.
7. `net-pools`: from the host, a flood ping (`ping -f -s 1400`) for 10 s may empty the packet
   pool (frames are dropped, nothing else); if it doesn't, the host tests cover the rule and the
   step shows the pool's `max`. `wifi stats` pools line before and after.
8. **The baseline, a later follow-up** (decided on review, not part of this phase's run): the
   board left up for 24 hours in its ordinary use (SSH, the log files, NTS syncs), with
   `health state-dump` read at the start and the end, and once during an SFTP upload: real
   driver errors, drops and pool failures per day. The provisional figures (3 and 30 errors an
   hour, 4 drops a day, the pools) are fixed from it.

**Cost estimate:** text about +3 KB for the checks, the hooks and the classifiers, +2.4 KB for
level B's debug lines; bss about 200 bytes (the counters, the drop times, the samples, the
version string). No flash sector, nothing on the card.

#### Decisions on review

Decided (Reviewer, on #268):

1. **Level B** is built, counting by function name (`__func__`), not by the lines' text; the
   configure-time guard checks the function names and the count of `DBG_PRINTF(` calls in
   `sd_card_spi.c`; `USE_DBG_PRINTF=1` is set on that file alone.
2. **`CYW43_WARN`** goes to the log at warning, one line per site every 10 s, the rest counted;
   never `printf`; from interrupt context through `log_msg()`'s stored path.
3. **The figures** are built as given, provisional; the 24-hour baseline is a later follow-up.
4. **`net-pools`** stays a check of its own.
5. **The test commands** `wifi fault drop`, `health fault wifi` and `sd fault read` are in test
   builds; `wifi fault drop` goes through the ordinary `W_UP` link loss (above).
6. **The clock alone is no WARN** (`sd-bus`): the PASS text names a clock above 25 MHz, and
   only an error there WARNs.
7. **The 30-minute text** says what to check: the network, and `wifi`.

### 9.10 As built (phase 6)

Built as §9.9 decides, with these details settled by the code. The judging and the classifiers
are in `countcheck.c` (`tests/test_countcheck.c`, 21 tests: the windows, each rule at and around
its figure, the classifiers), the counted function names in `countcheck_names.h`, the reads in
`countcheck_board.c`; the Wi-Fi driver's hooks in `net/wifistat.c` (named by
`config/cyw43_sos.h`), the drops, time down and `ver` line in `net/wifi.c` (`wifi_counts()`),
the card's counters in `files/sdcard.c` (`sdcard_counters()`), the read fault in `sdfault.c`
(`tests/test_sdfault.c`).

**Texts.** As in §9.9, but where they differ or §9.9 gave none:

| Check | §9.9 | As built |
|---|---|---|
| `wifi` | (connected, set up, no rule met) | while connecting, under 30 minutes: `no connection for 5 min; fw 7.95.49; no errors in 1 h` |
| `wifi` | `7 driver errors in 1 h (5 timeouts, 2 bus); fw 7.95.49` | the same; the `; fw ...` tail is left out when the whole wouldn't fit (`29 driver errors in 1 h (11 timeouts, 12 I/O, 6 bus)`), and is `fw ?` when the version wasn't read |
| `wifi` | PASS with `, driver 1.1.0` | the driver's version dropped if the line wouldn't fit |
| `sd-bus` | (no figure for a small amount) | under 1 MB in KB, from 10 GB in GB: `25.0 MHz; 512 KB since the card's init, no errors` |
| `net-pools` | `lwIP heap full 2x in 1 h: sends refused` | the same, and `segment pool full 4x in 1 h: sends refused`; the packet pool named first, then the segments, then the heap |

**Also settled:**

- **The SD library's error lines** are counted by function too: the table in
  `countcheck_names.h` has a fourth column saying whether a function's error lines
  (`EMSG_PRINTF`) or its debug lines count, so `send_block()`'s refused block counts once (its
  error line) and its debug lines (`Card not ready yet`, after `sd_wait_ready`'s) count nothing.
  `error_message_printf()` then formats and hands the line to `put_out_error_message()`, as the
  library's weak version does, so the log reads as before.
- **`sd_cmd`'s lines during an initialisation** count nothing: `init_layer()` sets a flag around
  the driver's `init`. A real initialisation (the card not up before) zeroes the window first,
  so its own errors are in the new window; `init_fails` and `last_init_failed` say how it ended.
- **The clock** in the texts is the SPI block's (`spi_get_baudrate()`), read only once the
  driver has set it up and the last initialisation worked; otherwise the one set by `sd speed`
  (after `sd speed 50`, the block runs at 37.5 MHz, which the text names).
- **An injected write** (`sd fault fail`) counts as a failed write, and an injected read
  (`sd fault read`) as a failed read: what FatFs saw.
- **`wifi fault drop`** calls `cyw43_wifi_leave()` (the disassociation the design names) while
  `W_UP`, and changes nothing in `wifi.c`; `wifi_reconnect()` from `W_UP` resets the time down
  without counting a drop. Shown as `wifi fault drop` in test builds (hidden from Tab's list).
- **`health fault wifi timeout|bus <n>`** (n up to 1000) calls `wifistat_fail()` or
  `wifistat_warn()` with the driver's function names; its lines are a rate-limit site of their
  own, apart from the driver's.
- **The rate limit** keeps 8 sites (function and line); a ninth replaces the one logged longest
  ago. Lines held back are counted (`held_back`, in `health state-dump`) besides their kinds.
- **lwIP's failure counters** are 16 bits (`STAT_COUNTER`): `net-pools` carries them into
  32-bit totals at each run, so a window across a wrap counts right.
- **`CYW43_PRINTF`** is left to the SDK: `F2 not ready` (counted by its error return) and the
  initialisation's debug lines (debug builds) are still printed, as §9.9 says.
- **Where the counts show:** `wifi stats` adds `driver: N timeouts, N bus, N io, N tx, N other
  since boot; N link drops; firmware 7.95.49`; `sd` adds `errors: since init N failed, N
  retried, N timeouts; since boot ...`; `sos.wifi.status()` adds `firmware`, `drops` and
  `errors = {timeout, io, bus, tx, other}`; `sos.sd.info()` adds, once a card has started,
  `errors = {failed_reads, failed_writes, crc, cmd, write, timeouts}` and `spi_khz`;
  `sos.mem().lwip` adds `failures = {packets, segments, heap}`. `health state-dump` adds eight
  lines: three for `wifi` (the counts since boot and the last error, the window, the drops and
  the join's RSSI, the `ver` line), three for `sd-bus` (the initialisations and the clock, the
  counts since the init and since boot) and two for `net-pools` (the failures since boot and in
  the window; each pool's most and size, the connection pool's among them, and lwIP's receive
  checksum errors).
- **The guard** (`CMakeLists.txt`) checks each name in `countcheck_names.h` for a definition
  in the library's source (the name, its parameters, then `{`), `sd_card_spi.c`'s 51
  `DBG_PRINTF(` calls, and `#ifndef CYW43_FAIL_FAST_CHECK` and `#ifndef CYW43_WARN` in
  `cyw43_config.h`; a renamed function, a changed count or a hook no longer left to the port
  stops the configuration with the file and what to review. Each was tried by hand.
- **The registry:** 26 checks, 28 with the test-only gate checks, of 32.

**Step times** (estimates): each a copy of counters, under 50 us; `net-pools` takes lwIP's lock
for the copy.

**Cost** (`arm-none-eabi-size`, against main 76c8003): debug text +9.7 KB (1267024 to 1276752),
bss +1.1 KB; release text +9.8 KB (1523176 to 1532992), bss +1.1 KB; the test build text
+12.5 KB with the dump and the fault commands. About 2.4 KB of it is the SD library's debug
lines (§9.9); the rest the judging, the hooks at the driver's error sites, the rate limit, the
new lines in `wifi stats`, `sd` and Lua. bss: the two windows, the rate limit's sites, the
`ver` line, the counters and the gathered reads. No flash sector, nothing on the card.

**Builds:** debug, the test build and release build with no warning in the files this phase
touched; release keeps 8 warnings in `chat.c` (`-Wformat-truncation`), which it had before.

**Expected on dev-1:**

| Check | Text |
|---|---|
| `wifi` | `fw 7.95.49, driver 1.1.0; no errors in 1 h; no drops in 24 h` (`in 2 min` and the like until an hour up) |
| `sd-bus` | `25.0 MHz; N MB since the card's init, no errors` |
| `net-pools` | `no failures in 1 h; packets most N of 12` |

**Board run:** as §9.9 lists (steps 1 to 7; the 24-hour baseline is the later follow-up).

#### The driver's own paths: count and return (a reset found on dev-1)

**What happened.** On the first phase 6 test image, `sd speed 75` printed `mount failed: The
physical drive cannot work (3) after 4005 ms` and the board reset on the watchdog (8 s) a few
seconds later; main (f62c556) printed the same and carried on.

**What reading the code shows.** A failed mount at 75 MHz costs about 4 s: `mount()` initialises
the card twice (`card_open()`'s `disk_initialize()`, then `f_mount()`'s own), and each init dies in
one of the driver's time-based waits (`sd_wait_ready()` or `sd_wait_token()`, `sd_command` 2000 ms)
after `sd_spi_go_high_frequency()`. The command runs in `shell_poll()`, and in the same main-loop
pass `logfile_poll()` (`main.c`) sees no card mounted and, a minute or more after its last
retry, calls `retry_mount()` (`logfile.c`, the retry in `logfile_poll()`): `sdcard_mount_retry()`
runs the same two inits again, about 4 s more, before the loop comes back to `wdog_feed()`.
4 + 4 s is the watchdog's 8 s. That path is the same on main, so main sits on the same edge, and
whether a given run crosses it depends on how quickly the card answers in each wait; the code
doesn't say why the main run stayed under (if its retry wasn't due, e.g. within a minute of
boot, it would). What phase 6 adds inside the init is the driver's debug lines (`USE_DBG_PRINTF`
on `sd_card_spi.c`): about a dozen calls in a failed init, each a few string compares and a
counter, microseconds against the 2 s waits. None of the hooks formats, logs or calls back into
`sdcard.c`, FatFs or the log files: `debug_message_printf()` only counts, and
`error_message_printf()` hands its line to `put_out_error_message()` as the library's own weak
version does on main, which only stores it (`log_msg()` is deferred from boot on; only
`logfile_poll()`, from the main loop, writes the card). So there is no recursion into the driver
from its error path.

**The fix: never two blocking card inits in one pass.** The log writer's quiet retry now waits
`LOGFILE_RETRY_MS` (a minute) after any mount attempt, not only its own: `sdcard.c` records when
each attempt ended (`sdcard_last_mount_us()`), and `logfile.c` takes the later of that and its own
last retry (`logfile_hooks_t.last_mount_us`). After `sd speed 75` the pass ends after the one
failed mount (about 4 s, as on main), and the retry comes a minute later, alone. Host test:
`no_retry_right_after_another_mount` in `tests/test_logfile.c`.

**The rule, written down** (`files/sdstat.h`, `net/wifistat.c`): what a driver calls from inside
its own paths (the SD library's debug and error lines, the CYW43 driver's error returns and
warnings) counts and returns. It never formats a debug line, never logs to the card, never calls
into `sdcard.c`, FatFs, the log files or the driver, never allocates and never waits; the finding
is logged by the check's step, from the main loop. Two exceptions, argued:

- `error_message_printf()` keeps logging the library's error lines through
  `put_out_error_message()`, as on main: that only stores the line in the RAM history.
- `wifistat_warn()` keeps storing its rate-limited line with `log_msg()`: from interrupt context
  that only stores, and from thread context too once the log is deferred (before that, at
  `cyw43_arch_init()`, it prints to the console). Moving the line to the `wifi` check's step
  would lose the warning's own text (the step runs every 10 minutes and keeps counts only), and
  the store-only path can't reach the card.

To keep the SD rule checked, the counting moved from `sdcard.c` to a file of its own,
`files/sdstat.c` (the counters, `debug_message_printf()`, `sdstat_error_line()`), and the host
tests build it as an object and run `nm -u` on it (`sdstat-leaf`, `tests/leaf_check.cmake`): it may
reference `countcheck_sd_kind()` and nothing else (the sanitizers' symbols aside). Adding a
`log_msg()` call to it fails that test; tried by hand. `tests/test_countcheck.c` checks what the
hooks count for a failed init's lines.

**The board run confirms it:** `sd speed 75`: `mount failed ... after about 4000 ms`, then no
reset (`uptime` keeps counting; `uptime history` has no new watchdog boot); a minute later the
log writer's quiet retry runs alone (another few seconds, no reset). `sd speed 25` mounts again.
Then the rest of the plan: `sd fault read 1` with a `cat` and a remount, the flood ping.

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
   runs, and reset history. Design: §9.5 (heap totals in the background and the walk by hand,
   the detector needs no turning on, and the reset history is judged by `resets`; §9.5 lists
   the changes); as built: §9.6.
5. **Full tier:** a RAM march test over free heap and SCRATCH_Y in steps, the running-image
   hash, and `sd check` as a check. Design: §9.7 (the hash is the image's own HASH_VALUE, with
   every build hashed; `ram` and `image` hold KEYS and CRYPTO on their FAILs; `sd check` runs
   only on a card unmounted by hand); as built: §9.8.
6. **Counters:** CYW43 firmware version and bus errors, SD SPI CRC errors and retries. Design:
   §9.9 (`wifi`, `sd-bus` and `net-pools`, judged as rates; the counters taken through the
   libraries' own configuration hooks, with no patch to the Pico SDK or the SD library); as
   built: §9.10.

## 11. Left open for later phases

- **Image hash (phase 5):** either a post-build step that hashes the image and writes the hash
  into a known section, or the signature block of a signed build (only signed builds would
  have one). *Decided (Reviewer and the user, on #262; §9.7):* the HASH_VALUE item of the image's own block, the
  hash the boot ROM checks at every reset, with `pico_hash_binary` called on every build so
  that unsigned builds carry one too. The check recomputes it over the load map's ranges read
  through the uncached flash alias, and compares it with a copy taken at boot. A post-build
  section would duplicate it, would have to fit between linking and picotool's own
  post-processing, and would never be checked by the boot ROM.
- **Reset history (phase 4):** either a flash sector written like the failcount sector (bits
  cleared, rarely erased), or a POWMAN scratch register for the count plus the `--- boot`
  lines already in `/var/log/kern.log`. *Decided (the user, on #255; §9.5):* the flash
  sector, the sixth from the end of flash, below the failcount sector, because a brownout, a
  power cycle and the reset pin clear the POWMAN scratch registers, and the log lines need a
  mounted card. It also keeps the RTC drift check's baseline across restarts.
