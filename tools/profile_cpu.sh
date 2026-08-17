#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DURATION="${1:-30}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$ROOT/perf"
DATA="$OUT_DIR/animaniacs-$STAMP.data"
REPORT="$OUT_DIR/animaniacs-$STAMP.txt"

if [[ ! "$DURATION" =~ ^[1-9][0-9]*$ ]]; then
  echo "usage: $0 [duration-seconds]" >&2
  exit 2
fi
if ! command -v perf >/dev/null 2>&1; then
  echo "error: Linux perf is not installed" >&2
  echo "Arch Linux: sudo pacman -S perf" >&2
  exit 1
fi
if [[ ! -x "$ROOT/run.sh" ]]; then
  echo "error: $ROOT/run.sh is missing or not executable" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

echo "Recording $DURATION seconds of CPU samples..."
echo "Output: $DATA"
echo

set +e
timeout --signal=INT --kill-after=3s "${DURATION}s" \
  perf record \
    -F 999 \
    -g \
    --call-graph dwarf,16384 \
    -o "$DATA" \
    -- "$ROOT/run.sh"
status=$?
set -e

# timeout returns 124 after stopping the workload; that is expected here.
if [[ "$status" -ne 0 && "$status" -ne 124 && "$status" -ne 130 ]]; then
  echo "perf record failed with status $status" >&2
  exit "$status"
fi

perf report \
  --stdio \
  --sort comm,dso,symbol \
  --percent-limit 0.5 \
  -i "$DATA" > "$REPORT"

echo
echo "CPU report written to: $REPORT"
echo "Top entries:"
sed -n '1,80p' "$REPORT"
