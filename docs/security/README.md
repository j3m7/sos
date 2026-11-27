# Securing a board

Securing an SOS board means moving its root secret into one-time-programmable (OTP) memory and
turning on secure boot, so that only signed firmware runs and the secret can't be read over USB
(issue [#64](https://github.com/j3m7/sos/issues/64)).

| Page | What it is | Read it to |
|---|---|---|
| [design.md](design.md) | How the mechanisms work today and what they protect against | understand the system |
| [decisions.md](decisions.md) | Each decision, per board, with its date, reasons and costs; what is deferred; what is to confirm | know what was chosen and what is still open |
| [production-runbook.md](production-runbook.md) | The steps for a production board. **Draft: not to be run** | prepare a production board |
| [dev-1 runbook](../otp/dev-1/RUNBOOK.md) | The steps dev-1 went through on 2026-11-26 | repeat or check the dev-1 run |
| [dev-1 record](../otp/dev-1/RECORD.md) | Why the dev-1 run was shaped as it was, and what it showed | know what has been tested on hardware |

**Status (2026-11-26):** dev-1 is secured (root secret in OTP, two public dev keys, secure
boot on). No production board exists; its runbook waits for decisions 1, 2, 5 and 11 and the
items to confirm.
