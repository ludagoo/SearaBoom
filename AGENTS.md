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
- Bump only the patch vs the **current USB factory image** if that factory firmware is still `0.0.63` — that build ignores patch on boot. First OTA after that USB image must be a **minor** bump (`0.1.0` or later). Firmware after `0.1.0` applies any newer `X.Y.Z` on boot.
- Use `idf.py flash` to ship boxes. USB install for testers is https://searaboom.goossen.dev/ (ESP Web Tools). `idf.py flash` is local-dev only (`dev_ota.sh --flash`).

Version source of truth: `firmware/VERSION` (also `CONFIG_SEARABOOM_FW_VERSION` in `firmware/sdkconfig.defaults`). CMake embeds `firmware/VERSION` into the app.

OTA + logs UI: https://searaboom.goossen.dev/admin  
Check lag: `GET /api/status` (`firmware.version` = OTA, `factory.version` = USB).

Server is `~/.config/systemd/user/searaboom-server.service` (repo `server/app.py`). After editing `app.py`, restart that unit. Admin token default: `searaboom-dev`.
