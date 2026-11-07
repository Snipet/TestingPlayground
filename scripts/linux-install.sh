#!/usr/bin/env bash
# install.sh [--dry-run] [--uninstall] [product...]: installs the products of this Linux package (the folders under
# products/ beside this script; scripts/package-linux.sh makes the package) for you alone, in your home; nothing for
# other users, no root, no password:
#
#   VST3       products/<Name>/<Name>.vst3   ->  ~/.vst3/<Name>.vst3                  (where every Linux host looks)
#   app        products/<Name>/app/          ->  ~/.local/lib/<Company>/<Name>/       (the Standalone, its shaderc and
#              products/<Name>/server/       ->  ~/.local/lib/<Company>/<Name>/server/ licences; the MCP server, the GUI
#              products/<Name>/<Name>.mcpb   ->  ~/.local/lib/<Company>/<Name>/        sandbox's helper and the headless
#                                                                                      renderer; Claude Desktop's extension)
#   launchers  ~/.local/bin/<Name> and ~/.local/bin/<Name>Render (links into the app's folder)
#   menu       $XDG_DATA_HOME/applications/<id>.desktop and its icon in $XDG_DATA_HOME/icons/hicolor/<size>/apps/
#              ($XDG_DATA_HOME: ~/.local/share unless you set it)
#
#   ./install.sh                     every product in the package
#   ./install.sh TestingPlayground   one, by its name
#   ./install.sh --dry-run           say what it would do, change nothing
#   ./install.sh --uninstall         every product's copy installed so, to the Trash
#
# How it installs a folder: a copy beside its place (a hidden name, which no host loads), then one rename into place;
# what was there goes to your Trash ($XDG_DATA_HOME/Trash, as file managers keep it: restore it from there), so a
# failure leaves the old copy whole. Nothing is deleted. It never writes through a link: a part whose place is a link
# (to another disk, say) is refused and left alone; a launcher or a menu entry that is not one this script made is left
# alone, and said so. A staging copy a stopped run left behind goes to the Trash on the next run. It ends with how
# Claude Code (or another MCP client) reaches each product's MCP server; it changes no client's settings itself.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ME="install.sh"
die() { echo "$ME: $*" >&2; exit 1; }
note() { echo "$ME: $*"; }

UNINSTALL=0
DRY=0
ASKED=()
for arg in "$@"; do
  case "$arg" in
    -n|--dry-run) DRY=1 ;;
    --uninstall)  UNINSTALL=1 ;;
    -h|--help)    sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*)           die "unknown option $arg (--dry-run, --uninstall, --help)" ;;
    *)            ASKED+=("$arg") ;;
  esac
done

[[ "$(id -u)" != 0 ]] || die "run it as yourself, not as root: it installs for you, into your home"
[[ "${HOME:-}" == /* && -d "$HOME" ]] || die "HOME is not set to your home folder"
for v in HOME XDG_DATA_HOME; do
  [[ "${!v:-}" != *$'\n'* ]] || die "$v holds a line break"
done
DATA="${XDG_DATA_HOME:-}"
[[ "$DATA" == /* ]] || DATA="$HOME/.local/share"      # a relative XDG_DATA_HOME is ignored, as the specification says
VST3="$HOME/.vst3"
LIB="$HOME/.local/lib"
BIN="$HOME/.local/bin"
APPS="$DATA/applications"
ICONS="$DATA/icons/hicolor"
TRASH="$DATA/Trash"
SIZES=(16 32 128 256 512)

# ---- the products in this package: products/<Name>/product.env (KEY=value lines, read, never run) -------------------
declare -A P_ID P_VERSION P_COMPANY P_KIND P_MCP P_TITLE
NAMES=()
for dir in "$HERE"/products/*/; do
  [[ -f "$dir/product.env" ]] || continue
  name="$(basename "$dir")"
  [[ "$name" =~ ^[A-Za-z0-9._-]+$ ]] || die "products/$name: not a product's name"
  while IFS='=' read -r k v; do
    case "$k" in
      ID) P_ID[$name]="$v" ;; VERSION) P_VERSION[$name]="$v" ;; COMPANY) P_COMPANY[$name]="$v" ;;
      KIND) P_KIND[$name]="$v" ;; MCP) P_MCP[$name]="$v" ;; TITLE) P_TITLE[$name]="$v" ;;
    esac
  done < "$dir/product.env"
  for v in "${P_ID[$name]:-}" "${P_VERSION[$name]:-}" "${P_COMPANY[$name]:-}" "${P_KIND[$name]:-}" "${P_MCP[$name]:-}"; do
    [[ "$v" =~ ^[A-Za-z0-9._-]+$ ]] || die "products/$name/product.env: a value is missing or not plain"
  done
  [[ "${P_TITLE[$name]:-}" =~ ^[A-Za-z0-9\ ._-]*$ ]] || die "products/$name/product.env: its TITLE is not plain"
  NAMES+=("$name")
done
[[ ${#NAMES[@]} -gt 0 ]] || die "no products/ beside this script: run it from the unpacked package"
WANT=()
if [[ ${#ASKED[@]} -eq 0 ]]; then
  WANT=("${NAMES[@]}")
else
  for a in "${ASKED[@]}"; do
    found=""
    for n in "${NAMES[@]}"; do [[ "$a" == "$n" ]] && found="$n"; done
    [[ -n "$found" ]] || die "'$a' is not in this package (${NAMES[*]})"
    WANT+=("$found")
  done
fi

# ---- the Trash (the freedesktop.org specification: files/ and info/, an info file first) -----------------------------
urlencode() {   # a path as the Trash's info file holds it: percent-encoded bytes, '/' and the unreserved kept
  local LC_ALL=C s="$1" out="" c i h
  for ((i = 0; i < ${#s}; i++)); do
    c="${s:i:1}"
    case "$c" in
      [a-zA-Z0-9/._~-]) out+="$c" ;;
      *) printf -v h '%%%02X' "'$c"; out+="$h" ;;
    esac
  done
  printf '%s' "$out"
}
trash() {   # trash <path> [<the path it had>]: moved to the Trash, its info file naming where it was
  local p="$1" was="${2:-$1}" base name n=1
  if [[ $DRY -eq 1 ]]; then note "would move $was to the Trash"; return 0; fi
  mkdir -p -- "$TRASH/files" "$TRASH/info"
  chmod 700 -- "$TRASH" 2>/dev/null || true
  base="$(basename -- "$was")"
  name="$base"
  while [[ -e "$TRASH/files/$name" || -L "$TRASH/files/$name" || -e "$TRASH/info/$name.trashinfo" ]]; do
    n=$((n + 1))
    name="$base.$n"
  done
  ( set -o noclobber
    printf '[Trash Info]\nPath=%s\nDeletionDate=%s\n' "$(urlencode "$was")" "$(date +%Y-%m-%dT%H:%M:%S)" \
      > "$TRASH/info/$name.trashinfo" ) || die "cannot write the Trash's info for $was"
  if ! mv -T -- "$p" "$TRASH/files/$name"; then
    mv -f -- "$TRASH/info/$name.trashinfo" "$TRASH/info/.$name.failed" 2>/dev/null || true
    die "cannot move $was to the Trash"
  fi
}

# ---- placing a folder or a file: staged beside its place, renamed in, the old copy to the Trash ----------------------
leftovers() {   # a stopped run's staging copies of <place>, to the Trash
  local dst="$1" parent base f
  parent="$(dirname -- "$dst")"
  base="$(basename -- "$dst")"
  [[ -d "$parent" ]] || return 0
  for f in "$parent/.$base.installing-"* "$parent/.$base.old-"*; do
    [[ -e "$f" || -L "$f" ]] || continue
    trash "$f"
    [[ $DRY -eq 1 ]] || note "moved $f, left by an install that stopped, to the Trash"
  done
}
refused() {   # whether <place> may not be replaced: said, and nothing done
  local dst="$1"
  if [[ -L "$dst" ]]; then
    note "$dst is a link: left alone (remove the link, or install that part by hand)"
    return 0
  fi
  return 1
}
STAGE=""
staging() {   # staging <place>: STAGE, the hidden name beside <place> a copy is made under (its folder made, a stopped
              # run's copies to the Trash)
  local dst="$1"
  mkdir -p -- "$(dirname -- "$dst")"
  leftovers "$dst"
  STAGE="$(dirname -- "$dst")/.$(basename -- "$dst").installing-$$"
}
dry_place() {   # what placing <place> would do, said (a dry run)
  if [[ -e "$1" ]]; then note "would replace $1 (the old copy to the Trash)"; else note "would install $1"; fi
}
place() {   # place <source> <place>: a copy of <source> (a folder or a file) at <place>
  local src="$1" dst="$2"
  if [[ $DRY -eq 1 ]]; then dry_place "$dst"; return 0; fi
  staging "$dst"
  cp -a -- "$src" "$STAGE" || die "cannot copy $src to $(dirname -- "$dst")"
  swap_in "$STAGE" "$dst"
}
swap_in() {   # swap_in <staged copy> <place>: renamed into place, what was there to the Trash
  local stage="$1" dst="$2" parent base old
  parent="$(dirname -- "$dst")"
  base="$(basename -- "$dst")"
  if [[ -e "$dst" ]]; then
    old="$parent/.$base.old-$$"
    mv -T -- "$dst" "$old" || die "cannot move the old $dst aside (nothing changed)"
    if ! mv -T -- "$stage" "$dst"; then
      mv -T -- "$old" "$dst" || true
      die "cannot put the new $dst in place (the old copy is back)"
    fi
    trash "$old" "$dst"
    note "replaced $dst (the old copy is in the Trash)"
  else
    mv -T -- "$stage" "$dst" || die "cannot put $dst in place"
    note "installed $dst"
  fi
}

# A desktop entry's Exec value for one path (the specification's quoting: in double quotes, '"', '`', '$' and '\'
# escaped; then its string escape, which doubles every backslash; '%' doubled).
exec_quote() {
  local s="$1" out="" c i
  for ((i = 0; i < ${#s}; i++)); do
    c="${s:i:1}"
    case "$c" in
      '"'|'`'|'$') out+='\\'"$c" ;;
      '\') out+='\\\\' ;;
      '%') out+='%%' ;;
      *) out+="$c" ;;
    esac
  done
  printf '"%s"' "$out"
}
ours() {   # whether <file> is a menu entry this script made for <name>
  [[ -f "$1" && ! -L "$1" ]] && grep -qx "X-Funk-Product=$2" -- "$1"
}

# ---- install ------------------------------------------------------------------------------------------------------
install_one() {
  local name="$1" src="$HERE/products/$1" company="${P_COMPANY[$1]}" id="${P_ID[$1]}" home desktop s link at to
  home="$LIB/$company/$name"
  [[ -d "$src/$name.vst3" && -x "$src/app/$name" && -d "$src/server" ]] || die "products/$name is incomplete"
  # The VST3.
  refused "$VST3/$name.vst3" || place "$src/$name.vst3" "$VST3/$name.vst3"
  # The app's folder, assembled under its staging name beside its place.
  if ! refused "$home"; then
    if [[ $DRY -eq 1 ]]; then
      dry_place "$home"
    else
      staging "$home"
      mkdir -- "$STAGE"
      cp -a -- "$src/app/." "$STAGE/"
      cp -a -- "$src/server" "$STAGE/server"
      if [[ -f "$src/$name.mcpb" ]]; then cp -a -- "$src/$name.mcpb" "$STAGE/"; fi
      cp -a -- "$src/product.env" "$STAGE/"
      swap_in "$STAGE" "$home"
    fi
  fi
  # The launchers: links into the app's folder (one that is not such a link is left alone).
  for link in "$name:$home/$name" "${name}Render:$home/server/${name}Render"; do
    at="$BIN/${link%%:*}"
    to="${link#*:}"
    if [[ -e "$at" || -L "$at" ]] && ! { [[ -L "$at" ]] && [[ "$(readlink -- "$at")" == "$LIB/$company/$name/"* ]]; }; then
      note "$at is not a link this script made: left alone"
      continue
    fi
    if [[ $DRY -eq 1 ]]; then note "would link $at to $to"; continue; fi
    mkdir -p -- "$BIN"
    ln -sfn -- "$to" "$at"
    note "linked $at"
  done
  # The menu entry and its icons.
  desktop="$APPS/$id.desktop"
  if [[ -e "$desktop" || -L "$desktop" ]] && ! ours "$desktop" "$name"; then
    note "$desktop is not an entry this script made: left alone"
  elif ! refused "$desktop"; then
    if [[ $DRY -eq 1 ]]; then
      dry_place "$desktop"
    else
      staging "$desktop"
      {
      echo "[Desktop Entry]"
      echo "Type=Application"
      echo "Name=$name"
      echo "GenericName=${P_TITLE[$name]:-$name}"
      echo "Comment=${P_TITLE[$name]:-$name}: write its sound as a program, with Claude"
      echo "Exec=$(exec_quote "$home/$name")"
      echo "Icon=$id"
      echo "Terminal=false"
      echo "Categories=AudioVideo;Audio;"
      echo "StartupNotify=true"
      echo "X-Funk-Product=$name"
      } > "$STAGE"
      chmod 644 -- "$STAGE"
      swap_in "$STAGE" "$desktop"
    fi
    for s in "${SIZES[@]}"; do
      [[ -f "$src/icons/$s.png" ]] || continue
      refused "$ICONS/${s}x$s/apps/$id.png" || place "$src/icons/$s.png" "$ICONS/${s}x$s/apps/$id.png"
    done
  fi
}

# ---- uninstall ----------------------------------------------------------------------------------------------------
uninstall_one() {
  local name="$1" company="${P_COMPANY[$1]}" id="${P_ID[$1]}" home s any=0 at
  home="$LIB/$company/$name"
  for at in "$VST3/$name.vst3" "$home"; do
    if [[ -L "$at" ]]; then note "$at is a link: left alone"; continue; fi
    if [[ -d "$at" ]]; then
      leftovers "$at"
      trash "$at"
      [[ $DRY -eq 1 ]] || note "moved $at to the Trash"
      any=1
    fi
  done
  for at in "$BIN/$name" "$BIN/${name}Render"; do
    if [[ -L "$at" && "$(readlink -- "$at")" == "$home/"* ]]; then
      if [[ $DRY -eq 1 ]]; then note "would remove the link $at"; else rm -f -- "$at"; note "removed the link $at"; fi
      any=1
    fi
  done
  if ours "$APPS/$id.desktop" "$name"; then
    trash "$APPS/$id.desktop"
    [[ $DRY -eq 1 ]] || note "moved $APPS/$id.desktop and its icons to the Trash"
    any=1
    for s in "${SIZES[@]}"; do
      at="$ICONS/${s}x$s/apps/$id.png"
      [[ -f "$at" && ! -L "$at" ]] && trash "$at"
    done
  fi
  [[ $any -eq 1 ]] || note "$name: nothing installed by this script found"
}

for name in "${WANT[@]}"; do
  if [[ $UNINSTALL -eq 1 ]]; then
    uninstall_one "$name"
  else
    install_one "$name"
  fi
done
[[ $UNINSTALL -eq 1 || $DRY -eq 1 ]] && exit 0

echo
echo "Done. Rescan plugins in your DAW to find the VST3 (~/.vst3); the apps are in your desktop's menu."
[[ ":$PATH:" == *":$BIN:"* ]] || echo "($BIN is not on your PATH: add it to run the apps by name from a terminal.)"
echo
echo "Claude Code (or another MCP client) reaches an open plugin or app through its MCP server. Once, for Claude Code:"
for name in "${WANT[@]}"; do
  printf '  claude mcp add --scope user %s -- %q\n' "${P_MCP[$name]}" "$LIB/${P_COMPANY[$name]}/$name/server/${name}MCP"
done
echo "Another client takes the same program as a stdio server (its command, no arguments)."
for name in "${WANT[@]}"; do
  [[ -f "$LIB/${P_COMPANY[$name]}/$name/$name.mcpb" ]] || continue
  echo "Claude Desktop, where it runs on Linux, takes $LIB/${P_COMPANY[$name]}/$name/$name.mcpb as an extension (open it there)."
done
