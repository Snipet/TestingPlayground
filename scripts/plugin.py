#!/usr/bin/env python3
"""
Plugin utility for a project made from the FunkGui plugin template. Subcommands:

  info          Print this project's identity (cmake/Product.cmake).
  clone         Copy this project into a new plugin: a new name, codes, bundle id and namespace; effect or instrument.
  rename        Rename this project in place (same steps as clone, no copy).
  starter-diff  Show how this project's algorithm (the starter's files, below) differs from its kind's starter.
  codesign      Codesign the built AU / VST3 / Standalone with a Developer ID Application identity.
  notarize      Submit codesigned bundles to Apple's notary service and staple the tickets.

clone, by example:

  scripts/plugin.py clone --name MyPlugin                       a copy of this project (its kind), beside it
  scripts/plugin.py clone --name MySynth --type instrument      an instrument: when this project is an effect, the
                                                                algorithm, its probes, CLAUDE.md and README parts
                                                                become the instrument starter's
                                                                (scripts/starters/instrument)
  scripts/plugin.py clone --name MyPlugin --starter             this project's kind, but from its starter
  scripts/plugin.py clone --name MyDelay --dest ~/x/MyDelay --code Mydl --yes

What a clone changes:
  - cmake/Product.cmake is rewritten with the new identity (name, kind, codes, bundle id, namespace, FunkGui prefixes);
  - the old name (as written, in upper case and in lower case), namespace, bundle id and environment prefix are
    replaced in every text file (sources, docs, CMake, scripts) where they stand as whole words, except in the
    vendored DSP core (Source/fsdsp/**) and SEED.tsv, which stay byte for byte;
  - the probes' alias of the namespace (`namespace md = mydelay;` in a plugin named MyDelay: the name's initials)
    follows the new name;
  - with a kind change or --starter, the starter's files replace the project's: Source/dsp/, Tools/probes/dsp/,
    Tools/probes/plugin/process.cpp and CLAUDE.md from scripts/starters/<kind>/, and README.md's kind-specific blocks
    (<!-- starter:<name> --> ... <!-- /starter:<name> -->) from scripts/starters/<kind>/README-parts.md;
  - README.md and CLAUDE.md lose their <!-- origin --> ... <!-- /origin --> blocks: text about the source project's
    own history (its git tags, where it came from), which the new project does not have;
  - nothing else: the build directories are not copied (a git project copies what git tracks or would track).
A project without scripts/starters/<kind> (one that replaced its algorithm and dropped the starters) clones only
itself: a kind change, --starter and starter-diff refuse there, before anything is asked or written; start the other
kind from {template}.
The plugin code must differ from every other plugin's: the clone refuses a code used by another project in the
user's plugin folders (~/audio/plugins, or the folders in $FUNK_PLUGIN_DIRS, separated by ':'), beside the source or
the destination, or beside the project the source was cloned from with git, unless --allow-code-clash is given. The
manufacturer code stays the same for all of the user's plugins.

Missing values are asked for when stdin is a terminal; otherwise they are derived from the name (or --yes takes the
derived ones). Stdlib only.
"""

from __future__ import annotations

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_PATH = Path(__file__).resolve()
PROJECT_ROOT = SCRIPT_PATH.parent.parent
# The template every project starts from, spelt in two parts so that a clone's rename (which replaces this project's
# name wherever it stands as a word) leaves it alone.
TEMPLATE = "Funk" "PluginTemplate"

KINDS = ("effect", "instrument")
PRODUCT_FILE = Path("cmake") / "Product.cmake"
PRODUCT_KEYS = ("NAME", "KIND", "VERSION", "COMPANY", "MFR_CODE", "CODE", "BUNDLE_ID", "NAMESPACE",
                "ENV_PREFIX", "OBJC_PREFIX")

# Never copied (a project that is not a git work tree is copied by walking it).
COPY_EXCLUDES = {".git", ".DS_Store", "__pycache__", ".idea", ".vscode", "node_modules", "Testing"}
# Never rewritten: the vendored core and its manifest stay byte for byte (SEED.tsv holds their sha256).
REWRITE_EXCLUDES = ("Source/fsdsp/", "SEED.tsv")
TEXT_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".m", ".mm", ".cmake", ".in", ".txt",
                 ".md", ".py", ".sh", ".json", ".plist", ".xml", ".tsv", ".def", ".gitignore"}
# The parts of a project its starter replaces (scripts/starters/<kind>/ holds the same paths: folders and files), and
# the starter's README blocks, spliced into README.md between <!-- starter:<name> --> and <!-- /starter:<name> -->.
STARTER_PATHS = (Path("Source") / "dsp", Path("Tools") / "probes" / "dsp",
                 Path("Tools") / "probes" / "plugin" / "process.cpp", Path("CLAUDE.md"))
STARTER_README = Path("README-parts.md")
README_BLOCK = re.compile(r"<!-- starter:([a-z][a-z0-9-]*) -->\n.*?<!-- /starter:\1 -->\n", re.DOTALL)
# The source project's own history, left out of a clone (with the blank line after the block).
ORIGIN_BLOCK = re.compile(r"<!-- origin -->\n.*?<!-- /origin -->\n(?:\n)?", re.DOTALL)
ORIGIN_DOCS = (Path("README.md"), Path("CLAUDE.md"))
# Where the user's plugins live: a clone's plugin code must differ from every project's there, wherever the clone is
# made (a scratch folder too). $FUNK_PLUGIN_DIRS (folders separated by ':') replaces the list.
DEFAULT_PLUGIN_DIRS = (Path("~/audio/plugins"),)

CPP_KEYWORDS = {"alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class",
                "const", "constexpr", "continue", "default", "delete", "do", "double", "else", "enum", "explicit",
                "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
                "mutable", "namespace", "new", "not", "operator", "or", "private", "protected", "public", "register",
                "return", "short", "signed", "sizeof", "static", "struct", "switch", "template", "this", "throw",
                "true", "try", "typedef", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
                "while", "std", "juce", "funkgui", "fsdsp", "probe"}


# ----------------------------------------------------------------------------------------------------------------------
# Product.cmake
# ----------------------------------------------------------------------------------------------------------------------

def read_product(root: Path) -> dict[str, str]:
    path = root / PRODUCT_FILE
    if not path.exists():
        sys.exit(f"{path} not found: is {root} a project made from the plugin template?")
    text = path.read_text(encoding="utf-8")
    cfg = {}
    for key in PRODUCT_KEYS:
        m = re.search(rf'^set\(PRODUCT_{key}\s+"([^"]*)"\)', text, re.MULTILINE)
        if not m:
            sys.exit(f"{path}: no line `set(PRODUCT_{key} \"...\")`")
        cfg[key] = m.group(1)
    return cfg


def write_product(root: Path, cfg: dict[str, str]) -> None:
    path = root / PRODUCT_FILE
    text = path.read_text(encoding="utf-8")
    for key in PRODUCT_KEYS:
        text, n = re.subn(rf'^(set\(PRODUCT_{key}\s+)"[^"]*"\)', lambda m: f'{m.group(1)}"{cfg[key]}")', text,
                          count=1, flags=re.MULTILINE)
        if n != 1:
            sys.exit(f"{path}: cannot rewrite PRODUCT_{key}")
    path.write_text(text, encoding="utf-8")


# ----------------------------------------------------------------------------------------------------------------------
# Validation and derivation
# ----------------------------------------------------------------------------------------------------------------------

def check_name(v: str) -> str | None:
    return None if re.fullmatch(r"[A-Z][A-Za-z0-9]*", v) else "PascalCase letters and digits, e.g. MyDelay"


def check_code(v: str) -> str | None:
    if not re.fullmatch(r"[A-Za-z0-9]{4}", v):
        return "exactly 4 letters or digits"
    if not any(c.isupper() for c in v):
        return "at least one upper-case letter (Apple reserves all-lower-case codes)"
    return None


def check_namespace(v: str) -> str | None:
    if not re.fullmatch(r"[a-z_][a-z0-9_]*", v):
        return "a lower-case C++ identifier"
    if v in CPP_KEYWORDS:
        return f"'{v}' is a keyword or a namespace already in use"
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
# Plugin codes in use beside a project
# ----------------------------------------------------------------------------------------------------------------------

CODE_PATTERNS = (
    re.compile(r'^\s*set\(PRODUCT_CODE\s+"([A-Za-z0-9]{4})"\)', re.MULTILINE),       # this template's projects
    re.compile(r'^\s*PLUGIN_CODE\s+"?([A-Za-z0-9]{4})"?\s*$', re.MULTILINE),          # juce_add_plugin(... PLUGIN_CODE)
    re.compile(r'set\(\w*_PLUGIN_CODE\s+"([A-Za-z0-9]{4})"\)'),                        # FSynth, FCompressor
)


def codes_in(project: Path) -> set[str]:
    codes: set[str] = set()
    files = [project / "CMakeLists.txt", project / PRODUCT_FILE]
    cmake_dir = project / "cmake"
    if cmake_dir.is_dir():
        files += sorted(cmake_dir.glob("*.cmake"))
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


def origin_parent(root: Path) -> Path | None:
    """The folder holding the project `root` was cloned from with git, when that is a local path (a scratch clone of
    a project in ~/audio/plugins: its siblings are the user's plugins)."""
    try:
        url = subprocess.run(["git", "-C", str(root), "config", "--get", "remote.origin.url"], capture_output=True,
                             text=True).stdout.strip()
    except FileNotFoundError:
        return None
    remote = ("://" in url and not url.startswith("file://")) or re.match(r"^[\w.-]+@[\w.-]+:", url)
    if not url or remote:
        return None
    path = Path(url.removeprefix("file://")).expanduser()
    path = path if path.is_absolute() else (root / path)
    return path.resolve().parent if path.is_dir() else None


def code_dirs(*beside: Path) -> list[Path]:
    """Every folder whose projects a new plugin code must not clash with: the user's plugin folders, the folders
    given (the source's and the destination's parents), and the folder beside this project's git origin."""
    dirs = [*plugin_dirs(), *beside]
    origin = origin_parent(PROJECT_ROOT)
    if origin is not None:
        dirs.append(origin)
    unique: list[Path] = []
    for d in dirs:
        r = d.resolve()
        if r not in unique:
            unique.append(r)
    return unique


def sibling_codes(dirs: list[Path], skip: set[Path]) -> dict[str, list[str]]:
    """Plugin code -> the projects using it, over the directories in `dirs` (one level down)."""
    used: dict[str, list[str]] = {}
    seen: set[Path] = set()
    for parent in dirs:
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


# ----------------------------------------------------------------------------------------------------------------------
# Files
# ----------------------------------------------------------------------------------------------------------------------

def project_files(root: Path) -> list[Path]:
    """The project's files, relative: what git tracks or would track, else a walk without build trees."""
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                             capture_output=True, check=True).stdout.decode("utf-8")
        files = [Path(p) for p in out.split("\0") if p]
        return [p for p in files if (root / p).is_file()]
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in COPY_EXCLUDES and not d.startswith("build")]
        for f in filenames:
            if f not in COPY_EXCLUDES:
                files.append((Path(dirpath) / f).relative_to(root))
    return files


def is_text(rel: Path) -> bool:
    return rel.suffix.lower() in TEXT_SUFFIXES or rel.name in {"CMakeLists.txt", ".gitignore", "CLAUDE.md"}


def rewrite_tokens(root: Path, rules: list[tuple[str, str]]) -> int:
    """Replace each old token by its new one in every text file, where the token is not part of a longer word (a
    token ending in "_", an environment prefix, may run on into the variable's name: TSTN_UI_THEME)."""
    compiled = []
    for old, new in rules:
        if old and old != new:
            tail = "" if old.endswith("_") else "(?![A-Za-z0-9])"
            compiled.append((re.compile(rf"(?<![A-Za-z0-9]){re.escape(old)}{tail}"), new))
    changed = 0
    for rel in project_files(root):
        posix = rel.as_posix()
        if posix == PRODUCT_FILE.as_posix() or any(posix == e or posix.startswith(e) for e in REWRITE_EXCLUDES):
            continue
        if not is_text(rel):
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


def readme_blocks(text: str) -> dict[str, str]:
    """The kind-specific blocks of a README: name -> the block, its marker lines included."""
    return {m.group(1): m.group(0) for m in README_BLOCK.finditer(text)}


def splice_readme(parts: Path, readme: Path) -> None:
    """Replace each starter block of `readme` with the block of the same name in `parts`."""
    if not readme.is_file():
        return
    blocks = readme_blocks(parts.read_text(encoding="utf-8"))
    kept: list[str] = []

    def repl(m: re.Match[str]) -> str:
        if m.group(1) in blocks:
            return blocks[m.group(1)]
        kept.append(m.group(1))
        return m.group(0)

    text = readme.read_text(encoding="utf-8")
    readme.write_text(README_BLOCK.sub(repl, text), encoding="utf-8")
    missing = sorted(set(blocks) - set(readme_blocks(text)))
    if kept or missing:
        print(f"warning: README.md: blocks kept as they were: {', '.join(kept) or '-'}; starter blocks with no place "
              f"in it: {', '.join(missing) or '-'} (update it by hand from {parts})")


def apply_starter(src_root: Path, dst_root: Path, kind: str) -> None:
    starter = src_root / "scripts" / "starters" / kind
    for rel in (*STARTER_PATHS, STARTER_README):
        if not (starter / rel).exists():
            sys.exit(f"{starter / rel} is missing: this project has no complete {kind} starter")
    for rel in STARTER_PATHS:
        source, target = starter / rel, dst_root / rel
        if target.is_dir() and not target.is_symlink():
            shutil.rmtree(target)
        elif target.exists() or target.is_symlink():
            target.unlink()
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_dir():
            shutil.copytree(source, target)
        else:
            shutil.copy2(source, target)
    splice_readme(starter / STARTER_README, dst_root / "README.md")


def require_starter(root: Path, kind: str) -> None:
    """Exit with one line when this project has no `kind` starter to take code from."""
    if not (root / "scripts" / "starters" / kind).is_dir():
        sys.exit(f"this project has no {kind} starter (scripts/starters/{kind} is missing): it clones only itself, "
                 f"with its own algorithm. Start a new {kind} from {TEMPLATE} (its scripts/plugin.py clone).")


def drop_origin_blocks(root: Path) -> None:
    for rel in ORIGIN_DOCS:
        path = root / rel
        if path.is_file():
            text = path.read_text(encoding="utf-8")
            new_text = ORIGIN_BLOCK.sub("", text)
            if new_text != text:
                path.write_text(new_text, encoding="utf-8")


def alias_for(name: str) -> str:
    """The probes' alias of a plugin's namespace: the initials of its name's words in lower case (MyDelay: md;
    TapeEcho2: te2), or its first three letters when that is a single letter (Synth: syn)."""
    initials = "".join(w[0] for w in re.findall(r"[A-Z][a-z]*|[a-z]+|[0-9]+", name)).lower()
    alias = initials if len(initials) >= 2 else lower_alnum(name)[:3]
    if not re.fullmatch(r"[a-z_][a-z0-9_]*", alias) or alias in CPP_KEYWORDS:
        alias = "p" + alias
    return alias


def rename_aliases(root: Path, namespace: str, alias: str) -> int:
    """In each text file that declares `namespace <x> = <namespace>;`, rename <x> to `alias` (whole words, in that
    file only). A file where `alias` already names something keeps its alias, with a warning."""
    decl = re.compile(rf"^namespace ([A-Za-z_][A-Za-z0-9_]*) = {re.escape(namespace)};$", re.MULTILINE)

    def word(w: str) -> re.Pattern[str]:
        return re.compile(rf"(?<![A-Za-z0-9_]){re.escape(w)}(?![A-Za-z0-9_])")

    changed = 0
    for rel in project_files(root):
        posix = rel.as_posix()
        if not is_text(rel) or any(posix == e or posix.startswith(e) for e in REWRITE_EXCLUDES):
            continue
        path = root / rel
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        m = decl.search(text)
        if not m or m.group(1) == alias:
            continue
        if word(alias).search(text):
            print(f"warning: {posix}: kept the namespace alias {m.group(1)} ({alias} already names something there)")
            continue
        path.write_text(word(m.group(1)).sub(alias, text), encoding="utf-8")
        changed += 1
    return changed


def rename_rules(old: dict[str, str], new: dict[str, str]) -> list[tuple[str, str]]:
    """The identity tokens a rename replaces in the text files. The four-character codes are not among them (a code
    can be an ordinary word, "Gain"): they live in cmake/Product.cmake only, which is rewritten whole."""
    rules = [
        (old["NAME"], new["NAME"]),
        (old["NAME"].upper(), new["NAME"].upper()),
        (old["NAMESPACE"], new["NAMESPACE"]),
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


def new_identity(args: argparse.Namespace, cur: dict[str, str], keep_company: bool) -> dict[str, str]:
    y = args.yes
    name = ask("Plugin name (PascalCase)", args.name, None, check_name, y)
    kind = ask("Kind (effect | instrument)", args.type, cur["KIND"],
               lambda v: None if v in KINDS else "effect or instrument", y)
    if keep_company:
        company = cur["COMPANY"]
    else:
        company = ask("Company", args.company, cur["COMPANY"], None, y)
    mfr = ask("Manufacturer code (the same for all your plugins)", args.mfr, cur["MFR_CODE"], check_code, y)
    code = ask("Plugin code (4 characters, unique)", args.code, derive_code(name), check_code, y)
    bundle = ask("Bundle id", args.bundle_id, f"com.{lower_alnum(company) or 'funk'}.{lower_alnum(name)}",
                 check_bundle_id, y)
    namespace = ask("C++ namespace", args.namespace, lower_alnum(name), check_namespace, y)
    env = ask("FunkGui environment prefix", args.env_prefix, re.sub(r"[^A-Z0-9]", "", code.upper()) + "_",
              check_env_prefix, y)
    return {"NAME": name, "KIND": kind, "VERSION": "0.1.0" if args.reset_version else cur["VERSION"],
            "COMPANY": company, "MFR_CODE": mfr, "CODE": code, "BUNDLE_ID": bundle, "NAMESPACE": namespace,
            "ENV_PREFIX": env, "OBJC_PREFIX": objc_prefix(code)}


def check_code_free(code: str, dirs: list[Path], skip: set[Path], allow: bool) -> None:
    used = sibling_codes(dirs, skip)
    if code in used:
        msg = f"plugin code {code} is already used by {', '.join(sorted(set(used[code])))}"
        if allow:
            print(f"warning: {msg} (--allow-code-clash)")
        else:
            taken = ", ".join(sorted(used))
            sys.exit(f"{msg}: hosts would confuse the two plugins. Choose another with --code (codes in use: {taken})")


def print_identity(cfg: dict[str, str], title: str) -> None:
    print(title)
    for key in PRODUCT_KEYS:
        print(f"  {key.lower():<12} {cfg[key]}")


# ----------------------------------------------------------------------------------------------------------------------
# Subcommands
# ----------------------------------------------------------------------------------------------------------------------

def cmd_info(args: argparse.Namespace) -> int:
    print_identity(read_product(PROJECT_ROOT), f"{PROJECT_ROOT}")
    return 0


def cmd_clone(args: argparse.Namespace) -> int:
    src = PROJECT_ROOT
    cur = read_product(src)
    if args.starter or (args.type and args.type != cur["KIND"]):
        require_starter(src, args.type or cur["KIND"])
    print(f"Cloning {cur['NAME']} ({cur['KIND']}) from {src}\n")
    new = new_identity(args, cur, keep_company=False)

    dest = Path(args.dest).expanduser().resolve() if args.dest else (src.parent / new["NAME"])
    if dest == src or src in dest.parents:
        sys.exit(f"the destination {dest} is inside the source project")
    if new["CODE"] == cur["CODE"]:
        sys.exit(f"the clone needs its own plugin code (this project's is {cur['CODE']}): pass --code")
    check_code_free(new["CODE"], code_dirs(src.parent, dest.parent), {dest.resolve()}, args.allow_code_clash)

    starter = args.starter or new["KIND"] != cur["KIND"]
    if starter:
        require_starter(src, new["KIND"])                   # a kind given at the prompt
    print()
    print_identity(new, f"New plugin at {dest}:")
    print(f"  {'dsp code':<12} " + (f"the {new['KIND']} starter (scripts/starters/{new['KIND']})" if starter
                                    else f"this project's ({cur['KIND']})"))
    print()

    if dest.exists() and any(dest.iterdir()):
        if not args.force:
            sys.exit(f"{dest} exists and is not empty: pass --force to replace its contents (its .git is kept)")
        if not confirm(f"Replace everything in {dest} except .git?", args.yes):
            print("Aborted.")
            return 1
    elif not confirm("Proceed?", args.yes):
        print("Aborted.")
        return 1

    files = [rel for rel in project_files(src) if not (rel.parts and rel.parts[0] in COPY_EXCLUDES)]
    with tempfile.TemporaryDirectory(prefix="plugin-clone-") as tmp:
        stage = Path(tmp) / "stage"
        for rel in files:
            target = stage / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src / rel, target)
        if starter:
            apply_starter(src, stage, new["KIND"])
        drop_origin_blocks(stage)
        write_product(stage, new)
        n = rewrite_tokens(stage, rename_rules(cur, new))
        n += rename_aliases(stage, new["NAMESPACE"], alias_for(new["NAME"]))

        dest.mkdir(parents=True, exist_ok=True)
        for entry in dest.iterdir():
            if entry.name == ".git":
                continue
            if entry.is_dir() and not entry.is_symlink():
                shutil.rmtree(entry)
            else:
                entry.unlink()
        for entry in stage.iterdir():
            shutil.move(str(entry), str(dest / entry.name))

    for script in (dest / "scripts").glob("*"):
        if script.suffix in (".py", ".sh"):
            script.chmod(0o755)
    print(f"Cloned {len(files)} files into {dest} ({n} rewritten).")
    print("Next:")
    print(f"  cd {dest}")
    print("  cmake --workflow --preset dev-verify      # configure, build the plugin and the probes, run every test")
    if not (dest / ".git").exists():
        print("  git init && git add -A && git commit -m 'Start from the template'")
    return 0


def cmd_rename(args: argparse.Namespace) -> int:
    root = PROJECT_ROOT
    cur = read_product(root)
    print(f"Renaming {cur['NAME']} in place ({root})\n")
    if args.type and args.type != cur["KIND"]:
        sys.exit("rename keeps the kind; make an instrument from an effect (or back) with clone --type")
    args.type = cur["KIND"]
    new = new_identity(args, cur, keep_company=args.company is None)
    if new["CODE"] != cur["CODE"]:
        check_code_free(new["CODE"], code_dirs(root.parent), {root.resolve()}, args.allow_code_clash)
    print()
    print_identity(new, "Renamed to:")
    if not confirm("Apply the rename in place?", args.yes):
        print("Aborted.")
        return 1
    write_product(root, new)
    n = rewrite_tokens(root, rename_rules(cur, new))
    n += rename_aliases(root, new["NAMESPACE"], alias_for(new["NAME"]))
    print(f"Renamed: {n} files rewritten. Delete the build directories (rm -rf build-*) before building again.")
    return 0


def cmd_starter_diff(args: argparse.Namespace) -> int:
    cur = read_product(PROJECT_ROOT)
    kind = args.kind or cur["KIND"]
    require_starter(PROJECT_ROOT, kind)
    starter = PROJECT_ROOT / "scripts" / "starters" / kind
    rc = 0
    for rel in STARTER_PATHS:
        r = subprocess.run(["diff", "-ru", str(starter / rel), str(PROJECT_ROOT / rel)])
        rc = max(rc, r.returncode)
    readme_parts, readme = starter / STARTER_README, PROJECT_ROOT / "README.md"
    parts = readme_blocks(readme_parts.read_text(encoding="utf-8")) if readme_parts.is_file() else {}
    ours = readme_blocks(readme.read_text(encoding="utf-8")) if readme.is_file() else {}
    for name, block in parts.items():
        if ours.get(name) != block:
            rc = max(rc, 1)
            sys.stdout.writelines(difflib.unified_diff(block.splitlines(keepends=True),
                                                       ours.get(name, "").splitlines(keepends=True),
                                                       f"{starter / STARTER_README} [{name}]",
                                                       f"README.md [{name}]"))
    if rc == 0:
        paths = ", ".join(p.as_posix() for p in STARTER_PATHS)
        print(f"{paths} and the README's starter blocks are the {kind} starter, unchanged.")
    return 0 if rc in (0, 1) else rc


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


def cmd_codesign(args: argparse.Namespace) -> int:
    name = read_product(PROJECT_ROOT)["NAME"]
    for bundle in find_bundles((PROJECT_ROOT / args.build_dir).resolve(), name):
        subprocess.run(["xattr", "-cr", str(bundle)], check=True)
        cmd = ["codesign", "--force", "--deep", "--options", "runtime", "--timestamp", "--sign", args.identity,
               str(bundle)]
        print("$ " + " ".join(cmd))
        if subprocess.run(cmd).returncode != 0:
            sys.exit(f"codesign failed for {bundle}")
    print("All bundles signed.")
    return 0


def cmd_notarize(args: argparse.Namespace) -> int:
    name = read_product(PROJECT_ROOT)["NAME"]
    bundles = find_bundles((PROJECT_ROOT / args.build_dir).resolve(), name)
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
    p.add_argument("--type", choices=KINDS, help="effect or instrument (default: this project's kind)")
    p.add_argument("--code", help="4-character plugin code, unique among your plugins (default: from the name)")
    p.add_argument("--mfr", help="4-character manufacturer code (default: this project's)")
    p.add_argument("--company", help="company name (default: this project's)")
    p.add_argument("--bundle-id", help="reverse-DNS bundle id (default: com.<company>.<name>)")
    p.add_argument("--namespace", help="C++ namespace (default: the name in lower case)")
    p.add_argument("--env-prefix", help="FunkGui's environment prefix (default: the code in upper case, then _)")
    p.add_argument("--allow-code-clash", action="store_true", help="accept a plugin code another project uses")
    p.add_argument("--reset-version", action="store_true", help="start at version 0.1.0")
    p.add_argument("--yes", "-y", action="store_true", help="take the derived defaults and do not ask")


def main() -> int:
    parser = argparse.ArgumentParser(prog="plugin.py", description=__doc__.strip().replace("{template}", TEMPLATE),
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    subs = parser.add_subparsers(dest="command", required=True)

    p = subs.add_parser("info", help="print this project's identity")
    p.set_defaults(func=cmd_info)

    p = subs.add_parser("clone", help="copy this project into a new plugin")
    add_identity_options(p)
    p.add_argument("--dest", help="the new project's directory (default: beside this one, named after it)")
    p.add_argument("--starter", action="store_true",
                   help="replace the algorithm, its probes, CLAUDE.md and README blocks with the kind's starter "
                        "(automatic for a new kind)")
    p.add_argument("--force", action="store_true", help="replace a non-empty destination's contents (not its .git)")
    p.set_defaults(func=cmd_clone)

    p = subs.add_parser("rename", help="rename this project in place")
    add_identity_options(p)
    p.set_defaults(func=cmd_rename)

    p = subs.add_parser("starter-diff",
                        help="diff the algorithm, its probes, CLAUDE.md and README blocks against a starter")
    p.add_argument("--kind", choices=KINDS, help="the starter to compare with (default: this project's kind)")
    p.set_defaults(func=cmd_starter_diff)

    p = subs.add_parser("codesign", help="codesign the built bundles")
    p.add_argument("--identity", required=True, help='"Developer ID Application: Name (TEAMID)"')
    p.add_argument("--build-dir", default="build-release", help="build directory (default: build-release)")
    p.set_defaults(func=cmd_codesign)

    p = subs.add_parser("notarize", help="notarize and staple the signed bundles")
    p.add_argument("--keychain-profile", required=True, help="notarytool keychain profile")
    p.add_argument("--build-dir", default="build-release", help="build directory (default: build-release)")
    p.set_defaults(func=cmd_notarize)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
