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
([above](#trusted-root-certificates)), and redirects aren't followed (`fetch` follows them). The `http` command
filters what the server sends before it reaches the terminal (no escape sequences, invisible or
bidi characters); `sos.http` gives a script the bytes as they are.

- Two requests (`http`, `sos.http`, `fetch` over https or plain http) can run at once; a third
  is refused ("too many HTTP requests at once") until one finishes. One more TLS connection is
  always kept free for time sync (NTS).
- Only `fetch` takes plain `http://` URLs; `http` and `sos.http` refuse them.
- A request is refused while the heap is too low for a TLS handshake (about 64 KB free).

See [man/http.txt](../man/http.txt) and [`src/net/`](../src/net/README.md).

## Downloading files

`fetch [-f] [-k] <url> [<file>]` saves a URL's body to a file on the card, from any shell:

```
sos> fetch https://example.org/pkg-1.2.tar.gz /apps
-> https://cdn.example.org/pkg-1.2.tar.gz
saved /apps/pkg-1.2.tar.gz: 183204 bytes in 2.4 s (74 KB/s)
sha256: 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
```

- **Name:** without `<file>`, the last part of the URL's path, percent-decoded; a directory as
  `<file>` keeps that name inside it. An existing file isn't replaced without `-f`.
- **Never half a file:** the body goes to `<file>.part` and is renamed only when complete (the
  announced length, the last chunk, or a clean TLS close; on plain http, the TCP close). A
  failure or Ctrl-C deletes it.
- **Only `2xx` is saved;** another status prints its line and the first 256 bytes of the body
  (filtered), and saves nothing.
- **Redirects** are followed (at most 5, each hop printed as `-> <url>`, each with its own
  certificate check); https to plain http only with `-k`, http to https always.
- **Plain http** works, unprotected: `fetch` prints `http: not encrypted; check its hash` for
  an `http://` URL or hop, so compare the SHA-256 with one published over https. A body without
  a length ends at the TCP close, which a cut connection also gives: the file is kept and the
  summary adds `size not known: check its hash`. A reset connection fails as usual.
- **The SHA-256** of what was written is printed at the end, to compare with a published hash
  without reading the file again. On the console a progress line shows the bytes, percent and
  rate; over SSH or into a pipe only the summary.
- **Roots:** many download hosts chain to roots other than the 2 built-in ones. The request then
  fails at the handshake and `fetch` names the issuer it couldn't verify: add that root to
  `/certs/roots.pem` and approve it ([above](#trusted-root-certificates)).
- One `fetch` runs at a time across all shells; it counts as one of the two requests.

See [man/fetch.txt](../man/fetch.txt).
