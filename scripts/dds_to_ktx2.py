#!/usr/bin/env python3
r"""
Batch convert DDS/TGA PBR textures to KTX2 using ktx (KTX-Software 5.x).

Pipeline per file:
  1. Decode DDS/TGA -> temporary PNG (via texconv/ImageMagick/Pillow, first available)
  2. Run `ktx create` with the correct color space for the texture type:
       _BaseColor / _Diffuse  -> sRGB
       _Normal                -> linear, --normal-mode
       _Specular / _MR / _AO  -> linear
       _Emissive              -> sRGB (emissive color is authored in sRGB)
       (unknown suffix)       -> skipped, reported

Defaults to a DRY RUN. Pass --execute to actually write files.
Uses a bounded thread pool (default 2 workers) so it won't saturate the machine.

Usage (Nushell / PowerShell / cmd all fine):
    python dds_to_ktx2.py C:\in C:\out                 # dry run
    python dds_to_ktx2.py C:\in C:\out --execute       # do it
    python dds_to_ktx2.py C:\in C:\out --execute -j 4  # 4 threads
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

# Pretty progress UI (required dependency).
from rich.progress import (
    BarColumn,
    MofNCompleteColumn,
    Progress,
    TextColumn,
    TimeRemainingColumn,
)

# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------


class ColorSpace(Enum):
    SRGB = "srgb"
    LINEAR = "linear"


@dataclass(frozen=True)
class TextureKind:
    label: str
    color_space: ColorSpace
    normal_mode: bool = False


# Suffix (case-insensitive, matched before the extension) -> kind.
# Order matters: longer / more specific suffixes first.
KIND_BY_SUFFIX: dict[str, TextureKind] = {
    "_basecolor": TextureKind("BaseColor", ColorSpace.SRGB),
    "_diffuse": TextureKind("Diffuse", ColorSpace.SRGB),
    "_albedo": TextureKind("Albedo", ColorSpace.SRGB),
    "_emissive": TextureKind("Emissive", ColorSpace.SRGB),
    "_normal": TextureKind("Normal", ColorSpace.LINEAR, normal_mode=True),
    "_specularglossiness": TextureKind("SpecularGlossiness", ColorSpace.LINEAR),
    "_sg": TextureKind("SpecularGlossiness", ColorSpace.LINEAR),
    "_specular": TextureKind("Specular", ColorSpace.LINEAR),
    "_metallicroughness": TextureKind("MetallicRoughness", ColorSpace.LINEAR),
    "_mr": TextureKind("MetallicRoughness", ColorSpace.LINEAR),
    "_roughness": TextureKind("Roughness", ColorSpace.LINEAR),
    "_metallic": TextureKind("Metallic", ColorSpace.LINEAR),
    "_ao": TextureKind("AO", ColorSpace.LINEAR),
    "_occlusion": TextureKind("Occlusion", ColorSpace.LINEAR),
    "_orm": TextureKind("ORM", ColorSpace.LINEAR),
    "_height": TextureKind("Height", ColorSpace.LINEAR),
    "_mask": TextureKind("Mask", ColorSpace.LINEAR),
}

INPUT_EXTS = {".dds", ".tga"}


def classify(path: Path) -> TextureKind | None:
    stem = path.stem.lower()
    # match the longest suffix first
    for suffix in sorted(KIND_BY_SUFFIX, key=len, reverse=True):
        if stem.endswith(suffix):
            return KIND_BY_SUFFIX[suffix]
    return None


# ---------------------------------------------------------------------------
# Tool discovery
# ---------------------------------------------------------------------------


def find_ktx() -> str | None:
    return shutil.which("ktx")


def find_decoder() -> tuple[str, str] | None:
    """Return (kind, exe). kind in {'texconv','magick','pillow'}."""
    for exe in ("texconv", "texconv.exe"):
        p = shutil.which(exe)
        if p:
            return ("texconv", p)
    for exe in ("magick", "convert"):
        p = shutil.which(exe)
        if p:
            return ("magick", p)
    try:
        import PIL  # noqa: F401

        return ("pillow", "PIL")
    except ImportError:
        return None


# ---------------------------------------------------------------------------
# Per-file work
# ---------------------------------------------------------------------------


def build_ktx_cmd(ktx: str, png: Path, out: Path, kind: TextureKind) -> list[str]:
    cmd = [
        ktx,
        "create",
        "--format",
        "R8G8B8A8_UNORM" if kind.color_space is ColorSpace.LINEAR else "R8G8B8A8_SRGB",
        "--encode",
        "uastc",
        "--uastc-quality",
        "2",
        "--zstd",
        "18",
        "--generate-mipmap",
        "--assign-tf",
        kind.color_space.value,
        "--assign-primaries",
        "bt709",
    ]
    if kind.normal_mode:
        cmd += ["--normal-mode"]
    cmd += [str(png), str(out)]
    return cmd


def decode_to_png(decoder: tuple[str, str], src: Path, png: Path) -> None:
    dkind, exe = decoder
    if dkind == "texconv":
        # texconv writes <name>.png into the output dir; -ft png, -y overwrite
        subprocess.run(
            [exe, "-nologo", "-y", "-ft", "png", "-o", str(png.parent), str(src)],
            check=True,
            capture_output=True,
        )
        produced = png.parent / (src.stem + ".png")
        if produced != png:
            produced.replace(png)
    elif dkind == "magick":
        cmd = [exe] if Path(exe).name.startswith("magick") else [exe]
        subprocess.run(cmd + [str(src), str(png)], check=True, capture_output=True)
    else:  # pillow
        from PIL import Image

        with Image.open(src) as im:
            im.convert("RGBA").save(png)


@dataclass
class Job:
    src: Path
    out: Path
    kind: TextureKind


@dataclass
class Result:
    job: Job
    ok: bool
    msg: str = ""


def process(job: Job, ktx: str, decoder: tuple[str, str]) -> Result:
    with tempfile.TemporaryDirectory() as td:
        png = Path(td) / (job.src.stem + ".png")
        try:
            decode_to_png(decoder, job.src, png)
        except subprocess.CalledProcessError as e:
            return Result(
                job, False, f"decode failed: {e.stderr.decode(errors='replace')[:200]}"
            )
        except Exception as e:  # noqa: BLE001
            return Result(job, False, f"decode failed: {e}")

        cmd = build_ktx_cmd(ktx, png, job.out, job.kind)
        try:
            subprocess.run(cmd, check=True, capture_output=True)
        except subprocess.CalledProcessError as e:
            return Result(
                job, False, f"ktx failed: {e.stderr.decode(errors='replace')[:200]}"
            )
    return Result(job, True)


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------


def run_jobs(
    jobs: list[Job], ktx: str, decoder: tuple[str, str], n_jobs: int
) -> list[Result]:
    failed: list[Result] = []
    total = len(jobs)
    with (
        ThreadPoolExecutor(max_workers=n_jobs) as pool,
        Progress(
            TextColumn("[progress.description]{task.description}"),
            BarColumn(),
            MofNCompleteColumn(),
            TimeRemainingColumn(),
            transient=False,
        ) as progress,
    ):
        task = progress.add_task("Converting", total=total)
        futs = {pool.submit(process, j, ktx, decoder): j for j in jobs}
        for fut in as_completed(futs):
            res = fut.result()
            if not res.ok:
                failed.append(res)
                # console.print routes around the live bar so it isn't corrupted
                progress.console.print(
                    f"[red]FAIL[/red] {res.job.src.name}  -- {res.msg}"
                )
            progress.advance(task)
    return failed


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description="DDS/TGA -> KTX2 batch converter")
    ap.add_argument("input_dir", type=Path, help="Directory containing .dds/.tga files")
    ap.add_argument("output_dir", type=Path, help="Directory to write .ktx2 files into")
    ap.add_argument(
        "-j", "--jobs", type=int, default=2, help="Worker threads (default 2)"
    )
    ap.add_argument(
        "--execute", action="store_true", help="Actually run. Omit for a dry run."
    )
    ap.add_argument(
        "--overwrite",
        action="store_true",
        help="Re-create .ktx2 files that already exist",
    )
    args = ap.parse_args()

    in_dir: Path = args.input_dir
    if not in_dir.is_dir():
        print(f"error: not a directory: {in_dir}", file=sys.stderr)
        return 2
    out_dir: Path = args.output_dir

    files = sorted(
        p for p in in_dir.iterdir() if p.is_file() and p.suffix.lower() in INPUT_EXTS
    )

    jobs: list[Job] = []
    skipped: list[tuple[Path, str]] = []
    for f in files:
        kind = classify(f)
        if kind is None:
            skipped.append((f, "unrecognized suffix"))
            continue
        out = out_dir / (f.stem + ".ktx2")
        if out.exists() and not args.overwrite:
            skipped.append((f, "output exists (use --overwrite)"))
            continue
        jobs.append(Job(f, out, kind))

    # --- report plan ---
    print(f"Input : {in_dir}")
    print(f"Output: {out_dir}")
    print(
        f"Found {len(files)} input files -> {len(jobs)} to convert, {len(skipped)} skipped\n"
    )

    by_kind: dict[str, int] = {}
    for j in jobs:
        by_kind[j.kind.label] = by_kind.get(j.kind.label, 0) + 1
    if by_kind:
        print("By type:")
        for label, n in sorted(by_kind.items()):
            cs = next(j.kind.color_space.value for j in jobs if j.kind.label == label)
            nm = (
                " +normal-mode"
                if any(j.kind.normal_mode for j in jobs if j.kind.label == label)
                else ""
            )
            print(f"  {label:<18} {n:>4}  [{cs}{nm}]")
        print()

    ktx = find_ktx()
    decoder = find_decoder()

    if not args.execute:
        print("DRY RUN — sample commands that would run:\n")
        for j in jobs[:6]:
            png = "<tmp>/" + j.src.stem + ".png"
            cmd = build_ktx_cmd(ktx or "ktx", Path(png), j.out, j.kind)
            print("  decode:", j.src.name, "->", Path(png).name)
            print("  ktx   :", " ".join(cmd), "\n")
        if len(jobs) > 6:
            print(f"  ... and {len(jobs) - 6} more\n")
        if skipped:
            print("Skipped:")
            for p, why in skipped[:20]:
                print(f"  {p.name}: {why}")
            if len(skipped) > 20:
                print(f"  ... and {len(skipped) - 20} more")
            print()
        print("Tooling check:")
        print(
            f"  ktx     : {'found at ' + ktx if ktx else 'NOT FOUND (install KTX-Software 5.x)'}"
        )
        print(
            f"  decoder : {decoder[0] + ' (' + decoder[1] + ')' if decoder else 'NONE — need texconv, ImageMagick, or Pillow'}"
        )
        print("\nRe-run with --execute to convert.")
        return 0

    # --- execute ---
    if not ktx:
        print(
            "error: ktx not found on PATH. Install KTX-Software 5.x.", file=sys.stderr
        )
        return 3
    if not decoder:
        print(
            "error: no DDS/TGA decoder (texconv, ImageMagick, or Pillow).",
            file=sys.stderr,
        )
        return 3
    if not jobs:
        print("Nothing to do.")
        return 0

    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"Decoder: {decoder[0]}   Threads: {args.jobs}\n")

    failed = run_jobs(jobs, ktx, decoder, args.jobs)

    total = len(jobs)
    print(f"\nDone. {total - len(failed)} succeeded, {len(failed)} failed.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
