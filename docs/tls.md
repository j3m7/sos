# TLS: trusted roots and the HTTPS client

## Trusted root certificates

TLS (used by NTS and the HTTPS client) trusts the built-in roots and, optionally, extra ones
from the card in `/certs/roots.pem`: a PEM bundle (text between the certificates, as in system
CA files, is fine) or DER certificates, up to 64 KB and 32 certificates. The file is used only
while it's exactly the file approved on the console:

1. Upload it anywhere with SFTP, e.g. `put roots.pem incoming/roots.pem`.
2. On the console: `mkdir /certs` (once), then `mv /incoming/roots.pem /certs/roots.pem`.
3. On the console: `tls roots approve`. It lists each certificate (common name, key type, whether
   it's a CA, expiry, SHA-256 fingerprint) and checks TLS can use it; type `yes` to trust them.
   The file's SHA-256 is saved with the configuration.

- **Only the console can change `/certs`.** SSH shells and SFTP can read it but not write,
  delete or rename anything in it, under any spelling FatFs accepts (`/CERTS.`, `\certs`,
  `/x/../certs`).
- **A changed file isn't used** until it's approved again: TLS then logs a warning once and uses
  the built-in roots alone. The same happens if the file or the card is missing, so NTS keeps
  working without the card.
- `tls roots` shows the built-in roots and the file's state; `tls roots forget` (console) stops
  trusting it. A private key, or any PEM block other than a certificate, makes the file invalid.

## HTTPS client

The device can make HTTPS requests: from Lua scripts with `sos.http` (see [Lua scripts](lua.md#https)), and from any shell
with `http get|head <url>` and `http post <url> <text>`, mostly for testing. Every request uses
TLS 1.3 with the server certificate checked against the trusted roots
([above](#trusted-root-certificates)), and redirects aren't followed. The `http` command
filters what the server sends before it reaches the terminal (no escape sequences, invisible or
bidi characters); `sos.http` gives a script the bytes as they are.

- Two HTTPS requests can run at once; a third is refused ("too many HTTPS requests at once")
  until one finishes. One more TLS connection is always kept free for time sync (NTS).
- A request is refused while the heap is too low for a TLS handshake (about 64 KB free).

See [man/http.txt](../man/http.txt) and [`src/net/`](../src/net/README.md).
