#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME="$ROOT/runtime/moderngekko-run"
MODULE="$ROOT/module/gGANE7U_recomp.so"
GAME="$ROOT/extracted"
USER_DIR="$ROOT/user"

# Compatibility range currently required for correct gameplay/camera behavior.
# Keep this as small as possible: every address in this interval runs through
# Dolphin's interpreter instead of the native static-recompiled module.

export STATICRECOMP_FALLBACK_RANGES="${STATICRECOMP_FALLBACK_RANGES:-8016C620-80172D5C}"

# CPU optimization: amortize native module <-> C++ runtime transitions by
# chaining verified static-recomp chunks inside one native burst. Set to 0 to
# return to the conservative one-dispatch path when debugging regressions.
export STATICRECOMP_NATIVE_BURST="${STATICRECOMP_NATIVE_BURST:-1}"

if [[ ! -x "$RUNTIME" ]]; then
  echo "error: local runtime is missing: $RUNTIME" >&2
  echo "run $ROOT/build.sh first" >&2
  exit 1
fi
if [[ ! -f "$MODULE" ]]; then
  echo "error: local Animaniacs module is missing: $MODULE" >&2
  echo "run $ROOT/build.sh first" >&2
  exit 1
fi
if [[ ! -f "$GAME/sys/main.dol" ]]; then
  echo "error: extracted Animaniacs game is missing: $GAME/sys/main.dol" >&2
  echo "run $ROOT/build.sh after placing your own ISO in iso/ANIMANIACS-USA.iso" >&2
  exit 1
fi

mkdir -p "$USER_DIR"

exec "$RUNTIME" \
  --game "$GAME" \
  --module "$MODULE" \
  --user-dir "$USER_DIR" \
  --graphics Vulkan \
  --wayland \
  "$@"
