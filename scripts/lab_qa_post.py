#!/usr/bin/env python3
"""Post Origin check runs from qa-result.json (or a launcher failure)."""
from __future__ import annotations

import argparse
import json
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server"))
from lab_origin import post_check  # noqa: E402
from lab_qa import write_status  # noqa: E402


def _post(sha: str, key: str, conclusion: str, title: str, summary: str, text: str, details: str) -> None:
    post_check(
        head_sha=sha,
        check_key=key,
        status="completed",
        conclusion=conclusion,
        title=title[:255],
        summary=summary,
        text=text,
        external_id=str(uuid.uuid4()),
        details_url=details,
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--job", required=True)
    ap.add_argument("--sha", required=True)
    ap.add_argument("--pr", required=True)
    ap.add_argument("--details", default="")
    ap.add_argument("--result")
    ap.add_argument("--failed")
    args = ap.parse_args()
    job = args.job
    sha = args.sha
    details = args.details

    if args.failed:
        keys = {
            "unattended": ["hw-test-zero", "hw-test-supermini"],
            "soak": ["hw-soak-zero", "hw-soak-supermini"],
            "listen": ["hw-listen"],
            "hands": ["hw-hands"],
        }.get(job, ["hw-test-zero"])
        for key in keys:
            _post(sha, key, "failure", "Lab QA failed to start", args.failed, args.failed, details)
        write_status({"running": False, "job": job, "pr": args.pr, "sha": sha, "error": args.failed})
        return 1

    data = json.loads(Path(args.result).read_text())
    summary = data.get("summary") or data.get("title") or "QA finished"
    title = data.get("title") or "QA finished"
    text = data.get("serial_highlights") or json.dumps(data, indent=2)
    gated = data.get("gated") or []
    carriers = data.get("carriers") or {}

    if job == "unattended":
        for box, key in (("zero-fast", "hw-test-zero"), ("supermini-fast", "hw-test-supermini")):
            rec = carriers.get(box) or {}
            ok = bool(rec.get("pass", data.get("pass")))
            detail = rec.get("detail") or summary
            _post(sha, key, "success" if ok else "failure", title, detail, text, details)
        listen_items = [g for g in gated if g.get("kind") == "listen"]
        hands_items = [g for g in gated if g.get("kind") == "hands"]
        if listen_items:
            body = "\n".join(
                f"- **{g.get('box')}**: {g.get('instruction')} (expect {g.get('expect')})"
                for g in listen_items
            )
            _post(
                sha, "hw-listen", "action_required",
                "Needs /hw-test listen",
                "Quiet QA listed listen steps. Comment `/hw-test listen` when the room is OK.\n\n" + body,
                body, details,
            )
        if hands_items:
            body = "\n".join(
                f"- **{g.get('box')}**: {g.get('instruction')} (expect {g.get('expect')})"
                for g in hands_items
            )
            _post(
                sha, "hw-hands", "action_required",
                "Needs /hw-test hands",
                "Quiet QA listed human steps. Comment `/hw-test hands` when you are at the boxes.\n\n" + body,
                body, details,
            )
    elif job == "soak":
        for box, key in (("zero-soak", "hw-soak-zero"), ("supermini-soak", "hw-soak-supermini")):
            rec = carriers.get(box) or {}
            ok = bool(rec.get("pass", data.get("pass")))
            _post(sha, key, "success" if ok else "failure", title, rec.get("detail") or summary, text, details)
    elif job == "listen":
        ok = bool(data.get("pass"))
        _post(sha, "hw-listen", "success" if ok else "failure", title, summary, text, details)
    elif job == "hands":
        ok = bool(data.get("pass"))
        _post(sha, "hw-hands", "success" if ok else "failure", title, summary, text, details)

    write_status({"running": False, "job": job, "pr": args.pr, "sha": sha, "pass": data.get("pass")})
    return 0 if data.get("pass") else 1


if __name__ == "__main__":
    raise SystemExit(main())
