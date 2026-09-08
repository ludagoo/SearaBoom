# Agent notes

## Shipping firmware

USB factory flash stays **one OTA publish behind**. A newly USB-flashed box should OTA on first Wi‑Fi.

| Task | Command |
|------|---------|
| Normal firmware update | `./scripts/dev_ota.sh` |
| Publish an already-built `firmware/build/searaboom.bin` | `./scripts/publish_firmware.sh X.Y.Z` |
| Freeze what USB writes (rare) | `./scripts/snapshot_factory.sh [X.Y.Z]` |

`dev_ota.sh` / `publish_firmware.sh`:

1. Upload the new app as **OTA latest**
2. Promote `server/firmware/factory-next/` → USB factory (previous OTA full image)
3. Stage this build as `factory-next` (not live USB)

Do **not**:

- Upload the new version to `/api/factory/upload` with `slot=live` (that makes USB == OTA, so first Wi‑Fi will not update)
- Point USB flash at `firmware/build/` — factory files live only in `server/firmware/factory/`
- USB factory is **0.5.0**. OTA latest is **0.5.2**. Firmware after `0.1.0` applies any newer `X.Y.Z` on boot. Do not bump only the patch vs a `0.0.63` factory image — that build ignored patch.
- Use `idf.py flash` to ship boxes. USB install for testers is https://searaboom.goossen.dev/ (ESP Web Tools). `idf.py flash` is local-dev only (`dev_ota.sh --flash`).

Version source of truth: `firmware/VERSION` (also `CONFIG_SEARABOOM_FW_VERSION` in `firmware/sdkconfig.defaults`). CMake embeds `firmware/VERSION` into the app.

OTA + logs UI: https://searaboom.goossen.dev/admin 
Check lag: `GET /api/status` (`firmware.version` = OTA, `factory.version` = USB).

Server is `~/.config/systemd/user/searaboom-server.service` (repo `server/app.py`). After editing `app.py`, restart that unit. Admin token default: `searaboom-dev`.

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
