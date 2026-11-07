#!/usr/bin/env bash
# scripts/package-linux.sh [options] [product...]: one Linux package (a .tar.gz) of every product (or of the ones named,
# by key or name), from the Linux release build, written to <build>/dist/<Project>-<version>-linux-x86_64.tar.gz. It
# holds, for each product, what the build made and an install script (scripts/linux-install.sh, as install.sh):
#
#   install.sh                               installs for the user who runs it (~/.vst3, ~/.local: see its header)
#   README.txt
#   products/<Name>/<Name>.vst3              the VST3
#   products/<Name>/app/                     the Standalone, its shaderc and licences/
#   products/<Name>/server/                  the MCP server, the GUI sandbox's helper and the headless renderer (as the
#                                            extension holds them), their licences/
#   products/<Name>/<Name>.mcpb              the Claude Desktop extension
#   products/<Name>/icons/<size>.png         the app's icon (the kind's: Assets/logo/<kind>/icon.iconset)
#   products/<Name>/product.env              its id, version, company, kind, MCP name and title, KEY=value lines
#
#   cmake --workflow --preset linux-release-build && scripts/package-linux.sh     both products
#   scripts/package-linux.sh --build synth                                       build first, the synth alone
#   scripts/package-linux.sh --build-dir build-linux-dev                         a dev build instead
#
# Then, on any x86-64 Linux with the run-time libraries (README, "Linux"): tar xzf <package>.tar.gz, cd into it,
# ./install.sh. Nothing is installed here; nothing is signed.
#
# Options:
#   --build             run `cmake --workflow --preset linux-release-build` first (else the build must be there)
#   --build-dir <dir>   the build to package (default build-linux-release; its CMakeCache names its products and type)
#   --out <dir>         where the package goes (default <build>/dist)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
die() { echo "package-linux.sh: $*" >&2; exit 1; }

BUILD_FIRST=0
BUILD=""
OUT=""
ASKED=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build)     BUILD_FIRST=1 ;;
    --build-dir) shift; BUILD="${1:?--build-dir needs a folder}" ;;
    --out)       shift; OUT="${1:?--out needs a folder}" ;;
    -h|--help)   sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*)          die "unknown option $1" ;;
    *)           ASKED+=("$1") ;;
  esac
  shift
done
[[ "$(uname -s)" == Linux ]] || die "this packages a Linux build: run it on Linux"
BUILD="${BUILD:-build-linux-release}"
[[ "$BUILD" = /* ]] || BUILD="$ROOT/$BUILD"
if [[ $BUILD_FIRST -eq 1 ]]; then
  (cd "$ROOT" && cmake --workflow --preset linux-release-build)
fi

# ---- the products: products/<key>.cmake ----------------------------------------------------------------------------------
value() { sed -n "s/^set(PRODUCT_$2[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$ROOT/products/$1.cmake"; }
KEYS=()
for def in "$ROOT"/products/*.cmake; do
  KEYS+=("$(basename "$def" .cmake)")
done
[[ ${#KEYS[@]} -gt 0 ]] || die "products/ has no <key>.cmake"

CACHE="$BUILD/CMakeCache.txt"
[[ -f "$CACHE" ]] || die "$BUILD is no build directory (build first: cmake --workflow --preset linux-release-build, or --build)"
BUILT=" $(sed -n 's/^FP_BUILT_PRODUCTS:INTERNAL=//p' "$CACHE" | tr ';' ' ') "
TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$CACHE")"
[[ -n "${BUILT// /}" && -n "$TYPE" ]] || die "$CACHE names no products or no build type (configure it with its preset)"
[[ "$TYPE" == "Release" ]] || echo "package-linux.sh: note: $BUILD is a $TYPE build, not Release"

WANT=()
if [[ ${#ASKED[@]} -eq 0 ]]; then
  for k in "${KEYS[@]}"; do
    [[ "$BUILT" == *" $k "* ]] && WANT+=("$k")
  done
else
  for a in "${ASKED[@]}"; do
    found=""
    for k in "${KEYS[@]}"; do
      [[ "$a" == "$k" || "$a" == "$(value "$k" NAME)" ]] && found="$k"
    done
    [[ -n "$found" ]] || die "'$a' is no product (${KEYS[*]})"
    [[ "$BUILT" == *" $found "* ]] || die "$BUILD is not configured for $found (its products:$BUILT)"
    WANT+=("$found")
  done
fi
[[ ${#WANT[@]} -gt 0 ]] || die "$BUILD has none of the products built"

# The project's name: the products' names' common start (4 characters at least), else the first's (as
# cmake/Installer.cmake names the installer); the version, the first product's.
NAMES=()
for k in "${WANT[@]}"; do NAMES+=("$(value "$k" NAME)"); done
PROJECT="${NAMES[0]}"
for n in "${NAMES[@]}"; do
  while [[ -n "$PROJECT" && "$n" != "$PROJECT"* ]]; do PROJECT="${PROJECT%?}"; done
done
[[ ${#PROJECT} -ge 4 ]] || PROJECT="${NAMES[0]}"
VERSION="$(value "${WANT[0]}" VERSION)"
PACKAGE="$PROJECT-$VERSION-linux-x86_64"
OUT="${OUT:-$BUILD/dist}"
mkdir -p "$OUT"
STAGE="$OUT/$PACKAGE"
rm -rf "$STAGE"                                       # this script's own earlier folder (the build's output)
mkdir -p "$STAGE/products"

for k in "${WANT[@]}"; do
  NAME="$(value "$k" NAME)"
  KIND="$(value "$k" KIND)"
  ART="$BUILD/${NAME}_artefacts/$TYPE"
  DST="$STAGE/products/$NAME"
  for need in "$ART/VST3/$NAME.vst3/Contents/x86_64-linux/$NAME.so" "$ART/Standalone/$NAME" "$BUILD/mcpb/$k/server/${NAME}MCP" \
              "$BUILD/$NAME.mcpb"; do
    [[ -e "$need" ]] || die "$need is missing (build $NAME first: cmake --workflow --preset linux-release-build)"
  done
  mkdir -p "$DST/icons"
  cp -a "$ART/VST3/$NAME.vst3" "$DST/"
  cp -a "$ART/Standalone" "$DST/app"
  cp -a "$BUILD/mcpb/$k/server" "$DST/server"
  [[ ! -d "$BUILD/mcpb/$k/licences" ]] || cp -a "$BUILD/mcpb/$k/licences" "$DST/server/licences"
  cp -a "$BUILD/$NAME.mcpb" "$DST/"
  for s in 16 32 128 256 512; do
    icon="$ROOT/Assets/logo/$KIND/icon.iconset/icon_${s}x${s}.png"
    [[ -f "$icon" ]] && cp "$icon" "$DST/icons/$s.png"
  done
  if [[ "$KIND" == "instrument" ]]; then TITLE="Synthesizer"; else TITLE="Audio effect"; fi
  {
    echo "ID=$(value "$k" BUNDLE_ID)"
    echo "VERSION=$(value "$k" VERSION)"
    echo "COMPANY=$(value "$k" COMPANY)"
    echo "KIND=$KIND"
    echo "MCP=$(echo "$NAME" | tr '[:upper:]' '[:lower:]')"
    echo "TITLE=$TITLE"
  } > "$DST/product.env"
  echo "package-linux.sh: $NAME $(value "$k" VERSION) ($TYPE)"
done
install -m 755 "$ROOT/scripts/linux-install.sh" "$STAGE/install.sh"
cat > "$STAGE/README.txt" <<EOF
$PROJECT $VERSION for Linux (x86-64, a processor with AVX2 and FMA)

  ./install.sh               install every product here for you: ~/.vst3 and ~/.local (no root)
  ./install.sh --dry-run     say what it would do
  ./install.sh --uninstall   move what it installed to the Trash
  ./install.sh --help        the details

It needs a Vulkan driver (Mesa's, NVIDIA's, or lavapipe) and the system's ALSA, X11 (XWayland on Wayland), fontconfig,
freetype, OpenSSL and libcurl libraries; xdg-utils to open folders; libsecret (optional) to keep the chat's API key in
your desktop's keyring. Its last lines say how Claude Code reaches each product's MCP server.
EOF
TAR="$OUT/$PACKAGE.tar.gz"
rm -f "$TAR"
tar -C "$OUT" --owner=0 --group=0 --numeric-owner -czf "$TAR" "$PACKAGE"
echo "package-linux.sh: wrote $TAR ($(du -h "$TAR" | cut -f1)) and the folder $STAGE"
