#!/usr/bin/env bash
# On-device cutoff check: PCM start/end markers, then an AAC clip duration.
# Usage: audiotest.sh [--box ID]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOX=""
if [[ "${1:-}" == "--box" ]]; then
  BOX="${2:?}"
  shift 2
fi
if [[ -n "$BOX" ]]; then
  PORT="$(python3 "$ROOT/scripts/qa_boxes.py" path "$BOX")"
else
  PORT="${SEARABOOM_PORT:-/dev/ttyACM0}"
fi
PY="${SEARABOOM_ESPTOOL_PYTHON:-$HOME/.espressif/python_env/idf5.3_py3.14_env/bin/python}"
export PORT
"$PY" - <<'PY'
import os, re, serial, sys, time
port = os.environ["PORT"]
s = serial.Serial(port, 115200, timeout=0.5)
time.sleep(0.15)
s.write(b"audiotest\n")
s.flush()
buf = ""
end = time.time() + 20
while time.time() < end:
    data = s.read(4096)
    if data:
        chunk = data.decode("utf-8", "replace")
        sys.stdout.write(chunk)
        sys.stdout.flush()
        buf += chunk
        if "audiotest done" in buf:
            break
s.close()
pcm = re.search(r"AUDIOTEST pcm start=(\d) end=(\d) dur_ms=(\d+)", buf)
clip = re.search(r"AUDIOTEST clip name=\S+ expected_ms=(\d+) heard_ms=(\d+) ok=(\d)", buf)
ok = True
if not pcm:
    print("FAIL: no AUDIOTEST pcm line", file=sys.stderr)
    ok = False
else:
    start, endm, dur = int(pcm.group(1)), int(pcm.group(2)), int(pcm.group(3))
    if start != 1 or endm != 1 or dur < 800 or dur > 1300:
        print(f"FAIL: pcm markers start={start} end={endm} dur_ms={dur}", file=sys.stderr)
        ok = False
    else:
        print(f"ok pcm start/end dur_ms={dur}")
if not clip:
    print("FAIL: no AUDIOTEST clip line", file=sys.stderr)
    ok = False
else:
    if clip.group(3) != "1":
        print(f"FAIL: clip cutoff expected={clip.group(1)} heard={clip.group(2)}", file=sys.stderr)
        ok = False
    else:
        print(f"ok clip expected={clip.group(1)} heard={clip.group(2)}")
sys.exit(0 if ok else 1)
PY
