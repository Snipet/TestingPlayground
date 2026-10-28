#!/usr/bin/env bash
# scripts/test.sh [product] [ctest args...]: build the probes and run them, for every product or the one named.
#
#   scripts/test.sh                      configure build-dev if needed, build every product's probes, run them all
#   scripts/test.sh synth                the synth's only (a product's key, products/<key>.cmake)
#   scripts/test.sh -R fminus.contract   one probe, for every product (any ctest argument is passed on)
#   scripts/test.sh effect -R gui.dump -V   one probe of one product, with every spec row printed
#
# The tests are <key>:<layer>.<name> (effect:dsp.swap), labelled with their product's key. Outputs:
# <build-dev>/probe-out/<key>/ (the WAVs, the PNGs of gui.dump), <build-dev>/probe-results/<key>/ (one JSON per
# probe). Run the probes through ctest only: outside CTest the plugin's stores are the user's own folders.
# The full gate is `cmake --workflow --preset dev-verify` (it also builds the plugins and runs the lints).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

KEY=""
if [[ $# -gt 0 && -f "products/$1.cmake" ]]; then
  KEY="$1"
  shift
fi

if [[ ! -f build-dev/CMakeCache.txt ]]; then
  cmake --preset dev
fi
if [[ -z "$KEY" ]]; then
  cmake --build --preset dev-probes
  ctest --preset dev-probes "$@"
  OUT="build-dev/probe-out"
else
  NAME="$(sed -n 's/^set(PRODUCT_NAME[[:space:]]*"\([^"]*\)".*/\1/p' "products/$KEY.cmake")"
  cmake --build --preset dev-probes --target "${NAME}_probes"
  ctest --preset dev-probes -L "^$KEY\$" "$@"
  OUT="build-dev/probe-out/$KEY"
fi

echo
echo "probe outputs in $ROOT/$OUT:"
ls -1 "$OUT" 2>/dev/null | sed 's/^/  /' || true
