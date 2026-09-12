# SearaBoom factory desktop flasher

Operators download this program and run it on the computer that has the USB cable.
It flashes with **esptool** (`--before default_reset`, DTR/RTS). It does **not** use
Chrome WebSerial, so you should not need the BOOT button.

Same image as the website: `GET /api/factory` from https://searaboom.goossen.dev/
(layout in `factory_flasher/layout.py`, matching `scripts/hw_restore.sh`).

## Run

```bash
cd factory_flasher
./run.sh
```

That opens http://127.0.0.1:8765/ . Then:

1. Confirm **ARM** (the app asks you to confirm).
2. Plug in a box. It auto-flashes.
3. After write: `ver`, `board`, then **button calibration** (`touch cal` — hold + then −).
4. Unplug. Plug the next box. Stay armed for a batch.

Disarm when you are done. Dedicated QA nodes (`/dev/searaboom-qa-*`) are never flashed.

```bash
./run.sh --demo          # UI only, no USB writes
./run.sh --image-dir ../server/firmware/factory
./run.sh --no-browser
```

## Firmware calibration

USB factory still wipes pad cal (`/spiffs/usb_factory`). The flasher runs `touch cal`,
which stores NVS values at `tsens_rev` 3. Firmware **loads stored cal only when
`rev >= 3`**. Older field cal (rev 1/2) is ignored so OTA keeps the 0.5.20 fixed
threshold. It does **not** auto-start calibration on boot.

## Linux serial access

Once per machine (same as `scripts/setup_host.sh`):

```bash
sudo cp scripts/99-searaboom-esp.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo usermod -aG uucp "$USER"
```

Log out/in after the group change.
