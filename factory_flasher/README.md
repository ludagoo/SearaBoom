# SearaBoom factory desktop flasher

Operators download this program and run it on the computer that has the USB cable.
It flashes with **esptool** (`--before default_reset`, DTR/RTS). It does **not** use
Chrome WebSerial, so you should not need the BOOT button.

Same image as the website: `GET /api/factory` from https://searaboom.goossen.dev/
(layout in `factory_flasher/layout.py`, matching `scripts/hw_restore.sh`).

The website zip (`/api/factory/desktop.zip`) exists only **after this PR is deployed
on the OTA server**. Until then, run from the git branch (below) or write a zip
locally with `python3 -m factory_flasher --write-zip`.

## Run

From a git checkout of this repo (PR branch or `main` after merge):

```bash
cd factory_flasher
./run.sh
```

That opens http://127.0.0.1:8765/ . Then:

1. Confirm **ARM** (the app asks you to confirm).
2. Plug in a box. It auto-flashes. One box already plugged in is flashed on ARM.
3. After write: `ver`, `board`, then **button calibration** (`touch cal` — hold + then −).
4. Unplug. Plug the next box. Stay armed for a batch.

Disarm when you are done. Dedicated QA nodes (`/dev/searaboom-qa-*`) are never flashed.

```bash
./run.sh --demo          # UI only, no USB writes
./run.sh --image-dir ../server/firmware/factory
./run.sh --no-browser
python3 -m factory_flasher --write-zip   # operator zip; does not flash
```

## Firmware calibration

USB factory still wipes pad cal (`/spiffs/usb_factory`). The flasher runs `touch cal`,
which stores NVS values at `tsens_rev` 3. Firmware **loads stored cal only when
`rev >= 3`**. Older field cal (rev 1/2) is ignored so OTA keeps the 0.5.20 fixed
threshold. It does **not** auto-start calibration on boot.

Pad cal only sticks after this firmware is in the USB factory image on the OTA
server. The live slot today is 0.5.20, which still ignores stored cal.

## Linux serial access

Once per factory PC:

```bash
cd factory_flasher
./install-serial-linux.sh
```

That copies `99-searaboom-esp.rules` (same as `scripts/99-searaboom-esp.rules`)
and adds you to `uucp` or `dialout`. Log out/in after the group change, then
unplug/replug the box.

Needs Python 3 with `venv` (`python3 -m venv`). `./run.sh` creates `.venv` and
installs `esptool` + `pyserial`.
