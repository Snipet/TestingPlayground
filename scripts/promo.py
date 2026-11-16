#!/usr/bin/env python3
"""
Promotional videos of a product's projects: short vertical videos (1080 x 1920, 60 fps, H.264 and AAC) of the user's
own projects playing, made offline and the same every run. README.md, "Promotional videos", says how to use them.

  scripts/promo.py <key> list                         the product's projects, each with its storyboard and video
  scripts/promo.py <key> info <project>               a project's dials, pages, presets and files (JSON)
  scripts/promo.py <key> new <project>                a starter storyboard for it (never over one)
  scripts/promo.py <key> render [<project>...]        the videos of these projects' storyboards (all when none named)
        [--stills 1,6.5]  PNGs of each view at those seconds instead (in its work folder), to check a storyboard
        [--keep]          keep the work folders (the views' videos, the audio, the cards)
        [--no-build]      use the renderer as it is built (for several renders at once: one build folder, one ninja)

<key> names products/<key>.cmake. The folder (--dir) is ~/Movies/<Name> Promos/: its storyboards/<project>.json say
what each video plays and shows, the videos land beside them as <project>.mp4, and defaults.json (optional) holds
keys every storyboard takes unless it says otherwise. The projects are read, never written: the renderer copies the
one it plays into a sandbox of its own.

How a video is made, for each storyboard:
  1. Tools/promo/cards.swift draws the text (this ffmpeg has no text filter) round the views' sizes (the renderer's
     --sizes): the header, the labels, the rounded corners and the footer (overlay.png), the end card (end.png), and
     the layout (layout.json).
  2. Tools/promo (<Name>Promo, built into --build's folder by `--target <Name>Promo`, which this script runs) plays the
     project in a real processor from the storyboard's notes and dial moves and draws the editor headless on every
     frame: the PANEL view's interface (panel.mp4) and the CODE view's program (code.mp4, as tall as the layout shows
     it), with audio.wav and meta.json.
  3. ffmpeg composes them on the theme's ground, the audio at -14 LUFS (two-pass loudnorm, then a limiter at -2 dBFS
     at 192 kHz), the end card faded in for its last seconds and the sound faded out under it.

A storyboard (JSON) holds the renderer's keys (Tools/promo/Main.cpp's header: seconds, tempo, preset, page, notes,
repeat, midi, dials, code, window) and the compositor's:
  project      the project's name, as PROJECTS shows it (default: the file's name)
  title        the header's big words (default: the project's name); subtitle: a line or two under it
  panel_label  the words over the panel (default "THE SOUND"); code_label: over the code (default "<file>  ·  N LINES")
  tagline      the footer's line, in the accent (default: the product's, as its lockup says it); footer: under it
  end          the end card's seconds (default 2.5; 0: none)

Stdlib only; needs ffmpeg and swift on the PATH (or --ffmpeg, --swift).
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
W, H = 1080, 1920
VIDEO_W = 1000                     # the views' width in the video (W less its margins)

# The GRAPHITE theme as Tools/logo/logo.swift has it (funkgui::Theme::graphite()), the ink ladder's middle steps mixed
# from its ends as FunkGui mixes them.
GROUND, INK = 0x16171A, 0xECEAE4


def mix(a: int, b: int, t: float) -> str:
    out = 0
    for shift in (16, 8, 0):
        x, y = (a >> shift) & 255, (b >> shift) & 255
        out |= int(round(x + (y - x) * t)) << shift
    return f"#{out:06x}"


def read_product(key: str) -> dict[str, str]:
    path = ROOT / "products" / f"{key}.cmake"
    if not path.exists():
        keys = ", ".join(sorted(p.stem for p in (ROOT / "products").glob("*.cmake")))
        sys.exit(f"no products/{key}.cmake (the products: {keys})")
    text = path.read_text(encoding="utf-8")
    cfg = {}
    for k in ("NAME", "KIND", "COMPANY"):
        m = re.search(rf'^set\(PRODUCT_{k}\s+"([^"]*)"\)', text, re.MULTILINE)
        if not m:
            sys.exit(f"{path}: no PRODUCT_{k}")
        cfg[k] = m.group(1)
    return cfg


def accent(kind: str) -> str:
    """The product's accent on GRAPHITE: the instrument's from Source/editor/Theme.h (as logo.swift reads it), the
    effect's FunkGui's own orange."""
    if kind != "instrument":
        return "#ff5a1f"
    text = (ROOT / "Source" / "editor" / "Theme.h").read_text(encoding="utf-8")
    m = re.search(r"t\.accent = dark \? funkgui::Col\{ 0x([0-9A-Fa-f]{2}), 0x([0-9A-Fa-f]{2}), 0x([0-9A-Fa-f]{2}) \}", text)
    return "#" + "".join(m.groups()).lower() if m else "#4fc3f7"


def tagline(kind: str) -> str:
    return "an effect you write while it plays" if kind == "effect" else "a synth you write while it plays"


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    r = subprocess.run(cmd, text=True, capture_output=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit(f"failed: {' '.join(str(c) for c in cmd[:3])} ...")
    return r


class Promo:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.product = read_product(args.key)
        self.name = self.product["NAME"]
        self.kind = self.product["KIND"]
        self.projects = Path.home() / "Library" / "Audio" / "Presets" / self.product["COMPANY"] / self.name
        self.dir = Path(args.dir).expanduser() if args.dir else Path.home() / "Movies" / f"{self.name} Promos"
        self.storyboards = self.dir / "storyboards"
        self.build = (ROOT / args.build).resolve()
        self.exe = self.build / f"{self.name}Promo"

    # ---- the projects and the storyboards ------------------------------------------------------------------------------
    def project_file(self, project: str) -> Path:
        path = self.projects / f"{project}.fmproject"
        if path.exists():
            return path
        for p in sorted(self.projects.glob("*.fmproject")):
            try:
                if json.loads(p.read_text(encoding="utf-8")).get("name") == project:
                    return p
            except (OSError, ValueError):
                continue
        sys.exit(f"no project \"{project}\" in {self.projects}")

    def defaults(self) -> dict:
        path = self.dir / "defaults.json"
        return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}

    def storyboard(self, path: Path) -> dict:
        story = self.defaults()
        story.update(json.loads(path.read_text(encoding="utf-8")))
        story.setdefault("project", path.stem)
        return story

    def ensure_built(self) -> None:
        if self.args.no_build:
            if not self.exe.exists():
                sys.exit(f"no {self.exe}: run once without --no-build")
            return
        if not (self.build / "CMakeCache.txt").exists():
            sys.exit(f"{self.build} is not configured: cmake --preset dev")
        print(f"building {self.exe.name} ...", flush=True)
        run(["cmake", "--build", str(self.build), "--target", self.exe.name, "-j6"])

    # ---- list, info, new ---------------------------------------------------------------------------------------------
    def list(self) -> None:
        print(f"projects: {self.projects}\nvideos:   {self.dir}")
        for p in sorted(self.projects.glob("*.fmproject")):
            name = p.stem
            sb = self.storyboards / f"{name}.json"
            video = self.dir / f"{name}.mp4"
            print(f"  {name:28s} {'storyboard' if sb.exists() else '-':11s} {'video' if video.exists() else '-'}")

    def info(self, project: str) -> None:
        self.ensure_built()
        with tempfile.TemporaryDirectory(prefix="promo-info-") as tmp:   # its own sandbox: several at once
            r = run([str(self.exe), "--project-file", str(self.project_file(project)), "--info", "--out", tmp])
        print(r.stdout, end="")

    def new(self, project: str) -> None:
        path = self.storyboards / f"{self.project_file(project).stem}.json"
        if path.exists():
            sys.exit(f"{path} exists")
        root = 48 if self.kind == "instrument" else 60
        notes = [[b * 0.5, root + n, 0.45, 0.85] for b, n in enumerate((0, 7, 12, 15, 19, 15, 12, 7))]
        story = {"project": project, "title": project, "subtitle": "", "seconds": 16, "tempo": 110,
                 "notes": notes, "repeat": {"beats": 4, "times": 6}, "dials": [], "code": {"file": "program.fm"}}
        self.storyboards.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(story, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {path}")

    # ---- render ----------------------------------------------------------------------------------------------------------
    def render(self, names: list[str]) -> None:
        paths = [self.storyboards / f"{n}.json" for n in names] if names else sorted(self.storyboards.glob("*.json"))
        missing = [str(p) for p in paths if not p.exists()]
        if missing:
            sys.exit("no storyboard " + ", ".join(missing))
        if not paths:
            sys.exit(f"no storyboards in {self.storyboards} (scripts/promo.py {self.args.key} new <project>)")
        self.ensure_built()
        failed = []
        for path in paths:
            try:
                self.render_one(path)
            except SystemExit as e:
                print(f"  {path.stem}: {e}", file=sys.stderr)
                failed.append(path.stem)
        if failed:
            sys.exit("failed: " + ", ".join(failed))

    def render_one(self, path: Path) -> None:
        story = self.storyboard(path)
        project = story["project"]
        work = self.dir / "work" / path.stem
        work.mkdir(parents=True, exist_ok=True)
        # The renderer reads the storyboard with the defaults applied.
        merged = work / "storyboard.json"
        merged.write_text(json.dumps(story, indent=2), encoding="utf-8")
        # A MIDI file is beside the storyboard.
        if story.get("midi"):
            shutil.copyfile(path.parent / story["midi"], work / Path(story["midi"]).name)
            story["midi"] = Path(story["midi"]).name
            merged.write_text(json.dumps(story, indent=2), encoding="utf-8")
        base = [str(self.exe), "--project-file", str(self.project_file(project)), "--storyboard", str(merged),
                "--out", str(work), "--width", str(VIDEO_W)]
        # The views' sizes first, so the cards can lay the video out, and the renderer then knows how much of the code
        # the video shows (its scroll ends with the file's last line at the bottom of that).
        r = subprocess.run(base + ["--sizes"], text=True, capture_output=True)
        if r.returncode != 0:
            sys.stderr.write(r.stderr)
            sys.exit("the renderer failed")
        sizes = json.loads(r.stdout.strip().splitlines()[-1])
        self.cards(story, sizes, work)
        layout = json.loads((work / "layout.json").read_text(encoding="utf-8"))
        shown = layout["code"][3] * sizes["code"][0] // max(1, layout["code"][2]) if layout["code"][3] > 0 else 0
        cmd = base + ["--fps", str(self.args.fps), "--ffmpeg", self.args.ffmpeg, "--code-shown", str(shown)]
        if self.args.stills:
            cmd += ["--stills", self.args.stills]
        print(f"{path.stem}: rendering {story.get('seconds', 20)} s ...", flush=True)
        r = subprocess.run(cmd, text=True, capture_output=True)
        for line in r.stderr.splitlines():
            if "warning" in line:
                print("  " + line.strip())
        if r.returncode != 0:
            sys.stderr.write(r.stderr)
            sys.exit("the renderer failed")
        if self.args.stills:
            print(f"  stills in {work}")
            return
        meta = json.loads((work / "meta.json").read_text(encoding="utf-8"))
        if not meta["gpu"]:
            print(f"  note: drawn without the GPU ({meta['gpu_why']}): shaders are placeholders")
        if meta["peak"] < 1e-4:
            sys.exit("the audio is silent: do the storyboard's notes reach the program?")
        out = self.compose(story, meta, work)
        print(f"  {out}")
        if not self.args.keep:
            for name in ("panel.mp4", "code.mp4", "audio.wav"):
                (work / name).unlink(missing_ok=True)

    def cards(self, story: dict, sizes: dict, work: Path) -> None:
        code = story.get("code", {}).get("file", "program.fm")
        spec = {
            "w": W, "h": H,
            "font": str(self.build / "_deps" / "funkgui-src" / "fonts" / "upstream" / "JetBrainsMono-Regular.ttf"),
            "mark": str(ROOT / "Assets" / "logo" / self.kind / "icon.iconset" / "icon_128x128@2x.png"),
            "lockup": str(ROOT / "Assets" / "logo" / self.kind / "lockup-graphite.png"),
            "ground": mix(GROUND, INK, 0.0), "ink": mix(GROUND, INK, 1.0), "ink70": mix(GROUND, INK, 0.70),
            "ink52": mix(GROUND, INK, 0.52), "ink16": mix(GROUND, INK, 0.16), "accent": accent(self.kind),
            "product": self.name,
            "title": story.get("title", story["project"]),
            "subtitle": story.get("subtitle", ""),
            "tagline": story.get("tagline", tagline(self.kind)),
            "footer": story.get("footer", ""),
            "panelLabel": story.get("panel_label", "THE SOUND"),
            "codeLabel": story.get("code_label", f"{code.upper()}  ·  {sizes['code_lines']} LINES"),
            "panelSize": sizes["panel"], "codeSize": sizes["code"],
        }
        (work / "cards.json").write_text(json.dumps(spec, indent=2), encoding="utf-8")
        run([self.args.swift, str(ROOT / "Tools" / "promo" / "cards.swift"), str(work / "cards.json"), str(work)])

    def loudness(self, wav: Path) -> dict | None:
        r = subprocess.run([self.args.ffmpeg, "-hide_banner", "-nostats", "-i", str(wav), "-af",
                            "loudnorm=I=-14:TP=-1.5:LRA=11:print_format=json", "-f", "null", "-"],
                           text=True, capture_output=True)
        m = re.search(r"\{[^{}]*\"input_i\"[^{}]*\}", r.stderr)
        if not m:
            return None
        d = json.loads(m.group(0))
        return d if math.isfinite(float(d["input_i"])) else None

    def compose(self, story: dict, meta: dict, work: Path) -> Path:
        seconds = float(meta["seconds"])
        fps = int(meta["fps"])
        end = max(0.0, min(float(story.get("end", 2.5)), seconds - 1.0))
        layout = json.loads((work / "layout.json").read_text(encoding="utf-8"))
        px, py, pw, ph = layout["panel"]
        cx, cy, cw, ch = layout["code"]
        ground = f"0x{GROUND:06x}"

        inputs = ["-f", "lavfi", "-i", f"color=c={ground}:s={W}x{H}:r={fps}:d={seconds}",
                  "-i", str(work / "panel.mp4"), "-i", str(work / "code.mp4"),
                  "-loop", "1", "-framerate", str(fps), "-t", str(seconds), "-i", str(work / "overlay.png"),
                  "-loop", "1", "-framerate", str(fps), "-t", str(seconds), "-i", str(work / "end.png"),
                  "-i", str(work / "audio.wav")]
        v = []
        panel_w, panel_h = meta["panel"]
        if (pw, ph) != (panel_w, panel_h):
            v.append(f"[1:v]scale={pw}:{ph}:flags=lanczos[p]")
        else:
            v.append("[1:v]null[p]")
        v.append(f"[0:v][p]overlay={px}:{py}:eof_action=repeat[a]")
        if ch > 0:
            v.append(f"[2:v]crop={meta['code'][0]}:{min(ch, meta['code'][1])}:0:0,scale={cw}:{ch}:flags=lanczos[c]")
            v.append(f"[a][c]overlay={cx}:{cy}:eof_action=repeat[b]")
        else:
            v.append("[a]null[b]")
        v.append("[b][3:v]overlay=0:0[d]")
        if end > 0:
            v.append(f"[4:v]format=rgba,fade=t=in:st={seconds - end:.3f}:d=0.5:alpha=1[e]")
            v.append("[d][e]overlay=0:0[f]")
        else:
            v.append("[d]null[f]")
        v.append(f"[f]fade=t=in:st=0:d=0.2:color={ground},format=yuv420p[v]")

        a = "[5:a]"
        lufs = self.loudness(work / "audio.wav")
        if lufs is not None:
            a += ("loudnorm=I=-14:TP=-1.5:LRA=11:linear=true:"
                  f"measured_I={lufs['input_i']}:measured_TP={lufs['input_tp']}:measured_LRA={lufs['input_lra']}:"
                  f"measured_thresh={lufs['input_thresh']}:offset={lufs['target_offset']},")
        fade = min(1.5, max(0.3, end))
        # A limiter at -2 dBFS after it, at four times the rate (loudnorm's own) so that it catches what lies between the
        # samples too: what loudnorm's linear gain lets through (a drum's transient) stays under -1 dB true peak once AAC
        # has encoded it. Its delay compensated, so the sound stays on its frames.
        a += ("aresample=192000,alimiter=limit=0.79:attack=5:release=60:level=false:latency=true,aresample=48000,"
              f"afade=t=out:st={seconds - fade:.3f}:d={fade:.3f}[aout]")

        out = self.dir / f"{work.name}.mp4"
        cmd = [self.args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", *inputs,
               "-filter_complex", ";".join(v + [a]), "-map", "[v]", "-map", "[aout]",
               "-c:v", "libx264", "-preset", "slow", "-crf", "16", "-profile:v", "high", "-level:v", "4.2",
               "-r", str(fps), "-g", str(fps * 2),
               "-c:a", "aac", "-b:a", "256k", "-ar", "48000", "-movflags", "+faststart", "-t", f"{seconds}", str(out)]
        run(cmd)
        return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("key", help="the product: products/<key>.cmake")
    ap.add_argument("command", choices=("list", "info", "new", "render"))
    ap.add_argument("projects", nargs="*")
    ap.add_argument("--dir", help="the videos' folder (default ~/Movies/<Name> Promos)")
    ap.add_argument("--build", default="build-dev", help="the build folder (default build-dev)")
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--stills", help="seconds, comma-separated: PNGs of each view instead of a video")
    ap.add_argument("--keep", action="store_true", help="keep each video's work files")
    ap.add_argument("--no-build", action="store_true", help="use the renderer as built (several renders at once)")
    ap.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg")
    ap.add_argument("--swift", default=shutil.which("swift") or "swift")
    args = ap.parse_args()
    p = Promo(args)
    if args.command == "list":
        p.list()
    elif args.command in ("info", "new"):
        if len(args.projects) != 1:
            sys.exit(f"{args.command} takes one project")
        getattr(p, args.command)(args.projects[0])
    else:
        p.render(args.projects)


if __name__ == "__main__":
    main()
