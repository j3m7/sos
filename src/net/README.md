# net: Wi-Fi and HTTPS

`wifi.c` keeps the station connection up, without ever blocking the main loop. lwIP and the
CYW43 driver come from the Pico SDK (`pico_cyw43_arch_lwip_threadsafe_background`: network
processing runs in interrupt context). Their options are in `config/lwipopts.h`.

## Connection state machine

`wifi_poll()` moves through `W_OFF → W_CONNECTING → W_UP`, or `→ W_WAIT` after a failure, then
back to `W_CONNECTING`. It logs each change.

- **Connecting:** each attempt has 30 s. While the network isn't found, the join is re-issued
  every 3 s.
- **Retries:** the first 2 failed attempts are retried after 1 s, because some access points
  drop authentication frames now and then and an immediate retry usually works. After that the
  retry delay is 15 s. The attempt count resets once connected.
- **Link lost:** a new attempt starts straight away.
- **Wrong password:** the driver also reports `CYW43_LINK_BADAUTH` for a moment when an
  authentication frame times out, and clears it ~10 ms later when the retry succeeds. So it
  counts as a wrong password only after it lasts 3 s (`BADAUTH_GRACE_MS`).
- **Hostname:** the configured hostname is sent with DHCP requests.

`wifi_up()` (associated, with an IP address) is what the rest of the system waits for: the
SSH server starts, and NTS syncs, once it's true. `wifi set` and `wifi connect` on the console
call `wifi_reconnect()`. A changed hostname applies at the next connect.

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
- **Not done yet:** redirects (the caller sees the `3xx` and `Location`), keep-alive,
  compression, proxies.

`http get|head|post` (any shell, `docs/commands.md`) runs a request as a shell job, for
testing. Lua scripts use the client through `sos.http` (`src/lua/README.md`), from any shell.
