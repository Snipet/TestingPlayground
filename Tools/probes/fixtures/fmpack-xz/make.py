#!/usr/bin/env python3
# Tools/probes/fixtures/fmpack-xz/make.py: writes this folder's fixtures (README.md says what each is). Run from
# anywhere with python3 and XZ Utils' `xz` on the PATH: `python3 Tools/probes/fixtures/fmpack-xz/make.py`. The
# projects are made up here; the packages are their JSON compressed by `xz` and wrapped in a .fmpack's 20-byte header
# (plugin/ProjectExport.h). The same xz writes the same bytes, so a run changes nothing; another xz may write others,
# which read the same.
import json
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

KINDS = {
    "effect": ("TestingPlayground", "module Effect", "void process(float* restrict left, float* restrict right, int n)"),
    "instrument": ("TestingPlaygroundSynth", "module Voice", "bool process(float* restrict left, float* restrict right, int n)"),
}

# Each package: (file suffix, xz's options).
PACKAGES = [
    ("crc64-6", ["-6", "--check=crc64"]),   # xz's defaults, and as Apple's COMPRESSION_LZMA most likely writes
    ("crc32", ["-6", "--check=crc32"]),
    ("sha256", ["-9e", "--check=sha256"]),
    ("none", ["-1", "--check=none"]),
]


def project(kind):
    plugin, module, process = KINDS[kind]
    versions = []
    for n in range(1, 4):
        body = "".join(f"    param float gain{k} [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n" for k in range(n * 3))
        program = (f"// Xz Fixture: a made-up program, version {n}\n{module}\n{{\n{body}"
                   f"    {process}\n    {{\n" + ("        return true;\n" if kind == "instrument" else "")
                   + "    }\n};\n")
        versions.append({
            "number": n,
            "saved": 1763000000000 + n * 60000,
            "note": f"made-up version {n}",
            "active": 0,
            "files": [{"name": "program.fm", "text": program}, {"name": "notes.fm", "text": "// notes\n" * n}],
            "dials": [{"name": f"gain{k}", "value": 0.25 + k / 100.0} for k in range(n * 3)],
        })
    return {
        "format": "funk-playground-project",
        "version": 1,
        "plugin": plugin,
        "kind": kind,
        "name": "Xz Fixture",
        "versions": versions,
        "presets": [{"name": "Half", "saved": 1763000300000, "from": 3, "dials": [{"name": "gain0", "value": 0.5}]}],
        "info": {"description": "A made-up project for plugin.export's xz rows.", "author": "", "category": "Test",
                 "tags": ["fixture"], "genres": []},
    }


def package(raw, body):
    return b"FMPK" + bytes([1, 2, 0, 0]) + struct.pack("<Q", len(raw)) + struct.pack("<I", zlib.crc32(raw)) + body


def xz(raw, options):
    return subprocess.run(["xz", "--format=xz", "--stdout", *options], input=raw, check=True, capture_output=True).stdout


def main():
    for kind in KINDS:
        raw = (json.dumps(project(kind), indent=2) + "\n").encode()
        with open(os.path.join(HERE, f"{kind}.json"), "wb") as f:
            f.write(raw)
        for suffix, options in PACKAGES:
            with open(os.path.join(HERE, f"{kind}-{suffix}.fmpack"), "wb") as f:
                f.write(package(raw, xz(raw, options)))
    # A filter the plugins' decoder lacks: x86's BCJ before LZMA2 (the effect's project; the decoder refuses it
    # before the kind matters).
    raw = open(os.path.join(HERE, "effect.json"), "rb").read()
    with open(os.path.join(HERE, "effect-x86.fmpack"), "wb") as f:
        f.write(package(raw, xz(raw, ["--x86", "--lzma2=preset=6", "--check=crc64"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
