# tz: the time zone bundle

- `zones.bundle`: each IANA zone's current rule, one line per zone. It is built by `tools/tz-bundle.sh`; see "Time zone bundle" in `docs/development.md`.
- `VERSION`: the IANA release the bundle was built from.
- `maintainers.asc`: the TZ Coordinator's public key, which the script checks IANA's release signatures against. It lives here rather than in `keys/` because `keys/` holds our own firmware signing keys.
