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

Agent / automation notes: [`AGENTS.md`](AGENTS.md).

## How we work

`main` is the only long-lived branch. Work on a PR. Firmware changes get **USB box** checks on the lab (both S3-Zero and SuperMini). Do not flash `/dev/searaboom-qa-*` yourself. Publish OTA + USB factory from `main` only when releasing (`./scripts/dev_ota.sh`).

Rerun / extra QA on a PR: `/hw-test`, `/hw-test soak`, `/hw-test listen` (noise OK), `/hw-test hands` (at the boxes).

## Flash vs OTA

A release ships the **same** version to OTA and USB factory.

| Situation | What to do | **When to run** |
|-----------|------------|-----------------|
| New or bricked unit | https://searaboom.goossen.dev/ in Chrome/Edge with the box on **that computer’s** USB | Manual flash (new/bricked units) |
| Firmware release | `./scripts/dev_ota.sh` | From main when releasing |
| USB factory only | `./scripts/snapshot_factory.sh` | Rare |
| PR hardware QA | push the PR; lab webhook runs Grok on the QA fleet | automatic on firmware paths |

`dev_ota.sh` bumps the **patch** digit, builds, and publishes that build as both OTA latest and USB factory. Firmware after `0.1.0` applies any newer X.Y.Z on boot. Do not `idf.py flash` dedicated QA boxes. OTA and USB factory app images must be signed (`docs/SIGNED_FIRMWARE.md`); USB download stays the unbrick path. Signed flash/publish stays blocked until Lucas creates `~/.config/searaboom/signing_key_backed_up` after storing the key.

Factory flash (new boxes): https://searaboom.goossen.dev/  
OTA + logs: https://searaboom.goossen.dev/admin

Backlog: [`docs/IMPROVEMENTS.md`](docs/IMPROVEMENTS.md)
