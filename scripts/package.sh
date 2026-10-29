#!/usr/bin/env bash
# scripts/package.sh [options] [product...]: one macOS installer (.pkg) for every product (or for the ones named, by key
# or name), from the release build, written to build-release/dist/. Each product is a choice of its own, made of four
# (the installer's Customize pane):
#
#   Audio Unit      <Name>.component   ->  /Library/Audio/Plug-Ins/Components   (then the AU registrar restarts)
#   VST3            <Name>.vst3        ->  /Library/Audio/Plug-Ins/VST3
#   App             <Name>.app         ->  /Applications                         (the Standalone)
#   Claude Desktop  <Name>.mcpb        ->  /Library/Application Support/<Company>/<Name>/
#
# The installer opens nothing: its last page says to double-click each extension in the Finder, and Claude Desktop then
# offers to add it (an installer runs as root and must never open Claude Desktop).
#
#   cmake --workflow --preset release-build && scripts/package.sh       both products, unsigned
#   scripts/package.sh --build synth                                   build first, the synth alone
#   scripts/package.sh --installer                                     the GUI installer's disk image instead (below)
#
# --installer: the disk image of the GUI installer (Source/installer/), build-release/dist/<Project>-Installer-<version>.dmg
# (UDZO): "<Project> Installer.app", which the release build made with every product inside it (its payload), and a
# short Read Me. No Applications link: the installer installs itself nowhere; it is opened from the image. With --sign,
# a copy of the app is signed under dist/stage-installer/ (its payload's bundles inside out, as the .pkg's, the apps with
# their entitlements, each extension's server; then the installer app itself, the hardened runtime and a timestamp),
# and the disk image too; with --notarize, the disk image is notarized and stapled (--sign-installer is not used).
# Without them the app keeps the build's ad hoc signature. Product names are not taken: the installer holds what the
# build built.
#
# Options:
#   --build                 run `cmake --workflow --preset release-build` first (else the build must be there)
#   --build-dir <dir>       the build to package (default build-release; its CMakeCache names its products and type)
#   --sign <identity>       codesign every bundle and binary with this Developer ID Application identity, the hardened
#                           runtime and a timestamp; the Standalone apps with scripts/entitlements/<kind>.plist
#                           (env DEVELOPER_ID_APPLICATION)
#   --sign-installer <id>   sign the .pkg with this Developer ID Installer identity (env DEVELOPER_ID_INSTALLER)
#   --notarize <profile>    submit the signed .pkg to Apple's notary service with this notarytool keychain profile
#                           (`xcrun notarytool store-credentials <profile>`), wait, and staple the ticket to it (env
#                           NOTARY_PROFILE). Needs both identities (--installer: --sign alone, and the .dmg's)
#   --installer             the GUI installer's disk image instead of the .pkg (above)
#
# Unsigned by default: the bundles keep the build's ad hoc signatures, and the .pkg is not signed, so Gatekeeper asks
# before it opens (Control-click it, Open). Signing works on copies under build-release/dist/stage/; the build's own
# bundles are never changed. Nothing is installed: open the .pkg to install, or inspect it with `pkgutil --expand`.
# In an agent session the lead runs it with identities, not an agent.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
die() { echo "package.sh: $*" >&2; exit 1; }

# ---- the arguments -----------------------------------------------------------------------------------------------------
BUILD_FIRST=0
INSTALLER=0
BUILD=""
APP_ID="${DEVELOPER_ID_APPLICATION:-}"
PKG_ID="${DEVELOPER_ID_INSTALLER:-}"
NOTARY="${NOTARY_PROFILE:-}"
ASKED=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build)          BUILD_FIRST=1 ;;
    --installer)      INSTALLER=1 ;;
    --build-dir)      BUILD="${2:?--build-dir needs a directory}"; shift ;;
    --sign)           APP_ID="${2:?--sign needs an identity}"; shift ;;
    --sign-installer) PKG_ID="${2:?--sign-installer needs an identity}"; shift ;;
    --notarize)       NOTARY="${2:?--notarize needs a keychain profile}"; shift ;;
    -h|--help)        sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*)               die "unknown option $1 (--help lists them)" ;;
    *)                ASKED+=("$1") ;;
  esac
  shift
done
if [[ $INSTALLER -eq 1 ]]; then
  [[ -z "$NOTARY" || -n "$APP_ID" ]] || die "--notarize needs --sign (Apple notarizes only signed code)"
  [[ ${#ASKED[@]} -eq 0 ]] || die "--installer takes no product: the installer holds every product the build built"
elif [[ -n "$NOTARY" && ( -z "$APP_ID" || -z "$PKG_ID" ) ]]; then
  die "--notarize needs --sign and --sign-installer (Apple notarizes only signed code)"
fi
BUILD="${BUILD:-build-release}"
[[ "$BUILD" = /* ]] || BUILD="$ROOT/$BUILD"

if [[ $BUILD_FIRST -eq 1 ]]; then
  (cd "$ROOT" && cmake --workflow --preset release-build)
fi

# ---- the products: products/<key>.cmake ----------------------------------------------------------------------------------
value() { sed -n "s/^set(PRODUCT_$2[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$ROOT/products/$1.cmake"; }
KEYS=()
for def in "$ROOT"/products/*.cmake; do
  KEYS+=("$(basename "$def" .cmake)")
done
[[ ${#KEYS[@]} -gt 0 ]] || die "products/ has no <key>.cmake"

CACHE="$BUILD/CMakeCache.txt"
[[ -f "$CACHE" ]] || die "$BUILD is no build directory (build first: cmake --workflow --preset release-build, or --build)"
BUILT=" $(sed -n 's/^FP_BUILT_PRODUCTS:INTERNAL=//p' "$CACHE" | tr ';' ' ') "
TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$CACHE")"
[[ -n "${BUILT// /}" && -n "$TYPE" ]] || die "$CACHE names no products or no build type (configure it with its preset)"
[[ "$TYPE" == "Release" ]] || echo "package.sh: note: $BUILD is a $TYPE build, not Release"

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

# ---- signing (optional): inside out, every Mach-O, the hardened runtime and a timestamp -----------------------------------
sign() {   # sign <path> [entitlements]
  local args=(--force --options runtime --timestamp --sign "$APP_ID")
  [[ -n "${2:-}" ]] && args+=(--entitlements "$2")
  codesign "${args[@]}" "$1" || die "codesign failed for $1"
}
sign_bundle() {   # sign_bundle <bundle> [entitlements]: its nested binaries (shaderc), then itself
  local bundle="$1" main f
  main="$bundle/Contents/MacOS/$(defaults read "$bundle/Contents/Info" CFBundleExecutable)"
  chmod -R u+w "$bundle"                         # a read-only file (a licence from the deps cache) keeps its attributes
  xattr -cr "$bundle"                            # codesign refuses Finder information and resource forks
  while IFS= read -r -d '' f; do
    [[ "$f" == "$main" ]] && continue
    [[ "$(file -b "$f")" == *Mach-O* ]] && sign "$f"
  done < <(find "$bundle" -type f -perm +111 -print0)
  sign "$bundle" "${2:-}"
  codesign --verify --strict --deep "$bundle" || die "$bundle: the signature does not verify"
}
sign_mcpb() {   # sign_mcpb <file.mcpb>: its programs (the server and the GUI sandbox's helper beside it, server/*), each
                #   signed as the bundles' binaries are, inside a copy of the archive, packed again as the build packs it
                #   (with its licences/)
  local mcpb="$1" dir f
  local files=()
  dir="$(mktemp -d "$STAGE/mcpb.XXXXXX")"
  ditto -x -k "$mcpb" "$dir"
  for f in "$dir"/server/*; do
    sign "$f"
    files+=("server/$(basename "$f")")
  done
  for f in "$dir"/licences/*; do
    if [[ -f "$f" ]]; then files+=("licences/$(basename "$f")"); fi
  done
  rm -f "$mcpb"
  (cd "$dir" && cmake -E tar cf "$mcpb" --format=zip manifest.json icon.png "${files[@]}")
  rm -rf "$dir"
}

# ---- --installer: the GUI installer's disk image ---------------------------------------------------------------------------
if [[ $INSTALLER -eq 1 ]]; then
  APPNAME="$(sed -n 's/^FP_INSTALLER_NAME:INTERNAL=//p' "$CACHE")"
  IVERSION="$(sed -n 's/^FP_INSTALLER_VERSION:INTERNAL=//p' "$CACHE")"
  [[ -n "$APPNAME" && -n "$IVERSION" ]] || die "$CACHE names no installer (configure it again with its preset)"
  SRC="$BUILD/Installer_artefacts/$TYPE/$APPNAME.app"
  PAYLOAD="Contents/Resources/payload"
  [[ -f "$SRC/$PAYLOAD/payload.json" ]] || die "$SRC has no payload (build first: cmake --workflow --preset release-build)"
  codesign --verify --strict "$SRC" 2>/dev/null || die "$SRC: its signature does not hold (build it again)"
  DIST="$BUILD/dist"
  STAGE="$DIST/stage-installer"
  rm -rf "$STAGE"
  mkdir -p "$STAGE/image"
  APP="$STAGE/image/$APPNAME.app"
  ditto --clone "$SRC" "$APP"                    # a copy that costs nothing on APFS until it is signed
  if [[ -n "$APP_ID" ]]; then
    # The payload inside out (each product's bundles, its apps with their entitlements, its extension's server), then
    # the installer app alone: its seal covers the payload as resources, signed already.
    for k in "${KEYS[@]}"; do
      [[ "$BUILT" == *" $k "* ]] || continue
      NAME="$(value "$k" NAME)"
      sign_bundle "$APP/$PAYLOAD/$NAME.component"
      sign_bundle "$APP/$PAYLOAD/$NAME.vst3"
      sign_bundle "$APP/$PAYLOAD/$NAME.app" "$ROOT/scripts/entitlements/$(value "$k" KIND).plist"
      sign_mcpb "$APP/$PAYLOAD/$NAME.mcpb"
    done
    chmod -R u+w "$APP"
    sign "$APP"
    codesign --verify --strict --deep "$APP" || die "$APP: the signature does not verify"
  fi
  # The Read Me: what the installer does, in a few lines.
  ITEMS=()
  for k in "${KEYS[@]}"; do
    [[ "$BUILT" == *" $k "* ]] || continue
    [[ "$(value "$k" KIND)" == "instrument" ]] && kind="a synth" || kind="an effect"
    ITEMS+=("$(value "$k" NAME) ($kind)")
  done
  WHAT="${ITEMS[0]}"
  for ((i = 1; i < ${#ITEMS[@]}; ++i)); do
    [[ $i -eq $((${#ITEMS[@]} - 1)) ]] && WHAT+=" and " || WHAT+=", "
    WHAT+="${ITEMS[$i]}"
  done
  {                                              # a paragraph a line: TextEdit wraps them
    echo "$APPNAME $IVERSION"
    echo
    echo "Open \"$APPNAME\" and follow it. It installs $WHAT: their Audio Unit and VST3 plugins, their apps and their Claude Desktop extensions, the parts you choose, just for you (no password) or for every user of this Mac (macOS asks for an administrator's password; the installer never sees it). It can also move them to the Trash (Uninstall...). Nothing changes until you click Install."
    echo
    echo "The installer installs itself nowhere: eject this disk image when you are done."
    if [[ -z "$APP_ID" ]]; then
      echo
      echo "This build is not signed by a developer: if macOS will not open the installer, Control-click it and choose Open."
    fi
  } > "$STAGE/image/Read Me.txt"
  OUT="$DIST/${APPNAME// /-}-$IVERSION.dmg"
  rm -f "$OUT"
  hdiutil create -quiet -volname "$APPNAME" -srcfolder "$STAGE/image" -fs HFS+ -format UDZO -ov "$OUT" \
    || die "hdiutil could not make $OUT"
  if [[ -n "$APP_ID" ]]; then
    codesign --force --timestamp --sign "$APP_ID" "$OUT" || die "codesign failed for $OUT"
  fi
  if [[ -n "$NOTARY" ]]; then
    xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY" --wait || die "notarytool refused $OUT (xcrun notarytool log <id> says why)"
    xcrun stapler staple "$OUT" || die "stapler failed for $OUT"
    spctl --assess --type open --context context:primary-signature -vv "$OUT" || die "Gatekeeper refuses $OUT"
  fi
  echo "wrote $OUT ($(du -h "$OUT" | cut -f1 | tr -d ' ')): $APPNAME.app and Read Me.txt"
  if [[ -z "$APP_ID" ]]; then
    echo "unsigned: the installer keeps the build's ad hoc signature (Gatekeeper asks before it opens)"
  else
    echo "signed: the installer and its payload by $APP_ID${NOTARY:+; the disk image notarized and stapled}"
  fi
  echo "inspect without installing: hdiutil attach -readonly -nobrowse \"$OUT\", then look in the volume"
  exit 0
fi

# ---- staging: copies of the built bundles, checked ------------------------------------------------------------------------
DIST="$BUILD/dist"
STAGE="$DIST/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/components" "$STAGE/resources"

# The installer's parts: "<key>|<part>|<source>|<install location>|<package file>", one a line.
PARTS=()
for k in "${WANT[@]}"; do
  NAME="$(value "$k" NAME)"
  COMPANY="$(value "$k" COMPANY)"
  ART="$BUILD/${NAME}_artefacts/$TYPE"
  for pair in "au:AU/$NAME.component:/Library/Audio/Plug-Ins/Components" \
              "vst3:VST3/$NAME.vst3:/Library/Audio/Plug-Ins/VST3" \
              "app:Standalone/$NAME.app:/Applications"; do
    part="${pair%%:*}"; rest="${pair#*:}"
    src="$ART/${rest%%:*}"
    [[ -d "$src" ]] || die "$src is missing (build first: cmake --workflow --preset release-build)"
    codesign --verify --strict "$src" 2>/dev/null || die "$src: its signature does not hold (build it again)"
    PARTS+=("$k|$part|$src|${rest#*:}|$k-$part.pkg")
  done
  [[ -f "$BUILD/$NAME.mcpb" ]] || die "$BUILD/$NAME.mcpb is missing (build first)"
  PARTS+=("$k|mcpb|$BUILD/$NAME.mcpb|/Library/Application Support/$COMPANY/$NAME|$k-mcpb.pkg")
done

for p in "${PARTS[@]}"; do
  IFS='|' read -r k part src _ _ <<<"$p"
  root="$STAGE/$k-$part/root"
  mkdir -p "$root"
  if [[ "$part" == "mcpb" ]]; then
    cp "$src" "$root/"
    [[ -n "$APP_ID" ]] && sign_mcpb "$root/$(basename "$src")"
  else
    ditto "$src" "$root/$(basename "$src")"
    if [[ -n "$APP_ID" ]]; then
      ent=""
      [[ "$part" == "app" ]] && ent="$ROOT/scripts/entitlements/$(value "$k" KIND).plist"
      sign_bundle "$root/$(basename "$src")" "$ent"
    fi
  fi
done

# ---- the component packages ------------------------------------------------------------------------------------------------
# The AU's restarts the AU registrar, as scripts/install.sh does, so hosts see it at once.
mkdir -p "$STAGE/scripts-au"
cat > "$STAGE/scripts-au/postinstall" <<'EOF'
#!/bin/sh
killall -q AudioComponentRegistrar 2>/dev/null
exit 0
EOF
chmod 755 "$STAGE/scripts-au/postinstall"

for p in "${PARTS[@]}"; do
  IFS='|' read -r k part _ location pkgfile <<<"$p"
  root="$STAGE/$k-$part/root"
  args=(--root "$root" --identifier "$(value "$k" BUNDLE_ID).pkg.$part" --version "$(value "$k" VERSION)"
        --install-location "$location" --ownership recommended)
  if [[ "$part" != "mcpb" ]]; then
    # Bundles are installed where the package says, never "relocated" to a copy of the same bundle id found elsewhere
    # (a build directory), and replace what is there whatever its version.
    plist="$STAGE/$k-$part/components.plist"
    pkgbuild --analyze --root "$root" "$plist" >/dev/null
    i=0
    while /usr/libexec/PlistBuddy -c "Print :$i" "$plist" >/dev/null 2>&1; do
      for key in BundleIsRelocatable BundleIsVersionChecked; do
        /usr/libexec/PlistBuddy -c "Delete :$i:$key" "$plist" >/dev/null 2>&1 || true
        /usr/libexec/PlistBuddy -c "Add :$i:$key bool false" "$plist"
      done
      i=$((i + 1))
    done
    args+=(--component-plist "$plist")
  fi
  [[ "$part" == "au" ]] && args+=(--scripts "$STAGE/scripts-au")
  pkgbuild "${args[@]}" "$STAGE/components/$pkgfile" >/dev/null || die "pkgbuild failed for $k $part"
done

# ---- the distribution: a choice a product, a choice a part ------------------------------------------------------------------
html() { sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g; s/"/\&quot;/g'; }
NAMES=()
for k in "${WANT[@]}"; do NAMES+=("$(value "$k" NAME)"); done
# The package's name: the products' names' common start (TestingPlayground and TestingPlaygroundSynth: TestingPlayground),
# else the first's.
PKGNAME="${NAMES[0]}"
for n in "${NAMES[@]}"; do
  while [[ "${n#"$PKGNAME"}" == "$n" && -n "$PKGNAME" ]]; do PKGNAME="${PKGNAME%?}"; done
done
[[ ${#PKGNAME} -ge 4 ]] || PKGNAME="${NAMES[0]}"
VERSION="$(value "${WANT[0]}" VERSION)"
TITLE="${NAMES[0]}"
for ((i = 1; i < ${#NAMES[@]}; ++i)); do
  [[ $i -eq $((${#NAMES[@]} - 1)) ]] && TITLE+=" and " || TITLE+=", "
  TITLE+="${NAMES[$i]}"
done

DIST_XML="$STAGE/distribution.xml"
{
  echo '<?xml version="1.0" encoding="utf-8"?>'
  echo '<installer-gui-script minSpecVersion="2">'
  echo "    <title>$(html <<<"$TITLE")</title>"
  echo '    <welcome file="welcome.html" mime-type="text/html"/>'
  echo '    <conclusion file="conclusion.html" mime-type="text/html"/>'
  echo '    <domains enable_localSystem="true" enable_anywhere="false" enable_currentUserHome="false"/>'
  echo '    <options customize="allow" require-scripts="false" hostArchitectures="arm64"/>'
  echo '    <allowed-os-versions><os-version min="14.0"/></allowed-os-versions>'
  echo '    <choices-outline>'
  for k in "${WANT[@]}"; do
    echo "        <line choice=\"$k\">"
    for part in au vst3 app mcpb; do echo "            <line choice=\"$k.$part\"/>"; done
    echo '        </line>'
  done
  echo '    </choices-outline>'
  for k in "${WANT[@]}"; do
    NAME="$(value "$k" NAME)"; COMPANY="$(value "$k" COMPANY)"; ID="$(value "$k" BUNDLE_ID)"
    [[ "$(value "$k" KIND)" == "instrument" ]] && what="the synth" || what="the effect"
    echo "    <choice id=\"$k\" title=\"$NAME\" description=\"$NAME, $what: its plugins, its app and its Claude Desktop extension.\"/>"
    echo "    <choice id=\"$k.au\" title=\"Audio Unit\" description=\"$NAME.component, in /Library/Audio/Plug-Ins/Components (Logic, GarageBand, Live, ...).\"><pkg-ref id=\"$ID.pkg.au\"/></choice>"
    echo "    <choice id=\"$k.vst3\" title=\"VST3\" description=\"$NAME.vst3, in /Library/Audio/Plug-Ins/VST3.\"><pkg-ref id=\"$ID.pkg.vst3\"/></choice>"
    echo "    <choice id=\"$k.app\" title=\"App\" description=\"$NAME.app, in /Applications: $NAME on its own, without a host.\"><pkg-ref id=\"$ID.pkg.app\"/></choice>"
    echo "    <choice id=\"$k.mcpb\" title=\"Claude Desktop extension\" description=\"$NAME.mcpb, in /Library/Application Support/$(html <<<"$COMPANY")/$NAME: double-click it to add it to Claude Desktop.\"><pkg-ref id=\"$ID.pkg.mcpb\"/></choice>"
  done
  for p in "${PARTS[@]}"; do
    IFS='|' read -r k part _ _ pkgfile <<<"$p"
    echo "    <pkg-ref id=\"$(value "$k" BUNDLE_ID).pkg.$part\" version=\"$(value "$k" VERSION)\" onConclusion=\"none\">$pkgfile</pkg-ref>"
  done
  echo '</installer-gui-script>'
} > "$DIST_XML"

# The welcome and the last page (HTML, the installer's own fonts).
STYLE='<style>body{font-family:-apple-system,sans-serif;font-size:13px;line-height:1.45}code{font-size:12px}</style>'
{
  echo "<html><head><meta charset=\"utf-8\">$STYLE</head><body>"
  echo "<p>This installs $(html <<<"$TITLE") for every user of this Mac: the Audio Unit and VST3 plugins, the app, and the extension that lets Claude Desktop work with them. <b>Customize</b> chooses the parts of each.</p>"
  echo "<p>Each is a plugin whose DSP is a program you write, or ask Claude to write, while it plays.</p>"
  echo "</body></html>"
} > "$STAGE/resources/welcome.html"
{
  echo "<html><head><meta charset=\"utf-8\">$STYLE</head><body>"
  echo "<p>The plugins are in <code>/Library/Audio/Plug-Ins</code> (your host finds them at its next scan) and the apps in <code>/Applications</code>.</p>"
  echo "<p><b>Claude Desktop:</b> to let it work with a plugin, double-click its extension in the Finder (<b>Go &gt; Go to Folder…</b>, then paste the folder), and Claude Desktop offers to add it:</p><ul>"
  for k in "${WANT[@]}"; do
    NAME="$(value "$k" NAME)"; COMPANY="$(value "$k" COMPANY)"
    echo "<li><code>/Library/Application Support/$(html <<<"$COMPANY")/$NAME/$NAME.mcpb</code></li>"
  done
  echo "</ul><p>The installer does not open Claude Desktop itself. Then open a plugin in your host, or its app, and ask Claude for a sound.</p>"
  echo "</body></html>"
} > "$STAGE/resources/conclusion.html"

# ---- the product archive ---------------------------------------------------------------------------------------------------
OUT="$DIST/$PKGNAME-$VERSION.pkg"
rm -f "$OUT"
args=(--distribution "$DIST_XML" --resources "$STAGE/resources" --package-path "$STAGE/components")
[[ -n "$PKG_ID" ]] && args+=(--sign "$PKG_ID" --timestamp)
productbuild "${args[@]}" "$OUT" >/dev/null || die "productbuild failed"

if [[ -n "$NOTARY" ]]; then
  xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY" --wait || die "notarytool refused $OUT (xcrun notarytool log <id> says why)"
  xcrun stapler staple "$OUT" || die "stapler failed for $OUT"
  spctl --assess --type install -vv "$OUT" || die "Gatekeeper refuses $OUT"
fi

echo "wrote $OUT ($(du -h "$OUT" | cut -f1 | tr -d ' '))"
for p in "${PARTS[@]}"; do
  IFS='|' read -r k part src location _ <<<"$p"
  echo "  $k $part: $(basename "$src") -> $location"
done
if [[ -z "$APP_ID" ]]; then
  echo "unsigned: the bundles keep the build's ad hoc signatures and the .pkg is not signed (Gatekeeper asks before it opens)"
elif [[ -z "$PKG_ID" ]]; then
  echo "the bundles are signed by $APP_ID; the .pkg is not (--sign-installer)"
else
  echo "signed: the bundles by $APP_ID, the .pkg by $PKG_ID${NOTARY:+, notarized and stapled}"
fi
echo "inspect without installing: pkgutil --expand \"$OUT\" /tmp/pkg-x; pkgutil --payload-files \"$OUT\"; open it to install"
