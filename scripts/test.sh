#!/usr/bin/env bash
# scripts/test.sh [ctest args...]: build the probes and run them.
#
#   scripts/test.sh                      configure build-dev if needed, build the probe executable, run every probe
#   scripts/test.sh -R dsp.response      one probe (any ctest argument is passed on)
#   scripts/test.sh -V                   with every spec row printed
#
# Outputs: <build-dev>/probe-out/ (the WAVs of dsp.render, the PNGs of gui.dump), <build-dev>/probe-results/ (one JSON
# per probe). The probe executable runs a probe by hand too: build-dev/<Name>_probes --list.
# The full gate is `cmake --workflow --preset dev-verify` (it also builds the plugin and runs lint.layers).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ ! -f build-dev/CMakeCache.txt ]]; then
  cmake --preset dev
fi
cmake --build --preset dev-probes
ctest --preset dev-probes "$@"

echo
echo "probe outputs in $ROOT/build-dev/probe-out:"
ls -1 build-dev/probe-out 2>/dev/null | sed 's/^/  /' || true
