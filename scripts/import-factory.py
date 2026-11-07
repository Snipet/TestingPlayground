#!/usr/bin/env python3
"""
The factory projects' import: each chosen project's latest version, renamed, into this repository's factory folders,
which CMakeLists.txt compiles into the plugin, read-only (Source/plugin/Factory.h; README.md, "Factory projects").

  scripts/import-factory.py --source <projects folder> --names <map.tsv> [--dry-run] [--author <name>]

--source  a folder of projects (<name>.fmproject files, as the plugin saves them), or one whose subfolders are the
          products' (<ProductName>/<name>.fmproject: ~/Library/Audio/Presets/<Company>/ holds them so). A row's
          project is <source>/<ProductName>/<from>.fmproject, else <source>/<from>.fmproject, its name case aside.
--names   the name map: a TSV, a row a project, three columns: product (a product's key, synth or effect, or its
          name), from (the project's name in the source folder) and to (its name as a factory project), and a fourth
          if wanted, category: the factory project's (PROJECTS shows it; its own when the column is empty or missing).
          Blank lines, lines that start with # and a first row naming the columns are skipped. The map is the user's:
          keep it out of the repository (give it by its path).
--dry-run says what it would write and remove, and writes nothing. Either way it prints each project's bytes and its
          largest text (each text is one string literal in the plugin's source: over 64 KB it still compiles).
--author  sets every project's author ("" leaves it empty); by default each keeps its own.
--factory the folder to write into (default: this repository's factory/), and --products where the products' files
          are (default: this repository's products/): the probes use them.

What it writes, for each product the map names (the others are left as they are): factory/<key>/<to>/, a folder a
project: project.json (a project file's JSON, as the plugin writes one, of one version, its files' texts left out)
and each of that version's files beside it, by its name, as its text. Of a project it keeps its latest version only
(numbered 1, its note "factory version"): its files, the tab shown, its dials (with their values' names) and its
netlists' self-test answers; its presets (each one's "from" 1 when it was saved with that version, else 0, and its
category, when it has one); and its info (description, author, category, tags, genres; the map's category over its
own). Nothing else of the file comes: its older versions, and any field this script does not know.

Re-runnable: factory/<key>/imported.txt lists what its last run wrote there, and a run removes those folders before
it writes the map's, so it replaces what it wrote and nothing else. A folder of a name the map gives that no import
wrote (one made by hand) stops it.

What it refuses, writing nothing: a row it cannot read (a product it does not know, a project it cannot find or read,
one of the other kind or from a newer plugin, a name the plugin would change, a file name a tab may not have, a name
twice); and anything private in what it would write: a path on someone's machine (/Users/<name>, /home/<name>,
/Volumes/..., C:\\..., file://...), an email address, or a key or token (an API key's shapes, a private key's block, a
password or secret assigned, or a long run of mixed letters and digits). It says where (the project, the field, the
line) with the text masked: fix the project, or the map, and run it again.

Stdlib only.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from pathlib import Path

SCRIPT_PATH = Path(__file__).resolve()
PROJECT_ROOT = SCRIPT_PATH.parent.parent

FORMAT = "funk-playground-project"
FORMAT_VERSION = 1
EXTENSION = ".fmproject"
MANIFEST = "imported.txt"
META = "project.json"
MAX_NAME = 40          # ProjectStore.h's kMaxName
MAX_FILE_NAME = 24     # Sources.h's kMaxNameLength
NOTE = "factory version"
MAX_CATEGORY = 24     # ProjectStore.h's kMaxTag: a category's length
LITERAL_LIMIT = 65536  # a string literal's length a compiler must take: longer ones compile (FactoryProjects.h)

# What is private (refused): a path on someone's machine, an email address, a key or a token.
PRIVATE = [
    ("a path on someone's machine", re.compile(r"/(?:Users|home)/[^/\s\"'<>|]+")),
    ("a path on someone's machine", re.compile(r"/(?:Volumes|private|var/folders|tmp)/[^\s\"'<>|]+")),
    ("a path on someone's machine", re.compile(r"(?<![A-Za-z0-9])[A-Za-z]:\\[^\s\"'<>|]+")),
    ("a path on someone's machine", re.compile(r"\\\\[A-Za-z0-9._-]+\\[^\s\"'<>|]+")),
    ("a path on someone's machine", re.compile(r"file://[^\s\"'<>|]+", re.IGNORECASE)),
    ("an email address", re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}")),
    ("a key or token", re.compile(r"sk-ant-[A-Za-z0-9_-]{8,}")),
    ("a key or token", re.compile(r"(?<![A-Za-z0-9])sk-[A-Za-z0-9_-]{20,}")),
    ("a key or token", re.compile(r"(?<![A-Za-z0-9])AKIA[0-9A-Z]{16}(?![A-Za-z0-9])")),
    ("a key or token", re.compile(r"(?<![A-Za-z0-9])gh[pousr]_[A-Za-z0-9]{20,}")),
    ("a key or token", re.compile(r"github_pat_[A-Za-z0-9_]{20,}")),
    ("a key or token", re.compile(r"(?<![A-Za-z0-9])xox[abprs]-[A-Za-z0-9-]{10,}")),
    ("a key or token", re.compile(r"(?<![A-Za-z0-9])AIza[0-9A-Za-z_-]{30,}")),
    ("a key or token", re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")),
    ("a key or token", re.compile(r"(?i)(?:api[_-]?key|secret|token|passw(?:or)?d)\s*[:=]\s*[\"']?[A-Za-z0-9_\-/+=.]{12,}")),
]
# A long run of letters and digits mixing upper case, lower case and digits: a secret's look, never a word's.
LONG_RUN = re.compile(r"[A-Za-z0-9+/_-]{40,}")


def looks_secret(run: str) -> bool:
    return any(c.isupper() for c in run) and any(c.islower() for c in run) and any(c.isdigit() for c in run)


def masked(text: str) -> str:
    return text[:4] + "..." if len(text) > 4 else "..."


def tidy_name(name: str) -> str:
    """ProjectStore's tidyName: printable ASCII but / \\ : * ? " < > | and a leading '.', spaces collapsed and trimmed,
    at most 40 characters."""
    out: list[str] = []
    space = False
    for ch in name:
        if ch in " \t\n\r":
            space = bool(out)
            continue
        c = ord(ch)
        if c < 0x21 or c > 0x7E or ch in '/\\:*?"<>|':
            continue
        if not out and ch == ".":
            continue
        if space:
            out.append(" ")
        space = False
        out.append(ch)
    return "".join(out)[:MAX_NAME].rstrip(" ")


def valid_file_name(name: str) -> bool:
    """Sources.h's validFileName, and never the folder's own project.json."""
    return (0 < len(name) <= MAX_FILE_NAME and not name.startswith(".") and name != META
            and re.fullmatch(r"[A-Za-z0-9_.-]+", name) is not None)


class Refusal(Exception):
    pass


def read_products(products_dir: Path) -> dict[str, dict[str, str]]:
    """Every products/<key>.cmake's name and kind, by key."""
    out: dict[str, dict[str, str]] = {}
    for path in sorted(products_dir.glob("*.cmake")):
        text = path.read_text(encoding="utf-8")
        values = {}
        for k in ("NAME", "KIND", "COMPANY"):
            m = re.search(rf'^set\(PRODUCT_{k}\s+"([^"]*)"\)', text, re.MULTILINE)
            if not m:
                sys.exit(f"{path}: no line `set(PRODUCT_{k} \"...\")`")
            values[k] = m.group(1)
        out[path.stem] = values
    if not out:
        sys.exit(f"{products_dir}: no products/<key>.cmake")
    return out


def product_key(word: str, products: dict[str, dict[str, str]]) -> str | None:
    for key, p in products.items():
        if word.lower() in (key.lower(), p["NAME"].lower()):
            return key
    return None


def tidy_category(text: str) -> str:
    """A category as the plugin keeps one (ProjectStore's tidyInfo): printable ASCII, the spaces collapsed, at most 24."""
    out = " ".join("".join(c if " " <= c <= "~" else ("?" if c not in " \t\r\n" else " ") for c in text).split())
    return out[:MAX_CATEGORY].rstrip()


def read_map(path: Path, products: dict[str, dict[str, str]]) -> list[tuple[str, str, str, int, str]]:
    """The map's rows: (product key, from, to, line number, category: "" for the project's own)."""
    rows: list[tuple[str, str, str, int, str]] = []
    errors: list[str] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as e:
        raise Refusal(f"{path}: {e.strerror}")
    for n, line in enumerate(lines, 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cols = [c.strip() for c in line.split("\t")]
        if not rows and not errors and [c.lower() for c in cols[:3]] == ["product", "from", "to"]:
            continue
        if len(cols) not in (3, 4) or not all(cols[:3]):
            errors.append(f"{path.name}:{n}: a row is three columns, separated by tabs: product, from, to "
                          "(and a fourth, category, if wanted)")
            continue
        category = cols[3] if len(cols) == 4 else ""
        if category and tidy_category(category) != category:
            errors.append(f"{path.name}:{n}: {category!r} is no category (the plugin would keep {tidy_category(category)!r})")
            continue
        key = product_key(cols[0], products)
        if key is None:
            errors.append(f"{path.name}:{n}: no product {cols[0]!r} (the keys: {', '.join(products)})")
            continue
        rows.append((key, cols[1], cols[2], n, category))
    if errors:
        raise Refusal("\n".join(errors))
    if not rows:
        raise Refusal(f"{path.name}: no row")
    return rows


def find_project(source: Path, product_name: str, name: str) -> Path | None:
    """The project's file, as the plugin finds it: its name case aside, in the product's folder first."""
    for folder in (source / product_name, source):
        if not folder.is_dir():
            continue
        for f in folder.iterdir():
            if f.is_file() and f.name.lower() == (name + EXTENSION).lower():
                return f
    return None


def private_findings(where: str, text: str) -> list[str]:
    found: list[str] = []
    for n, line in enumerate(text.splitlines() or [text], 1):
        at = f"{where}, line {n}" if "\n" in text else where
        for what, pattern in PRIVATE:
            for m in pattern.finditer(line):
                found.append(f"{at}: {what} ({masked(m.group(0))})")
        for m in LONG_RUN.finditer(line):
            if looks_secret(m.group(0)):
                found.append(f"{at}: a key or token, by its look ({masked(m.group(0))})")
    return list(dict.fromkeys(found))                    # once each, in order


def strings_of(value, path: str):
    """Every string in a JSON value, with where it is."""
    if isinstance(value, str):
        yield path, value
    elif isinstance(value, list):
        for i, v in enumerate(value):
            yield from strings_of(v, f"{path}[{i}]")
    elif isinstance(value, dict):
        for k, v in value.items():
            yield from strings_of(v, f"{path}.{k}" if path else k)


def dials_of(value) -> list[dict]:
    out = []
    for d in value if isinstance(value, list) else []:
        if not isinstance(d, dict) or not isinstance(d.get("name"), str):
            continue
        dial = {"name": d["name"], "value": d.get("value", 0.0)}
        if isinstance(d.get("label"), str) and d["label"]:
            dial["label"] = d["label"]
        out.append(dial)
    return out


def convert(src: Path, product: dict[str, str], to: str, author: str | None) -> tuple[dict, list[tuple[str, str]]]:
    """The factory project from a project file: its project.json and its files (name, text)."""
    label = src.name
    try:
        root = json.loads(src.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as e:
        raise Refusal(f"{label}: not a project ({e})")
    if not isinstance(root, dict) or root.get("format") != FORMAT:
        raise Refusal(f"{label}: not a project (or it is damaged)")
    if int(root.get("version", 0) or 0) > FORMAT_VERSION:
        raise Refusal(f"{label}: from a newer version of the plugin")
    if root.get("kind") != product["KIND"]:
        raise Refusal(f"{label}: a project of the {root.get('kind')} playground, not of {product['NAME']} "
                      f"({product['KIND']})")
    versions = [v for v in root.get("versions", []) if isinstance(v, dict) and isinstance(v.get("files"), list)
                and v["files"] and int(v.get("number", 0) or 0) > 0]
    if not versions:
        raise Refusal(f"{label}: no program")
    latest = max(versions, key=lambda v: int(v["number"]))
    number = int(latest["number"])

    files: list[tuple[str, str]] = []
    for f in latest["files"]:
        if not isinstance(f, dict) or not isinstance(f.get("name"), str) or not isinstance(f.get("text"), str):
            raise Refusal(f"{label}: version {number} has a file with no name or text")
        if not valid_file_name(f["name"]):
            raise Refusal(f"{label}: version {number}'s file {f['name']!r} is no name a tab may have")
        if any(f["name"].lower() == g.lower() for g, _ in files):
            raise Refusal(f"{label}: version {number} has {f['name']!r} twice")
        if ")factory\"" in f["text"]:
            raise Refusal(f"{label}: {f['name']} holds the text )factory\" (the end of the raw string it is compiled into)")
        files.append((f["name"], f["text"]))

    version = {
        "number": 1,
        "saved": int(latest.get("saved", 0) or 0),
        "note": NOTE,
        "active": max(0, min(int(latest.get("active", 0) or 0), len(files) - 1)),
        "files": [{"name": name} for name, _ in files],
        "dials": dials_of(latest.get("dials")),
    }
    circuits = [{"key": c["key"], "text": c["text"]} for c in latest.get("circuits", []) or []
                if isinstance(c, dict) and isinstance(c.get("key"), str) and isinstance(c.get("text"), str)]
    if circuits:
        version["circuits"] = circuits

    presets = []
    seen: set[str] = set()
    for p in root.get("presets", []) or []:
        if not isinstance(p, dict) or not isinstance(p.get("name"), str):
            continue
        name = tidy_name(p["name"])
        if not name or name.lower() in seen:
            continue
        seen.add(name.lower())
        preset = {"name": name, "saved": int(p.get("saved", 0) or 0),
                  "from": 1 if int(p.get("from", 0) or 0) == number else 0}
        if isinstance(p.get("category"), str) and tidy_category(p["category"]):
            preset["category"] = tidy_category(p["category"])   # the top bar's list groups by it
        preset["dials"] = dials_of(p.get("dials"))
        presets.append(preset)
    presets.sort(key=lambda p: (p["name"].lower(), p["name"]))

    out = {"format": FORMAT, "version": FORMAT_VERSION, "plugin": product["NAME"], "kind": product["KIND"], "name": to,
           "versions": [version], "presets": presets}
    info = root.get("info")
    if isinstance(info, dict):
        kept = {"description": info.get("description", ""), "author": info.get("author", ""),
                "category": info.get("category", ""),
                "tags": [t for t in info.get("tags", []) or [] if isinstance(t, str)],
                "genres": [g for g in info.get("genres", []) or [] if isinstance(g, str)]}
        for k in ("description", "author", "category"):
            if not isinstance(kept[k], str):
                kept[k] = ""
        if author is not None:
            kept["author"] = author
        if any(kept.values()):
            out["info"] = kept
    elif author:
        out["info"] = {"description": "", "author": author, "category": "", "tags": [], "genres": []}
    return out, files


def read_manifest(folder: Path) -> list[str]:
    path = folder / MANIFEST
    if not path.is_file():
        return []
    return [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")]


def write_manifest(folder: Path, names: list[str]) -> None:
    lines = ["# Written by scripts/import-factory.py: the factory projects its last run wrote here, one a line,",
             "# which its next run replaces. A folder not listed here is not the import's: it never touches one."]
    (folder / MANIFEST).write_text("\n".join(lines + sorted(names, key=lambda n: (n.lower(), n))) + "\n",
                                   encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Import factory projects (README.md, \"Factory projects\").")
    ap.add_argument("--source", required=True, type=Path, help="the projects folder")
    ap.add_argument("--names", required=True, type=Path, help="the name map: product, from, to (a TSV)")
    ap.add_argument("--dry-run", action="store_true", help="say what it would do, write nothing")
    ap.add_argument("--author", default=None, help="every project's author (\"\" for none)")
    ap.add_argument("--factory", type=Path, default=PROJECT_ROOT / "factory", help=argparse.SUPPRESS)
    ap.add_argument("--products", type=Path, default=PROJECT_ROOT / "products", help=argparse.SUPPRESS)
    args = ap.parse_args()

    products = read_products(args.products)
    try:
        rows = read_map(args.names, products)
        if not args.source.is_dir():
            raise Refusal(f"{args.source}: no such folder")

        # Read and check everything first: a refusal writes nothing.
        errors: list[str] = []
        plan: dict[str, list[tuple[str, dict, list[tuple[str, str]]]]] = {}
        tos: dict[str, set[str]] = {}
        froms: dict[str, set[str]] = {}
        for key, src_name, to, line, category in rows:
            product = products[key]
            where = f"{args.names.name}:{line}"
            if tidy_name(to) != to or not to:
                errors.append(f"{where}: {to!r} is no project's name (the plugin would call it {tidy_name(to)!r})")
                continue
            if to.lower() in tos.setdefault(key, set()):
                errors.append(f"{where}: {to!r} twice for {key}")
                continue
            if src_name.lower() in froms.setdefault(key, set()):
                errors.append(f"{where}: {src_name!r} twice for {key}")
                continue
            tos[key].add(to.lower())
            froms[key].add(src_name.lower())
            src = find_project(args.source, product["NAME"], src_name)
            if src is None:
                errors.append(f"{where}: no project {src_name!r} of {product['NAME']} in the source folder")
                continue
            try:
                meta, files = convert(src, product, to, args.author)
            except Refusal as e:
                errors.append(f"{where}: {e}")
                continue
            if category:                                 # the map's, over the project's own
                info = meta.setdefault("info", {"description": "", "author": "", "category": "", "tags": [], "genres": []})
                info["category"] = category
            found = []
            for path, text in strings_of(meta, ""):
                found += private_findings(f"{to}: {path}", text)
            for name, text in files:
                found += private_findings(f"{to}: {name}", text)
            errors += [f"{where}: {f}" for f in found]
            plan.setdefault(key, []).append((to, meta, files))

        for key in plan:
            folder = args.factory / key
            ours = {n.lower() for n in read_manifest(folder)}
            for to, _, _ in plan[key]:
                target = folder / to
                if target.exists() and to.lower() not in ours:
                    errors.append(f"factory/{key}/{to} is there and no import wrote it: rename one of them")
        if errors:
            raise Refusal("\n".join(errors))
    except Refusal as e:
        print(f"Refused (nothing written):\n{e}", file=sys.stderr)
        return 1

    for key in sorted(plan):
        folder = args.factory / key
        old = read_manifest(folder)
        new = [to for to, _, _ in plan[key]]
        gone = [n for n in old if n.lower() not in {t.lower() for t in new}]
        total = 0
        for to, meta, files in plan[key]:
            sizes = [(META, len(json.dumps(meta, indent=2, ensure_ascii=True)) + 1)]
            sizes += [(name, len(text.encode("utf-8"))) for name, text in files]
            size = sum(s for _, s in sizes)
            total += size
            largest = max(sizes, key=lambda s: s[1])     # each text is one string literal in the plugin's source
            over = (f", over the {LITERAL_LIMIT:,} bytes -Wpedantic warns about: it compiles"
                    if largest[1] > LITERAL_LIMIT else "")
            print(f"{key}: {to} ({len(files)} file{'s' if len(files) != 1 else ''}, {len(meta['presets'])} "
                  f"preset{'s' if len(meta['presets']) != 1 else ''}, {size:,} bytes; the largest text "
                  f"{largest[0]}, {largest[1]:,} bytes{over})")
        for n in gone:
            print(f"{key}: {n} removed (the last import's, not in the map)")
        print(f"{key}: {len(new)} factory project{'s' if len(new) != 1 else ''}, {total:,} bytes"
              f"{' (dry run: nothing written)' if args.dry_run else ''}")
        if args.dry_run:
            continue
        folder.mkdir(parents=True, exist_ok=True)
        for n in old:                                    # what the last run wrote, and only that
            target = folder / n
            if target.is_dir() and target.parent == folder:
                shutil.rmtree(target)
        for to, meta, files in plan[key]:
            target = folder / to
            target.mkdir()
            (target / META).write_text(json.dumps(meta, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
            for name, text in files:
                (target / name).write_bytes(text.encode("utf-8"))
        write_manifest(folder, new)
    return 0


if __name__ == "__main__":
    sys.exit(main())
