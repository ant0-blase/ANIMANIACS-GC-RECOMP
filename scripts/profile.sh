#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
DURATION="${1:-15}"
FREQ="${PERF_FREQ:-999}"
MODULE_PATTERN="ANIMANIACS-GC-RECOMP/module/gGANE7U_recomp.so"

[[ "$DURATION" =~ ^[1-9][0-9]*$ ]] || { echo "usage: $0 [seconds]" >&2; exit 2; }
command -v perf >/dev/null 2>&1 || { echo "error: perf is not installed" >&2; exit 1; }

PID=""
for pid in $(pgrep moderngekko-run 2>/dev/null || true); do
  if grep -q "$MODULE_PATTERN" "/proc/$pid/maps" 2>/dev/null; then
    PID="$pid"
    break
  fi
done
[[ -n "$PID" ]] || { echo "error: launch ./run.sh first" >&2; exit 1; }

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$ROOT/profiles/$STAMP"
mkdir -p "$OUT"

printf 'Animaniacs PID=%s duration=%ss freq=%sHz\n' "$PID" "$DURATION" "$FREQ"
ps -L -p "$PID" -o pid,tid,psr,pcpu,comm --sort=-pcpu | tee "$OUT/threads-before.txt"
sudo -v

sudo perf record -F "$FREQ" -e cycles:u -g --call-graph dwarf,16384 \
  -p "$PID" -o "$OUT/perf.data" -- sleep "$DURATION"

sudo perf report -i "$OUT/perf.data" --stdio --no-children -g none \
  --sort=dso,symbol --percent-limit 0.1 > "$OUT/hotspots.txt"

sudo perf report -i "$OUT/perf.data" --stdio --no-children -g none \
  --sort=pid,tid,comm,dso,symbol --percent-limit 0.25 > "$OUT/threads.txt"

grep -E 'StaticRecompCore::Run|ChunkIndexOf|SingleStepInner|IsForcedFallbackAddress|FastDispatchableAt|DispatchableAt|ResolveNativeAddress|SyncIn|SyncOut|HookExternalWrite' \
  "$OUT/hotspots.txt" > "$OUT/staticrecomp.txt" || true

grep -E 'RunGpuLoop|RunGpu|RunFifo|GatherPipe|SetCPStatus|UpdateGatherPipe|VertexManager|Flush|LoadBPReg|LoadXFReg|Presenter|Framebuffer' \
  "$OUT/hotspots.txt" > "$OUT/gpu.txt" || true

grep -E 'gGANE7U_recomp\\.so' "$OUT/hotspots.txt" | head -60 > "$OUT/game.txt" || true

printf '\n===== TOP HOTSPOTS =====\n'
head -60 "$OUT/hotspots.txt"
printf '\n===== STATIC RECOMP =====\n'
cat "$OUT/staticrecomp.txt"
printf '\n===== GPU/FIFO =====\n'
cat "$OUT/gpu.txt"
printf '\nResults: %s\n' "$OUT"
