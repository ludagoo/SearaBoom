"""QA webhook handler: classify PR events and launch the lab runner."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path

from lab_origin import configured, get_pull, iso_now, list_pull_files, post_check

_SCRIPTS = str(Path(__file__).resolve().parent.parent / "scripts")
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)
from lab_parse import needs_firmware_qa, parse_hw_comment  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
RUNNER = REPO / "scripts" / "lab_qa_run.sh"
LOG_DIR = REPO / "server" / "logs" / "lab-qa"
STATUS_PATH = LOG_DIR / "last.json"
PUBLIC_URL = os.environ.get("SEARABOOM_PUBLIC_URL", "https://searaboom.goossen.dev").rstrip("/")

_seen_deliveries: dict[str, float] = {}
_seen_lock = threading.Lock()


def check_key_for(job: str, box_id: str | None = None) -> str:
    if job == "listen":
        return "hw-listen"
    if job == "hands":
        return "hw-hands"
    if job == "soak":
        if box_id and "supermini" in box_id:
            return "hw-soak-supermini"
        return "hw-soak-zero"
    if box_id and "supermini" in box_id:
        return "hw-test-supermini"
    return "hw-test-zero"


def _pr_number(payload: dict) -> str | None:
    pr = payload.get("pullRequest") or {}
    n = pr.get("number") or pr.get("id")
    if n is None:
        ref = payload.get("pullRequest")
        if isinstance(ref, dict):
            n = ref.get("number")
    if n is None:
        return None
    return str(n)


def _comment_body(payload: dict) -> str:
    c = payload.get("comment") or {}
    return c.get("body") or c.get("content") or c.get("text") or ""


def _head_sha(pr: dict) -> str:
    for path in (
        ("head", "sha"),
        ("headSha",),
        ("head", "oid"),
        ("latestVersion", "headSha"),
        ("headCommit", "sha"),
    ):
        cur = pr
        ok = True
        for k in path:
            if not isinstance(cur, dict) or k not in cur:
                ok = False
                break
            cur = cur[k]
        if ok and isinstance(cur, str) and len(cur) >= 7:
            return cur
    return ""


def seen_delivery(delivery_id: str) -> bool:
    if not delivery_id:
        return False
    now = time.time()
    with _seen_lock:
        stale = [k for k, t in _seen_deliveries.items() if now - t > 3600]
        for k in stale:
            _seen_deliveries.pop(k, None)
        if delivery_id in _seen_deliveries:
            return True
        _seen_deliveries[delivery_id] = now
    return False


def write_status(update: dict) -> None:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    prev = {}
    if STATUS_PATH.is_file():
        try:
            prev = json.loads(STATUS_PATH.read_text())
        except json.JSONDecodeError:
            prev = {}
    prev.update(update)
    prev["updated_at"] = iso_now()
    STATUS_PATH.write_text(json.dumps(prev, indent=2) + "\n")


def details_url() -> str:
    return f"{PUBLIC_URL}/api/lab/status"


def deadline(minutes: int = 45) -> str:
    return (datetime.now(timezone.utc) + timedelta(minutes=minutes)).isoformat()


def launch_runner(*, job: str, pr: str, sha: str, pr_url: str, paths: list[str]) -> None:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log = LOG_DIR / f"pr-{pr}-{sha[:8]}-{job}.log"
    env = os.environ.copy()
    env["LAB_QA_JOB"] = job
    env["LAB_QA_PR"] = str(pr)
    env["LAB_QA_SHA"] = sha
    env["LAB_QA_PR_URL"] = pr_url
    env["LAB_QA_PATHS"] = "\n".join(paths)
    env["LAB_QA_DETAILS_URL"] = details_url()
    with log.open("ab") as fh:
        fh.write(f"\n--- launch {iso_now()} job={job} pr={pr} sha={sha} ---\n".encode())
        subprocess.Popen(
            [str(RUNNER)],
            cwd=str(REPO),
            env=env,
            stdout=fh,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
    write_status({
        "running": True,
        "job": job,
        "pr": pr,
        "sha": sha,
        "log": str(log),
    })


def handle_origin_event(envelope: dict) -> dict:
    event = envelope.get("event") or envelope
    etype = event.get("type") or envelope.get("type") or ""
    payload = event.get("payload") or event
    if etype in ("pull_request.merged", "pull_request.closed") or etype.startswith("repository."):
        return {"ok": True, "ignored": etype}

    job = None
    if etype == "pull_request.comment.created":
        job = parse_hw_comment(_comment_body(payload))
        if not job:
            return {"ok": True, "ignored": "comment"}
    elif etype in (
        "pull_request.created",
        "pull_request.reopened",
        "pull_request.head_ref.pushed",
        "pull_request.published",
    ):
        job = "unattended"
    else:
        return {"ok": True, "ignored": etype or "unknown"}

    number = _pr_number(payload)
    if not number:
        return {"ok": False, "error": "no pull request number"}

    if not configured():
        return {"ok": False, "error": "Origin app not configured on this host"}

    pr = get_pull(number)
    sha = _head_sha(pr) or _head_sha(payload.get("pullRequest") or {})
    if not sha:
        return {"ok": False, "error": "no head sha"}
    paths = list_pull_files(number)
    pr_url = (
        pr.get("url")
        or pr.get("htmlUrl")
        or f"https://cursor.com/codebase/{os.environ.get('ORIGIN_OWNER', 'goossen')}/{os.environ.get('ORIGIN_REPO', 'searaboom')}/pulls/{number}"
    )

    if job == "unattended" and etype != "pull_request.comment.created" and not needs_firmware_qa(paths):
        for key in ("hw-test-zero", "hw-test-supermini"):
            post_check(
                head_sha=sha,
                check_key=key,
                status="completed",
                conclusion="skipped",
                title="No firmware changes",
                summary="Docs/server-only PR. Hardware QA skipped. Comment `/hw-test` to force.",
                text="\n".join(paths) or "(no files listed)",
                external_id=str(uuid.uuid4()),
                details_url=details_url(),
            )
        write_status({"running": False, "job": "skipped", "pr": number, "sha": sha})
        return {"ok": True, "skipped": True, "pr": number}

    keys = []
    if job == "unattended":
        keys = ["hw-test-zero", "hw-test-supermini"]
    elif job == "soak":
        keys = ["hw-soak-zero", "hw-soak-supermini"]
    elif job == "listen":
        keys = ["hw-listen"]
    elif job == "hands":
        keys = ["hw-hands"]

    for key in keys:
        post_check(
            head_sha=sha,
            check_key=key,
            status="in_progress",
            title=f"QA {job} running",
            summary=f"Lab QA agent started for PR {number} (`{job}`).",
            text="",
            external_id=str(uuid.uuid4()),
            details_url=details_url(),
            deadline_at=deadline(45),
        )

    launch_runner(job=job, pr=number, sha=sha, pr_url=pr_url, paths=paths)
    return {"ok": True, "queued": job, "pr": number, "sha": sha}


def flask_status() -> dict:
    scripts = str(REPO / "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    from qa_boxes import status as box_status
    last = {}
    if STATUS_PATH.is_file():
        try:
            last = json.loads(STATUS_PATH.read_text())
        except json.JSONDecodeError:
            last = {}
    return {
        "ok": True,
        "origin_configured": configured(),
        "last": last,
        "fleet": box_status(),
    }
