# Agent notes

## How we work

```
branch off main → PR → USB box checks (firmware) → merge
                 → publish from main only when Lucas asks
```

- `main` is the only long-lived branch (trunk + release). No standing `dev` or `release` branch.
- Do **not** publish OTA or USB factory from a PR. Do **not** bump `firmware/VERSION` except at release.
- Do **not** flash dedicated QA boxes (`/dev/searaboom-qa-*`). Those belong to the Origin webhook QA agent.
- Firmware/device PRs: push and wait for **USB box (s3-zero)** and **USB box (s3-supermini)**.
  - Rerun quiet QA: comment `/hw-test`
  - Soak: `/hw-test soak`
  - Loud / mic: `/hw-test listen` (only when Lucas says the room is OK)
  - Pads / human steps: `/hw-test hands`
- Local bring-up: a USB box that is **not** in `~/.config/searaboom/qa-boxes.json`.
- When opening a PR, use `PULL_REQUEST_TEMPLATE.md`.

## Agent publishing policy

**Do NOT auto-publish OTA or update the live server.**

- Not on a schedule, not from log sweeps, not from PR branches.
- Publishing is **from `main` when Lucas asks.** Then do it.

A release ships **one** version to OTA **and** USB factory (`slot=live`).

| Task | Command | When |
|------|---------|------|
| Firmware release | `./scripts/dev_ota.sh` | From main when Lucas asks |
| Already-built bin | `./scripts/publish_firmware.sh X.Y.Z` | From main when releasing |
| USB factory only | `./scripts/snapshot_factory.sh [X.Y.Z]` | Rare |

Do **not** point the public flash page at `firmware/build/`. Factory files live in `server/firmware/factory/`. USB install for testers is the **single-binary factory flasher** from https://searaboom.goossen.dev/ (`/api/factory/flasher/`, esptool-style DTR/RTS). The binary fetches the live USB image; do not embed firmware. Chrome WebSerial is a fallback. `idf.py flash` is local-dev only, never on QA nodes.

Version source of truth: `firmware/VERSION` (also `CONFIG_SEARABOOM_FW_VERSION` in `firmware/sdkconfig.defaults`).

OTA + logs: https://searaboom.goossen.dev/admin  
Lab fleet: `GET /api/lab/status`  
After a release, `GET /api/status` — `firmware.version` and `factory.version` should match.

Server: `~/.config/systemd/user/searaboom-server.service` (repo `server/app.py`). After editing `app.py`, restart that unit. Do **not** restart it from a PR. Admin token default: `searaboom-dev`.

Portable standup (new host, or a **test** instance beside Mini prod): [`docs/hosting.md`](docs/hosting.md). Do **not** retarget `searaboom.goossen.dev`. Do **not** restart `searaboom-server` / `searaboom-tunnel` from a PR.

QA Origin app (this host): `./scripts/setup_origin_app.sh`. Webhook: `POST /api/lab/origin-webhook`.

## Log acknowledgments

Device logs persist under `server/logs/`. To avoid reprocessing the same events in sweeps, mark them as acknowledged:

**Persistence:** `server/logs/log_acks.json` stores fingerprints (recurring error patterns) and watermarks (per-device "processed through" timestamps).

**Admin API** (token-protected like other admin routes):

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/admin/log-acks` | GET | List all fingerprint acks and device watermarks |
| `/api/admin/log-acks/mark` | POST | Mark logs as acked (fingerprint or watermark) |
| `/api/admin/log-acks/clear` | POST | Clear a fingerprint ack or device watermark |
| `/api/admin/log-acks/fingerprint` | GET | Compute fingerprint for a log pattern |

**Marking logs:**

- **Fingerprint mode** (for recurring error patterns): `POST /api/admin/log-acks/mark` with `device_id`, `tag`, `msg`, optional `note`, `pr_url`.
  - Fingerprint = hash(device_id + tag + normalized message). Normalization removes numbers/IPs/timestamps.
- **Watermark mode** (for "scanned through"): `POST /api/admin/log-acks/mark` with `device_id`, `watermark` (unix timestamp), optional `note`, `pr_url`.

**Querying logs:**

`GET /api/logs?unacked=1` filters out acked logs (by fingerprint or watermark).

**Sweep workflow:**

1. Scan logs with `GET /api/logs?unacked=1` or equivalent.
2. For each issue filed/fixed, call `/api/admin/log-acks/mark` with the log pattern (fingerprint) or set a device watermark.
3. Include a `note` describing the fix and/or a `pr_url` linking to the PR.
4. Skip acked signatures on subsequent sweeps unless volume spikes again (check total vs acked count).
