# net: Wi-Fi, HTTPS and ping

`wifi.c` keeps the station connection up, without ever blocking the main loop. lwIP and the
CYW43 driver come from the Pico SDK (`pico_cyw43_arch_lwip_threadsafe_background`: network
processing runs in interrupt context). Their options are in `config/lwipopts.h`.

## Connection state machine

`wifi_poll()` moves through `W_OFF → W_SCANNING → W_CONNECTING → W_UP`, or `→ W_WAIT` after a
failure, then back to `W_SCANNING`. It logs each change.

- **Several networks:** up to 5 are saved (`wifi_ssid`, then `wifi_extra[]`), in the order
  they are preferred. Each attempt's scan looks for all of them, and `wifi_pick.c` (pure C,
  host-tested) chooses: the first saved network the scan saw that hasn't failed in this round,
  else the first not seen (hidden, or out of reach: the chip looks for it by name). So a hidden
  network is tried only when no other saved network is in sight. A network is given up for the
  round after 3 failed attempts, or, with others to try, at once on a wrong password or if the
  scan didn't see it. Once all have failed, the next round starts after 15 s. With one network
  this is the same timing as before. A connected device doesn't move to a preferred network
  that comes into reach until the link drops or `wifi connect`.
- **Names for Tab:** while an SSID question is pending (`wifi set`, `wifi add`, `setup`),
  `wifi_names_start()` keeps the names of all networks every scan hears, on the heap (tag
  `wifi`, 16 at most, strongest first: `wifi_names.c`, pure C, host-tested), and starts a scan
  unless one is running. The scan callback is shared: the connection's own scans feed the names
  too, and while the names' scan runs a connection attempt joins by name, unscanned. The names'
  scan also records the saved networks' access points in `scan_rx`, like any scan (more readings
  of the same networks, bounded by `SCAN_MAX_APS`; `begin_attempt()` clears them before its own
  scan). The answer
  frees them (`wifi_names_stop()`), and so does `wifi_poll()` 10 minutes after their last use.
- **Configuration copies:** the scan callback runs in interrupt context, and the console edits
  the live configuration in place. So each attempt copies the saved networks (under the
  driver's lock), and the callback and the join use only the copy. The callback writes its
  results to its own table, which the main loop clears and copies, also under the lock: past
  the scan deadline a scan may still be running.
- **Strongest access point:** a network may have several (a mesh, repeaters). Joining by name,
  the chip took one at -66 dBm with another at -34 dBm in reach in 8 of 12 boots. So:
  - Each attempt scans first (about 0.5 s) and logs the network's access points with their
    signal. The chip then joins by name.
  - Once connected, the one it joined is compared with the scan. If another of the same
    network was stronger by
    10 dB (`SWITCH_MARGIN_DB`), the device leaves and joins that one by address, about 3 s
    more. At most twice per connection (`SWITCH_TRIES`).
  - A scan can miss an access point. If it found fewer than two, a second scan runs after
    connecting, before the comparison (`W_VERIFY`).
  - Joining by address from the start isn't used: the first such join after boot failed
    every time (the retry worked). Passing the scan's channel as well made the join never
    complete.
  - The choice is made when connecting. A device that is moved while connected stays where
    it is until the link drops.
- **Connecting:** each attempt has 30 s. While the network isn't found, the join is re-issued
  every 3 s.
- **Retries:** the first 2 failed attempts on a network are retried after 1 s, because some
  access points drop authentication frames now and then and an immediate retry usually works;
  moving on to the next network also waits 1 s. Once every network has failed, the retry delay
  is 15 s. The counts reset once connected.
- **Link lost:** a new attempt starts straight away.
- **Wrong password:** the driver also reports `CYW43_LINK_BADAUTH` for a moment when an
  authentication frame times out, and clears it ~10 ms later when the retry succeeds. So it
  counts as a wrong password only after it lasts 3 s (`BADAUTH_GRACE_MS`).
- **Hostname:** the configured hostname is sent with DHCP requests.

`wifi_up()` (associated, with an IP address) is what the rest of the system waits for: the
SSH server starts, and NTS syncs, once it's true. `wifi set` and `wifi connect` on the console
call `wifi_reconnect()`, and so do `wifi add` when not connected and `wifi remove` of the
network in use. `wifi_ssid()` names the network connected to. A changed hostname applies at the next connect.

## Country

`wifi_country_code()` gives the saved country (`wifi_country`, two letters) to
`cyw43_arch_init_with_country()` in `main()`, so it takes effect at boot. Empty means the
chip's worldwide set, which is allowed everywhere; the SDK notes it may perform worse. With
`US`, measured against worldwide on one network, nothing changed that could be told from
run-to-run variation.

The driver doesn't check that the chip accepted the country. A code the chip doesn't know (`UK`
for `GB`, say) is ignored and the chip stays on its default (`US` here), so `wifi_start()` reads
the chip's country back and logs a warning when it differs from the saved one.

## Power saving

`wifi_apply_power()` sets the radio's power mode from the configuration (`wifi_powersave`):

- **Off (default):** `CYW43_NONE_PM`, the radio stays awake.
- **On:** `CYW43_PERFORMANCE_PM`, the SDK's default (PM2: the radio sleeps between beacons
  after 200 ms without traffic).

`wifi_start()` applies the setting after enabling station mode, which resets the driver to its
default. `wifi power on|off` and `config reset` apply it straight away.

## Keepalive

While connected, `wifi_poll()` sends a gratuitous ARP (a broadcast saying "this IP address is at this MAC address") every 60 s.

Without it, the device became unreachable after about 20 idle minutes, with power saving on
or off, and before and after resetting the access points. The device still showed itself as
connected. Clients got "no route to host": the router's ARP entry had expired, and the
device didn't answer the router's ARP queries. Anything the device sent brought it back.

lwIP's ARP counter (shown by `net`) showed why. With no traffic of its own, the device
received no ARP packets at all, from a minute or two after it stopped sending until the end,
including queries sent straight at it with `arping`. So broadcasts stop reaching the device,
either at the access point or in the Wi-Fi chip, while unicast to it still works. With the
announcement every minute, broadcasts kept arriving and the device stayed reachable through
26 idle minutes. Reports of Pico W boards becoming unreachable when idle (e.g. MicroPython
issue #9455) look like the same problem.

`wifi keepalive off` (console, until reboot) turns the announcement off to test this.

### Packet loss and `wifi stats`

`wifi stats` is for telling where packets go missing. It shows the radio's state (`wifi_radio()`:
signal, noise floor, channel, access point, transmit rate) and lwIP's counters, which
`config/lwipopts.h` turns on: packets per protocol, TCP segments resent (`MIB2_STATS`), and
how full the pools got (`MEMP_STATS`). The pools matter because the driver drops a received
frame silently when the packet pool (`PBUF_POOL_SIZE`) is empty. The pool holds 12 frames.
In normal use about 3 are in use at most, since a frame is freed as soon as it is delivered.
What can fill it is held back: a connection queues at most 6 segments that arrived out of
order (`TCP_OOSEQ_MAX_PBUFS`), and IP fragments are not accepted at all (`IP_REASSEMBLY 0`),
since an unfinished fragmented datagram would otherwise sit in the pool for 15 s.

To find the direction of a loss, ping the board and compare: `icmp received` counts the
requests that arrived and `icmp sent` the replies handed to the chip. `wifi stats chip` adds
the chip's own counters (`wifi_chip_counters()`), raw, and `wifi iovar` (console) reads and
sets chip variables, e.g. to fix the transmit rate for a test.

Measured in November 2026: about 1% of pings lost (bursts up to 5%), all of them replies; no
pool failures; noise floor about -95 dBm. Moving the board from -70 to -35 dBm changed
nothing, and neither did a fixed 12 Mbit/s rate, turning aggregation off, or lower transmit
power. By the chip's counters a third to a half of its transmissions went unacknowledged, so
the loss is in the air at the access point's end (the network was on channel 5, which
overlaps channels 1 and 6). `LWIP_TCP_RTO_TIME` (1 s) is what keeps TCP working through it.

## HTTPS client

`http.c` makes one HTTPS request per connection (HTTP/1.1, `Connection: close`) over `tls/`.
It's the base for talking to web services (issue #17): notifications, JSON APIs, and later
Lua's networking.

```
http_start() → HTTP_CONNECTING (tls/: DNS, TCP, handshake) → HTTP_SENDING → HTTP_WAITING
             → HTTP_BODY (http_read()) → HTTP_DONE           any step can end in HTTP_FAILED
```

- **Non-blocking:** `http_poll()` advances the request from the main loop. Once the headers
  are in, `http_read()` hands over the body as it arrives.
- **Pull, not push:** the body is read from TLS only when the caller asks for it. A slow
  consumer (an SSH client reading the output, say) leaves data in lwIP, whose receive window
  then closes, so the server slows down rather than memory filling up. Each request holds
  about 6 KB besides its TLS connection.
- **Requests:** the options are copied (body up to 16 KB). The client always sends `Host`,
  `User-Agent: sos`, `Accept-Encoding: identity` (it can't decompress) and `Connection: close`.
  Extra headers can't override those or the framing headers, and can't contain control
  characters, so a header value can't inject a line.
- **Responses** are parsed by `http_msg.c` (pure C, host tested). It's incremental: any split
  of the input gives the same result. It hands the body back as slices of the input without
  copying, whether sized by `Content-Length`, chunked, or running to the close. Header lines,
  chunk-size lines and trailers are size-limited, `1xx` responses are skipped, and framing
  that could be read two ways (both `Content-Length` and chunked, conflicting lengths, other
  transfer codings) is refused.
- **End of the body:** a body without a length is trusted only if the server ends TLS with
  `close_notify`. Otherwise the request fails, since an attacker could cut it short by closing
  the TCP connection.
- **Timeout:** `timeout_ms` (default 30 s) runs until the headers arrive, then between pieces
  of the body.
- **Redirects, opt-in:** with `max_redirects` set (at most 5), a `301`, `302`, `303`, `307` or
  `308` with a `Location` is followed inside `http.c`: the 3xx's body is dropped, the
  connection closed, and the same request (GET or HEAD only) made to the new URL on a new
  connection, with its own certificate check and timeout. `http_msg.c` resolves the `Location`
  against the request's URL (RFC 3986, dots removed) and refuses a target that isn't https (from
  https to http only with `allow_downgrade`, and not until plain http exists), one with
  `user:password@`, or one longer than the URL parser takes (`HTTP_URL_MAX`). Requests with
  extra headers or a body can't opt in, so a header such as `Authorization` never goes to a
  host a server names. `http_redirects()` and `http_url()` tell the caller each hop. Without
  the option (`http get`, `sos.http`) the caller sees the `3xx` and `Location`, as before.
- **Untrusted roots:** when the handshake fails because no trusted root signs the chain,
  `tls/` notes the issuer it couldn't verify (`tls_stats_t.untrusted_issuer`), for messages
  that say which root to add.
- **Not done yet:** keep-alive, compression, proxies, plain http (docs/design/archives.md
  §3.3).

`http get|head|post` (any shell, `docs/commands.md`) runs a request as a shell job, for
testing. Lua scripts use the client through `sos.http` (`src/lua/README.md`), from any shell.

`fetch.c` runs `fetch [-f] [-k] <url> [<file>]` (docs/design/archives.md §3), a shell job like
`cp`: it follows redirects (printing `-> <url>` per hop), writes a `2xx` body through a 4 KB
buffer to `<file>.part`, hashing it with wolfCrypt's SHA-256 as it writes, and renames the
`.part` only when `http.c` says the response is complete (`HTTP_DONE`: the `Content-Length`
reached, the last chunk, or `close_notify`). Any failure or Ctrl-C deletes the `.part`. Writes
pass `files_write_allowed()`, the job holds the card (`files_hold()`) and syncs once a second
(`files_sync_if_due()`). The progress line is drawn only on the console (`io->admin`) and not
into a pipe. One archive job runs at a time (`files/bigjob.h`). The pure parts (Location,
the name a URL suggests, `path_name_check()`) are host tested and fuzzed (`fuzz_http`).

## Ping

`ping.c` runs `ping <host> [<count>]` (`docs/commands.md`) as a shell job, from any shell.
`ping_msg.c` holds the parts without lwIP: the arguments, the echo request and the check of a
reply, and the statistics and their text (pure C, host tested).

- **Raw PCB:** each run opens its own raw ICMP PCB (`LWIP_RAW`). lwIP's
  `contrib/apps/ping/ping.c` isn't used: it reports only through `LWIP_DEBUGF`, keeps a single
  static target, and paces itself with `sys_timeout`, of which there is one spare.
- **Interrupt context:** lwIP hands every incoming ICMP packet to the raw PCBs before its own
  ICMP code, in interrupt context. The callback copies the packet (at most 168 bytes) to the
  stack and takes it only if it is an echo reply of exactly our size, with the run's
  identifier, a sequence number already sent, our data and a correct checksum. It records the
  arrival time, TTL and sender for the request in flight, and nothing more: no heap, no
  logging, no output. Everything else goes back to lwIP (return 0), so the board keeps
  answering pings and other ICMP isn't lost.
- **ICMP errors:** a destination unreachable or time exceeded (correct checksum) that quotes an
  echo request with the run's identifier is recorded for the request in flight, like a reply,
  with its type, code and sender, and the job prints it instead of a timeout. It still goes back
  to lwIP, which counts it in its statistics. Routers quote at least the original IP header and
  8 bytes of ICMP, which hold the identifier and sequence number; the parser reads no more. 168
  bytes holds the longest error about our request (both IP headers with options, all of the
  request quoted). A longer one, as with RFC 4884 extensions (MPLS labels on some ISP routers),
  isn't read and shows as a timeout. A host on the LAN that doesn't answer ARP produces no error
  at all (lwIP drops the queued packet), so that stays a timeout too. "Time exceeded" needs the
  TTL to run out, and lwIP sends with 255.
- **Several at once:** every run gets its own ICMP identifier (a counter, seeded at random once
  per boot), so runs in different shells don't take each other's replies. lwIP has 4 raw PCBs,
  one per shell (the console and 3 SSH sessions); a fifth run is refused.
- **Names:** `dns_gethostbyname()`, as `tls/` does. The DNS callback gets the run's id, not a
  pointer, and looks the run up: a run can be cancelled and freed before DNS answers. The raw
  PCB's callback can take a pointer, because the PCB is removed (under the lwIP lock) before the
  run is freed.
- **Timing:** one request at a time. Each waits up to 1 s for its reply or an error, and the
  next goes out 1 s after it, so a lost reply costs no extra time; a reply or error after that
  is dropped. The send
  time is taken under the lwIP lock just before `raw_sendto()`, and the arrival time in the
  callback, so the main loop's pace (5 ms) doesn't count in the round trip.
- **Ctrl-C** prints the summary. The same cancel ends a run when its SSH session goes away;
  its output then goes nowhere (`ssh_session_write()` refuses a closed session), as with `chat`.
