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

## Flash vs OTA

| Situation | What to do |
|-----------|------------|
| New or bricked unit | USB: `idf.py -p /dev/ttyACM0 build flash` |
| Day-to-day firmware | `./scripts/dev_ota.sh` (keeps OTA tested) |

`dev_ota.sh` bumps the **patch** digit. Boot OTA ignores patch — bump **minor** when field units should pick up an update on reboot without serial `ota`.

Management UI: https://searaboom.goossen.dev/

Backlog: [`docs/IMPROVEMENTS.md`](docs/IMPROVEMENTS.md)
