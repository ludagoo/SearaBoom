#!/usr/bin/env bash
# Launch the lab QA agent for one job. Called from the Origin webhook worker.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JOB="${LAB_QA_JOB:?LAB_QA_JOB}"
PR="${LAB_QA_PR:?LAB_QA_PR}"
SHA="${LAB_QA_SHA:?LAB_QA_SHA}"
PR_URL="${LAB_QA_PR_URL:-}"
PATHS="${LAB_QA_PATHS:-}"
DETAILS="${LAB_QA_DETAILS_URL:-https://searaboom.goossen.dev/api/lab/status}"
WT="/tmp/searaboom-qa/${SHA:0:12}"
RESULT=""
PY="$ROOT/server/.venv/bin/python"
export SEARABOOM_LAB_ROOT="$ROOT"
mkdir -p "$ROOT/server/logs/lab-qa"

mapfile -t BOXES < <(python3 "$ROOT/scripts/qa_boxes.py" job "$JOB")

cleanup() {
  local box
  if [[ "$JOB" != "soak" ]]; then
    for box in "${BOXES[@]+"${BOXES[@]}"}"; do
      if ! restore_out="$("$ROOT/scripts/hw_restore.sh" --box "$box" 2>&1)"; then
        echo "hw_restore.sh --box $box failed (box may still be on PR firmware): $restore_out" >&2
      fi
    done
  fi
  if [[ -n "$RESULT" && -f "$RESULT" ]]; then
    "$PY" "$ROOT/scripts/lab_qa_post.py" \
      --job "$JOB" --sha "$SHA" --pr "$PR" --details "$DETAILS" --result "$RESULT" || true
  else
    "$PY" "$ROOT/scripts/lab_qa_post.py" \
      --job "$JOB" --sha "$SHA" --pr "$PR" --details "$DETAILS" \
      --failed "QA agent did not write qa-result.json" || true
  fi
  git -C "$ROOT" worktree remove --force "$WT" >/dev/null 2>&1 || rm -rf "$WT"
}
trap cleanup EXIT

fd=8
for box in "${BOXES[@]+"${BOXES[@]}"}"; do
  lock="$(python3 "$ROOT/scripts/qa_boxes.py" lock "$box")"
  eval "exec ${fd}>\"$lock\""
  if ! flock -n "$fd"; then
    trap - EXIT
    "$PY" "$ROOT/scripts/lab_qa_post.py" \
      --job "$JOB" --sha "$SHA" --pr "$PR" --details "$DETAILS" \
      --failed "QA box $box busy"
    exit 4
  fi
  fd=$((fd + 1))
done

git -C "$ROOT" fetch --quiet origin "$SHA" 2>/dev/null \
  || git -C "$ROOT" fetch --quiet origin
rm -rf "$WT"
git -C "$ROOT" worktree add --detach "$WT" "$SHA"

export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
export ADF_PATH="${ADF_PATH:-$HOME/esp/esp-adf}"
export LAB_QA_JOB LAB_QA_PR LAB_QA_SHA LAB_QA_PR_URL LAB_QA_PATHS

python3 "$ROOT/scripts/lab_qa_context.py" --root "$ROOT" --wt "$WT"

set +e
grok --prompt-file "$WT/qa-prompt.md" \
  --cwd "$WT" \
  --permission-mode bypassPermissions \
  --max-turns 50 \
  --rules "Never run scripts/dev_ota.sh or scripts/publish_firmware.sh. Never flash a tty other than QA boxes from qa-context.json. Always write qa-result.json."
gstat=$?
set -e

if [[ -f "$WT/qa-result.json" ]]; then
  RESULT="$WT/qa-result.json"
  cp "$RESULT" "$ROOT/server/logs/lab-qa/pr-${PR}-${SHA:0:8}-${JOB}-result.json" 2>/dev/null || true
fi
exit "$gstat"
