#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE="$ROOT/ModernGekko"
BUILD_ROOT="$ROOT/build"
RUNTIME_BUILD="$BUILD_ROOT/runtime"
PORT_BUILD="$ROOT/port-build"
EXTRACTED="$ROOT/extracted"
ISO="$ROOT/iso/ANIMANIACS-USA.iso"

GAME_ID="${GAME_ID:-GANE7U}"
DOL_SHA256="aad70bd7c6e38bed47fa1218066a0ec770850b2e5706240d70d3e7ec4afeb0e1"
BACKEND="${BACKEND:-c}"
JOBS="${JOBS:-$(nproc)}"
MODULE_OPT_LEVEL="${MODULE_OPT_LEVEL:-3}"
TOOLCHAIN="${TOOLCHAIN:-auto}"

if [[ "$GAME_ID" != "GANE7U" ]]; then
  echo "error: this project builds only GAME_ID=GANE7U" >&2
  exit 2
fi
if [[ "$BACKEND" != "c" && "$BACKEND" != "llvm" ]]; then
  echo "error: BACKEND must be c or llvm" >&2
  exit 2
fi
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: JOBS must be a positive integer" >&2
  exit 2
fi
if [[ ! "$MODULE_OPT_LEVEL" =~ ^[0-3]$ ]]; then
  echo "error: MODULE_OPT_LEVEL must be 0, 1, 2, or 3" >&2
  exit 2
fi
if [[ ! -f "$SOURCE/CMakeLists.txt" ]]; then
  echo "error: local ModernGekko source is missing: $SOURCE" >&2
  exit 1
fi

# Prefer Clang for the C backend because moderngekko-port enables ThinLTO for
# Clang-built modules. Fall back to GCC when Clang is unavailable.
if [[ "$TOOLCHAIN" == "auto" ]]; then
  if command -v clang >/dev/null 2>&1; then
    TOOLCHAIN="clang"
  else
    TOOLCHAIN="gcc"
  fi
fi
if [[ "$TOOLCHAIN" != "clang" && "$TOOLCHAIN" != "gcc" ]]; then
  echo "error: TOOLCHAIN must be auto, clang, or gcc" >&2
  exit 2
fi
if [[ "$BACKEND" == "llvm" && "$TOOLCHAIN" != "clang" ]]; then
  echo "error: BACKEND=llvm requires TOOLCHAIN=clang" >&2
  exit 2
fi

exec 9>"$ROOT/.build.lock"
if ! flock -n 9; then
  echo "error: another Animaniacs build is already running" >&2
  exit 1
fi

mkdir -p "$ROOT/.cache/dolrecomp/llvm" "$ROOT/.cache/ccache" "$ROOT/.cache/sccache"

export XDG_CACHE_HOME="$ROOT/.cache"
export CMAKE_BUILD_PARALLEL_LEVEL="$JOBS"
export MODERNGEKKO_BUILD_JOBS="$JOBS"
export MODERNGEKKO_MODULE_OPT_LEVEL="$MODULE_OPT_LEVEL"
export DOLRECOMP_LLVM_CACHE="$ROOT/.cache/dolrecomp/llvm"
export CCACHE_DIR="$ROOT/.cache/ccache"
export SCCACHE_DIR="$ROOT/.cache/sccache"

LLVM_ENABLED=OFF
if [[ "$BACKEND" == "llvm" ]]; then
  LLVM_ENABLED=ON
fi

echo "==> Configuring ModernGekko runtime"
echo "    backend:          $BACKEND"
echo "    module toolchain: $TOOLCHAIN"
echo "    module opt level: O$MODULE_OPT_LEVEL"
echo "    jobs:             $JOBS"

cmake -S "$SOURCE" -B "$RUNTIME_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DMODERNGEKKO_REQUIRED_DISC_ID="$GAME_ID" \
  -DMODERNGEKKO_REQUIRED_DOL_SHA256="$DOL_SHA256" \
  -DMODERNGEKKO_DEFAULT_WINDOW_TITLE="Animaniacs: The Great Edgar Hunt" \
  -DDOLRECOMP_ENABLE_LLVM="$LLVM_ENABLED"

cmake --build "$RUNTIME_BUILD" \
  --target moderngekko-run moderngekko-port dolrecomp \
  -j "$JOBS"

# A clean checkout only needs the user's own ISO. The extractor is now built,
# so create extracted/ automatically when needed.
if [[ ! -f "$EXTRACTED/sys/main.dol" ]]; then
  if [[ ! -f "$ISO" ]]; then
    echo "error: no extracted game found and ISO is missing: $ISO" >&2
    echo "place your own USA GameCube ISO there, then rerun ./build.sh" >&2
    exit 1
  fi
  echo "==> Extracting user-supplied ISO"
  rm -rf "$EXTRACTED"
  "$RUNTIME_BUILD/dolrecomp" extract "$ISO" "$EXTRACTED"
fi

read -r ACTUAL_DOL_SHA256 _ < <(sha256sum "$EXTRACTED/sys/main.dol")
if [[ "$ACTUAL_DOL_SHA256" != "$DOL_SHA256" ]]; then
  echo "error: extracted main.dol does not match the supported Animaniacs USA build" >&2
  echo "expected: $DOL_SHA256" >&2
  echo "actual:   $ACTUAL_DOL_SHA256" >&2
  exit 1
fi

echo "==> Building optimized native module"
"$RUNTIME_BUILD/moderngekko-port" build "$EXTRACTED" \
  --backend "$BACKEND" \
  --toolchain "$TOOLCHAIN" \
  --output "$PORT_BUILD"

ACTIVE_MODULE_FILE="$PORT_BUILD/$GAME_ID/active-module.txt"
if [[ ! -f "$ACTIVE_MODULE_FILE" ]]; then
  echo "error: module build did not publish $ACTIVE_MODULE_FILE" >&2
  exit 1
fi
IFS= read -r BUILT_MODULE_RAW < "$ACTIVE_MODULE_FILE"
BUILT_MODULE="$(realpath -e -- "$BUILT_MODULE_RAW")"
EXPECTED_PREFIX="$(realpath -e -- "$PORT_BUILD/$GAME_ID")/"
if [[ "$BUILT_MODULE" != "$EXPECTED_PREFIX"* || \
      "${BUILT_MODULE##*/}" != "g${GAME_ID}_recomp.so" || \
      ! -f "$BUILT_MODULE" ]]; then
  echo "error: module activation points outside the local project cache" >&2
  exit 1
fi
if [[ ! -x "$RUNTIME_BUILD/moderngekko-run" ]]; then
  echo "error: runtime build did not produce moderngekko-run" >&2
  exit 1
fi
if [[ ! -d "$RUNTIME_BUILD/Sys" ]]; then
  echo "error: runtime build did not produce its adjacent Sys directory" >&2
  exit 1
fi

STAGE="$(mktemp -d "$BUILD_ROOT/.publish.XXXXXX")"
mkdir -p "$STAGE/new-runtime/Sys" "$STAGE/new-module"
install -m 0755 "$RUNTIME_BUILD/moderngekko-run" "$STAGE/new-runtime/moderngekko-run"
cp -a "$RUNTIME_BUILD/Sys/." "$STAGE/new-runtime/Sys/"
install -m 0755 "$BUILT_MODULE" "$STAGE/new-module/g${GAME_ID}_recomp.so"

RUNTIME_TARGET="$ROOT/runtime"
MODULE_TARGET="$ROOT/module"
HAD_RUNTIME=0
HAD_MODULE=0
RUNTIME_PUBLISHED=0
MODULE_PUBLISHED=0
PUBLISH_COMPLETE=0

finish_publish() {
  status=$?
  trap - EXIT INT TERM HUP
  rollback_ok=1
  if [[ "$PUBLISH_COMPLETE" -ne 1 ]]; then
    if [[ "$RUNTIME_PUBLISHED" -eq 1 && -e "$RUNTIME_TARGET" ]]; then
      mv "$RUNTIME_TARGET" "$STAGE/failed-runtime" || rollback_ok=0
    fi
    if [[ "$MODULE_PUBLISHED" -eq 1 && -e "$MODULE_TARGET" ]]; then
      mv "$MODULE_TARGET" "$STAGE/failed-module" || rollback_ok=0
    fi
    if [[ "$HAD_RUNTIME" -eq 1 && -e "$STAGE/old-runtime" ]]; then
      mv "$STAGE/old-runtime" "$RUNTIME_TARGET" || rollback_ok=0
    fi
    if [[ "$HAD_MODULE" -eq 1 && -e "$STAGE/old-module" ]]; then
      mv "$STAGE/old-module" "$MODULE_TARGET" || rollback_ok=0
    fi
  fi

  if [[ "$rollback_ok" -eq 1 ]]; then
    rm -rf -- "$STAGE"
  else
    echo "error: publication rollback was incomplete; preserved $STAGE" >&2
  fi
  exit "$status"
}
trap finish_publish EXIT
trap 'exit 130' INT TERM HUP

if [[ -e "$RUNTIME_TARGET" ]]; then
  HAD_RUNTIME=1
  mv "$RUNTIME_TARGET" "$STAGE/old-runtime"
fi
if [[ -e "$MODULE_TARGET" ]]; then
  HAD_MODULE=1
  mv "$MODULE_TARGET" "$STAGE/old-module"
fi

RUNTIME_PUBLISHED=1
mv "$STAGE/new-runtime" "$RUNTIME_TARGET"
MODULE_PUBLISHED=1
mv "$STAGE/new-module" "$MODULE_TARGET"
PUBLISH_COMPLETE=1

echo "==> Build complete"
echo "published runtime: $ROOT/runtime/moderngekko-run"
echo "published module:  $ROOT/module/g${GAME_ID}_recomp.so"
echo "run with:          $ROOT/run.sh"
