#!/usr/bin/env bash
# scripts/install.sh [--dry-run] [build-dir] [product...]: copy the built AU and VST3 of every product (or of the ones
# named) into ~/Library/Audio/Plug-Ins, where hosts find them.
#
#   cmake --workflow --preset release-build && scripts/install.sh     both products, the release build (build-release)
#   scripts/install.sh synth                                           one product: its key (products/<key>.cmake)
#   scripts/install.sh <Name>                                          or its name (PRODUCT_NAME)
#   scripts/install.sh build-dev effect                                a dev build instead
#   scripts/install.sh --dry-run                                       check the bundles, say what would be copied
#
# A deliberate step, never part of a build (CMakeLists.txt sets COPY_PLUGIN_AFTER_BUILD FALSE). It replaces the
# installed bundles of the products named only (a bundle whose signature does not hold is refused: a host would not
# load it), clears the immutable flags a codesigned bundle may carry, and restarts the AU registrar so hosts rescan.
# In an agent session the lead runs it, not an agent.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The products: key -> name, from products/<key>.cmake.
KEYS=()
NAMES=()
for def in "$ROOT"/products/*.cmake; do
  key="$(basename "$def" .cmake)"
  name="$(sed -n 's/^set(PRODUCT_NAME[[:space:]]*"\([^"]*\)".*/\1/p' "$def")"
  if [[ -z "$name" ]]; then
    echo "install.sh: no PRODUCT_NAME in products/$key.cmake" >&2
    exit 1
  fi
  KEYS+=("$key")
  NAMES+=("$name")
done
if [[ ${#KEYS[@]} -eq 0 ]]; then
  echo "install.sh: products/ has no <key>.cmake" >&2
  exit 1
fi

# The arguments: a product's key or name selects it; anything else is the build directory.
BUILD=""
WANT=()
DRY=0
for arg in "$@"; do
  if [[ "$arg" == "-n" || "$arg" == "--dry-run" ]]; then
    DRY=1
    continue
  fi
  found=""
  for i in "${!KEYS[@]}"; do
    if [[ "$arg" == "${KEYS[$i]}" || "$arg" == "${NAMES[$i]}" ]]; then
      found="${NAMES[$i]}"
    fi
  done
  if [[ -n "$found" ]]; then
    WANT+=("$found")
  elif [[ -z "$BUILD" ]]; then
    BUILD="$arg"
  else
    echo "install.sh: '$arg' is no product (${KEYS[*]}, or ${NAMES[*]}) and the build directory is '$BUILD'" >&2
    exit 2
  fi
done
if [[ ${#WANT[@]} -eq 0 ]]; then
  WANT=("${NAMES[@]}")
fi
BUILD="${BUILD:-build-release}"
[[ "$BUILD" = /* ]] || BUILD="$ROOT/$BUILD"

COMPONENTS="$HOME/Library/Audio/Plug-Ins/Components"
VST3="$HOME/Library/Audio/Plug-Ins/VST3"

# Check every bundle first, so that a missing one installs nothing.
SOURCES=()
TARGETS=()
CONFIGS=()
for NAME in "${WANT[@]}"; do
  ART="$BUILD/${NAME}_artefacts"
  CONFIG="$(ls -1 "$ART" 2>/dev/null | grep -E '^(Release|RelWithDebInfo|Debug|MinSizeRel)$' | head -n 1 || true)"
  if [[ -z "$CONFIG" ]]; then
    echo "install.sh: no build of $NAME in $ART (build first: cmake --workflow --preset release-build)" >&2
    exit 1
  fi
  for pair in "AU/$NAME.component:$COMPONENTS" "VST3/$NAME.vst3:$VST3"; do
    src="$ART/$CONFIG/${pair%%:*}"
    if [[ ! -d "$src" ]]; then
      echo "install.sh: $src is missing" >&2
      exit 1
    fi
    if ! codesign --verify --strict "$src" 2>/dev/null; then
      echo "install.sh: $src: its signature does not hold, and hosts refuse such a bundle (build it again)" >&2
      exit 1
    fi
    SOURCES+=("$src")
    TARGETS+=("${pair##*:}/$(basename "$src")")
    CONFIGS+=("$CONFIG")
  done
done

if [[ $DRY -eq 1 ]]; then
  for i in "${!SOURCES[@]}"; do
    echo "would install ${TARGETS[$i]} (${CONFIGS[$i]}) from ${SOURCES[$i]}"
  done
  exit 0
fi
mkdir -p "$COMPONENTS" "$VST3"
for i in "${!SOURCES[@]}"; do
  src="${SOURCES[$i]}"
  dst="${TARGETS[$i]}"
  if [[ -e "$dst" ]]; then
    chflags -R nouchg "$dst" 2>/dev/null || true
    rm -rf "$dst"
  fi
  ditto "$src" "$dst"
  echo "installed $dst (${CONFIGS[$i]})"
done
killall -q AudioComponentRegistrar 2>/dev/null || true
# The MCP servers, for Claude Desktop: each product's extension is installed from Claude Desktop, by opening its .mcpb
# (once).
for NAME in "${WANT[@]}"; do
  if [[ -f "$BUILD/$NAME.mcpb" ]]; then
    echo "Claude Desktop: open $BUILD/$NAME.mcpb to add $NAME's MCP server (once; its tools then reach any open instance)"
  fi
done
