#!/usr/bin/env python3
"""
Product utility for this repository: one source tree, several products (products/<key>.cmake), made from the FunkGui
plugin template. Subcommands:

  info          Print every product's identity (products/<key>.cmake).
  add           Add a product: a copy of one (its kind, its examples and its icon) under a new key and identity.
  rename        Give a product a new identity in place: its products/<key>.cmake, and its names in the docs.
  codesign      Codesign the built AU / VST3 / Standalone with a Developer ID Application identity.
  notarize      Submit codesigned bundles to Apple's notary service and staple the tickets.

By example:

  scripts/plugin.py info
  scripts/plugin.py add --from synth --key bass --name FunkBass --code Fbas      a third product, an instrument
  scripts/plugin.py rename --product effect --name Playground --code Plgd        the effect renamed
  scripts/plugin.py codesign --identity "Developer ID Application: ..." [--product synth]

A product is its products/<key>.cmake (the PRODUCT_* values), its examples in Source/fminus/examples/<key>/ and its
extension's icon, Source/mcp/icon-<key>.png; everything else is shared, and nothing else names a product (the C++
namespace is `playground` in all of them). CMakeLists.txt builds every products/<key>.cmake it finds.

What a rename changes: products/<key>.cmake is rewritten with the new identity (name, codes, bundle id, FunkGui
prefixes); the old name (as written, in upper case and in lower case), bundle id and environment prefix are replaced in
every text file (docs, scripts) where they stand as whole words, except in products/, the vendored DSP core
(Source/fsdsp/**) and SEED.tsv. A rename changes what users have: hosts find a plugin by its codes, and the product
keeps its projects, preferences and sockets in folders named after it, so an installed product renamed is a new one.

The plugin code must differ from every other plugin's: add and rename refuse a code used by another product here or
by another project in the user's plugin folders (~/audio/plugins, or the folders in $FUNK_PLUGIN_DIRS, separated by
':'), unless --allow-code-clash is given. The manufacturer code stays the same for all of the user's plugins.

Missing values are asked for when stdin is a terminal; otherwise they are derived from the name (or --yes takes the
derived ones). Stdlib only.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_PATH = Path(__file__).resolve()
PROJECT_ROOT = SCRIPT_PATH.parent.parent

KINDS = ("effect", "instrument")
PRODUCTS_DIR = Path("products")
PRODUCT_KEYS = ("NAME", "KIND", "VERSION", "COMPANY", "MFR_CODE", "CODE", "BUNDLE_ID", "ENV_PREFIX", "OBJC_PREFIX")
EXAMPLES_DIR = Path("Source") / "fminus" / "examples"
ICON_DIR = Path("Source") / "mcp"

# Never rewritten: the product files (written whole), the vendored core and its manifest (SEED.tsv holds their sha256).
REWRITE_EXCLUDES = ("products/", "Source/fsdsp/", "SEED.tsv")
TEXT_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".m", ".mm", ".cmake", ".in", ".txt",
                 ".md", ".py", ".sh", ".json", ".plist", ".xml", ".tsv", ".def", ".gitignore"}
# Where the user's plugins live: a new plugin code must differ from every project's there. $FUNK_PLUGIN_DIRS (folders
# separated by ':') replaces the list.
DEFAULT_PLUGIN_DIRS = (Path("~/audio/plugins"),)


# ----------------------------------------------------------------------------------------------------------------------
# products/<key>.cmake
# ----------------------------------------------------------------------------------------------------------------------

def product_file(root: Path, key: str) -> Path:
    return root / PRODUCTS_DIR / f"{key}.cmake"


def product_keys(root: Path) -> list[str]:
    folder = root / PRODUCTS_DIR
    keys = sorted(p.stem for p in folder.glob("*.cmake")) if folder.is_dir() else []
    if not keys:
        sys.exit(f"{folder} has no <key>.cmake: is {root} this repository?")
    return keys


def read_product(root: Path, key: str) -> dict[str, str]:
    path = product_file(root, key)
    if not path.exists():
        sys.exit(f"{path} not found (the products: {', '.join(product_keys(root))})")
    text = path.read_text(encoding="utf-8")
    cfg = {}
    for k in PRODUCT_KEYS:
        m = re.search(rf'^set\(PRODUCT_{k}\s+"([^"]*)"\)', text, re.MULTILINE)
        if not m:
            sys.exit(f"{path}: no line `set(PRODUCT_{k} \"...\")`")
        cfg[k] = m.group(1)
    return cfg


def write_product(path: Path, cfg: dict[str, str]) -> None:
    text = path.read_text(encoding="utf-8")
    for k in PRODUCT_KEYS:
        text, n = re.subn(rf'^(set\(PRODUCT_{k}\s+)"[^"]*"\)', lambda m: f'{m.group(1)}"{cfg[k]}")', text,
                          count=1, flags=re.MULTILINE)
        if n != 1:
            sys.exit(f"{path}: cannot rewrite PRODUCT_{k}")
    path.write_text(text, encoding="utf-8")


def all_products(root: Path) -> dict[str, dict[str, str]]:
    return {key: read_product(root, key) for key in product_keys(root)}


# ----------------------------------------------------------------------------------------------------------------------
# Validation and derivation
# ----------------------------------------------------------------------------------------------------------------------

def check_name(v: str) -> str | None:
    return None if re.fullmatch(r"[A-Z][A-Za-z0-9]*", v) else "PascalCase letters and digits, e.g. MyDelay"


def check_key(v: str) -> str | None:
    return None if re.fullmatch(r"[a-z][a-z0-9]*", v) else "lower-case letters and digits, e.g. bass"


def check_code(v: str) -> str | None:
    if not re.fullmatch(r"[A-Za-z0-9]{4}", v):
        return "exactly 4 letters or digits"
    if not any(c.isupper() for c in v):
        return "at least one upper-case letter (Apple reserves all-lower-case codes)"
    return None


def check_bundle_id(v: str) -> str | None:
    parts = v.split(".")
    if len(parts) < 2 or not all(re.fullmatch(r"[A-Za-z][A-Za-z0-9-]*", p) for p in parts):
        return "reverse-DNS, e.g. com.funk.mydelay"
    return None


def check_env_prefix(v: str) -> str | None:
    return None if re.fullmatch(r"[A-Z][A-Z0-9_]*_", v) else "upper case, ending in _, e.g. MYDL_"


def lower_alnum(s: str) -> str:
    return "".join(c for c in s if c.isalnum()).lower()


def derive_code(name: str) -> str:
    """An Apple-style 4-character code: one upper-case letter, then three lower-case, consonants first."""
    letters = [c for c in name if c.isalpha()]
    if not letters:
        return "Plug"
    rest = letters[1:]
    consonants = [c for c in rest if c.lower() not in "aeiou"]
    pool = consonants if len(consonants) >= 3 else rest
    return (letters[0].upper() + "".join(pool[:3]).lower()).ljust(4, "x")[:4]


def objc_prefix(code: str) -> str:
    return code if code[0].isalpha() else "P" + code


# ----------------------------------------------------------------------------------------------------------------------
# Plugin codes in use beside this repository, and in it
# ----------------------------------------------------------------------------------------------------------------------

CODE_PATTERNS = (
    re.compile(r'^\s*set\(PRODUCT_CODE\s+"([A-Za-z0-9]{4})"\)', re.MULTILINE),       # the template's projects
    re.compile(r'^\s*PLUGIN_CODE\s+"?([A-Za-z0-9]{4})"?\s*$', re.MULTILINE),          # juce_add_plugin(... PLUGIN_CODE)
    re.compile(r'set\(\w*_PLUGIN_CODE\s+"([A-Za-z0-9]{4})"\)'),                        # FSynth, FCompressor
)


def codes_in(project: Path) -> set[str]:
    codes: set[str] = set()
    files = [project / "CMakeLists.txt", project / "cmake" / "Product.cmake"]
    for folder in (project / "cmake", project / PRODUCTS_DIR):
        if folder.is_dir():
            files += sorted(folder.glob("*.cmake"))
    for f in files:
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for pattern in CODE_PATTERNS:
            codes.update(pattern.findall(text))
    return codes


def plugin_dirs() -> list[Path]:
    env = os.environ.get("FUNK_PLUGIN_DIRS")
    dirs = [Path(d) for d in env.split(os.pathsep) if d] if env is not None else list(DEFAULT_PLUGIN_DIRS)
    return [d.expanduser() for d in dirs]


def sibling_codes(skip: set[Path]) -> dict[str, list[str]]:
    """Plugin code -> the projects using it, over the user's plugin folders and this repository's parent (one level
    down), this repository and its worktrees left out (its own products are checked one by one)."""
    used: dict[str, list[str]] = {}
    seen: set[Path] = set()
    for parent in [*plugin_dirs(), PROJECT_ROOT.parent]:
        if not parent.is_dir():
            continue
        for project in sorted(parent.iterdir()):
            real = project.resolve()
            if not project.is_dir() or real in seen or real in skip:
                continue
            seen.add(real)
            for code in codes_in(project):
                used.setdefault(code, []).append(project.name)
    return used


def check_code_free(code: str, own: dict[str, str], allow: bool) -> None:
    """Exit when `code` is another product's here (`own`: code -> key) or another project's beside this one."""
    if code in own:
        sys.exit(f"plugin code {code} is the {own[code]} product's: each product needs its own (pass --code)")
    used = sibling_codes({PROJECT_ROOT.resolve()})
    if code in used:
        msg = f"plugin code {code} is already used by {', '.join(sorted(set(used[code])))}"
        if allow:
            print(f"warning: {msg} (--allow-code-clash)")
        else:
            taken = ", ".join(sorted(set(used) | set(own)))
            sys.exit(f"{msg}: hosts would confuse the two plugins. Choose another with --code (codes in use: {taken})")


def check_distinct(new: dict[str, str], others: dict[str, dict[str, str]]) -> None:
    """Exit when the new identity shares what tells products apart (a host loads them all into one process)."""
    for key, cfg in others.items():
        for k in ("NAME", "BUNDLE_ID", "ENV_PREFIX", "OBJC_PREFIX"):
            if cfg[k] == new[k]:
                sys.exit(f"{k.lower()} {new[k]} is the {key} product's: each product needs its own")


# ----------------------------------------------------------------------------------------------------------------------
# Files
# ----------------------------------------------------------------------------------------------------------------------

def project_files(root: Path) -> list[Path]:
    """The repository's files, relative: what git tracks or would track, else a walk without build trees."""
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                             capture_output=True, check=True).stdout.decode("utf-8")
        files = [Path(p) for p in out.split("\0") if p]
        return [p for p in files if (root / p).is_file()]
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    files = []
    excludes = {".git", ".DS_Store", "__pycache__", ".idea", ".vscode", "node_modules", "Testing"}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in excludes and not d.startswith("build")]
        for f in filenames:
            if f not in excludes:
                files.append((Path(dirpath) / f).relative_to(root))
    return files


def is_text(rel: Path) -> bool:
    return rel.suffix.lower() in TEXT_SUFFIXES or rel.name in {"CMakeLists.txt", ".gitignore", "CLAUDE.md"}


def rewrite_tokens(root: Path, rules: list[tuple[str, str]]) -> int:
    """Replace each old token by its new one in every text file, where the token is not part of a longer word (a
    token ending in "_", an environment prefix, may run on into the variable's name: MYDL_UI_THEME)."""
    compiled = []
    for old, new in rules:
        if old and old != new:
            tail = "" if old.endswith("_") else "(?![A-Za-z0-9])"
            compiled.append((re.compile(rf"(?<![A-Za-z0-9]){re.escape(old)}{tail}"), new))
    changed = 0
    for rel in project_files(root):
        posix = rel.as_posix()
        if any(posix == e or posix.startswith(e) for e in REWRITE_EXCLUDES) or not is_text(rel):
            continue
        path = root / rel
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        new_text = text
        for pattern, repl in compiled:
            new_text = pattern.sub(repl, new_text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changed += 1
    return changed


def rename_rules(old: dict[str, str], new: dict[str, str]) -> list[tuple[str, str]]:
    """The identity tokens a rename replaces in the text files. The four-character codes are not among them (a code
    can be an ordinary word, "Gain"): they live in products/<key>.cmake only, which is rewritten whole."""
    rules = [
        (old["NAME"], new["NAME"]),
        (old["NAME"] + "MCP", new["NAME"] + "MCP"),         # the product's MCP server
        (old["NAME"].upper(), new["NAME"].upper()),
        (lower_alnum(old["NAME"]), lower_alnum(new["NAME"])),
        (old["BUNDLE_ID"], new["BUNDLE_ID"]),
        (old["ENV_PREFIX"], new["ENV_PREFIX"]),
    ]
    rules.sort(key=lambda r: -len(r[0]))          # longer tokens first (a bundle id before the names inside it)
    seen, ordered = set(), []
    for o, n in rules:
        if o and o not in seen:
            seen.add(o)
            ordered.append((o, n))
    return ordered


# ----------------------------------------------------------------------------------------------------------------------
# Prompts
# ----------------------------------------------------------------------------------------------------------------------

def interactive() -> bool:
    return sys.stdin.isatty()


def ask(prompt: str, given: str | None, default: str | None, check, assume_default: bool) -> str:
    if given is not None:
        err = check(given) if check else None
        if err:
            sys.exit(f"{prompt}: '{given}' is invalid: {err}")
        return given
    if assume_default or not interactive():
        if default is None:
            sys.exit(f"{prompt}: required (pass it as an option)")
        err = check(default) if check else None
        if err:
            sys.exit(f"{prompt}: the derived value '{default}' is invalid ({err}); pass one")
        return default
    while True:
        suffix = f" [{default}]" if default else ""
        raw = input(f"{prompt}{suffix}: ").strip()
        value = raw or (default or "")
        if not value:
            print("  required")
            continue
        err = check(value) if check else None
        if err:
            print(f"  invalid: {err}")
            continue
        return value


def confirm(prompt: str, assume_yes: bool) -> bool:
    if assume_yes or not interactive():
        return True
    raw = input(f"{prompt} [Y/n]: ").strip().lower()
    return raw in ("", "y", "yes")


def choose_product(root: Path, given: str | None, what: str, assume: bool) -> str:
    keys = product_keys(root)
    if given is None and len(keys) == 1:
        return keys[0]
    key = ask(f"The product to {what} ({' | '.join(keys)})", given, None,
              lambda v: None if v in keys else f"one of {', '.join(keys)}", assume)
    return key


def new_identity(args: argparse.Namespace, cur: dict[str, str]) -> dict[str, str]:
    y = args.yes
    name = ask("Plugin name (PascalCase)", args.name, None, check_name, y)
    company = ask("Company", args.company, cur["COMPANY"], None, y)
    mfr = ask("Manufacturer code (the same for all your plugins)", args.mfr, cur["MFR_CODE"], check_code, y)
    code = ask("Plugin code (4 characters, unique)", args.code, derive_code(name), check_code, y)
    bundle = ask("Bundle id", args.bundle_id, f"com.{lower_alnum(company) or 'funk'}.{lower_alnum(name)}",
                 check_bundle_id, y)
    env = ask("FunkGui environment prefix", args.env_prefix, re.sub(r"[^A-Z0-9]", "", code.upper()) + "_",
              check_env_prefix, y)
    return {"NAME": name, "KIND": cur["KIND"], "VERSION": "0.1.0" if args.reset_version else cur["VERSION"],
            "COMPANY": company, "MFR_CODE": mfr, "CODE": code, "BUNDLE_ID": bundle, "ENV_PREFIX": env,
            "OBJC_PREFIX": objc_prefix(code)}


def print_identity(cfg: dict[str, str], title: str) -> None:
    print(title)
    for k in PRODUCT_KEYS:
        print(f"  {k.lower():<12} {cfg[k]}")


# ----------------------------------------------------------------------------------------------------------------------
# Subcommands
# ----------------------------------------------------------------------------------------------------------------------

def cmd_info(args: argparse.Namespace) -> int:
    products = all_products(PROJECT_ROOT)
    print(f"{PROJECT_ROOT}: {len(products)} products")
    for key, cfg in products.items():
        print()
        print_identity(cfg, f"{key} (products/{key}.cmake, Source/fminus/examples/{key}/, "
                            f"Source/mcp/icon-{key}.png)")
    return 0


def cmd_add(args: argparse.Namespace) -> int:
    root = PROJECT_ROOT
    products = all_products(root)
    src = choose_product(root, args.source, "copy", args.yes)
    cur = products[src]
    print(f"A new product from {src} ({cur['NAME']}, {cur['KIND']}): its kind, examples and icon\n")
    new = new_identity(args, cur)
    key = ask("Product key (products/<key>.cmake)", args.key, lower_alnum(new["NAME"]), check_key, args.yes)
    if key in products:
        sys.exit(f"products/{key}.cmake exists already")
    check_distinct(new, products)
    check_code_free(new["CODE"], {cfg["CODE"]: k for k, cfg in products.items()}, args.allow_code_clash)
    print()
    print_identity(new, f"New product {key}:")
    if not confirm("Add it?", args.yes):
        print("Aborted.")
        return 1

    examples_src, examples_dst = root / EXAMPLES_DIR / src, root / EXAMPLES_DIR / key
    icon_src, icon_dst = root / ICON_DIR / f"icon-{src}.png", root / ICON_DIR / f"icon-{key}.png"
    for path in (examples_dst, icon_dst):
        if path.exists():
            sys.exit(f"{path} exists already")
    with tempfile.TemporaryDirectory(prefix="plugin-add-") as tmp:
        staged = Path(tmp) / f"{key}.cmake"
        text = product_file(root, src).read_text(encoding="utf-8")
        text = text.replace(f"products/{src}.cmake", f"products/{key}.cmake")
        text = text.replace(f"examples/{src}/", f"examples/{key}/").replace(f"icon-{src}.png", f"icon-{key}.png")
        staged.write_text(text, encoding="utf-8")
        write_product(staged, new)
        shutil.copytree(examples_src, examples_dst)
        shutil.copy2(icon_src, icon_dst)
        shutil.copy2(staged, product_file(root, key))
    print(f"Added products/{key}.cmake, {examples_dst.relative_to(root)}/ (from {src}'s) and "
          f"{icon_dst.relative_to(root)}.")
    print("Next: describe it in README.md and CLAUDE.md, change its examples and icon, and run")
    print("  cmake --workflow --preset dev-verify      # configure, build every product, run every test")
    return 0


def cmd_rename(args: argparse.Namespace) -> int:
    root = PROJECT_ROOT
    products = all_products(root)
    key = choose_product(root, args.product, "rename", args.yes)
    cur = products[key]
    others = {k: cfg for k, cfg in products.items() if k != key}
    print(f"Renaming the {key} product, {cur['NAME']} ({cur['KIND']}), in place ({root})\n")
    new = new_identity(args, cur)
    check_distinct(new, others)
    if new["CODE"] != cur["CODE"]:
        check_code_free(new["CODE"], {cfg["CODE"]: k for k, cfg in others.items()}, args.allow_code_clash)
    print()
    print_identity(new, "Renamed to:")
    print("\nInstalled copies of the old identity stay as they are: hosts see the renamed product as a new plugin "
          "when its codes change, and it keeps its projects and preferences in folders of the new name.")
    if not confirm("Apply the rename in place?", args.yes):
        print("Aborted.")
        return 1
    write_product(product_file(root, key), new)
    n = rewrite_tokens(root, rename_rules(cur, new))
    print(f"Renamed: products/{key}.cmake and {n} files rewritten. Delete the build directories (rm -rf build-*) "
          "before building again.")
    return 0


def find_bundles(build_dir: Path, name: str) -> list[Path]:
    art = build_dir / f"{name}_artefacts"
    if not art.is_dir():
        sys.exit(f"no {art}: build first (cmake --workflow --preset release-build)")
    found: list[Path] = []
    for pattern in (f"*/AU/{name}.component", f"*/VST3/{name}.vst3", f"*/Standalone/{name}.app"):
        found.extend(art.glob(pattern))
    if not found:
        sys.exit(f"no bundles under {art}")
    return sorted(found)


def selected_names(args: argparse.Namespace) -> list[str]:
    products = all_products(PROJECT_ROOT)
    if args.product is None:
        return [cfg["NAME"] for cfg in products.values()]
    if args.product not in products:
        sys.exit(f"no product {args.product} (the products: {', '.join(products)})")
    return [products[args.product]["NAME"]]


def cmd_codesign(args: argparse.Namespace) -> int:
    build = (PROJECT_ROOT / args.build_dir).resolve()
    for name in selected_names(args):
        for bundle in find_bundles(build, name):
            subprocess.run(["xattr", "-cr", str(bundle)], check=True)
            cmd = ["codesign", "--force", "--deep", "--options", "runtime", "--timestamp", "--sign", args.identity,
                   str(bundle)]
            print("$ " + " ".join(cmd))
            if subprocess.run(cmd).returncode != 0:
                sys.exit(f"codesign failed for {bundle}")
    print("All bundles signed.")
    return 0


def cmd_notarize(args: argparse.Namespace) -> int:
    build = (PROJECT_ROOT / args.build_dir).resolve()
    bundles = [b for name in selected_names(args) for b in find_bundles(build, name)]
    with tempfile.TemporaryDirectory() as tmp:
        for bundle in bundles:
            archive = Path(tmp) / f"{bundle.name}.zip"
            subprocess.run(["ditto", "-c", "-k", "--keepParent", str(bundle), str(archive)], check=True)
            submit = ["xcrun", "notarytool", "submit", str(archive), "--keychain-profile", args.keychain_profile,
                      "--wait"]
            print("$ " + " ".join(submit))
            if subprocess.run(submit).returncode != 0:
                sys.exit(f"notarytool failed for {bundle.name}")
            if subprocess.run(["xcrun", "stapler", "staple", str(bundle)]).returncode != 0:
                sys.exit(f"stapler failed for {bundle.name}")
    print("All bundles notarized and stapled.")
    return 0


def add_identity_options(p: argparse.ArgumentParser) -> None:
    p.add_argument("--name", help="PascalCase name: the target, the bundles, the window title")
    p.add_argument("--code", help="4-character plugin code, unique among your plugins (default: from the name)")
    p.add_argument("--mfr", help="4-character manufacturer code (default: the product's)")
    p.add_argument("--company", help="company name (default: the product's)")
    p.add_argument("--bundle-id", help="reverse-DNS bundle id (default: com.<company>.<name>)")
    p.add_argument("--env-prefix", help="FunkGui's environment prefix (default: the code in upper case, then _)")
    p.add_argument("--allow-code-clash", action="store_true", help="accept a plugin code another project uses")
    p.add_argument("--reset-version", action="store_true", help="start at version 0.1.0")
    p.add_argument("--yes", "-y", action="store_true", help="take the derived defaults and do not ask")


def main() -> int:
    parser = argparse.ArgumentParser(prog="plugin.py", description=__doc__.strip(),
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    subs = parser.add_subparsers(dest="command", required=True)

    p = subs.add_parser("info", help="print every product's identity")
    p.set_defaults(func=cmd_info)

    p = subs.add_parser("add", help="add a product, a copy of one under a new identity")
    p.add_argument("--from", dest="source", help="the product to copy (its kind, examples and icon)")
    p.add_argument("--key", help="the new product's key (default: its name in lower case)")
    add_identity_options(p)
    p.set_defaults(func=cmd_add)

    p = subs.add_parser("rename", help="give a product a new identity in place")
    p.add_argument("--product", help="the product's key (products/<key>.cmake)")
    add_identity_options(p)
    p.set_defaults(func=cmd_rename)

    p = subs.add_parser("codesign", help="codesign the built bundles")
    p.add_argument("--identity", required=True, help='"Developer ID Application: Name (TEAMID)"')
    p.add_argument("--build-dir", default="build-release", help="build directory (default: build-release)")
    p.add_argument("--product", help="one product's key (default: every product)")
    p.set_defaults(func=cmd_codesign)

    p = subs.add_parser("notarize", help="notarize and staple the signed bundles")
    p.add_argument("--keychain-profile", required=True, help="notarytool keychain profile")
    p.add_argument("--build-dir", default="build-release", help="build directory (default: build-release)")
    p.add_argument("--product", help="one product's key (default: every product)")
    p.set_defaults(func=cmd_notarize)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
