#!/usr/bin/env bash
#
# release.sh — produce a distributable, codesigned (and optionally notarized)
# build of this plugin.
#
# This is NOT a per-build step. Run it deliberately when you want artefacts to
# ship. Normal dev builds stay unsigned and fast; signing (Developer ID +
# hardened runtime + Apple timestamp server) only matters for distribution.
#
# Usage:
#   ./scripts/release.sh --identity "Developer ID Application: Name (TEAMID)" \
#                        [--keychain-profile PROFILE] \
#                        [--build-dir build] [--no-clean]
#
# Find your identity string with:  security find-identity -v -p codesigning
#
# Steps:
#   1. clear immutable flags left on previously-signed installed bundles
#      (otherwise JUCE's COPY_PLUGIN_AFTER_BUILD step fails with EPERM)
#   2. clean the build directory (unless --no-clean)
#   3. configure + build Release
#   4. codesign the built bundles            (scripts/plugin.py codesign)
#   5. notarize + staple, if --keychain-profile given (scripts/plugin.py notarize)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

IDENTITY=""
KEYCHAIN_PROFILE=""
BUILD_DIR="build"
CLEAN=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --identity)         IDENTITY="${2:-}"; shift 2 ;;
    --keychain-profile) KEYCHAIN_PROFILE="${2:-}"; shift 2 ;;
    --build-dir)        BUILD_DIR="${2:-}"; shift 2 ;;
    --no-clean)         CLEAN=0; shift ;;
    -h|--help)          sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
done

if [[ -z "$IDENTITY" ]]; then
  echo "error: --identity is required" >&2
  echo "       e.g. --identity \"Developer ID Application: Name (TEAMID)\"" >&2
  echo "       list yours with: security find-identity -v -p codesigning" >&2
  exit 1
fi

# Plugin name as declared in CMakeLists.txt: juce_add_plugin(<Name> ...).
PLUGIN_NAME="$(grep -m1 -E '^[[:space:]]*juce_add_plugin[[:space:]]*\(' CMakeLists.txt \
  | sed -E 's/.*juce_add_plugin[[:space:]]*\([[:space:]]*([A-Za-z0-9_]+).*/\1/')"
if [[ -z "$PLUGIN_NAME" ]]; then
  echo "error: could not determine plugin name from CMakeLists.txt" >&2
  exit 1
fi

echo "==> Releasing $PLUGIN_NAME"

# 1. Clear immutable flags on previously-signed installed bundles.
COMPONENT="$HOME/Library/Audio/Plug-Ins/Components/$PLUGIN_NAME.component"
VST3="$HOME/Library/Audio/Plug-Ins/VST3/$PLUGIN_NAME.vst3"
for bundle in "$COMPONENT" "$VST3"; do
  if [[ -e "$bundle" ]]; then
    chflags -R nouchg "$bundle" 2>/dev/null || true
  fi
done

# 2. Clean.
if [[ "$CLEAN" -eq 1 ]]; then
  echo "==> Cleaning $BUILD_DIR/"
  rm -rf "$BUILD_DIR"
fi

# 3. Configure + build Release.
echo "==> Configuring ($BUILD_DIR)"
cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release

echo "==> Building Release"
cmake --build "$BUILD_DIR" --config Release

# 4. Codesign.
echo "==> Codesigning"
python3 scripts/plugin.py codesign --identity "$IDENTITY" --build-dir "$BUILD_DIR"

# 5. Notarize (optional).
if [[ -n "$KEYCHAIN_PROFILE" ]]; then
  echo "==> Notarizing"
  python3 scripts/plugin.py notarize --keychain-profile "$KEYCHAIN_PROFILE" --build-dir "$BUILD_DIR"
else
  echo "==> Skipping notarization (pass --keychain-profile to enable)"
fi

echo "==> Release complete for $PLUGIN_NAME"
