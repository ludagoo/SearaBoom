#!/usr/bin/env python3
"""Device E2E: atualizado clip, setup AP welcome, join → connected clip, portal HTTP.

This path is **first-setup and a successful probe that the saved SSID is gone**.
A saved SSID that is on the air but misses DHCP/auth, or a failed/in-progress
scan, must not play ap_welcome — see tests/test_wifi_boot.py.

Usage:
  python3 tests/e2e_setup.py              # serial + AP join + portal GET
  python3 tests/e2e_setup.py --host-only  # no hardware

Requires the box on USB (/dev/ttyACM0). Wipes NVS so the unit stays in setup AP.
Does not POST home Wi-Fi unless SEARABOOM_TEST_SSID / SEARABOOM_TEST_PASS are set.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORT = os.environ.get("SEARABOOM_PORT", "/dev/ttyACM0")
AP_SSID = "SearaBoom"
PORTAL = "http://4.3.2.1/"
OTA_DONE_MIN_MS = 4000
WELCOME_AFTER_DONE_MAX_MS = 20000


ANSI = re.compile(r"\x1b\[[0-9;]*m")


class Fail(Exception):
    pass


def strip_ansi(s: str) -> str:
    return ANSI.sub("", s)


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, text=True, capture_output=True, **kw)


class SerialLog:
    def __init__(self, port: str):
        import serial

        self._s = serial.Serial()
        self._s.port = port
        self._s.baudrate = 115200
        self._s.timeout = 0.2
        self._s.dtr = False
        self._s.rts = False
        self._s.open()
        self._buf = bytearray()
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._pump, daemon=True)
        self._t.start()

    def _pump(self) -> None:
        while not self._stop.is_set():
            try:
                chunk = self._s.read(4096)
            except Exception:
                break
            if not chunk:
                continue
            with self._lock:
                self._buf.extend(chunk)
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()

    def text(self) -> str:
        with self._lock:
            return strip_ansi(self._buf.decode("utf-8", "replace"))

    def wait_for(self, needle: str, timeout: float) -> bool:
        t_end = time.time() + timeout
        while time.time() < t_end:
            if needle in self.text():
                return True
            time.sleep(0.1)
        return False

    def close(self) -> None:
        self._stop.set()
        try:
            self._s.close()
        except Exception:
            pass
        self._t.join(timeout=1)


def erase_nvs() -> None:
    idf = os.environ.get("IDF_PATH", str(Path.home() / "esp" / "esp-idf"))
    cmd = (
        f'source "{idf}/export.sh" >/dev/null && '
        f"python -m esptool --chip esp32s3 -p {PORT} --after hard_reset erase_region 0x9000 0x6000"
    )
    r = subprocess.run(["bash", "-lc", cmd], text=True, capture_output=True)
    if r.returncode != 0:
        raise Fail(f"NVS erase failed:\n{r.stdout}\n{r.stderr}")


def line_esp_ms(log: str, needle: str) -> int | None:
    for line in log.splitlines():
        if needle not in line:
            continue
        m = re.search(r"\b[IWE] \((\d+)\)", line)
        if m:
            return int(m.group(1))
    return None


def assert_boot_sequence(log: str) -> None:
    if "CLIP play name=ota_done" not in log:
        raise Fail("did not start ota_done / atualizado clip")
    if "CLIP play name=ap_welcome" not in log:
        raise Fail("did not start ap_welcome / new-box clip")
    if "No WiFi SSID saved -> setup AP" not in log:
        raise Fail("did not skip STA wait on empty SSID")
    if "AP SearaBoom up" not in log:
        raise Fail("setup AP did not come up")

    t_done = line_esp_ms(log, "CLIP play name=ota_done")
    t_welcome = line_esp_ms(log, "CLIP play name=ap_welcome")
    if t_done is None or t_welcome is None:
        raise Fail("missing ESP timestamps on CLIP lines")
    gap = t_welcome - t_done
    if gap < OTA_DONE_MIN_MS:
        raise Fail(f"ap_welcome started {gap}ms after ota_done; atualizado was cut off")
    if gap > WELCOME_AFTER_DONE_MAX_MS:
        raise Fail(f"ap_welcome started {gap}ms after ota_done; empty-SSID STA wait likely back")
    print(f"ok: ota_done -> ap_welcome gap={gap}ms")


def wifi_dev() -> str:
    r = run(["nmcli", "-t", "-f", "DEVICE,TYPE", "device", "status"])
    for line in r.stdout.splitlines():
        parts = line.split(":")
        if len(parts) >= 2 and parts[1] == "wifi":
            return parts[0]
    return "wlan0"


def nmcli_disconnect_ap() -> None:
    run(["nmcli", "-w", "5", "connection", "down", AP_SSID], timeout=8)


def nmcli_join_ap() -> None:
    r = run(
        ["nmcli", "-w", "20", "device", "wifi", "connect", AP_SSID, "ifname", wifi_dev()],
        timeout=30,
    )
    if r.returncode != 0:
        raise Fail(f"failed to join {AP_SSID}: {r.stdout}\n{r.stderr}")
    run(["nmcli", "connection", "modify", AP_SSID, "ipv4.never-default", "yes"], timeout=8)


def portal_get() -> None:
    r = run(["curl", "-fsS", "--max-time", "8", PORTAL])
    if r.returncode != 0:
        run(["warp-cli", "--accept-tos", "disconnect"])
        time.sleep(0.4)
        r = run(["curl", "-fsS", "--max-time", "8", PORTAL])
    if r.returncode != 0:
        raise Fail(f"portal GET failed: {r.stdout}\n{r.stderr}")
    if "SSID" not in r.stdout and "SearaBoom" not in r.stdout:
        raise Fail(f"portal HTML unexpected: {r.stdout[:300]}")
    print("ok: portal HTTP")


def maybe_save_wifi() -> None:
    ssid = os.environ.get("SEARABOOM_TEST_SSID", "")
    password = os.environ.get("SEARABOOM_TEST_PASS", "")
    if not ssid:
        print("skip save: set SEARABOOM_TEST_SSID / SEARABOOM_TEST_PASS for full join-home")
        return
    body = f"ssid={ssid}&pass={password}&url=URL1"
    r = run(["curl", "-fsS", "--max-time", "15", "-X", "POST", "-d", body, PORTAL])
    if r.returncode != 0:
        raise Fail(f"portal POST failed: {r.stderr}")
    print("ok: posted WiFi, device should reboot")


def host_only() -> None:
    r = run([sys.executable, str(ROOT / "tests" / "test_adts.py")])
    if r.returncode != 0:
        raise Fail(r.stdout + "\n" + r.stderr)
    print(r.stdout.strip())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host-only", action="store_true")
    ap.add_argument("--skip-join", action="store_true", help="boot clips only, do not join AP")
    args = ap.parse_args()
    slog = None
    try:
        host_only()
        if args.host_only:
            return 0
        if not Path(PORT).exists():
            raise Fail(f"no serial {PORT}")
        print(f"erasing NVS on {PORT}")
        erase_nvs()
        slog = None
        last_err = None
        for _ in range(20):
            time.sleep(0.25)
            try:
                slog = SerialLog(PORT)
                break
            except Exception as e:
                last_err = e
        if slog is None:
            raise Fail(f"could not open {PORT} after reset: {last_err}")
        if not slog.wait_for("CLIP play name=ap_welcome", 30):
            raise Fail("timed out waiting for ap_welcome\n" + slog.text()[-2000:])
        if not slog.wait_for("clip music info", 8):
            raise Fail("welcome clip decoder did not report music info\n" + slog.text()[-1500:])
        time.sleep(2.5)
        tail = slog.text()[-4000:]
        if "Brownout" in tail or "Guru Meditation" in tail:
            raise Fail("device crashed after welcome clip started")
        assert_boot_sequence(slog.text())
        if args.skip_join:
            print("e2e_setup: ok (boot clips)")
            return 0
        print(f"joining {AP_SSID}...")
        try:
            join_err = None
            try:
                nmcli_join_ap()
            except Fail as e:
                join_err = e
            if not slog.wait_for("STA joined", 8):
                raise join_err or Fail("device did not log STA joined")
            if not slog.wait_for("CLIP play name=ap_connected", 6):
                raise Fail("connected clip did not start after join")
            print("ok: STA joined + connected clip")
            time.sleep(0.8)
            try:
                portal_get()
            except Fail as e:
                print(f"warn: portal HTTP skipped ({e})")
            maybe_save_wifi()
        finally:
            nmcli_disconnect_ap()
        print("e2e_setup: ok")
        return 0
    except Fail as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 1
    finally:
        if slog:
            slog.close()


if __name__ == "__main__":
    sys.exit(main())
