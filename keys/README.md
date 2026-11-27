# Development signing keys: PUBLIC, NOT SECRET

These two private keys are published on purpose. Anyone can sign firmware with them, so a board
that accepts them accepts anyone's firmware.

| File | Public key hash (as `bootkey` and `tools/flash.sh` print it) |
|---|---|
| `dev-INSECURE-1.pem` | `e2f7cff033600f5ab00e33b6f891a0ff6e635ad0f2088613ca8b71b5f56d0d51` |
| `dev-INSECURE-2.pem` | `b97c0fd7efb68937e733c4916b44ce4f940141c4e6ecc90994c941f396d7c910` |

- **Use them only for development boards** (dev-1), which keep debug on and hold no secrets that
  matter.
- **Never write these hashes into a production board's OTP.** That board would then run firmware
  from anyone, for good. A production board gets its own keys, made and kept off the
  development machine (`docs/security/decisions.md`, decision 4).

To sign a build with one: `cmake --preset debug -DSOS_SIGN_KEY=keys/dev-INSECURE-1.pem`.

To check a hash: `openssl ec -in <key>.pem -pubout -outform DER | tail -c 64 | sha256sum`.
