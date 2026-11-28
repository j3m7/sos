# Timekeeping

The Adalogger's PCF8523 RTC (I2C0: SDA GP4, SCL GP5), kept running by its CR1220 coin cell,
is the source of truth for wall-clock time. At boot SOS sets the system clock (`time()`,
`gettimeofday()`) from it and turns on the chip's battery switch-over, which is off by default.
All times are UTC. A time zone only adds local time to what `time` shows (see
[Time zones](#time-zones)).

Network time comes from **NTS** (Network Time Security, RFC 8915): NTP whose replies are
authenticated. SOS does a TLS key exchange with the NTS server (TCP port 4460), then sends
NTP requests that carry a single-use cookie and an AES-SIV tag, and accepts only replies that
check out under the server's key. Each sync sends 4 queries and uses the one with the shortest
round trip, which keeps Wi-Fi power-saving delays out of the result. The server is set on the
console with `time server <name>`; the default is the build-time `SOS_NTS_SERVER`
(`time.cloudflare.com`). The server's certificate chain must end at a trusted root: built in
(`src/tls/tls_roots.c`: SSL.com Root Certification Authority ECC for Cloudflare, ISRG Root X1 for
Let's Encrypt, which most other NTS servers use) or approved from the SD card (see
[Trusted root certificates](tls.md#trusted-root-certificates)).

SOS syncs in three cases:

- **The RTC has no valid time:** no chip, a first power-up, or a coin cell that ran out while
  the board was unpowered. A failed sync is retried after 30 s, then with doubling delays up to
  30 minutes, until one works.
- **`time sync`**, typed in any shell, SSH included.
- **Periodically, if turned on** with `time auto <hours>` on the console (1 to 720; default off).
  It syncs one interval after the last successful sync, whatever started it, or after boot if
  there has been none: a restart doesn't sync by itself when the RTC had the time, so a board
  that restarts often doesn't keep asking the server. This corrects RTC drift (see
  [RTC calibration](#rtc-calibration)) on a board that stays up. Without it, or on a board that restarts more
  often than the interval, run `time sync` now and then.

A successful sync sets the system clock and the RTC. NTS time is authenticated, so a sync may
move the clock any distance; a correction of more than 5 minutes is logged as a warning. Any
time earlier than the firmware's configure time is rejected.

While the clock isn't set, certificate dates can't be checked, so that first key exchange
accepts certificates whatever their dates (signatures and the host name are still checked) and
logs `certificate dates not checked (clock not set)`. Once the clock is set, dates are checked
as usual.

There is no automatic fallback to plain, unauthenticated NTP: if the NTS server can't be
reached (for example because the network blocks port 4460) and the RTC has no time, the clock
stays unset until NTS works. `time sync sntp` (console only) does one plain SNTP sync on request,
from the build-time `SOS_NTP_SERVER` (`pool.ntp.org`); `time` then shows the time as
unauthenticated.

- `time` shows the date and time and where it came from (RTC, NTS, or SNTP), the RTC's reading,
  backup-battery state and crystal load (`rtc:    2026-11-28 12:31:02 UTC; crystal load 12.5 pF`),
  the RTC calibration, the drift the last sync measured (below), the
  last sync with its correction and round trip, a failed attempt and when it's retried, and the
  server and periodic settings.
- `time auto`, `time server` and `time cal` (without a value) show those settings; `config`
  lists all three.
- `nts` shows the NTS state (cookies, last key exchange and query). `nts ke [host]` and
  `nts query` run the two steps by hand, for diagnosis; they don't set the clock. Naming another
  host is console only.

## RTC crystal load

A quartz crystal runs at its marked frequency only with the load capacitance it is made for; with
less it runs fast. The PCF8523 has the load built in, in two sizes chosen by the CAP_SEL bit of
its Control_1 register: 7 pF (the chip's power-on default) or 12.5 pF. The Adalogger's
32.768 kHz crystal is a 12.5 pF part, so at the default it runs fast: dev-1 measured 70 to 92 ppm
fast that way (issue #275).

At boot SOS sets CAP_SEL to the board's crystal if the chip has the other, and logs it once:
`rtc: crystal load set to 12.5 pF (was 7 pF)`. The board's crystal is a build setting,
`SOS_RTC_LOAD_PF` (`12.5` by default, or `7`; see
[building.md](building.md#the-boards-rtc-crystal)), not a device setting. `time` shows the load in
force on its `rtc:` line, with `(the board's is 12.5 pF)` if the chip didn't take it.

CAP_SEL is kept in the RTC while its coin cell lasts, like the time, so the next boot finds it set
and logs nothing; older firmware, which leaves the bit as it finds it, keeps the load set too.
A coin cell that runs out with the board unpowered resets it to 7 pF, and the next boot sets it
again.

A change of load changes the RTC's rate, so it starts a new drift measurement, as a change of
calibration does (below). Set the load first and calibrate after: a calibration measured at the
wrong load is wrong at the right one.

## RTC calibration

The RTC's crystal isn't trimmed, and an uncalibrated one can be well off: the board this was
developed on ran 92 ppm fast, about 8 s a day. The PCF8523 can correct its own rate, and
`time cal` (console only) sets that:

1. **Measure.** Run `time sync`, leave the board for a day or more (restarting it in between
   is fine), and run `time sync` once more. `time`'s `drift:` line then shows the RTC's rate,
   for example `rtc +92.0+-13 ppm in 22 h (no cal, 12.5 pF)`: positive means fast, and the figure after
   `+-` is how far off the measurement may be. The RTC counts whole seconds, so each
   measurement is good to about a second: the longer the wait, the better (about 15 hours
   for 20 ppm).
2. **Set it:** `time cal 92`. The RTC is slowed by the nearest step it can do (4.34 ppm each,
   here 91.1 ppm), and `time` shows `cal: slowed by 91.1 ppm`. A slow RTC takes a negative
   value. The `rtc-drift` health check's warning suggests the value.

The value isn't added to the one before: after measuring again with a calibration set, give
the rate `time` shows on its `cal:` line (91.1 here, not the 92 that was typed) plus the new
drift. A change of calibration starts a new measurement at the next sync. `time cal off`
removes it. The calibration is a saved
setting (`rtc_cal_ppm` when provisioning), written into the RTC at boot if the chip doesn't
already have it, so it survives a dead coin cell. The RTC applies it as a few short
corrections every two hours.

### How the drift is measured

Every sync that sets the RTC reads it first, at the second boundary it is about to be set on,
and compares the reading with the time it was set to at the sync before. That time, with the
calibration and the crystal load then in force, is kept in the history sector of flash
(`src/core/history.h`), so the measurement survives restarts and power cuts; a change of
calibration or load, or an RTC that lost its time, starts again (`time` then says
`rtc measured after the next sync (crystal load changed)`, or `calibration changed`). The uncertainty is the RTC's whole second, plus 32 ms per calibration step
(the PCF8523 applies the correction once every two hours, so in between it runs at its own rate),
plus the sync's round trip.

When the sync before was in the same uptime, the step that sync makes also measures the board's
own 12 MHz crystal (`xosc`), which the clock runs on between syncs; that is the one measurement
of the crystal against an outside reference (the `clocks` health check compares the clocks with
each other, all made from the same crystal).

The `rtc-drift` health check (`health`) judges both: WARN when the RTC is surely more than 20 ppm
off (a 32.768 kHz crystal is typically good to 20 ppm), or the crystal more than 65 ppm (its
tolerance, temperature and first year's aging); FAIL beyond 1000 ppm, the chip's clock margin. It
never judges while the uncertainty straddles a line.

After running older firmware: an older image's sync sets the RTC without recording it, so the
first measurement back reads too small (nearer zero) once; that sync starts a new baseline. Sync
twice after running older firmware before trusting the figure. A baseline recorded by firmware
from before the crystal load was kept (#275) has no load, and counts as another load: the first
sync back is a new baseline.

## Time zones

Everything the board keeps is UTC: the clock, the RTC, file times, log files and log rotation.
Chat and wall stamps and `ls` say `UTC`. A time zone only changes what `time` shows: one more
line, after the UTC one.

```
time:   2026-11-27 13:03:05 UTC (from nts, authenticated)
local:  2026-11-27 14:03:05 CET (UTC+01:00, Europe/Madrid)
```

- **`time zone <name>`** (console only) sets it by IANA name (`Europe/Madrid`,
  `America/New_York`). Case doesn't matter, and a space can stand for `_`. The name is looked up
  in `/tz/zones.bundle` on the card. A name that isn't there gets the closest ones (`madrid`
  offers `Europe/Madrid`). On the console, Tab completes zone names from the card's bundle:
  `time zone Europe/Ma` and Tab offers `Europe/Madrid`, `Europe/Malta` and `Europe/Mariehamn`.
  Case matters there, as in the bundle. Each Tab reads the bundle, about 100 ms. The name and the zone's rule, a POSIX TZ string such as
  `CET-1CEST,M3.5.0,M10.5.0/3`, are saved in the configuration, so local time shows with the
  card out too.
- **`time zone utc`** (console only) clears it.
- **`time zone`** shows the zone, its rule, local time, and the tzdata release of the card's
  bundle. If the card's rule for the zone differs from the saved one (the card has a newer
  release), it says so, and `time zone <name>` again takes the card's rule.
- **`time zone list [<prefix>]`** lists the card's zones with their rules. `time zone list
  europe/ | more` pages them.
- **`time zone install`** downloads `tz/zones.bundle` from this repository's main branch on
  GitHub (not from the firmware's commit, so a board gets a tzdata update without a reflash). It
  writes `/tz/zones.~new`, checks that every line is a zone with a rule the board can use, then
  renames it into place, so a failed download leaves the old bundle. It then says whether the
  saved zone's rule changed. It works from any shell, SSH included, because the card can be
  written over SFTP anyway and only `time zone <name>` on the console changes the setting. It
  isn't available while `sd check` has the card. On dev-1 (an encrypted exFAT card) it took
  about 0.8 s in all. Each pass over the bundle (the check here, or a `time zone` lookup) is
  about 100 ms, done in one go.
- **`tools/tz-install.sh [user@]<host>`** copies the same file over SFTP.
- **`tz = <name>`** in the provisioning file sets it when provisioning: `tools/provision.sh`
  looks the name up in the repository's `tz/zones.bundle`, so a new board shows local time
  before any card is in.
- **Lua scripts** get UTC from `sos.date` and `sos.time`, and local time only by asking:
  `sos.localdate`, `sos.localtime` and `sos.zone()` (see [Local time](lua.md#local-time)).

The bundle holds each zone's current rule, not its history: local time is right from the
release's date onwards, which is what a clock needs. It is built from IANA's signed tzdata
release (see "Time zone bundle" in [development.md](development.md#time-zone-bundle)). A rule
is the footer `zic` writes into each compiled zone; `src/time/tzrule.c` reads it, with
RFC 9636's extensions, and is checked against the C library for every zone in the bundle.
