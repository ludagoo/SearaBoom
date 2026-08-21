# SearaBoom

ESP32-S3 web radio (ESP-ADF) with captive WiFi setup, AAC streaming, OTA, and remote logs. Port of [CrazyGoosse/SearaBoom](https://github.com/CrazyGoosse/SearaBoom).

## Toolchain

ESP-IDF **5.3.x** + ESP-ADF. These modules boot-loop with Octal PSRAM — Quad only.

```bash
export IDF_PATH=~/esp/esp-idf ADF_PATH=~/esp/esp-adf
source "$IDF_PATH/export.sh"
./scripts/setup_host.sh   # udev + uucp; log out/in once
```

User units `searaboom-server` and `searaboom-tunnel` live in `~/.config/systemd/user/` (not in this repo). Same processes: `./scripts/run_server.sh` and `./scripts/run_tunnel.sh`.

Agent / automation notes for this pipeline: [`AGENTS.md`](AGENTS.md).

## Flash vs OTA

USB factory flash is **one publish behind** OTA. A newly flashed box picks up the current OTA the first time it joins Wi‑Fi.

| Situation | What to do |
|-----------|------------|
| New or bricked unit | https://searaboom.goossen.dev/ in Chrome/Edge with the box on **that computer’s** USB, or `idf.py -p /dev/ttyACM0 flash` |
| Day-to-day firmware | `./scripts/dev_ota.sh` (publishes OTA; USB factory stays behind) |
| Freeze a new USB image | `./scripts/snapshot_factory.sh` (only when you mean to change what USB writes) |

`dev_ota.sh` bumps the **patch** digit and publishes OTA. It promotes the previous OTA full image to USB factory, then stages this build as the next USB image.

The frozen USB image is **0.0.9**. Working tree / OTA latest is **0.1.0**, so a newly USB-flashed box updates on first Wi‑Fi. Firmware after `0.1.0` applies any newer X.Y.Z on boot.

Factory flash (new boxes): https://searaboom.goossen.dev/  
OTA + logs: https://searaboom.goossen.dev/admin

Backlog: [`docs/IMPROVEMENTS.md`](docs/IMPROVEMENTS.md)
