# Agent notes

## ⚠️ AGENT POLICY: NO AUTO-DEPLOYMENT

**Agents working on this repository:**

- **ONLY** create fixes and open pull requests
- **NEVER** publish OTA updates
- **NEVER** run `dev_ota.sh`, `publish_firmware.sh`, or `snapshot_factory.sh`
- **NEVER** upload to factory/OTA slots (`/api/factory/upload`, `/api/ota/upload`)
- **NEVER** restart `searaboom-server.service` or `searaboom-tunnel.service`
- **NEVER** deploy or modify the live server

**Only Lucas** publishes OTA updates or modifies the live server, and **only with an explicit direct command**.

All deployment commands below are **HUMAN/OPERATOR-ONLY**.

---

## Shipping firmware (HUMAN/OPERATOR-ONLY)

USB factory flash stays **one OTA publish behind**. A newly USB-flashed box should OTA on first Wi‑Fi.

**⚠️ The commands in this table are for human operators only. Agents must NOT run these.**

| Task | Command | **Who runs this** |
|------|---------|-------------------|
| Normal firmware update | `./scripts/dev_ota.sh` | **Human operator only** |
| Publish an already-built `firmware/build/searaboom.bin` | `./scripts/publish_firmware.sh X.Y.Z` | **Human operator only** |
| Freeze what USB writes (rare) | `./scripts/snapshot_factory.sh [X.Y.Z]` | **Human operator only** |

`dev_ota.sh` / `publish_firmware.sh`:

1. Upload the new app as **OTA latest**
2. Promote `server/firmware/factory-next/` → USB factory (previous OTA full image)
3. Stage this build as `factory-next` (not live USB)

Do **not** (especially agents):

- Upload the new version to `/api/factory/upload` with `slot=live` (that makes USB == OTA, so first Wi‑Fi will not update)
- Point USB flash at `firmware/build/` — factory files live only in `server/firmware/factory/`
- USB factory is **0.5.0**. OTA latest is **0.5.2**. Firmware after `0.1.0` applies any newer `X.Y.Z` on boot. Do not bump only the patch vs a `0.0.63` factory image — that build ignored patch.
- Use `idf.py flash` to ship boxes. USB install for testers is https://searaboom.goossen.dev/ (ESP Web Tools). `idf.py flash` is local-dev only (`dev_ota.sh --flash`).

Version source of truth: `firmware/VERSION` (also `CONFIG_SEARABOOM_FW_VERSION` in `firmware/sdkconfig.defaults`). CMake embeds `firmware/VERSION` into the app.

OTA + logs UI: https://searaboom.goossen.dev/admin  
Check lag: `GET /api/status` (`firmware.version` = OTA, `factory.version` = USB).

Server is `~/.config/systemd/user/searaboom-server.service` (repo `server/app.py`). **Human operators only:** After editing `app.py`, restart that unit. **Agents: do NOT restart this service.** Admin token default: `searaboom-dev`.
