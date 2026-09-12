#!/usr/bin/env bash
# Cross-compile the factory flasher: one executable per OS/arch.
# Firmware is not embedded. Output: factory_flasher/dist/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/factory_flasher"
VER="$(tr -d '[:space:]' < VERSION)"
DIST="$ROOT/factory_flasher/dist"
mkdir -p "$DIST"

if ! command -v go >/dev/null; then
  echo "go is required" >&2
  exit 2
fi
export GOTOOLCHAIN="${GOTOOLCHAIN:-auto}"

echo "go test"
go test ./...

targets=(
  linux/amd64
  linux/arm64
  windows/amd64
  darwin/amd64
  darwin/arm64
)

for spec in "${targets[@]}"; do
  os="${spec%/*}"
  arch="${spec#*/}"
  name="searaboom-factory-flasher-${os}-${arch}"
  if [[ "$os" == windows ]]; then
    name="${name}.exe"
  fi
  echo "building $name"
  CGO_ENABLED=0 GOOS="$os" GOARCH="$arch" go build \
    -trimpath \
    -ldflags "-s -w -X main.Version=${VER}" \
    -o "$DIST/$name" \
    .
done

python3 -c '
import hashlib, json, sys
from pathlib import Path
dist = Path(sys.argv[1])
ver = sys.argv[2]
files = []
for path in sorted(dist.iterdir()):
    if path.name == "manifest.json" or not path.is_file():
        continue
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    files.append({"filename": path.name, "size": path.stat().st_size, "sha256": digest})
    print(digest, path.name, path.stat().st_size)
(dist / "manifest.json").write_text(json.dumps({
    "tool": "searaboom-factory-flasher",
    "version": ver,
    "files": files,
    "note": "Firmware is fetched at runtime from the live factory server.",
}, indent=2) + "\n")
' "$DIST" "$VER"

echo "wrote $DIST"
