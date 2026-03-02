#!/usr/bin/env python3
"""Phase 1 validator for PMX-converted glTF packages.

Checks current runtime compatibility constraints documented in:
- docs/pmx/pmx_to_gltf_conversion_contract.md
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path
from typing import Any


class ValidationError(Exception):
    pass


def _read_json(path: Path) -> dict[str, Any]:
    with path.open("rb") as f:
        return json.load(f)


def _read_glb_json(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < 20:
        raise ValidationError("GLB too small")
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        raise ValidationError("GLB header magic mismatch")
    if version != 2:
        raise ValidationError(f"GLB version {version} unsupported; expected 2")
    if length != len(data):
        raise ValidationError("GLB declared length does not match file size")

    offset = 12
    if offset + 8 > len(data):
        raise ValidationError("GLB missing first chunk header")
    chunk_len, chunk_type = struct.unpack_from("<II", data, offset)
    offset += 8
    if chunk_type != 0x4E4F534A:  # JSON
        raise ValidationError("GLB first chunk is not JSON")
    if offset + chunk_len > len(data):
        raise ValidationError("GLB JSON chunk exceeds file bounds")

    chunk = data[offset : offset + chunk_len]
    try:
        text = chunk.decode("utf-8").rstrip(" \t\r\n\x00")
        return json.loads(text)
    except Exception as exc:
        raise ValidationError(f"Failed parsing GLB JSON chunk: {exc}") from exc


def _load_gltf_or_glb(path: Path) -> dict[str, Any]:
    suffix = path.suffix.lower()
    if suffix == ".gltf":
        return _read_json(path)
    if suffix == ".glb":
        return _read_glb_json(path)
    raise ValidationError("Asset must be .gltf or .glb")


def _require(cond: bool, message: str, errors: list[str]) -> None:
    if not cond:
        errors.append(message)


def _get_selected_skin(doc: dict[str, Any]) -> tuple[int, dict[str, Any]]:
    skins = doc.get("skins", [])
    if not isinstance(skins, list) or not skins:
        raise ValidationError("glTF has no skin")
    skin = skins[0]
    if not isinstance(skin, dict):
        raise ValidationError("glTF skin[0] is not an object")
    return 0, skin


def _joint_indices(skin: dict[str, Any]) -> list[int]:
    joints = skin.get("joints", [])
    if not isinstance(joints, list) or not joints:
        raise ValidationError("glTF skin has no joints")
    out: list[int] = []
    for j in joints:
        if not isinstance(j, int) or j < 0:
            raise ValidationError("skin.joints contains non-integer index")
        out.append(j)
    return out


def _node_joint_names(doc: dict[str, Any], joints: list[int]) -> set[str]:
    nodes = doc.get("nodes", [])
    if not isinstance(nodes, list):
        raise ValidationError("glTF nodes field is invalid")
    names: set[str] = set()
    for idx in joints:
        if idx >= len(nodes) or not isinstance(nodes[idx], dict):
            raise ValidationError("skin joint index out of nodes range")
        node = nodes[idx]
        if node.get("matrix") is not None:
            raise ValidationError("matrix-authored joint node encountered")
        name = node.get("name")
        if isinstance(name, str) and name:
            names.add(name)
    return names


def _collect_skin_primitives(doc: dict[str, Any], selected_skin: int) -> list[dict[str, Any]]:
    nodes = doc.get("nodes", [])
    meshes = doc.get("meshes", [])
    if not isinstance(nodes, list) or not isinstance(meshes, list):
        raise ValidationError("glTF nodes/meshes must be arrays")

    out: list[dict[str, Any]] = []
    seen: set[tuple[int, int]] = set()
    for node in nodes:
        if not isinstance(node, dict):
            continue
        if node.get("skin") != selected_skin:
            continue
        mesh_idx = node.get("mesh")
        if not isinstance(mesh_idx, int) or mesh_idx < 0 or mesh_idx >= len(meshes):
            continue
        mesh = meshes[mesh_idx]
        if not isinstance(mesh, dict):
            continue
        prims = mesh.get("primitives", [])
        if not isinstance(prims, list):
            continue
        for p_idx, prim in enumerate(prims):
            if not isinstance(prim, dict):
                continue
            mode = prim.get("mode", 4)
            if mode != 4:
                continue
            key = (mesh_idx, p_idx)
            if key in seen:
                continue
            seen.add(key)
            out.append(prim)
    return out


def _validate_animation(doc: dict[str, Any], errors: list[str], warnings: list[str]) -> set[str]:
    clip_names: set[str] = set()
    animations = doc.get("animations", [])
    if not isinstance(animations, list):
        errors.append("animations field is not an array")
        return clip_names

    if not animations:
        warnings.append("No animations found")
        return clip_names

    accessors = doc.get("accessors", [])
    if not isinstance(accessors, list):
        errors.append("accessors field is not an array")
        return clip_names

    for anim in animations:
        if not isinstance(anim, dict):
            errors.append("animation entry is not an object")
            continue
        name = anim.get("name")
        if isinstance(name, str) and name:
            clip_names.add(name)

        samplers = anim.get("samplers", [])
        if not isinstance(samplers, list):
            errors.append("animation.samplers is not an array")
            continue

        for sampler in samplers:
            if not isinstance(sampler, dict):
                errors.append("animation sampler is not an object")
                continue
            interp = sampler.get("interpolation", "LINEAR")
            if interp not in ("LINEAR", "STEP"):
                errors.append(f"Unsupported interpolation: {interp}")

            inp = sampler.get("input")
            out = sampler.get("output")
            if not isinstance(inp, int) or not isinstance(out, int):
                errors.append("animation sampler input/output must be accessor indices")
                continue
            if inp < 0 or inp >= len(accessors) or out < 0 or out >= len(accessors):
                errors.append("animation sampler input/output accessor index out of range")
                continue

            in_acc = accessors[inp]
            out_acc = accessors[out]
            if not isinstance(in_acc, dict) or not isinstance(out_acc, dict):
                errors.append("animation sampler accessor entry is invalid")
                continue

            in_count = in_acc.get("count")
            out_count = out_acc.get("count")
            if not isinstance(in_count, int) or not isinstance(out_count, int):
                errors.append("animation accessor count missing or non-integer")
                continue

            if in_count != out_count:
                errors.append(
                    f"mismatched animation key counts: input={in_count}, output={out_count}"
                )

    return clip_names


def _validate_material_image_refs(doc: dict[str, Any], asset_path: Path, warnings: list[str]) -> None:
    if asset_path.suffix.lower() != ".gltf":
        return

    images = doc.get("images", [])
    if not isinstance(images, list) or not images:
        warnings.append("No image references found")
        return

    for idx, img in enumerate(images):
        if not isinstance(img, dict):
            warnings.append(f"image[{idx}] is invalid")
            continue
        uri = img.get("uri")
        if not isinstance(uri, str) or not uri:
            warnings.append(f"image[{idx}] has no uri (possibly bufferView-backed)")
            continue
        if uri.startswith("data:"):
            continue
        image_path = asset_path.parent / uri
        if not image_path.exists():
            warnings.append(f"image[{idx}] uri does not resolve: {uri}")


def _validate_sidecar(sidecar: dict[str, Any], joint_names: set[str], errors: list[str], warnings: list[str]) -> None:
    required_top = ["schema_version", "converter", "colliders", "constraints", "collision_layers"]
    for key in required_top:
        _require(key in sidecar, f"physics sidecar missing required field: {key}", errors)

    converter = sidecar.get("converter")
    if isinstance(converter, dict):
        for key in ("name", "version", "command", "timestamp_utc"):
            _require(isinstance(converter.get(key), str) and bool(converter.get(key)),
                     f"converter.{key} missing or empty", errors)
    else:
        errors.append("converter section is missing or invalid")

    colliders = sidecar.get("colliders", [])
    if not isinstance(colliders, list):
        errors.append("colliders must be an array")
        colliders = []

    constraints = sidecar.get("constraints", [])
    if not isinstance(constraints, list):
        errors.append("constraints must be an array")
        constraints = []

    for idx, col in enumerate(colliders):
        if not isinstance(col, dict):
            errors.append(f"collider[{idx}] is invalid")
            continue
        bone_name = col.get("bone_name")
        if not isinstance(bone_name, str) or not bone_name:
            errors.append(f"collider[{idx}] bone_name missing")
            continue
        if joint_names and bone_name not in joint_names:
            errors.append(f"collider[{idx}] bone_name not found in skin joints: {bone_name}")

    for idx, con in enumerate(constraints):
        if not isinstance(con, dict):
            errors.append(f"constraint[{idx}] is invalid")
            continue
        a = con.get("bone_a_name")
        b = con.get("bone_b_name")
        if not isinstance(a, str) or not isinstance(b, str):
            errors.append(f"constraint[{idx}] missing bone_a_name/bone_b_name")
            continue
        if joint_names:
            if a not in joint_names:
                errors.append(f"constraint[{idx}] bone_a_name not found: {a}")
            if b not in joint_names:
                errors.append(f"constraint[{idx}] bone_b_name not found: {b}")

    if not colliders:
        warnings.append("No colliders in physics sidecar")


def validate_package(asset_path: Path, sidecar_path: Path | None) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []

    doc = _load_gltf_or_glb(asset_path)

    selected_skin, skin = _get_selected_skin(doc)
    joints = _joint_indices(skin)
    joint_names = _node_joint_names(doc, joints)

    prims = _collect_skin_primitives(doc, selected_skin)
    _require(bool(prims), "No triangle primitive attached to selected skin", errors)

    for idx, prim in enumerate(prims):
        attrs = prim.get("attributes")
        if not isinstance(attrs, dict):
            errors.append(f"primitive[{idx}] attributes missing")
            continue
        for required in ("POSITION", "JOINTS_0", "WEIGHTS_0"):
            _require(required in attrs, f"primitive[{idx}] missing {required}", errors)

    clip_names = _validate_animation(doc, errors, warnings)
    if clip_names:
        required_names = {"idle", "walk"}
        missing = sorted(required_names - clip_names)
        for name in missing:
            warnings.append(f"Recommended baseline clip missing: {name}")
        if len(clip_names) < 3:
            warnings.append("Recommended: at least 3 clips (idle, walk, test/action)")

    _validate_material_image_refs(doc, asset_path, warnings)

    if sidecar_path is None:
        errors.append("Missing required sidecar: <asset>.physics.json")
    else:
        sidecar = _read_json(sidecar_path)
        _validate_sidecar(sidecar, joint_names, errors, warnings)

    return errors, warnings


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate PMX-converted glTF package for isla Phase 1")
    parser.add_argument("asset", type=Path, help="Path to .gltf or .glb")
    parser.add_argument(
        "--sidecar",
        type=Path,
        default=None,
        help="Path to physics sidecar JSON (default: <asset>.physics.json)",
    )
    args = parser.parse_args()

    asset = args.asset
    if not asset.exists():
        print(f"ERROR: Asset not found: {asset}")
        return 2

    sidecar = args.sidecar if args.sidecar is not None else asset.with_suffix(".physics.json")
    if not sidecar.exists():
        sidecar = None

    try:
        errors, warnings = validate_package(asset, sidecar)
    except ValidationError as exc:
        print(f"ERROR: {exc}")
        return 2
    except json.JSONDecodeError as exc:
        print(f"ERROR: JSON parse failure: {exc}")
        return 2

    for w in warnings:
        print(f"WARN: {w}")
    for e in errors:
        print(f"ERROR: {e}")

    if errors:
        print(f"FAILED: {len(errors)} error(s), {len(warnings)} warning(s)")
        return 1

    print(f"OK: Phase 1 validation passed with {len(warnings)} warning(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())