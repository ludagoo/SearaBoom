#!/usr/bin/env python3
"""Origin check-run timestamps must be Z-normalized RFC 3339."""
from __future__ import annotations

import re
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server"))

from lab_origin import iso_now, post_check, rfc3339  # noqa: E402
from lab_qa import deadline  # noqa: E402

# protojson Timestamp: Z-normalized, 0 / 3 / 6 / 9 fractional digits
RFC3339_Z = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d{3}|\.\d{6}|\.\d{9})?Z$")


def _assert_rfc3339_z(value: str) -> None:
    assert isinstance(value, str), value
    assert "+00:00" not in value, value
    assert RFC3339_Z.match(value), value
    datetime.fromisoformat(value.replace("Z", "+00:00"))


def test_rfc3339_z_normalized() -> None:
    _assert_rfc3339_z(iso_now())
    _assert_rfc3339_z(rfc3339())
    dt = datetime(2026, 9, 12, 0, 19, 42, 570363, tzinfo=timezone.utc)
    assert rfc3339(dt) == "2026-09-12T00:19:42.570363Z"
    naive = datetime(2026, 9, 12, 0, 19, 42, 0)
    assert rfc3339(naive) == "2026-09-12T00:19:42.000000Z"
    _assert_rfc3339_z(deadline(45))
    soon = datetime.fromisoformat(deadline(45).replace("Z", "+00:00"))
    delta = soon - datetime.now(timezone.utc)
    assert timedelta(minutes=44) < delta < timedelta(minutes=46)


def test_post_check_body_timestamps() -> None:
    bodies: list[dict] = []

    def fake_post(path: str, body: dict) -> dict:
        assert path.endswith("/check-runs")
        bodies.append(body)
        return {"ok": True}

    with patch("lab_origin.origin_post", fake_post):
        post_check(
            head_sha="abc1234deadbeef",
            check_key="hw-test-zero",
            status="in_progress",
            title="QA running",
            summary="start",
            external_id="ext-1",
            deadline_at=deadline(45),
        )
        post_check(
            head_sha="abc1234deadbeef",
            check_key="hw-test-zero",
            status="completed",
            conclusion="success",
            title="QA finished",
            summary="done",
            external_id="ext-2",
        )

    assert len(bodies) == 2
    running = bodies[0]["checkRun"]
    done = bodies[1]["checkRun"]
    for key in ("externalUpdatedAt", "startedAt", "deadlineAt"):
        _assert_rfc3339_z(running[key])
    _assert_rfc3339_z(done["externalUpdatedAt"])
    _assert_rfc3339_z(done["completedAt"])
    assert "startedAt" not in done


if __name__ == "__main__":
    test_rfc3339_z_normalized()
    test_post_check_body_timestamps()
    print("ok")
