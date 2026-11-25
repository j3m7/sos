# time: RTC, system clock and network time

```
boot:   RTC (PCF8523) ──► system clock + AON timer
sync:   NTS-KE over TLS ──► cookies + keys ──► 4 NTS-protected NTP queries ──► best sample
        ──► system clock + AON timer + RTC
```

All times are UTC. Times before the firmware's configure time (`SOS_BUILD_EPOCH`), or after
2099 (the RTC's limit), are rejected as implausible.

## `rtc.c` and `timeconv.c`

- **`rtc.c`** drives the Adalogger's PCF8523 over I2C0 (SDA GP4, SCL GP5, address 0x68).
  - At init it turns on battery switch-over, which is off by default, and reports a low
    backup battery.
  - It writes the time on a second boundary, so the RTC keeps the sub-second phase.
  - The chip's OS flag means "the time was lost", so the RTC is read as invalid until the next
    sync.
  - `rtc_read_offset()` and `rtc_write_offset()` handle the offset register (0Eh), which trims
    the RTC's rate: -64..63 steps of 4.34 ppm, a positive value slowing it. The chip's
    correction mode stays at its default, every two hours.
- **`timeconv.c`** (pure C, host tested) converts between Unix time and the calendar, and
  between the calendar and the RTC's BCD registers. It also parses a rate in ppm and turns it
  into offset-register steps, for `time cal` and the provisioning tool.

## `timesync.c`: the clock

- **At boot,** `timesync_boot()` reads the RTC and, if its time is valid, sets the clock from
  it. Otherwise the clock stays unset until a sync.
- **One function sets the clock:** `set_clock_us()`. It sets `gettimeofday()` and the RP2350's
  always-on (AON) timer together. The SD card library defines its own `time()` and
  `get_fattime()`, which read the AON timer, and that `time()` replaces the C library's.
  wolfSSL checks certificate dates with `time()`, so both clocks must agree. They count the same
  crystal, so they stay together.
- **When it syncs:**
  - automatically while the RTC had no valid time, retrying after 30 s, then doubling up to
    30 minutes;
  - on `time sync`;
  - every N hours with `time auto` (off by default): N hours after the last accepted sync, or
    after boot if there was none. A boot with a valid RTC time doesn't sync.
- **Applying a sync** sets the clock and then the RTC. A step over 5 minutes is logged as a
  warning.
- **RTC calibration:** the setting `rtc_offset` holds the offset register's value (`time cal`
  takes ppm and stores the nearest step). `timesync_apply_cal()` writes it into the RTC after
  the settings load at boot, and when it changes, if the chip doesn't already hold it. The
  setting, not the chip, is the reference: the register is lost with the coin cell.
- **SNTP:** NTS is the only automatic source, with no fallback to plain NTP. `time sync sntp`
  (console only) does one unauthenticated SNTP sync (lwIP's SNTP app), and `time` then shows the time as
  unauthenticated.

## NTS: authenticated NTP (RFC 8915)

- **`nts_ke.c`** (pure C, host tested) encodes the NTS-KE request and parses the server's
  records. The request asks for AEAD 15 (AES-SIV-CMAC-256), and the response gives the cookies
  and the NTP server and port.
- **`nts_ntp.c`** (pure C, host tested) builds an NTS-protected NTP request and checks the
  reply.
  - Its extension fields are a Unique Identifier, a cookie, cookie placeholders, and an
    Authenticator whose AES-SIV tag covers everything before it.
  - A reply is accepted only if it carries the same identifier and a tag that checks out. The
    new cookies come inside its ciphertext.
  - The AEAD is passed in, so tests use a stand-in.
- **`nts.c`** runs it on the device, as two state machines polled from the main loop:
  - **Key exchange:** `tls/` connects to port 4460 with ALPN `ntske/1`. The C2S and S2C keys come
    from the TLS exporter (label `EXPORTER-network-time-security`). The keys and 8 cookies are
    kept for later queries.
  - **Query:** a sync sends 4 queries over UDP, each using one cookie, and keeps the sample with
    the shortest round trip. Wi-Fi power saving adds up to ~200 ms of delay to some replies, and
    the shortest one avoids it.
  - **Cookies:** each reply brings a new cookie, so queries need no TLS once a key exchange is
    done. When the cookies run out, the server sends an NTSN Kiss-o'-Death, or the configured
    server changes, the next sync does a new key exchange.
  - `nts ke [host]` and `nts query` run the two steps by hand; they don't set the clock.
    Naming another host is console only.
  - An NTS NAK is not authenticated, so it only counts if no valid reply follows before the
    query times out; then the keys are dropped and the sync does one new key exchange.

## Before the clock is set

The first key exchange can't check certificate dates. `tls/` then loads roots and accepts
chains whatever their dates (signatures and the host name are still checked), and logs
`certificate dates not checked (clock not set)`.
