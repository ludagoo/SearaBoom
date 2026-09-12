# SearaBoom factory flasher

One executable per OS/arch. Factory PCs download that file and run it.

The program does **not** contain firmware. On start and again on **ARM** it
asks the live factory server (`https://searaboom.goossen.dev/api/factory`)
for the current USB image and flashes that. A newer publish is picked up
without rebuilding this binary. No signed flash.

Reset is esptool-style DTR/RTS (USB-JTAG sequence on Espressif CDC). Not
Chrome WebSerial. No BOOT button.

## What a factory person downloads

From https://searaboom.goossen.dev/ — one of:

| PC | File |
|----|------|
| Linux x86_64 | `searaboom-factory-flasher-linux-amd64` |
| Linux arm64 | `searaboom-factory-flasher-linux-arm64` |
| Windows x64 | `searaboom-factory-flasher-windows-amd64.exe` |
| macOS Intel | `searaboom-factory-flasher-darwin-amd64` |
| macOS Apple silicon | `searaboom-factory-flasher-darwin-arm64` |

Direct URLs: `/api/factory/flasher/<id>` (ids in the table above, without the
filename prefix). Catalog: `GET /api/factory/flasher`.

Browser downloads never keep `+x`. After Download on Linux:

```bash
chmod +x searaboom-factory-flasher-linux-amd64
./searaboom-factory-flasher-linux-amd64 --linux-serial   # once: udev text
./searaboom-factory-flasher-linux-amd64                  # opens http://127.0.0.1:8765/
```

Or one line (download, chmod, run):

```bash
# linux-amd64
curl -fsSL -o searaboom-factory-flasher-linux-amd64 https://searaboom.goossen.dev/api/factory/flasher/linux-amd64 && chmod +x searaboom-factory-flasher-linux-amd64 && ./searaboom-factory-flasher-linux-amd64

# linux-arm64
curl -fsSL -o searaboom-factory-flasher-linux-arm64 https://searaboom.goossen.dev/api/factory/flasher/linux-arm64 && chmod +x searaboom-factory-flasher-linux-arm64 && ./searaboom-factory-flasher-linux-arm64
```

Windows: run the `.exe`. macOS: `chmod +x` then run; if Gatekeeper blocks it,
right-click → Open.

## Operator loop

1. **ARM** (checkbox + confirm). ARM re-checks the live server. One box already
   plugged in is flashed; otherwise the next plug-in flashes.
2. Progress, then `ver` → `board` → hold volume **+** then **−** (`touch cal`).
3. **PASS** → unplug → plug the next box. Stay armed for a batch.

Dedicated QA nodes (`/dev/searaboom-qa-*` and `qa-boxes.json`) are skipped.

```bash
./searaboom-factory-flasher-linux-amd64 --demo          # UI only, no USB, no download
./searaboom-factory-flasher-linux-amd64 --image-dir /path/to/factory   # dev override
./searaboom-factory-flasher-linux-amd64 --no-browser
```

`--image-dir` is for developers. Factory PCs should use the default live URL.

## Firmware / cal

USB factory still wipes pad cal (`/spiffs/usb_factory`). The flasher runs
`touch cal`, which stores NVS values at `tsens_rev` 3. Firmware **loads stored
cal only when `rev >= 3`**. Older field cal (rev 1/2) is ignored. Boot does not
auto-calibrate.

## Build the five binaries

```bash
./scripts/build_factory_flasher.sh
```

Writes `factory_flasher/dist/`. The OTA server serves those files from
`/api/factory/flasher/...`. Do not commit the binaries.
