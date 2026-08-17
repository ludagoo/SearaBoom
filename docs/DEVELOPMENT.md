# SearaBoom development guide

ESP-ADF port of [CrazyGoosse/SearaBoom](https://github.com/CrazyGoosse/SearaBoom) plus HTTPS OTA, stream proxy, and remote logs. Public host: `searaboom.goossen.dev`.

## Layout

| Path | Role |
|------|------|
| `firmware/` | ESP-IDF / ESP-ADF app (`esp32s3`); version in `VERSION` |
| `server/` | OTA UI, AAC proxy, device log hub |
| `tunnel/` | Cloudflare Tunnel (credentials gitignored) |
| `scripts/` | Host setup, OTA publish, serial/log helpers |

## Hardware assumptions

Same pinout as the Arduino reference: I2S 2/3/4, touch vol T7/T6, WS2812 on 21, MAX98357. Volume is digital ALC on the ESP (not the amp gain resistor). USB serial is typically `/dev/ttyACM0`.

## Toolchain

```bash
export IDF_PATH=~/esp/esp-idf ADF_PATH=~/esp/esp-adf
source "$IDF_PATH/export.sh"
./scripts/setup_host.sh   # once: udev + uucp, then log out/in
```

Expect ESP-IDF **5.3.x**. PSRAM must be **Quad** (Octal boot-loops on these modules). Keep ADF audio task stacks in **internal** RAM (`stack_in_ext=false`) or pipeline tasks fail.

## Services on this host

```bash
systemctl --user start searaboom-server searaboom-tunnel
# or: ./scripts/run_server.sh / ./scripts/run_tunnel.sh
```

Admin token default `searaboom-dev` (`SEARABOOM_ADMIN_TOKEN`). Devices stream via `/stream/102` and `/stream/104` on the public host — the proxy exists because ADF mishandles upstream `Content-Type: audio/aac`.

## Flashing and OTA

| Situation | What to do |
|-----------|------------|
| **New / empty unit** | USB: `idf.py -p /dev/ttyACM0 build flash` |
| **Day-to-day firmware** | `./scripts/dev_ota.sh` (bump patch → build → publish → serial `ota`) |
| **Brick / OTA broken** | USB flash again |

Prefer OTA for changes so that path stays tested. Boot OTA only applies when **major/minor** increases; patch bumps need serial `ota` (or `dev_ota.sh`). Bump minor when field units should pick up an update on reboot alone.

```bash
./scripts/dev_ota.sh
./scripts/dev_ota.sh --no-trigger    # publish only
./scripts/serial_cmd.sh ota|ver|help|reboot|logtest
./scripts/watch_serial.sh
./scripts/watch_logs.sh
```

`firmware/VERSION` is the app version (`PROJECT_VER`). Keep `CONFIG_SEARABOOM_FW_VERSION` in sync — `dev_ota.sh` does that when bumping.

## Captive portal

STA connect timeout (~25s) → AP `SearBoomSetup` → setup UI at `http://4.3.2.1`. WiFi/station live in NVS (survives OTA); volume does too.

## Remote logs

Devices POST to the public `/api/logs` after WiFi is up (LAN HTTP to the host often fails here — dual NIC / AP isolation). Watch the management UI or `watch_logs.sh`.

## Pitfalls worth knowing

- Do not raise the ALC ceiling back toward +9 dB — it distorted on this amp + Seara stream; current curve tops at +2 dB.
- Mono AAC is upmixed to stereo before I2S (Arduino did stereo frames).
- Don’t commit `firmware/sdkconfig`, `tunnel/*.json`, build artifacts, or published `.bin` files.

## New machine checklist

1. IDF + ADF paths, `setup_host.sh`, re-login  
2. Start server + tunnel  
3. USB-flash one unit, configure WiFi via captive portal  
4. `./scripts/dev_ota.sh` once to prove OTA  
5. Confirm logs on the management UI  

Backlog: [`IMPROVEMENTS.md`](IMPROVEMENTS.md)
