#!/usr/bin/env bash
# scripts/sync-dsp-core.sh <fsynth-tag> [--force]
#
# Refreshes the vendored DSP core, Source/fsdsp/core/**, from FSynth at a tag, and rewrites SEED.tsv (one row per
# vendored file: path, source repository, tag, commit, sha256).
#
#   scripts/sync-dsp-core.sh fsyn-s3            # what this project was seeded from
#   FSYNTH_REPO=/path/to/FSynth scripts/sync-dsp-core.sh <tag>   # a later fsyn-sN, from another checkout
#
# - FSynth is read with `git archive` only (never checked out, never written). FSYNTH_REPO defaults to
#   ~/audio/plugins/FSynth.
# - Local edits are protected: when a vendored file no longer matches the sha256 SEED.tsv recorded for it (you tweaked
#   it), or a file was added under Source/fsdsp/core, the script lists them and stops. --force overwrites them.
# - Only Source/fsdsp/core is taken: it is self-contained (std and NEON intrinsics only). The rest of fsdsp (engine,
#   graph, voices, Modes) depends on FSynth's parameter and engine layers and is not vendored.
# - Afterwards: build and test (scripts/test.sh), read the diff (git diff --stat Source/fsdsp SEED.tsv), commit.
set -euo pipefail

# The comment block above, from line 2 to the first line that is not a comment.
usage() { awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"; }

TAG=""
FORCE=0
for arg in "$@"; do
  case "$arg" in
    --force) FORCE=1 ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "sync-dsp-core: unknown option $arg" >&2; exit 2 ;;
    *) if [[ -n "$TAG" ]]; then echo "sync-dsp-core: one tag only" >&2; exit 2; fi; TAG="$arg" ;;
  esac
done
if [[ -z "$TAG" ]]; then
  usage >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="${FSYNTH_REPO:-$HOME/audio/plugins/FSynth}"
SUBDIR="Source/fsdsp/core"
SEED="$ROOT/SEED.tsv"

if ! git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1; then
  echo "sync-dsp-core: $REPO is not a git repository (set FSYNTH_REPO)" >&2
  exit 1
fi
if ! COMMIT="$(git -C "$REPO" rev-parse -q --verify "$TAG^{commit}")"; then
  echo "sync-dsp-core: $REPO has no tag or commit '$TAG'" >&2
  exit 1
fi
if ! git -C "$REPO" cat-file -e "$COMMIT:$SUBDIR" 2>/dev/null; then
  echo "sync-dsp-core: $TAG has no $SUBDIR" >&2
  exit 1
fi

sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/sync-dsp-core.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# 1. Local edits: every vendored file must still match SEED.tsv, and nothing may have been added beside them.
#    (Plain bash 3.2, as macOS ships it: no associative arrays.)
if [[ -d "$ROOT/$SUBDIR" && $FORCE -eq 0 ]]; then
  : > "$TMP/changed"
  : > "$TMP/recorded"
  if [[ -f "$SEED" ]]; then
    while IFS=$'\t' read -r path _repo _tag _commit sum; do
      [[ "$path" == "path" || "$path" == \#* || -z "$path" ]] && continue
      printf '%s\n' "$path" >> "$TMP/recorded"
      if [[ ! -f "$ROOT/$path" ]]; then
        printf '%s (deleted)\n' "$path" >> "$TMP/changed"
      elif [[ "$(sha256 "$ROOT/$path")" != "$sum" ]]; then
        printf '%s (edited)\n' "$path" >> "$TMP/changed"
      fi
    done < "$SEED"
  fi
  find "$ROOT/$SUBDIR" -type f ! -name .DS_Store | LC_ALL=C sort | while IFS= read -r f; do
    rel="${f#"$ROOT"/}"
    grep -qxF "$rel" "$TMP/recorded" || printf '%s (not in SEED.tsv)\n' "$rel" >> "$TMP/changed"
  done
  if [[ -s "$TMP/changed" ]]; then
    echo "sync-dsp-core: the vendored core has local changes; they would be overwritten:" >&2
    sed 's/^/  /' "$TMP/changed" >&2
    echo "Keep them (copy them aside, or upstream them to FSynth), or rerun with --force." >&2
    exit 1
  fi
fi

# 2. Export the tag's core into the scratch directory, then replace ours with it.
mkdir -p "$TMP/export"
git -C "$REPO" archive --format=tar "$COMMIT" "$SUBDIR" | tar -x -C "$TMP/export"
rm -rf "${ROOT:?}/$SUBDIR"
mkdir -p "$(dirname "$ROOT/$SUBDIR")"
cp -R "$TMP/export/$SUBDIR" "$ROOT/$SUBDIR"

# 3. SEED.tsv: one row per vendored file, sorted by path.
{
  printf '# Vendored from FSynth by scripts/sync-dsp-core.sh. Do not edit by hand: rerun the script.\n'
  printf 'path\tsource\ttag\tcommit\tsha256\n'
  while IFS= read -r f; do
    rel="${f#"$ROOT"/}"
    printf '%s\t%s\t%s\t%s\t%s\n' "$rel" "FSynth" "$TAG" "$COMMIT" "$(sha256 "$f")"
  done < <(find "$ROOT/$SUBDIR" -type f | LC_ALL=C sort)
} > "$SEED"

n="$(find "$ROOT/$SUBDIR" -type f | wc -l | tr -d ' ')"
echo "sync-dsp-core: $SUBDIR <- FSynth $TAG ($COMMIT), $n files; SEED.tsv rewritten."
if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  git -C "$ROOT" status --short -- "$SUBDIR" SEED.tsv || true
fi
