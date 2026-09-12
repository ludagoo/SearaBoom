"""Arm → detect → flash → confirm state machine. No hardware in unit tests."""
from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from factory_flasher.core import (
    UsbPort,
    cal_prompt_from_line,
    eligible_ports,
    flash_port,
    list_usb_ports,
    new_ports,
    parse_board_line,
    parse_progress_line,
    parse_version_line,
    serial_command,
    wait_for_port,
)

Phase = str  # idle|watching|detected|flashing|verify|calibrate|pass|fail


@dataclass
class ConfirmStep:
    id: str
    title: str
    detail: str = ""
    status: str = "pending"  # pending|active|pass|fail


@dataclass
class SessionState:
    armed: bool = False
    phase: Phase = "idle"
    port: str = ""
    identity: str = ""
    progress: int = 0
    message: str = "Disarmed. Arm when you are ready to flash."
    image_dir: str = ""
    image_version: str = ""
    image_ready: bool = False
    log: list[str] = field(default_factory=list)
    confirm: list[ConfirmStep] = field(default_factory=list)
    boxes_done: int = 0
    last_error: str = ""
    cal_prompt: str = ""

    def to_dict(self) -> dict:
        return {
            "armed": self.armed,
            "phase": self.phase,
            "port": self.port,
            "identity": self.identity,
            "progress": self.progress,
            "message": self.message,
            "image_dir": self.image_dir,
            "image_version": self.image_version,
            "image_ready": self.image_ready,
            "log": list(self.log[-80:]),
            "confirm": [
                {
                    "id": s.id,
                    "title": s.title,
                    "detail": s.detail,
                    "status": s.status,
                }
                for s in self.confirm
            ],
            "boxes_done": self.boxes_done,
            "last_error": self.last_error,
            "cal_prompt": self.cal_prompt,
        }


def default_confirm() -> list[ConfirmStep]:
    return [
        ConfirmStep("ver", "Firmware version"),
        ConfirmStep("board", "Board detect"),
        ConfirmStep("cal", "Button calibration (hold + then −)"),
    ]


def CAL_DONE_OK(text: str) -> bool:
    return cal_prompt_from_line(text) == "done" or "touch cal ESP_OK" in (text or "")


class FactorySession:
    def __init__(
        self,
        *,
        image_dir: Path,
        image_version: str = "",
        list_ports: Callable[[], list[UsbPort]] | None = None,
        flash: Callable[..., int] | None = None,
        serial_cmd: Callable[..., str] | None = None,
        wait_port: Callable[[str, float], bool] | None = None,
        qa_paths: list[str] | None = None,
        sleep: Callable[[float], None] = time.sleep,
        now: Callable[[], float] = time.time,
    ) -> None:
        self._image_dir = Path(image_dir)
        self._list_ports = list_ports or list_usb_ports
        self._flash = flash
        self._serial_cmd = serial_cmd
        self._wait_port = wait_port or wait_for_port
        self._qa_paths = qa_paths
        self._sleep = sleep
        self._now = now
        self._lock = threading.Lock()
        self._state = SessionState(
            image_dir=str(self._image_dir),
            image_version=image_version,
            image_ready=True,
            confirm=default_confirm(),
        )
        self._baseline: set[str] = set()
        self._done_ids: set[str] = set()
        self._busy = False
        self._busy_id = ""
        self._stop = False
        self._cal_retry = threading.Event()

    def snapshot(self) -> dict:
        with self._lock:
            return self._state.to_dict()

    def _log(self, line: str) -> None:
        text = (line or "").rstrip()
        if not text:
            return
        self._state.log.append(text)
        if len(self._state.log) > 400:
            self._state.log = self._state.log[-300:]

    def _set(self, **kwargs) -> None:
        for key, value in kwargs.items():
            setattr(self._state, key, value)

    def arm(self, armed: bool) -> dict:
        with self._lock:
            if armed and not self._state.image_ready:
                self._state.message = "No factory image. Cannot arm."
                return self._state.to_dict()
            self._state.armed = bool(armed)
            if armed:
                ports = eligible_ports(self._list_ports(), qa_paths=self._qa_paths)
                self._baseline = {p.identity for p in ports}
                already = [p for p in ports if p.identity not in self._done_ids]
                self._state.phase = "watching"
                self._state.last_error = ""
                if len(already) == 1:
                    # Plug-then-arm: flash the one eligible box that is already in.
                    self._baseline = {p.identity for p in ports if p.identity != already[0].identity}
                    self._state.message = (
                        f"Armed. Flashing the box on {already[0].device}."
                    )
                elif len(already) > 1:
                    self._state.message = (
                        "Armed. Unplug extra boxes, then plug one at a time."
                    )
                else:
                    self._state.message = "Armed. Plug in a SearaBoom to flash it."
            else:
                self._state.phase = "idle" if self._state.phase not in ("flashing", "calibrate") else self._state.phase
                if self._state.phase not in ("flashing", "calibrate"):
                    self._state.phase = "idle"
                    self._state.port = ""
                    self._state.identity = ""
                    self._state.progress = 0
                    self._state.cal_prompt = ""
                    self._state.confirm = default_confirm()
                self._state.message = "Disarmed. Arm when you are ready to flash."
            return self._state.to_dict()

    def request_cal_retry(self) -> dict:
        self._cal_retry.set()
        with self._lock:
            self._log("operator requested calibration retry")
            return self._state.to_dict()

    def stop(self) -> None:
        self._stop = True

    def tick(self) -> None:
        with self._lock:
            if self._busy or not self._state.armed:
                return
            if self._state.phase in ("flashing", "verify", "calibrate"):
                return
            ports = eligible_ports(self._list_ports(), qa_paths=self._qa_paths)
            fresh = [
                p
                for p in new_ports(ports, self._baseline)
                if p.identity not in self._done_ids and p.identity != self._busy_id
            ]
            if not fresh:
                if self._state.phase in ("pass", "fail"):
                    still = {p.identity for p in ports}
                    if self._state.identity and self._state.identity not in still:
                        self._baseline.discard(self._state.identity)
                        self._set(
                            phase="watching",
                            port="",
                            identity="",
                            progress=0,
                            cal_prompt="",
                            confirm=default_confirm(),
                            message="Armed. Plug in the next box.",
                        )
                return
            target = fresh[0]
            self._busy = True
            self._busy_id = target.identity
            self._set(
                phase="detected",
                port=target.device,
                identity=target.identity,
                progress=0,
                last_error="",
                confirm=default_confirm(),
                cal_prompt="",
                message=f"Detected {target.device}. Starting flash.",
            )
            self._log(f"detected {target.device} id={target.identity}")
            job = dict(device=target.device, identity=target.identity)
        self._run_box(job["device"], job["identity"])

    def _run_box(self, device: str, identity: str) -> None:
        try:
            self._flash_and_confirm(device, identity)
        except Exception as exc:
            with self._lock:
                self._log(f"error: {exc}")
                self._set(
                    phase="fail",
                    last_error=str(exc),
                    message=f"Fail: {exc}",
                    progress=0,
                )
        finally:
            with self._lock:
                self._busy = False
                self._busy_id = ""
                self._baseline.add(identity)
                if self._state.phase == "pass":
                    self._done_ids.add(identity)

    def _on_flash_line(self, line: str) -> None:
        with self._lock:
            self._log(line)
            pct = parse_progress_line(line)
            if pct is not None:
                self._state.progress = pct
                self._state.message = f"Flashing… {pct}%"

    def _flash_and_confirm(self, device: str, identity: str) -> None:
        with self._lock:
            self._set(phase="flashing", progress=0, message="Flashing…")
        if self._flash is not None:
            rc = int(self._flash(device, self._image_dir, self._on_flash_line))
        else:
            rc = flash_port(device, self._image_dir, on_line=self._on_flash_line)
        if rc != 0:
            with self._lock:
                self._set(
                    phase="fail",
                    last_error=f"esptool exited {rc}",
                    message="Flash failed. Unplug, check the USB cable, arm, try again.",
                )
            return
        with self._lock:
            self._set(phase="verify", progress=100, message="Flash wrote. Checking boot…")
            self._log("esptool ok — waiting for serial")
        self._sleep(2.0)
        if not self._wait_port(device, 12.0):
            with self._lock:
                self._set(
                    phase="fail",
                    last_error="serial did not return after reset",
                    message="Flash wrote but the USB serial port did not come back.",
                )
            return
        self._verify(device)
        if self._state.phase == "fail":
            return
        self._calibrate(device)
        with self._lock:
            if self._state.phase == "fail":
                return
            ok = all(s.status == "pass" for s in self._state.confirm)
            if ok:
                self._state.boxes_done += 1
                self._set(
                    phase="pass",
                    message="PASS. Unplug this box, then plug the next one.",
                )
            else:
                self._set(
                    phase="fail",
                    message="Confirmation failed. See steps below.",
                )

    def _serial(self, port: str, command: str, wait_s: float) -> str:
        lines: list[str] = []

        def on_line(line: str) -> None:
            lines.append(line)
            with self._lock:
                self._log(line)
                prompt = cal_prompt_from_line(line)
                if prompt:
                    self._state.cal_prompt = prompt

        if self._serial_cmd is not None:
            text = self._serial_cmd(port, command, wait_s)
            for line in text.splitlines():
                on_line(line)
            return text
        return serial_command(port, command, wait_s, on_line=on_line)

    def _mark(self, step_id: str, status: str, detail: str) -> None:
        for step in self._state.confirm:
            if step.id == step_id:
                step.status = status
                step.detail = detail
                return

    def _verify(self, device: str) -> None:
        with self._lock:
            self._mark("ver", "active", "sending ver")
            expect = self._state.image_version
        text = self._serial(device, "ver", 2.0)
        version = None
        board = None
        for line in text.splitlines():
            version = version or parse_version_line(line)
            board = board or parse_board_line(line)
        with self._lock:
            if version:
                match = (not expect) or version == expect
                self._mark(
                    "ver",
                    "pass" if match else "fail",
                    f"{version}" + ("" if match else f" (wanted {expect})"),
                )
                if not match:
                    self._set(phase="fail", last_error="version mismatch", message="Version mismatch after flash.")
                    return
            else:
                self._mark("ver", "fail", "no version= line")
                self._set(phase="fail", last_error="no ver", message="Box did not answer `ver`.")
                return
        board_text = self._serial(device, "board", 1.5)
        for line in board_text.splitlines():
            board = board or parse_board_line(line)
        with self._lock:
            self._mark("board", "active", "sending board")
            if board:
                self._mark("board", "pass", board)
                self._set(message=f"Booted {board} {version}. Starting pad calibration.")
            else:
                self._mark("board", "fail", "no board= line")
                self._set(phase="fail", last_error="no board", message="Box did not answer `board`.")

    def _calibrate(self, device: str) -> None:
        while not self._stop:
            with self._lock:
                if not self._state.armed:
                    self._set(phase="fail", message="Disarmed during calibration.")
                    return
                self._set(
                    phase="calibrate",
                    cal_prompt="hands-off",
                    message="Calibration: hands off both pads, then hold + , then −.",
                )
                self._mark("cal", "active", "hands off, then hold + then −")
                self._cal_retry.clear()
            text = self._serial(device, "touch cal", 50.0)
            prompt = None
            for line in text.splitlines():
                prompt = cal_prompt_from_line(line) or prompt
            with self._lock:
                if prompt == "done" or CAL_DONE_OK(text):
                    self._mark("cal", "pass", "touch cal ESP_OK")
                    self._state.cal_prompt = "done"
                    return
                self._mark("cal", "fail", "touch cal failed — retry or check pads")
                self._state.cal_prompt = "fail"
                self._state.message = "Calibration failed. Fix the pads and retry, or disarm."
            # wait for retry or unplug / disarm
            deadline = self._now() + 120.0
            while not self._stop and self._now() < deadline:
                if self._cal_retry.is_set():
                    break
                with self._lock:
                    if not self._state.armed:
                        self._set(phase="fail", message="Disarmed during calibration.")
                        return
                self._sleep(0.2)
            else:
                with self._lock:
                    self._set(phase="fail", last_error="cal timeout", message="Calibration failed.")
                return
            continue
