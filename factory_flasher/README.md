# SearaBoom factory flasher

One executable per OS/arch. Factory PCs install it with a **curl one-liner**
(not a browser download — browsers drop `+x` on Linux).

The program does **not** contain firmware. On start and again on **ARM** it
asks the live factory server (`https://searaboom.goossen.dev/api/factory`)
for the current USB image and flashes that. A newer publish is picked up
without rebuilding this binary. No signed flash.

Run the binary and a **terminal UI** opens (same TUI on Linux, Windows, macOS).
No localhost server. No browser. Double-click may open a console window.

Reset is esptool-style DTR/RTS (USB-JTAG sequence on Espressif CDC). Not
Chrome WebSerial. No BOOT button.

## Install (curl is the path)

```bash
# Linux x86_64
curl -fsSL -o searaboom-factory-flasher-linux-amd64 https://searaboom.goossen.dev/api/factory/flasher/linux-amd64 && chmod +x searaboom-factory-flasher-linux-amd64 && ./searaboom-factory-flasher-linux-amd64

# Linux arm64
curl -fsSL -o searaboom-factory-flasher-linux-arm64 https://searaboom.goossen.dev/api/factory/flasher/linux-arm64 && chmod +x searaboom-factory-flasher-linux-arm64 && ./searaboom-factory-flasher-linux-arm64
```

```bat
:: Windows x64 (cmd / PowerShell)
curl.exe -fsSL -o searaboom-factory-flasher-windows-amd64.exe https://searaboom.goossen.dev/api/factory/flasher/windows-amd64 && .\searaboom-factory-flasher-windows-amd64.exe
```

```bash
# macOS Apple silicon
curl -fsSL -o searaboom-factory-flasher-darwin-arm64 https://searaboom.goossen.dev/api/factory/flasher/darwin-arm64 && chmod +x searaboom-factory-flasher-darwin-arm64 && ./searaboom-factory-flasher-darwin-arm64

# macOS Intel
curl -fsSL -o searaboom-factory-flasher-darwin-amd64 https://searaboom.goossen.dev/api/factory/flasher/darwin-amd64 && chmod +x searaboom-factory-flasher-darwin-amd64 && ./searaboom-factory-flasher-darwin-amd64
```

Linux USB once: `./searaboom-factory-flasher-linux-amd64 --linux-serial`
(prints udev text) or `--install-linux-serial` (sudo).

Catalog: `GET /api/factory/flasher`. Raw files exist for builders; factory
people should paste a one-liner above.

## Operator loop

1. **F** flashes the box that is plugged in now (does not ARM; later plugs stay idle).
2. **Space** ARMs a batch — while armed, any ESP32-S3 plugged in is flashed.
3. Big screen: ARM → PLUG → FLASH → PASS / FAIL. Hold volume **+** then **−** when asked.

Dedicated QA nodes (`/dev/searaboom-qa-*` and `qa-boxes.json`) are skipped.

```bash
./searaboom-factory-flasher-linux-amd64 --demo          # TUI only, no USB, no download
./searaboom-factory-flasher-linux-amd64 --image-dir /path/to/factory   # dev override
```

`--image-dir` is for developers. Factory PCs should use the default live URL.

## Firmware / cal

USB factory still wipes pad cal (`/spiffs/usb_factory`). The flasher runs
`touch cal`, which stores NVS values at `tsens_rev` 3 (snapshotted in
`tsens_f_*`). Firmware **loads factory cal when `rev >= 3`**. Rev 4 is field
auto-cal from real presses. Older field cal (rev 1/2) is ignored. Uncalibrated
live start is `channel_sens` **0.50**.

## Build the five binaries

```bash
./scripts/build_factory_flasher.sh
```

Writes `factory_flasher/dist/`. The OTA server serves those files from
`/api/factory/flasher/...`. Do not commit the binaries.
