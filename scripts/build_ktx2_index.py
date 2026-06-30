#!/usr/bin/env python3
"""
Build img<N>.ktx2 index links for GLB files with unnamed embedded images.

For each image referenced by a material, derives the expected KTX2 filename
from the material name + texture semantic, then creates a hard link from
img<N>.ktx2 to the named file in the ktx2/ sidecar directory next to the GLB.

This feeds the engine's sidecar lookup, which falls back to img<N>.ktx2 when
image names are absent from the glTF JSON.

Defaults to a dry run. Pass --execute to create the links.

Usage:
    python build_ktx2_index.py <path/to/file.glb>           # dry run
    python build_ktx2_index.py <path/to/file.glb> --execute # apply
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from pathlib import Path

# Maps gltf semantic -> KTX2 filename suffix (matched against ktx2/ filenames).
SEMANTIC_SUFFIX: dict[str, str] = {
    "albedo": "_BaseColor",
    "normal": "_Normal",
    "specular_color": "_Specular",
    "specular": "_Specular",
    "specular_glossiness": "_SpecularGlossiness",
    "sg_diffuse": "_Diffuse",
    "emissive": "_Emissive",
    "metallic_roughness": "_MetallicRoughness",
    "occlusion": "_AO",
}


def read_glb_json(path: Path) -> dict:
    with open(path, "rb") as f:
        magic, _version, _total = struct.unpack("<III", f.read(12))
        if magic != 0x46546C67:
            raise ValueError(f"Not a GLB file: {path}")
        chunk_length, chunk_type = struct.unpack("<II", f.read(8))
        if chunk_type != 0x4E4F534A:
            raise ValueError("First GLB chunk is not JSON")
        return json.loads(f.read(chunk_length).decode("utf-8"))


def image_index_for(gltf: dict, texture_idx: int) -> int | None:
    textures = gltf.get("textures", [])
    if texture_idx >= len(textures):
        return None
    tex = textures[texture_idx]
    # KHR_texture_basisu takes priority (compressed textures).
    basisu = tex.get("extensions", {}).get("KHR_texture_basisu", {})
    if "source" in basisu:
        return basisu["source"]
    return tex.get("source")


def extract_bindings(gltf: dict) -> list[tuple[int, str, str]]:
    """Return (image_index, clean_material_name, semantic) for every binding."""
    bindings: list[tuple[int, str, str]] = []

    def add(texture_idx: int | None, name: str, semantic: str) -> None:
        if texture_idx is None:
            return
        idx = image_index_for(gltf, texture_idx)
        if idx is not None:
            bindings.append((idx, name, semantic))

    for mat in gltf.get("materials", []):
        # Strip variant suffixes like ".DoubleSided" that don't appear in filenames.
        name = mat.get("name", "").split(".")[0]
        pbr = mat.get("pbrMetallicRoughness", {})

        if "baseColorTexture" in pbr:
            add(pbr["baseColorTexture"]["index"], name, "albedo")
        if "metallicRoughnessTexture" in pbr:
            add(pbr["metallicRoughnessTexture"]["index"], name, "metallic_roughness")
        if "normalTexture" in mat:
            add(mat["normalTexture"]["index"], name, "normal")
        if "occlusionTexture" in mat:
            add(mat["occlusionTexture"]["index"], name, "occlusion")
        if "emissiveTexture" in mat:
            add(mat["emissiveTexture"]["index"], name, "emissive")

        exts = mat.get("extensions", {})
        spec = exts.get("KHR_materials_specular", {})
        if "specularColorTexture" in spec:
            add(spec["specularColorTexture"]["index"], name, "specular_color")
        if "specularTexture" in spec:
            add(spec["specularTexture"]["index"], name, "specular")

        sg = exts.get("KHR_materials_pbrSpecularGlossiness", {})
        if "diffuseTexture" in sg:
            add(sg["diffuseTexture"]["index"], name, "sg_diffuse")
        if "specularGlossinessTexture" in sg:
            add(sg["specularGlossinessTexture"]["index"], name, "specular_glossiness")

    return bindings


def main() -> int:
    ap = argparse.ArgumentParser(description="Build img<N>.ktx2 index links for a GLB")
    ap.add_argument("glb", type=Path, help="Path to the .glb file")
    ap.add_argument("--execute", action="store_true",
                    help="Create hard links. Omit for a dry run.")
    args = ap.parse_args()

    glb_path: Path = args.glb
    ktx2_dir = glb_path.parent / "ktx2"

    if not glb_path.exists():
        print(f"error: not found: {glb_path}", file=sys.stderr)
        return 2
    if not ktx2_dir.is_dir():
        print(f"error: ktx2 dir not found: {ktx2_dir}", file=sys.stderr)
        return 2

    print(f"GLB  : {glb_path}")
    print(f"KTX2 : {ktx2_dir}\n")

    gltf = read_glb_json(glb_path)
    bindings = extract_bindings(gltf)

    to_link: list[tuple[Path, Path]] = []
    already_exists = 0
    missing: list[str] = []
    unknown_semantic: list[str] = []

    for image_idx, material_name, semantic in sorted(bindings):
        suffix = SEMANTIC_SUFFIX.get(semantic)
        if suffix is None:
            unknown_semantic.append(f"img{image_idx}: {material_name} / {semantic}")
            continue

        source_name = material_name + suffix + ".ktx2"
        source = ktx2_dir / source_name
        target = ktx2_dir / f"img{image_idx}.ktx2"

        if target.exists():
            already_exists += 1
            continue

        if not source.exists():
            missing.append(f"img{image_idx:<4}  {source_name}")
            continue

        to_link.append((source, target))
        print(f"  img{image_idx:<4}  {semantic:<20}  {source_name}")

    print()
    print(f"{'Would create' if not args.execute else 'Creating'} : {len(to_link)}")
    print(f"Already exists   : {already_exists}")
    print(f"Missing source   : {len(missing)}")
    print(f"Unknown semantic : {len(unknown_semantic)}")

    if missing:
        print("\nMissing KTX2 sources:")
        for m in missing:
            print(f"  {m}")

    if unknown_semantic:
        print("\nUnknown semantics (not linked):")
        for u in unknown_semantic:
            print(f"  {u}")

    if not args.execute:
        if to_link:
            print(f"\nRe-run with --execute to create {len(to_link)} links.")
        return 0

    errors = 0
    for source, target in to_link:
        try:
            os.link(source, target)
        except OSError as e:
            print(f"  ERROR {target.name}: {e}")
            errors += 1

    if errors:
        print(f"\n{errors} link(s) failed.")
        return 1

    print(f"\nDone. {len(to_link)} link(s) created.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
