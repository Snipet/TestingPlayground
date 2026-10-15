#!/usr/bin/env python3
"""
Plugin utility — lifecycle tasks for a JUCE / visage plugin built from this
template. Subcommands:

  clone      Clone this plugin into a new plugin (interactive).
  rename     Rename the current plugin in place (interactive).
  codesign   Codesign built AU / VST3 / Standalone artefacts with a
             Developer ID Application identity.
  notarize   Submit codesigned artefacts to Apple notary and staple
             the resulting tickets.


This file is copied into every cloned plugin so that the cloned plugin can
itself be cloned, renamed, signed, and notarized from the same script.

Plugin name, codes, namespace, etc. are auto-discovered from the local
CMakeLists.txt and Source/dsp headers — the script works from any plugin
produced by this template, not just the original template directory.

Stdlib only. No pip install needed.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable

# ──────────────────────────────────────────────────────────────────────────
# Project layout
# ──────────────────────────────────────────────────────────────────────────

SCRIPT_PATH = Path(__file__).resolve()
PROJECT_ROOT = SCRIPT_PATH.parent.parent

# Top-level entries skipped when copying for a clone.
COPY_EXCLUDES = {
    "build", ".git", ".DS_Store", "__pycache__",
    ".idea", ".vscode", "node_modules",
}

# File suffixes treated as text for bulk string replacement.
TEXT_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp",
    ".m", ".mm",
    ".cmake", ".txt", ".md",
    ".py", ".sh",
    ".json", ".plist", ".xml",
    ".sc", ".sh",  # bgfx shader sources + includes
    ".gitignore",
}

PLUGIN_TYPES = ("effect", "instrument", "midi")


# ──────────────────────────────────────────────────────────────────────────
# Prompts and validation
# ──────────────────────────────────────────────────────────────────────────

def ask(prompt: str, default: str | None = None,
        validate: Callable[[str], str | None] | None = None) -> str:
    suffix = f" [{default}]" if default is not None else ""
    while True:
        raw = input(f"{prompt}{suffix}: ").strip()
        value = raw if raw else (default or "")
        if not value:
            print("  Required, please enter a value.")
            continue
        if validate is not None:
            err = validate(value)
            if err:
                print(f"  Invalid: {err}")
                continue
        return value


def ask_choice(prompt: str, options: tuple[str, ...], default: str | None = None) -> str:
    opts = "/".join(options)
    suffix = f" [{default}]" if default else ""
    while True:
        raw = input(f"{prompt} ({opts}){suffix}: ").strip().lower()
        value = raw if raw else (default or "").lower()
        if value in options:
            return value
        print(f"  Choose one of: {opts}")


def ask_yes_no(prompt: str, default: bool = True) -> bool:
    flag = "Y/n" if default else "y/N"
    while True:
        raw = input(f"{prompt} [{flag}]: ").strip().lower()
        if not raw:
            return default
        if raw in ("y", "yes"): return True
        if raw in ("n", "no"):  return False


def is_pascal_ident(name: str) -> str | None:
    if not name or not name[0].isalpha() or not name[0].isupper():
        return "must start with an uppercase letter"
    if not all(c.isalnum() or c == "_" for c in name):
        return "only letters, digits and underscores allowed"
    return None


def is_lower_ident(name: str) -> str | None:
    if not name or not name[0].isalpha():
        return "must start with a letter"
    if not name.islower():
        return "must be all lowercase"
    if not all(c.isalnum() or c == "_" for c in name):
        return "only letters, digits and underscores allowed"
    return None


def is_four_char(code: str) -> str | None:
    if len(code) != 4:
        return "must be exactly 4 characters"
    if not code.isalnum():
        return "must be alphanumeric"
    if not any(c.isupper() for c in code):
        return "must contain at least one uppercase letter (Apple convention)"
    return None


def is_bundle_id(bid: str) -> str | None:
    parts = bid.split(".")
    if len(parts) < 2:
        return "must be reverse-DNS (e.g. com.acme.plugin)"
    for p in parts:
        if not p:
            return "empty segment"
        if not p[0].isalpha():
            return "each segment must start with a letter"
        if not all(c.isalnum() or c in "-_" for c in p):
            return "segments may only contain letters, digits, hyphens, underscores"
    return None


# ──────────────────────────────────────────────────────────────────────────
# Derivations
# ──────────────────────────────────────────────────────────────────────────

def derive_plugin_code(name: str) -> str:
    """Apple 4-char code: 1 uppercase + 3 lowercase. Prefer consonants."""
    letters = [c for c in name if c.isalpha()]
    if not letters:
        return "Plug"
    first = letters[0].upper()
    rest = letters[1:]
    consonants = [c for c in rest if c.lower() not in "aeiou"]
    pool = consonants if len(consonants) >= 3 else rest
    tail = "".join(pool[:3]).lower()
    return (first + tail).ljust(4, "x")[:4]


def lower_alnum(name: str) -> str:
    return "".join(c for c in name if c.isalnum()).lower()


# ──────────────────────────────────────────────────────────────────────────
# File ops
# ──────────────────────────────────────────────────────────────────────────

def is_text_file(path: Path) -> bool:
    if path.suffix.lower() in TEXT_SUFFIXES:
        return True
    if path.name in {"CMakeLists.txt", ".gitignore"}:
        return True
    return False


def copy_template(src: Path, dst: Path) -> None:
    def ignore(_directory: str, contents: list[str]) -> list[str]:
        return [c for c in contents if c in COPY_EXCLUDES]
    shutil.copytree(src, dst, ignore=ignore)


def apply_text_replacements(root: Path, rules: list[tuple[str, str]]) -> int:
    """Apply ordered (old, new) replacements to every text file under root."""
    modified = 0
    for path in root.rglob("*"):
        if not path.is_file() or not is_text_file(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        new = text
        for old, repl in rules:
            if old and old != repl:
                new = new.replace(old, repl)
        if new != text:
            path.write_text(new, encoding="utf-8")
            modified += 1
    return modified


# ──────────────────────────────────────────────────────────────────────────
# Discover current plugin config from CMakeLists.txt + Source/dsp headers
# ──────────────────────────────────────────────────────────────────────────

def discover_config(root: Path) -> dict[str, str | None]:
    cml = root / "CMakeLists.txt"
    if not cml.exists():
        sys.exit(f"No CMakeLists.txt at {root}")
    text = cml.read_text(encoding="utf-8")

    def find(pattern: str) -> str | None:
        m = re.search(pattern, text)
        return m.group(1) if m else None

    name        = find(r"juce_add_plugin\s*\(\s*(\w+)")
    company     = find(r'COMPANY_NAME\s+"([^"]+)"')
    mfr_code    = find(r"PLUGIN_MANUFACTURER_CODE\s+(\w+)")
    plugin_code = find(r"PLUGIN_CODE\s+(\w+)")
    bundle_id   = find(r"BUNDLE_ID\s+(\S+)")

    # DSP namespace from first matching `namespace foo {` in Source/dsp/.
    namespace: str | None = None
    dsp_dir = root / "Source" / "dsp"
    if dsp_dir.is_dir():
        for header in sorted(dsp_dir.glob("*.h")):
            m = re.search(r"^namespace\s+(\w+)", header.read_text(encoding="utf-8"),
                          re.MULTILINE)
            if m:
                namespace = m.group(1)
                break

    return {
        "name":        name,
        "company":     company,
        "mfr_code":    mfr_code,
        "plugin_code": plugin_code,
        "bundle_id":   bundle_id,
        "namespace":   namespace,
    }


def require_config(cfg: dict[str, str | None]) -> None:
    missing = [k for k, v in cfg.items() if not v]
    if missing:
        sys.exit(f"Could not parse plugin config (missing: {', '.join(missing)}). "
                 f"Is this a plugin directory built from the template?")


# ──────────────────────────────────────────────────────────────────────────
# Plugin-type transformation
#
# Buses + isBusesLayoutSupported bodies are full blocks so we can recognise
# the current state regardless of how the plugin was previously configured.
# ──────────────────────────────────────────────────────────────────────────

BUSES_EFFECT = """    : AudioProcessor(BusesProperties()
                         .withInput("Input",  juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),"""

BUSES_INSTRUMENT = """    : AudioProcessor(BusesProperties()
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),"""

BUSES_MIDI = """    : AudioProcessor(BusesProperties()),"""

BUSES_BLOCKS = {
    "effect":     BUSES_EFFECT,
    "instrument": BUSES_INSTRUMENT,
    "midi":       BUSES_MIDI,
}


def _layout_block(name: str, ptype: str) -> str:
    if ptype == "effect":
        return (
            f"bool {name}Processor::isBusesLayoutSupported(const BusesLayout& layouts) const\n"
            f"{{\n"
            f"    const auto& in  = layouts.getMainInputChannelSet();\n"
            f"    const auto& out = layouts.getMainOutputChannelSet();\n"
            f"    return in == out && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());\n"
            f"}}"
        )
    if ptype == "instrument":
        return (
            f"bool {name}Processor::isBusesLayoutSupported(const BusesLayout& layouts) const\n"
            f"{{\n"
            f"    const auto& out = layouts.getMainOutputChannelSet();\n"
            f"    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();\n"
            f"}}"
        )
    # midi
    return (
        f"bool {name}Processor::isBusesLayoutSupported(const BusesLayout& /*layouts*/) const\n"
        f"{{\n"
        f"    return true;\n"
        f"}}"
    )


def set_plugin_type(root: Path, ptype: str, pascal_name: str) -> None:
    """Reconfigure CMakeLists + PluginProcessor.{h,cpp} for the target type."""
    if ptype not in PLUGIN_TYPES:
        raise ValueError(f"unknown plugin type: {ptype}")

    cml_path = root / "CMakeLists.txt"
    pph_path = root / "Source" / "PluginProcessor.h"
    ppc_path = root / "Source" / "PluginProcessor.cpp"

    cml = cml_path.read_text(encoding="utf-8")
    pph = pph_path.read_text(encoding="utf-8")
    ppc = ppc_path.read_text(encoding="utf-8")

    # CMake flags.
    is_synth        = "TRUE" if ptype == "instrument" else "FALSE"
    is_midi_effect  = ptype == "midi"
    needs_midi_in   = "TRUE" if ptype in ("instrument", "midi") else "FALSE"
    needs_midi_out  = "TRUE" if ptype == "midi" else "FALSE"

    cml = re.sub(r"IS_SYNTH\s+\w+",          f"IS_SYNTH                {is_synth}",       cml)
    cml = re.sub(r"NEEDS_MIDI_INPUT\s+\w+",  f"NEEDS_MIDI_INPUT        {needs_midi_in}",  cml)
    cml = re.sub(r"NEEDS_MIDI_OUTPUT\s+\w+", f"NEEDS_MIDI_OUTPUT       {needs_midi_out}", cml)

    # Add or remove the IS_MIDI_EFFECT line as needed.
    has_midi_effect_line = re.search(r"IS_MIDI_EFFECT\s+\w+", cml) is not None
    if is_midi_effect and not has_midi_effect_line:
        cml = re.sub(r"(IS_SYNTH\s+\w+\n)", r"\1    IS_MIDI_EFFECT          TRUE\n", cml)
    elif not is_midi_effect and has_midi_effect_line:
        cml = re.sub(r"\s*IS_MIDI_EFFECT\s+\w+\n", "\n", cml)

    cml_path.write_text(cml, encoding="utf-8")

    # PluginProcessor.h — acceptsMidi / producesMidi / isMidiEffect.
    accepts  = "true" if ptype in ("instrument", "midi") else "false"
    produces = "true" if ptype == "midi" else "false"
    midi_fx  = "true" if ptype == "midi" else "false"

    pph = re.sub(r"(bool acceptsMidi\(\)\s*const override \{ return )\w+;",
                 r"\g<1>" + accepts + ";", pph)
    pph = re.sub(r"(bool producesMidi\(\) const override \{ return )\w+;",
                 r"\g<1>" + produces + ";", pph)
    pph = re.sub(r"(bool isMidiEffect\(\) const override \{ return )\w+;",
                 r"\g<1>" + midi_fx + ";", pph)
    pph_path.write_text(pph, encoding="utf-8")

    # PluginProcessor.cpp — buses + layout block. We don't know the current
    # state so try each known variant.
    target_buses = BUSES_BLOCKS[ptype]
    for variant in BUSES_BLOCKS.values():
        if variant in ppc:
            ppc = ppc.replace(variant, target_buses)
            break

    target_layout = _layout_block(pascal_name, ptype)
    for variant_type in PLUGIN_TYPES:
        old = _layout_block(pascal_name, variant_type)
        if old in ppc:
            ppc = ppc.replace(old, target_layout)
            break

    ppc_path.write_text(ppc, encoding="utf-8")


# ──────────────────────────────────────────────────────────────────────────
# clone
# ──────────────────────────────────────────────────────────────────────────

def cmd_clone(args: argparse.Namespace) -> int:
    src_root = PROJECT_ROOT
    cfg = discover_config(src_root)
    require_config(cfg)

    print(f"\nCloning {cfg['name']} (from {src_root})\n")

    new_name = ask("Plugin name (PascalCase, e.g. MyDelay)",
                   validate=is_pascal_ident)
    new_name_lower = lower_alnum(new_name)

    new_type = ask_choice("Plugin type", PLUGIN_TYPES, default="effect")

    new_company = ask("Company name", default=cfg["company"] or "Funk")
    new_company_lower = lower_alnum(new_company)

    mfr_default = (cfg["mfr_code"] or
                   ((new_company[:1].upper() + new_company[1:4]).ljust(4, "x"))[:4])
    new_mfr = ask("Manufacturer code (4 chars, 1+ uppercase)",
                  default=mfr_default, validate=is_four_char)

    new_code = ask("Plugin code (4 chars, 1 uppercase + 3 lowercase)",
                   default=derive_plugin_code(new_name), validate=is_four_char)

    new_bundle = ask("Bundle ID",
                     default=f"com.{new_company_lower}.{new_name_lower}",
                     validate=is_bundle_id)

    new_namespace = ask("DSP namespace",
                        default=new_name_lower, validate=is_lower_ident)

    default_dst = src_root.parent / new_name
    raw = input(f"Destination directory [{default_dst}]: ").strip()
    dst = Path(raw).expanduser().resolve() if raw else default_dst

    print()
    print("Summary:")
    print(f"  Name:         {new_name}")
    print(f"  Type:         {new_type}")
    print(f"  Company:      {new_company}")
    print(f"  Mfr code:     {new_mfr}")
    print(f"  Plugin code:  {new_code}")
    print(f"  Bundle ID:    {new_bundle}")
    print(f"  DSP namespace: {new_namespace}")
    print(f"  Destination:  {dst}")
    print()

    if dst.exists():
        if not ask_yes_no(f"{dst} exists. Remove and overwrite?", default=False):
            print("Aborted.")
            return 1
        shutil.rmtree(dst)

    if not ask_yes_no("Proceed?", default=True):
        print("Aborted.")
        return 1

    copy_template(src_root, dst)

    # Create a fresh, empty build directory so the clone is ready for
    # `cmake -B build` without any stale artefacts from the source tree.
    (dst / "build").mkdir(exist_ok=True)

    # Bulk string rules. Sort by descending pattern length so longer patterns
    # match before any substring they contain (e.g. bundle id before lowercase
    # plugin name; lowercase plugin name before namespace if they happen to
    # overlap).
    rules: list[tuple[str, str]] = [
        (cfg["bundle_id"],        new_bundle),
        (cfg["name"],             new_name),
        (cfg["name"].lower(),     new_name_lower),
        (cfg["namespace"],        new_namespace),
        (cfg["plugin_code"],      new_code),
    ]
    rules.sort(key=lambda r: -len(r[0]))

    # Dedupe (e.g. name.lower() == namespace).
    seen, ordered = set(), []
    for old, new in rules:
        if old in seen:
            continue
        seen.add(old)
        ordered.append((old, new))

    # Company name + manufacturer code: line-specific, not bulk — these
    # strings are short and could appear elsewhere (e.g. "Funk" inside
    # "testingplayground" wouldn't actually match because of substring rules, but
    # being explicit on CMakeLists is safer).
    cml = dst / "CMakeLists.txt"
    cml_text = cml.read_text(encoding="utf-8")
    if cfg["company"] and cfg["company"] != new_company:
        cml_text = re.sub(rf'COMPANY_NAME\s+"{re.escape(cfg["company"])}"',
                          f'COMPANY_NAME            "{new_company}"', cml_text)
    if cfg["mfr_code"] and cfg["mfr_code"] != new_mfr:
        cml_text = re.sub(rf'PLUGIN_MANUFACTURER_CODE\s+{re.escape(cfg["mfr_code"])}',
                          f"PLUGIN_MANUFACTURER_CODE {new_mfr}", cml_text)
    cml.write_text(cml_text, encoding="utf-8")

    n_modified = apply_text_replacements(dst, ordered)

    set_plugin_type(dst, new_type, new_name)

    cloned_script = dst / "scripts" / "plugin.py"
    if cloned_script.exists():
        cloned_script.chmod(0o755)

    print(f"\n✓ Cloned {n_modified} file(s) to {dst}")
    print(f"  Build:  cd {dst} && cmake -B build && cmake --build build")
    return 0


# ──────────────────────────────────────────────────────────────────────────
# rename
# ──────────────────────────────────────────────────────────────────────────

def cmd_rename(args: argparse.Namespace) -> int:
    root = PROJECT_ROOT
    cfg = discover_config(root)
    require_config(cfg)

    print(f"\nRenaming {cfg['name']} (in place at {root})\n")

    new_name = ask("New plugin name (PascalCase)", validate=is_pascal_ident)
    new_name_lower = lower_alnum(new_name)
    new_namespace = ask("New DSP namespace",
                        default=new_name_lower, validate=is_lower_ident)
    new_code = ask("New plugin code (4 chars)",
                   default=derive_plugin_code(new_name), validate=is_four_char)

    # Keep company / mfr_code as-is unless user wants to change them.
    if ask_yes_no("Also change company / manufacturer code?", default=False):
        new_company = ask("Company name", default=cfg["company"] or "Funk")
        new_mfr = ask("Manufacturer code (4 chars)",
                      default=cfg["mfr_code"] or new_company[:4],
                      validate=is_four_char)
    else:
        new_company = cfg["company"]
        new_mfr = cfg["mfr_code"]

    company_lower = lower_alnum(new_company or "")
    new_bundle_default = (f"com.{company_lower}.{new_name_lower}"
                          if company_lower else f"com.acme.{new_name_lower}")
    new_bundle = ask("New bundle ID", default=new_bundle_default,
                     validate=is_bundle_id)

    print()
    print(f"  {cfg['name']:>22}  ->  {new_name}")
    print(f"  {cfg['namespace']:>22}  ->  {new_namespace}")
    print(f"  {cfg['plugin_code']:>22}  ->  {new_code}")
    print(f"  {cfg['bundle_id']:>22}  ->  {new_bundle}")
    if new_company != cfg["company"]:
        print(f"  {cfg['company']:>22}  ->  {new_company}")
    if new_mfr != cfg["mfr_code"]:
        print(f"  {cfg['mfr_code']:>22}  ->  {new_mfr}")
    print()

    if not ask_yes_no("Apply rename in place?", default=False):
        print("Aborted.")
        return 1

    rules: list[tuple[str, str]] = [
        (cfg["bundle_id"],    new_bundle),
        (cfg["name"],         new_name),
        (cfg["name"].lower(), new_name_lower),
        (cfg["namespace"],    new_namespace),
        (cfg["plugin_code"],  new_code),
    ]
    rules.sort(key=lambda r: -len(r[0]))
    seen, ordered = set(), []
    for old, new in rules:
        if old in seen:
            continue
        seen.add(old)
        ordered.append((old, new))

    cml = root / "CMakeLists.txt"
    cml_text = cml.read_text(encoding="utf-8")
    if new_company and cfg["company"] and cfg["company"] != new_company:
        cml_text = re.sub(rf'COMPANY_NAME\s+"{re.escape(cfg["company"])}"',
                          f'COMPANY_NAME            "{new_company}"', cml_text)
    if new_mfr and cfg["mfr_code"] and cfg["mfr_code"] != new_mfr:
        cml_text = re.sub(rf'PLUGIN_MANUFACTURER_CODE\s+{re.escape(cfg["mfr_code"])}',
                          f"PLUGIN_MANUFACTURER_CODE {new_mfr}", cml_text)
    cml.write_text(cml_text, encoding="utf-8")

    n = apply_text_replacements(root, ordered)
    print(f"\n✓ Renamed: {n} file(s) modified. Wipe build/ before rebuilding.")
    return 0


# ──────────────────────────────────────────────────────────────────────────
# codesign
# ──────────────────────────────────────────────────────────────────────────

def _find_artefacts(build_dir: Path, name: str) -> list[Path]:
    """Locate built bundles under build_dir for the given plugin name."""
    if not build_dir.is_dir():
        sys.exit(f"Build directory not found: {build_dir}")
    candidates: list[Path] = []
    artefacts = list(build_dir.rglob(f"{name}_artefacts"))
    if not artefacts:
        sys.exit(f"No {name}_artefacts directory under {build_dir}. Have you built?")
    for art in artefacts:
        for pattern in (f"**/{name}.component", f"**/{name}.vst3", f"**/{name}.app"):
            candidates.extend(art.glob(pattern))
    return sorted(set(candidates))


def cmd_codesign(args: argparse.Namespace) -> int:
    cfg = discover_config(PROJECT_ROOT)
    require_config(cfg)
    build_dir = (PROJECT_ROOT / args.build_dir).resolve()
    bundles = _find_artefacts(build_dir, cfg["name"])

    if not bundles:
        sys.exit(f"No bundles to sign under {build_dir}")

    print(f"Signing {len(bundles)} bundle(s) with identity: {args.identity}\n")

    for bundle in bundles:
        subprocess.run(["xattr", "-cr", str(bundle)], check=True)
        cmd = [
            "codesign", "--force", "--deep",
            "--options", "runtime",
            "--timestamp",
            "--sign", args.identity,
            str(bundle),
        ]
        print(f"  $ {' '.join(cmd)}")
        result = subprocess.run(cmd)
        if result.returncode != 0:
            sys.exit(f"codesign failed for {bundle}")

    print("\n✓ All bundles signed.")
    return 0


# ──────────────────────────────────────────────────────────────────────────
# notarize
# ──────────────────────────────────────────────────────────────────────────

def _ditto_zip(bundle: Path, out: Path) -> None:
    cmd = ["ditto", "-c", "-k", "--keepParent", str(bundle), str(out)]
    result = subprocess.run(cmd)
    if result.returncode != 0:
        sys.exit(f"ditto zip failed for {bundle}")


def cmd_notarize(args: argparse.Namespace) -> int:
    cfg = discover_config(PROJECT_ROOT)
    require_config(cfg)
    build_dir = (PROJECT_ROOT / args.build_dir).resolve()
    bundles = _find_artefacts(build_dir, cfg["name"])

    if not bundles:
        sys.exit(f"No bundles to notarize under {build_dir}")

    print(f"Notarizing {len(bundles)} bundle(s) "
          f"(keychain profile: {args.keychain_profile})\n")

    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        for bundle in bundles:
            zip_path = tmp_dir / f"{bundle.name}.zip"
            print(f"  zip → {zip_path.name}")
            _ditto_zip(bundle, zip_path)

            submit = [
                "xcrun", "notarytool", "submit", str(zip_path),
                "--keychain-profile", args.keychain_profile,
                "--wait",
            ]
            print(f"  $ {' '.join(submit)}")
            result = subprocess.run(submit)
            if result.returncode != 0:
                sys.exit(f"notarytool failed for {bundle.name}")

            staple = ["xcrun", "stapler", "staple", str(bundle)]
            print(f"  $ {' '.join(staple)}")
            result = subprocess.run(staple)
            if result.returncode != 0:
                sys.exit(f"stapler failed for {bundle.name}")

    print("\n✓ All bundles notarized and stapled.")
    return 0


# ──────────────────────────────────────────────────────────────────────────
# main
# ──────────────────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(
        prog="plugin",
        description=__doc__.strip(),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subs = parser.add_subparsers(dest="command", required=True)

    p = subs.add_parser("clone", help="Clone the plugin into a new plugin")
    p.set_defaults(func=cmd_clone)

    p = subs.add_parser("rename", help="Rename the current plugin in place")
    p.set_defaults(func=cmd_rename)

    p = subs.add_parser("codesign", help="Codesign built bundles")
    p.add_argument("--identity", required=True,
                   help='Developer ID Application identity, e.g. '
                        '"Developer ID Application: Name (TEAMID)"')
    p.add_argument("--build-dir", default="build",
                   help="Build directory under the project root (default: build)")
    p.set_defaults(func=cmd_codesign)

    p = subs.add_parser("notarize", help="Submit signed bundles to Apple notary")
    p.add_argument("--keychain-profile", required=True,
                   help="notarytool keychain profile name (set up via "
                        "`xcrun notarytool store-credentials`)")
    p.add_argument("--build-dir", default="build",
                   help="Build directory under the project root (default: build)")
    p.set_defaults(func=cmd_notarize)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
