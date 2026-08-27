#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
DATA="${1:-}"
SYMBOL="${2:-StaticRecompCore::Run()}"

if [[ -z "$DATA" ]]; then
  DATA="$(find "$ROOT/profiles" -name perf.data -type f -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -1 | cut -d' ' -f2-)"
fi
[[ -n "$DATA" && -f "$DATA" ]] || { echo "usage: $0 [perf.data] [symbol]" >&2; exit 2; }

OUT="${DATA%/*}/annotate-$(printf '%s' "$SYMBOL" | tr -cs 'A-Za-z0-9._-' '_').txt"
sudo perf annotate -i "$DATA" --stdio --symbol="$SYMBOL" > "$OUT"
grep -E '^[[:space:]]*[0-9]+\\.[0-9]+ :' "$OUT" | sort -nr | head -40
printf '\nFull annotate: %s\n' "$OUT"
