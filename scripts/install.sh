#!/usr/bin/env bash
# scripts/install.sh [build-dir]: copy the built AU and VST3 into ~/Library/Audio/Plug-Ins, where hosts find them.
#
#   cmake --workflow --preset release-build && scripts/install.sh            the release build (default: build-release)
#   scripts/install.sh build-dev                                               a dev build instead
#
# A deliberate step, never part of a build (CMakeLists.txt sets COPY_PLUGIN_AFTER_BUILD FALSE). It replaces the
# installed bundles of this plugin only, clears the immutable flags a codesigned bundle may carry, and restarts the
# AU registrar so hosts rescan. In an agent session the lead runs it, not an agent.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-build-release}"
[[ "$BUILD" = /* ]] || BUILD="$ROOT/$BUILD"

NAME="$(sed -n 's/^set(PRODUCT_NAME[[:space:]]*"\([^"]*\)".*/\1/p' "$ROOT/cmake/Product.cmake")"
if [[ -z "$NAME" ]]; then
  echo "install.sh: no PRODUCT_NAME in cmake/Product.cmake" >&2
  exit 1
fi
ART="$BUILD/${NAME}_artefacts"
CONFIG="$(ls -1 "$ART" 2>/dev/null | grep -E '^(Release|RelWithDebInfo|Debug|MinSizeRel)$' | head -n 1 || true)"
if [[ -z "$CONFIG" ]]; then
  echo "install.sh: no build in $ART (build first: cmake --workflow --preset release-build)" >&2
  exit 1
fi

COMPONENTS="$HOME/Library/Audio/Plug-Ins/Components"
VST3="$HOME/Library/Audio/Plug-Ins/VST3"
mkdir -p "$COMPONENTS" "$VST3"
for pair in "AU/$NAME.component:$COMPONENTS" "VST3/$NAME.vst3:$VST3"; do
  src="$ART/$CONFIG/${pair%%:*}"
  dst="${pair##*:}/$(basename "$src")"
  if [[ ! -d "$src" ]]; then
    echo "install.sh: $src is missing" >&2
    exit 1
  fi
  if [[ -e "$dst" ]]; then
    chflags -R nouchg "$dst" 2>/dev/null || true
    rm -rf "$dst"
  fi
  ditto "$src" "$dst"
  echo "installed $dst ($CONFIG)"
done
killall -q AudioComponentRegistrar 2>/dev/null || true
