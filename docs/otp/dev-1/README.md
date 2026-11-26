# OTP file for dev-1

`boot-keys.json` holds the boot key hashes for dev-1 (serial `F94525FD61D9A1CC`) and no other
board. It was loaded into dev-1 on 2026-11-26. Loading it is permanent; it is step 7 of
[`RUNBOOK.md`](RUNBOOK.md), which also has every other OTP write as a single `picotool otp set`.

The keys are the public development keys in [`keys/`](../../../keys/README.md): anyone can sign
with them. The file holds only their public-key hashes:

```
slot 0  e2f7cff033600f5ab00e33b6f891a0ff6e635ad0f2088613ca8b71b5f56d0d51   keys/dev-INSECURE-1.pem
slot 1  b97c0fd7efb68937e733c4916b44ce4f940141c4e6ecc90994c941f396d7c910   keys/dev-INSECURE-2.pem
```

In the file they are the same 32 bytes per slot, in the same order, as decimal numbers: the
form `picotool seal` writes. They land in rows 0x080 to 0x09f (page 2), with error correction,
low byte first in each row.

To check the file against the keys without trusting this page:

```sh
openssl ec -in <key>.pem -pubout -outform DER | tail -c 64 | sha256sum
python3 -c "import json; d=json.load(open('boot-keys.json')); print(bytes(d['bootkey0']).hex()); print(bytes(d['bootkey1']).hex())"
```

Checked: both hashes against the key files with `openssl`, and slot 0 against what `bootkey`
prints for an image signed with the first key. The JSON form was compared with a file
`picotool seal` wrote (same names, same byte order). Loaded into dev-1 on 2026-11-26: rows
0x080 to 0x09f read back as expected, and `bootkey` then reported a match in each slot.
