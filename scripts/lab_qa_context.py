#!/usr/bin/env python3
"""Write qa-context.json and qa-prompt.md into a QA worktree."""
from __future__ import annotations

import argparse
import json
import os
import subprocess
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--wt", required=True)
    args = ap.parse_args()
    root = Path(args.root)
    wt = Path(args.wt)
    job = os.environ.get("LAB_QA_JOB", "unattended")
    boxes = subprocess.check_output(
        ["python3", str(root / "scripts/qa_boxes.py"), "job", job], text=True
    ).split()
    st = json.loads(
        subprocess.check_output(
            ["python3", str(root / "scripts/qa_boxes.py"), "status"], text=True
        )
    )
    fleet = st.get("boxes") or {}
    wanted = {b: fleet.get(b) or {"id": b, "present": False} for b in boxes}
    ctx = {
        "job": job,
        "pr": os.environ.get("LAB_QA_PR", ""),
        "sha": os.environ.get("LAB_QA_SHA", ""),
        "pr_url": os.environ.get("LAB_QA_PR_URL", ""),
        "paths": [p for p in os.environ.get("LAB_QA_PATHS", "").splitlines() if p],
        "quiet_volume": st.get("quiet_volume", 1),
        "listen_box": st.get("listen_box"),
        "boxes": wanted,
        "lab_root": str(root),
    }
    (wt / "qa-context.json").write_text(json.dumps(ctx, indent=2) + "\n")
    prompt = (root / "scripts/lab_qa_prompt.md").read_text()
    (wt / "qa-prompt.md").write_text(
        prompt
        + "\n\n## qa-context.json\n\n```json\n"
        + json.dumps(ctx, indent=2)
        + "\n```\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
