# SearaBoom lab

- `main` is the only long-lived branch. Work on a PR. Do not publish OTA or USB factory from a PR.
- Do **not** flash `/dev/searaboom-qa-*` or `/dev/ttyACM*` “because USB exists.” Dedicated QA boxes belong to the Origin webhook QA agent.
- Firmware/device PRs: push and wait for **USB box (s3-zero)** and **USB box (s3-supermini)**. Rerun: comment `/hw-test`. Loud: `/hw-test listen` (only when Lucas says the room is OK). Hands: `/hw-test hands`.
- Local bring-up: a box that is **not** in `~/.config/searaboom/qa-boxes.json`.
- Publish from `main` only when Lucas asks: `./scripts/dev_ota.sh`.
- Never create `~/.config/searaboom/signing_key_backed_up`. Only Lucas creates that file after the signing key is stored offline. Until it exists, refuse signed USB flash and OTA/factory publish (`docs/SIGNED_FIRMWARE.md`). No `--force`.
