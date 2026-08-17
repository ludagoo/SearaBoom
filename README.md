# SearaBoom (ESP-ADF)

ESP32-S3 web radio (ESP-ADF) with captive WiFi setup, AAC streaming, OTA, and remote logs.

**Developer guide:** [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md)

## Layout

| Path | Purpose |
|------|---------|
| `firmware/` | Device firmware |
| `server/` | OTA, stream proxy, log hub |
| `tunnel/` | Cloudflare → `searaboom.goossen.dev` |
| `scripts/` | Setup / OTA / serial helpers |
| `docs/` | Dev guide + backlog |

## Quick start

```bash
export IDF_PATH=~/esp/esp-idf ADF_PATH=~/esp/esp-adf
source "$IDF_PATH/export.sh"
./scripts/setup_host.sh          # once

systemctl --user start searaboom-server searaboom-tunnel

cd firmware && idf.py -p /dev/ttyACM0 build flash   # new unit
./scripts/dev_ota.sh                                  # thereafter
```

Management UI: https://searaboom.goossen.dev/
