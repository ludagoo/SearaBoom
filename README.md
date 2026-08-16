# SearaBoom (ESP-ADF)

ESP32-S3 web radio replica of [CrazyGoosse/SearaBoom](https://github.com/CrazyGoosse/SearaBoom), rebuilt on **ESP-ADF** with **HTTPS OTA**.

## Layout

| Path | Purpose |
|------|---------|
| `firmware/` | ESP-ADF firmware (radio + captive portal + OTA) |
| `server/` | OTA management server (Flask) |
| `tunnel/` | Cloudflare Tunnel config for `searaboom.goossen.dev` |
| `scripts/` | Dev helpers |
| `docs/IMPROVEMENTS.md` | Follow-up ideas |

## Behavior (matches original)

- Streams Radio Seara 102.7 / 104.7 (AAC)
- Captive portal AP `SearBoomSetup` at `http://4.3.2.1` with original UI assets
- Volume buttons GPIO7 (up) / GPIO6 (down), range 1–21
- Status LED (WS2812 GPIO21): red=WiFi, yellow=OTA, green=playing, blue=setup
- I2S amp: DOUT=2, BCLK=3, LRC=4

## Extra: OTA + stream proxy

On every successful WiFi connect, the device checks:

`GET https://searaboom.goossen.dev/api/firmware/check?current=X.Y.Z`

Yellow LED blinks during the check/update.

Live AAC is also proxied via the same host (`/stream/102`, `/stream/104`) so ESP-ADF’s AAC decoder can ADTS-sync (upstream `Content-Type: audio/aac` otherwise forces a broken RAW path).

## Build / flash

```bash
export IDF_PATH=~/esp/esp-idf ADF_PATH=~/esp/esp-adf
source "$IDF_PATH/export.sh"
cd firmware
idf.py -p /dev/ttyACM0 build flash
```

Publish a build to OTA:

```bash
./scripts/publish_firmware.sh 1.0.1
```

## Server + tunnel

```bash
./scripts/run_server.sh
./scripts/run_tunnel.sh
```

Management UI: https://searaboom.goossen.dev/
