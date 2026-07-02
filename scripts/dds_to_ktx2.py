#!/usr/bin/env python3
r"""
Batch convert DDS/TGA PBR textures to KTX2 using ktx (KTX-Software 5.x).

Two-pass pipeline:
  Pass 1 (decode):  DDS/TGA -> PNG, written into a persistent intermediate
                     directory (not a tempdir -- survives between runs so
                     you can re-encode without re-decoding).
  Pass 2 (encode):  PNG -> KTX2 via `ktx create`, color space picked from
                     the texture suffix:
                       _BaseColor / _Diffuse / _Albedo -> sRGB
                       _Normal                          -> linear, --normal-mode
                       _Specular / _MR / _AO / etc.      -> linear
                       _Emissive                         -> sRGB
                       (unknown suffix)                  -> skipped, reported

Either pass can be run alone (--decode-only / --encode-only), so you can
decode once and then iterate purely on encode settings.

--dev switches to a fast iteration profile: uastc quality 0, no zstd
supercompression, no mipmap generation. This is drastically faster than
the production profile and is meant for quick in-engine sanity checks,
not for shipping assets.

Defaults to a DRY RUN. Pass --execute to actually write files.
Uses a bounded thread pool (default 2 workers) so it won't saturate the
machine.

Usage (Nushell / PowerShell / cmd all fine):
    python dds_to_ktx2.py C:\in C:\out                        # dry run
    python dds_to_ktx2.py C:\in C:\out --execute               # full run
    python dds_to_ktx2.py C:\in C:\out --execute --dev          # fast iteration
    python dds_to_ktx2.py C:\in C:\out --execute --decode-only  # just decode
    python dds_to_ktx2.py C:\in C:\out --execute --encode-only  # just encode
                                                                 # (reuses cached PNGs)
"""
from __future__ import annotations

import argparse
import re
import shutil
import signal
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

# ---------------------------------------------------------------------------
# Interrupt handling
# ---------------------------------------------------------------------------
# subprocess.run() blocks a worker thread until the child exits, and
# ThreadPoolExecutor's context manager waits for all workers to finish
# before letting KeyboardInterrupt propagate -- so plain Ctrl-C looks like
# it does nothing while texconv/ktx keep grinding away. We track every live
# child process and, on SIGINT, kill them directly and cancel any futures
# that haven't started yet.
_shutdown_requested = threading.Event()
_live_procs: set[subprocess.Popen] = set()
_live_procs_lock = threading.Lock()


def _popen_new_group(cmd: list[str]) -> subprocess.Popen:
    """Start cmd in its own process group/session so we can kill the whole
    tree later, not just the direct child (some tools spawn helpers)."""
    if sys.platform == "win32":
        return subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
        )
    return subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True
    )


def _kill_tree(proc: subprocess.Popen) -> None:
    if sys.platform == "win32":
        # taskkill /T walks the whole child tree; plain proc.kill() would
        # only hit the top-level process and leave helpers running.
        subprocess.run(
            ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    else:
        import os

        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass


def _run_tracked(cmd: list[str]) -> subprocess.CompletedProcess:
    """subprocess.run() replacement that registers the child so SIGINT can kill it."""
    proc = _popen_new_group(cmd)
    with _live_procs_lock:
        _live_procs.add(proc)
    try:
        out, err = proc.communicate()
    finally:
        with _live_procs_lock:
            _live_procs.discard(proc)
    if proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, cmd, output=out, stderr=err)
    return subprocess.CompletedProcess(cmd, proc.returncode, out, err)


def _sigint_handler(signum, frame):
    _shutdown_requested.set()
    with _live_procs_lock:
        procs = list(_live_procs)
    for p in procs:
        try:
            _kill_tree(p)
        except Exception:  # noqa: BLE001
            pass
    print("\nInterrupted -- killed running subprocesses, stopping.", file=sys.stderr)

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


def magick_bc5_caveat(exe: str) -> str | None:
    """
    ImageMagick only gained BC5_UNORM/ATI2 read support in 7.1.0-62 (Feb
    2023, PR #6039), and there's a still-open reliability bug (#8082) where
    even 7.1.1-47 sometimes fails to read legitimate BC5 DDS files anyway.
    BC5 is the format normal maps almost always use, so this is worth
    surfacing loudly rather than letting it show up as a cryptic per-file
    "image type not supported" failure. Returns a warning string, or None
    if the version looks new enough to at least attempt it.
    """
    try:
        out = subprocess.run(
            [exe, "-version"], capture_output=True, text=True, timeout=5
        ).stdout
    except Exception:  # noqa: BLE001
        return "could not determine ImageMagick version"
    m = re.search(r"ImageMagick (\d+)\.(\d+)\.(\d+)-(\d+)", out)
    if not m:
        return "could not determine ImageMagick version"
    major, minor, patch, build = (int(x) for x in m.groups())
    if (major, minor, patch, build) < (7, 1, 0, 62):
        return (
            f"ImageMagick {major}.{minor}.{patch}-{build} predates BC5_UNORM/ATI2 "
            "read support (added in 7.1.0-62) -- normal map DDS files will fail to decode"
        )
    return (
        f"ImageMagick {major}.{minor}.{patch}-{build} has BC5 support but it has "
        "known reliability issues reading some BC5 DDS files (see IM issue #8082) -- "
        "prefer texconv for normal maps if available"
    )


# ---------------------------------------------------------------------------
# Encode profiles (production vs. dev/fast-iteration)
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class EncodeProfile:
    name: str
    uastc_quality: int
    zstd_level: int | None  # None -> omit --zstd entirely
    generate_mipmap: bool


PRODUCTION_PROFILE = EncodeProfile(
    name="production", uastc_quality=2, zstd_level=18, generate_mipmap=True
)

# Fast iteration: lowest UASTC quality, no supercompression pass, no
# mip generation. This is the difference between "a couple seconds" and
# "minutes" per large texture -- zstd 18 and mip generation dominate the
# runtime, not the UASTC encode itself.
DEV_PROFILE = EncodeProfile(
    name="dev", uastc_quality=0, zstd_level=None, generate_mipmap=False
)


def build_ktx_cmd(
    ktx: str, png: Path, out: Path, kind: TextureKind, profile: EncodeProfile
) -> list[str]:
    cmd = [
        ktx,
        "create",
        "--format",
        "R8G8B8A8_UNORM" if kind.color_space is ColorSpace.LINEAR else "R8G8B8A8_SRGB",
        "--encode",
        "uastc",
        "--uastc-quality",
        str(profile.uastc_quality),
    ]
    if profile.zstd_level is not None:
        cmd += ["--zstd", str(profile.zstd_level)]
    if profile.generate_mipmap:
        cmd += ["--generate-mipmap"]
    cmd += [
        "--assign-tf",
        kind.color_space.value,
        "--assign-primaries",
        "bt709",
    ]
    if kind.normal_mode:
        cmd += ["--normal-mode"]
    cmd += [str(png), str(out)]
    return cmd


# ---------------------------------------------------------------------------
# Decode (pass 1)
# ---------------------------------------------------------------------------


def decode_to_png(decoder: tuple[str, str], src: Path, png: Path, kind: TextureKind) -> None:
    dkind, exe = decoder
    if dkind == "texconv":
        # texconv writes <name>.png into the output dir; -ft png, -y overwrite.
        # -f is mandatory here: BC5/BC4 decompress internally to R8G8_UNORM /
        # R8_UNORM, and WIC (which backs texconv's PNG writer) has no
        # matching two-/one-channel PNG pixel format for those, so an
        # unspecified target fails outright on every BC5 normal map (see
        # https://github.com/microsoft/DirectXTex/issues/146).
        #
        # The target's sRGB tag must match the source's: BaseColor/Emissive
        # are stored as sRGB-tagged BC formats (e.g. BC7_UNORM_SRGB), and
        # converting to a plain UNORM target is a colorspace mismatch that
        # makes texconv linearize the data on the way out, corrupting the
        # gamma-encoded bytes the encode pass expects. Normal/Specular/etc.
        # are linear, so they use the plain UNORM target instead. Matching
        # tags on both sides is a transparent passthrough either way.
        target_fmt = (
            "R8G8B8A8_UNORM_SRGB" if kind.color_space is ColorSpace.SRGB else "R8G8B8A8_UNORM"
        )
        _run_tracked(
            [exe, "-nologo", "-y", "-f", target_fmt, "-ft", "png", "-o", str(png.parent), str(src)]
        )
        produced = png.parent / (src.stem + ".png")
        if produced != png:
            produced.replace(png)
    elif dkind == "magick":
        # DDS mip chains are read as a multi-frame sequence (like a GIF or
        # multi-page TIFF). Without an explicit frame index, `magick
        # src.dds out.png` silently writes out-0.png, out-1.png, ... instead
        # of out.png -- [0] selects just the full-res top mip.
        _run_tracked([exe, f"{src}[0]", str(png)])
    else:  # pillow
        from PIL import Image

        with Image.open(src) as im:
            im.convert("RGBA").save(png)


def png_is_fresh(src: Path, png: Path) -> bool:
    """True if an already-decoded PNG exists and is newer than its source."""
    if not png.exists():
        return False
    try:
        return png.stat().st_mtime >= src.stat().st_mtime
    except OSError:
        return False


# ---------------------------------------------------------------------------
# Job model
# ---------------------------------------------------------------------------


@dataclass
class Job:
    src: Path
    png: Path
    out: Path
    kind: TextureKind


@dataclass
class Result:
    job: Job
    ok: bool
    msg: str = ""


def decode_job(job: Job, decoder: tuple[str, str], force: bool) -> Result:
    if _shutdown_requested.is_set():
        return Result(job, False, "cancelled")
    if not force and png_is_fresh(job.src, job.png):
        return Result(job, True, "cached")
    try:
        job.png.parent.mkdir(parents=True, exist_ok=True)
        decode_to_png(decoder, job.src, job.png, job.kind)
    except subprocess.CalledProcessError as e:
        stderr = (e.stderr or b"").decode(errors="replace").strip()
        stdout = (e.output or b"").decode(errors="replace").strip()
        # texconv reports failures ("FAILED (...)") on stdout, not stderr,
        # so stderr alone is often empty for it.
        msg = stderr or stdout or f"exit code {e.returncode}, no output captured"
        msg = msg[:200]
        if decoder[0] == "magick" and "not supported" in msg.lower():
            msg += " (likely a BC5/ATI2 normal map -- try texconv instead of ImageMagick)"
        return Result(job, False, f"decode failed: {msg}")
    except Exception as e:  # noqa: BLE001
        return Result(job, False, f"decode failed: {e}")
    return Result(job, True)


def encode_job(job: Job, ktx: str, profile: EncodeProfile) -> Result:
    if _shutdown_requested.is_set():
        return Result(job, False, "cancelled")
    if not job.png.exists():
        return Result(job, False, "no decoded PNG found -- run decode pass first")
    cmd = build_ktx_cmd(ktx, job.png, job.out, job.kind, profile)
    try:
        job.out.parent.mkdir(parents=True, exist_ok=True)
        _run_tracked(cmd)
    except subprocess.CalledProcessError as e:
        return Result(job, False, f"ktx failed: {e.stderr.decode(errors='replace')[:200]}")
    return Result(job, True)


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------


def run_pass(jobs: list[Job], label: str, n_jobs: int, fn) -> list[Result]:
    failed: list[Result] = []
    total = len(jobs)
    pool = ThreadPoolExecutor(max_workers=n_jobs)
    try:
        with Progress(
            TextColumn("[progress.description]{task.description}"),
            BarColumn(),
            MofNCompleteColumn(),
            TimeRemainingColumn(),
            transient=False,
        ) as progress:
            task = progress.add_task(label, total=total)
            futs = {pool.submit(fn, j): j for j in jobs}
            for fut in as_completed(futs):
                res = fut.result()
                if not res.ok:
                    failed.append(res)
                    progress.console.print(f"[red]FAIL[/red] {res.job.src.name}  -- {res.msg}")
                progress.advance(task)
                if _shutdown_requested.is_set():
                    break
    finally:
        # cancel_futures (3.9+) drops anything not yet started; combined with
        # killing live child processes in the SIGINT handler, this lets the
        # whole run stop within a second or two instead of finishing the
        # in-flight batch first.
        pool.shutdown(wait=not _shutdown_requested.is_set(), cancel_futures=_shutdown_requested.is_set())
    return failed


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    signal.signal(signal.SIGINT, _sigint_handler)
    ap = argparse.ArgumentParser(description="DDS/TGA -> KTX2 batch converter (two-pass)")
    ap.add_argument("input_dir", type=Path, help="Directory containing .dds/.tga files")
    ap.add_argument("output_dir", type=Path, help="Directory to write .ktx2 files into")
    ap.add_argument(
        "--png-dir",
        type=Path,
        default=None,
        help="Intermediate PNG cache directory (default: <output_dir>/_png_cache)",
    )
    ap.add_argument("-j", "--jobs", type=int, default=2, help="Worker threads (default 2)")
    ap.add_argument("--execute", action="store_true", help="Actually run. Omit for a dry run.")
    ap.add_argument(
        "--overwrite", action="store_true", help="Re-create .ktx2 files that already exist"
    )
    ap.add_argument(
        "--force-decode",
        action="store_true",
        help="Re-decode PNGs even if a fresh cached PNG already exists",
    )
    pass_group = ap.add_mutually_exclusive_group()
    pass_group.add_argument(
        "--decode-only", action="store_true", help="Only run pass 1 (DDS/TGA -> PNG)"
    )
    pass_group.add_argument(
        "--encode-only",
        action="store_true",
        help="Only run pass 2 (PNG -> KTX2), reusing cached PNGs from a prior decode pass",
    )
    ap.add_argument(
        "--dev",
        action="store_true",
        help=(
            "Fast iteration profile: uastc-quality 0, no --zstd, no "
            "--generate-mipmap. Much faster, lower quality -- for local "
            "iteration only, not for shipping assets."
        ),
    )
    args = ap.parse_args()

    in_dir: Path = args.input_dir
    if not in_dir.is_dir():
        print(f"error: not a directory: {in_dir}", file=sys.stderr)
        return 2

    out_dir: Path = args.output_dir
    png_dir: Path = args.png_dir or (out_dir / "_png_cache")
    profile = DEV_PROFILE if args.dev else PRODUCTION_PROFILE

    do_decode = not args.encode_only
    do_encode = not args.decode_only

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
        if do_encode and out.exists() and not args.overwrite:
            skipped.append((f, "output exists (use --overwrite)"))
            continue
        png = png_dir / (f.stem + ".png")
        jobs.append(Job(f, png, out, kind))

    # --- report plan ---
    print(f"Input     : {in_dir}")
    print(f"Output    : {out_dir}")
    print(f"PNG cache : {png_dir}")
    print(f"Profile   : {profile.name}")
    print(
        f"Found {len(files)} input files -> {len(jobs)} to process, {len(skipped)} skipped\n"
    )

    by_kind: dict[str, int] = {}
    for j in jobs:
        by_kind[j.kind.label] = by_kind.get(j.kind.label, 0) + 1
    if by_kind:
        print("By type:")
        for label, n in sorted(by_kind.items()):
            cs = next(j.kind.color_space.value for j in jobs if j.kind.label == label)
            nm = " +normal-mode" if any(j.kind.normal_mode for j in jobs if j.kind.label == label) else ""
            print(f"  {label:<18} {n:>4}  [{cs}{nm}]")
        print()

    ktx = find_ktx()
    decoder = find_decoder()

    if not args.execute:
        print("DRY RUN — sample commands that would run:\n")
        for j in jobs[:6]:
            if do_decode:
                print("  decode:", j.src.name, "->", j.png.relative_to(png_dir.parent))
            if do_encode:
                cmd = build_ktx_cmd(ktx or "ktx", j.png, j.out, j.kind, profile)
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
        print(f"  ktx     : {'found at ' + ktx if ktx else 'NOT FOUND (install KTX-Software 5.x)'}")
        print(
            f"  decoder : {decoder[0] + ' (' + decoder[1] + ')' if decoder else 'NONE — need texconv, ImageMagick, or Pillow'}"
        )
        if decoder and decoder[0] == "magick":
            caveat = magick_bc5_caveat(decoder[1])
            if caveat:
                print(f"  [yellow]caveat[/yellow] : {caveat}")
        print("\nRe-run with --execute to convert.")
        return 0

    # --- execute ---
    if do_encode and not ktx:
        print("error: ktx not found on PATH. Install KTX-Software 5.x.", file=sys.stderr)
        return 3
    if do_decode and not decoder:
        print("error: no DDS/TGA decoder (texconv, ImageMagick, or Pillow).", file=sys.stderr)
        return 3
    if not jobs:
        print("Nothing to do.")
        return 0

    total_failed: list[Result] = []
    total_jobs = len(jobs)

    if do_decode:
        png_dir.mkdir(parents=True, exist_ok=True)
        print(f"Decoder: {decoder[0]}   Threads: {args.jobs}\n")
        if decoder[0] == "magick":
            caveat = magick_bc5_caveat(decoder[1])
            if caveat:
                print(f"WARNING: {caveat}\n")
        failed = run_pass(
            jobs, "Decoding", args.jobs, lambda j: decode_job(j, decoder, args.force_decode)
        )
        total_failed += failed
        # don't try to encode files whose decode failed
        failed_srcs = {r.job.src for r in failed}
        jobs = [j for j in jobs if j.src not in failed_srcs]

    if do_encode and jobs and not _shutdown_requested.is_set():
        print(f"\nEncoder ({profile.name} profile): quality={profile.uastc_quality} "
              f"zstd={profile.zstd_level} mipmap={profile.generate_mipmap}   "
              f"Threads: {args.jobs}\n")
        failed = run_pass(jobs, "Encoding", args.jobs, lambda j: encode_job(j, ktx, profile))
        total_failed += failed

    n_failed = len([r for r in total_failed if not r.ok])
    if _shutdown_requested.is_set():
        print(f"\nInterrupted. {total_jobs - n_failed} succeeded, {n_failed} failed before stopping.")
        return 130
    print(f"\nDone. {total_jobs - n_failed} succeeded, {n_failed} failed.")
    return 1 if n_failed else 0


if __name__ == "__main__":
    sys.exit(main())

