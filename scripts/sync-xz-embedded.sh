#!/usr/bin/env bash
# scripts/sync-xz-embedded.sh <tag> [--force]
#
# Refreshes the vendored xz decoder, Source/xz/embedded/**, from XZ Embedded (the XZ Utils project's small .xz
# decoder, 0BSD) at a tag, and rewrites Source/xz/SEED.tsv (one row per vendored file: path, source, tag, commit,
# sha256, the day it was taken).
#
#   scripts/sync-xz-embedded.sh v2024-12-30                       # what this project vendors
#   XZ_EMBEDDED_REPO=/path/to/xz-embedded scripts/sync-xz-embedded.sh <tag>   # from a clone already here
#
# - XZ_EMBEDDED_REPO is a clone of https://github.com/tukaani-project/xz-embedded (it is read with `git archive` only,
#   never checked out or written). Without it the script clones that address into a temporary folder.
# - Only the decoder of .xz streams is taken, unmodified: its public header (xz.h), its private headers, the stream
#   and LZMA2 decoders and the CRC32, CRC64 and SHA-256 checks, with COPYING and AUTHORS. Not taken: the BCJ filters
#   (no package is written with one: a stream that has one is refused), MicroLZMA, the kernel's glue, the userspace
#   xz_config.h (ours is Source/xz/xz_config.h, the build's options Source/xz/XzOptions.h).
# - Local edits are protected: when a vendored file no longer matches the sha256 SEED.tsv recorded for it, or a file
#   was added under Source/xz/embedded, the script lists them and stops. --force overwrites them.
# - Afterwards: build and test (plugin.export reads xz packages with it), read the diff, commit.
set -euo pipefail

usage() { awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"; }

TAG=""
FORCE=0
for arg in "$@"; do
  case "$arg" in
    --force) FORCE=1 ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "sync-xz-embedded: unknown option $arg" >&2; exit 2 ;;
    *) if [[ -n "$TAG" ]]; then echo "sync-xz-embedded: one tag only" >&2; exit 2; fi; TAG="$arg" ;;
  esac
done
if [[ -z "$TAG" ]]; then
  usage >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
URL="https://github.com/tukaani-project/xz-embedded"
SUBDIR="Source/xz/embedded"
SEED="$ROOT/Source/xz/SEED.tsv"
# Each file taken: its path in XZ Embedded's tree.
FILES=(
  COPYING
  AUTHORS
  linux/include/linux/xz.h
  linux/lib/xz/xz_private.h
  linux/lib/xz/xz_stream.h
  linux/lib/xz/xz_lzma2.h
  linux/lib/xz/xz_crc32.c
  linux/lib/xz/xz_crc64.c
  linux/lib/xz/xz_sha256.c
  linux/lib/xz/xz_dec_stream.c
  linux/lib/xz/xz_dec_lzma2.c
)

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

REPO="${XZ_EMBEDDED_REPO:-}"
if [[ -z "$REPO" ]]; then
  git clone --quiet "$URL" "$TMP/repo"
  REPO="$TMP/repo"
fi
COMMIT="$(git -C "$REPO" rev-parse "$TAG^{commit}")"

# 1. Local edits: every vendored file must still match SEED.tsv, and nothing may have been added beside them.
if [[ -d "$ROOT/$SUBDIR" && $FORCE -eq 0 ]]; then
  : > "$TMP/changed"
  : > "$TMP/recorded"
  if [[ -f "$SEED" ]]; then
    while IFS=$'\t' read -r path _ _ _ sum _; do
      [[ "$path" == \#* || "$path" == path ]] && continue
      echo "$path" >> "$TMP/recorded"
      if [[ ! -f "$ROOT/$path" ]] || [[ "$(sha256 "$ROOT/$path")" != "$sum" ]]; then
        echo "$path" >> "$TMP/changed"
      fi
    done < "$SEED"
  fi
  while IFS= read -r f; do
    rel="${f#"$ROOT"/}"
    grep -qxF "$rel" "$TMP/recorded" || printf '%s (not in SEED.tsv)\n' "$rel" >> "$TMP/changed"
  done < <(find "$ROOT/$SUBDIR" -type f)
  if [[ -s "$TMP/changed" ]]; then
    echo "sync-xz-embedded: these vendored files were changed or added here (--force overwrites them):" >&2
    sed 's/^/  /' "$TMP/changed" >&2
    exit 1
  fi
fi

# 2. The files, flat, as they are at the tag.
mkdir -p "$TMP/export"
git -C "$REPO" archive --format=tar "$COMMIT" "${FILES[@]}" | tar -x -C "$TMP/export"
rm -rf "${ROOT:?}/$SUBDIR"
mkdir -p "$ROOT/$SUBDIR"
for f in "${FILES[@]}"; do
  cp "$TMP/export/$f" "$ROOT/$SUBDIR/$(basename "$f")"
done

# 3. SEED.tsv: one row per vendored file, sorted by path.
DAY="$(date -u +%Y-%m-%d)"
{
  printf '# Vendored from %s by scripts/sync-xz-embedded.sh. Do not edit by hand: rerun the script.\n' "$URL"
  printf 'path\tsource\ttag\tcommit\tsha256\tday\n'
  while IFS= read -r f; do
    rel="${f#"$ROOT"/}"
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$rel" "xz-embedded" "$TAG" "$COMMIT" "$(sha256 "$f")" "$DAY"
  done < <(find "$ROOT/$SUBDIR" -type f | LC_ALL=C sort)
} > "$SEED"

n="$(find "$ROOT/$SUBDIR" -type f | wc -l | tr -d ' ')"
echo "sync-xz-embedded: $SUBDIR <- xz-embedded $TAG ($COMMIT), $n files; Source/xz/SEED.tsv rewritten."
