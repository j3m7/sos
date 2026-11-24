# net: Wi-Fi

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
