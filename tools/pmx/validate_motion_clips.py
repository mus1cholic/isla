#!/usr/bin/env python3
"""Phase 6 validator for PMX motion-converted glTF clips.

Validates a motion clip package (single .gltf/.glb with named animations) against
runtime constraints used by isla's current animated glTF path.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from datetime import datetime
from pathlib import Path
from typing import Any


class ValidationError(Exception):
    pass


EXPECTED_SCHEMA_VERSION = "1.0.0"


def _read_json(path: Path) -> Any:
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
    if chunk_type != 0x4E4F534A:
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


def _is_non_empty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _is_rfc3339_datetime(value: Any) -> bool:
    if not isinstance(value, str) or not value:
        return False
    if value.endswith("Z"):
        normalized = value[:-1] + "+00:00"
    elif re.search(r"[+-]\d{2}:\d{2}$", value):
        normalized = value
    else:
        return False
    try:
        parsed = datetime.fromisoformat(normalized)
        return parsed.tzinfo is not None
    except ValueError:
        return False


def _resolve_root_node_index(doc: dict[str, Any], root_joint_name: str | None) -> int | None:
    nodes = doc.get("nodes", [])
    if not isinstance(nodes, list):
        return None

    if root_joint_name:
        for i, node in enumerate(nodes):
            if isinstance(node, dict) and node.get("name") == root_joint_name:
                return i
        return None

    skins = doc.get("skins", [])
    if isinstance(skins, list) and skins and isinstance(skins[0], dict):
        joints = skins[0].get("joints", [])
        if isinstance(joints, list) and joints and isinstance(joints[0], int):
            return joints[0]
    return None


def _sample_translation_extents(accessors: list[Any], accessor_index: int) -> tuple[float, float] | None:
    if accessor_index < 0 or accessor_index >= len(accessors):
        return None
    accessor = accessors[accessor_index]
    if not isinstance(accessor, dict):
        return None

    minimum = accessor.get("min")
    maximum = accessor.get("max")
    if not isinstance(minimum, list) or not isinstance(maximum, list):
        return None
    if len(minimum) < 3 or len(maximum) < 3:
        return None
    if not all(isinstance(v, (int, float)) for v in minimum[:3] + maximum[:3]):
        return None

    x_extent = max(abs(float(minimum[0])), abs(float(maximum[0])))
    z_extent = max(abs(float(minimum[2])), abs(float(maximum[2])))
    return x_extent, z_extent


def _validate_motion_sidecar(sidecar: Any, clip_names: set[str], errors: list[str]) -> None:
    if not isinstance(sidecar, dict):
        errors.append("motion sidecar top-level JSON value must be an object")
        return

    required = ["schema_version", "converter", "retarget", "root_motion_policy", "clips"]
    for key in required:
        if key not in sidecar:
            errors.append(f"motion sidecar missing required field: {key}")

    schema_version = sidecar.get("schema_version")
    if schema_version != EXPECTED_SCHEMA_VERSION:
        errors.append(
            f"motion sidecar schema_version '{schema_version}' is unsupported; "
            f"expected '{EXPECTED_SCHEMA_VERSION}'"
        )

    converter = sidecar.get("converter")
    if not isinstance(converter, dict):
        errors.append("motion sidecar converter section is missing or invalid")
    else:
        if not _is_non_empty_string(converter.get("name")):
            errors.append("motion sidecar converter.name missing or empty")
        if not _is_non_empty_string(converter.get("version")):
            errors.append("motion sidecar converter.version missing or empty")
        if not _is_non_empty_string(converter.get("command")):
            errors.append("motion sidecar converter.command missing or empty")
        if not _is_rfc3339_datetime(converter.get("timestamp_utc")):
            errors.append("motion sidecar converter.timestamp_utc missing or invalid RFC3339")

    retarget = sidecar.get("retarget")
    if not isinstance(retarget, dict):
        errors.append("motion sidecar retarget section is missing or invalid")
    else:
        mode = retarget.get("mode")
        if mode not in ("exact_joint_name", "map_file"):
            errors.append("motion sidecar retarget.mode must be exact_joint_name or map_file")
        map_path = retarget.get("map_path")
        if map_path is not None and not _is_non_empty_string(map_path):
            errors.append("motion sidecar retarget.map_path must be a non-empty string when set")

    root_policy = sidecar.get("root_motion_policy")
    if root_policy not in ("in_place", "allow"):
        errors.append("motion sidecar root_motion_policy must be in_place or allow")

    clips = sidecar.get("clips")
    if not isinstance(clips, list):
        errors.append("motion sidecar clips must be an array")
        return

    for i, clip in enumerate(clips):
        if not isinstance(clip, dict):
            errors.append(f"motion sidecar clips[{i}] is invalid")
            continue
        clip_name = clip.get("clip_name")
        if not _is_non_empty_string(clip_name):
            errors.append(f"motion sidecar clips[{i}].clip_name missing or empty")
            continue
        if clip_name not in clip_names:
            errors.append(
                f"motion sidecar clip '{clip_name}' not found in glTF animation clip set"
            )

        for field in ("source_vmd", "output_asset"):
            if not _is_non_empty_string(clip.get(field)):
                errors.append(f"motion sidecar clips[{i}].{field} missing or empty")

        mode = clip.get("root_motion_mode")
        if mode not in ("in_place", "allow"):
            errors.append(
                f"motion sidecar clips[{i}].root_motion_mode must be in_place or allow"
            )

        duration = clip.get("expected_duration_seconds")
        if not isinstance(duration, (int, float)) or float(duration) <= 0.0:
            errors.append(
                f"motion sidecar clips[{i}].expected_duration_seconds must be > 0"
            )


def validate_motion_package(
    asset_path: Path,
    sidecar_path: Path | None,
    root_joint_name: str | None,
    root_motion_policy: str,
    root_motion_epsilon: float,
    require_action_clip: bool,
) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []

    try:
        doc = _load_gltf_or_glb(asset_path)
    except ValidationError as exc:
        return [str(exc)], warnings

    animations = doc.get("animations", [])
    if not isinstance(animations, list) or not animations:
        errors.append("glTF must contain at least one animation clip")
        return errors, warnings

    nodes = doc.get("nodes", [])
    if not isinstance(nodes, list):
        errors.append("nodes field is not an array")
        return errors, warnings

    accessors = doc.get("accessors", [])
    if not isinstance(accessors, list):
        errors.append("accessors field is not an array")
        return errors, warnings

    root_node_index = _resolve_root_node_index(doc, root_joint_name)
    if root_joint_name is not None and root_node_index is None:
        errors.append(f"requested root joint '{root_joint_name}' was not found in glTF nodes")

    clip_names: set[str] = set()
    for anim_idx, animation in enumerate(animations):
        if not isinstance(animation, dict):
            errors.append(f"animation[{anim_idx}] is not an object")
            continue

        clip_name = animation.get("name")
        if not _is_non_empty_string(clip_name):
            errors.append(f"animation[{anim_idx}] missing non-empty name")
        else:
            if clip_name in clip_names:
                errors.append(f"duplicate animation clip name: {clip_name}")
            clip_names.add(clip_name)

        samplers = animation.get("samplers", [])
        channels = animation.get("channels", [])
        if not isinstance(samplers, list):
            errors.append(f"animation[{anim_idx}].samplers is not an array")
            continue
        if not isinstance(channels, list):
            errors.append(f"animation[{anim_idx}].channels is not an array")
            continue

        for sampler_idx, sampler in enumerate(samplers):
            if not isinstance(sampler, dict):
                errors.append(f"animation[{anim_idx}].samplers[{sampler_idx}] is not an object")
                continue
            interpolation = sampler.get("interpolation", "LINEAR")
            if interpolation not in ("LINEAR", "STEP"):
                errors.append(f"Unsupported interpolation: {interpolation}")

            input_accessor_index = sampler.get("input")
            output_accessor_index = sampler.get("output")
            if not isinstance(input_accessor_index, int) or not isinstance(output_accessor_index, int):
                errors.append(
                    f"animation[{anim_idx}].samplers[{sampler_idx}] input/output must be accessor indices"
                )
                continue
            if not (0 <= input_accessor_index < len(accessors)) or not (
                0 <= output_accessor_index < len(accessors)
            ):
                errors.append(
                    f"animation[{anim_idx}].samplers[{sampler_idx}] input/output accessor index out of range"
                )
                continue

            in_accessor = accessors[input_accessor_index]
            out_accessor = accessors[output_accessor_index]
            if not isinstance(in_accessor, dict) or not isinstance(out_accessor, dict):
                errors.append(
                    f"animation[{anim_idx}].samplers[{sampler_idx}] accessor entry is invalid"
                )
                continue

            in_count = in_accessor.get("count")
            out_count = out_accessor.get("count")
            if not isinstance(in_count, int) or not isinstance(out_count, int):
                errors.append(
                    f"animation[{anim_idx}].samplers[{sampler_idx}] accessor count missing or non-integer"
                )
                continue
            if in_count != out_count:
                errors.append(
                    f"mismatched animation key counts: input={in_count}, output={out_count}"
                )

        for channel_idx, channel in enumerate(channels):
            if not isinstance(channel, dict):
                errors.append(f"animation[{anim_idx}].channels[{channel_idx}] is not an object")
                continue
            sampler_index = channel.get("sampler")
            target = channel.get("target")
            if not isinstance(sampler_index, int) or not (0 <= sampler_index < len(samplers)):
                errors.append(
                    f"animation[{anim_idx}].channels[{channel_idx}] sampler index is invalid"
                )
                continue
            if not isinstance(target, dict):
                errors.append(f"animation[{anim_idx}].channels[{channel_idx}] target is invalid")
                continue

            target_path = target.get("path")
            target_node = target.get("node")
            if target_path not in ("translation", "rotation", "scale"):
                errors.append(
                    f"animation[{anim_idx}].channels[{channel_idx}] target.path '{target_path}' is unsupported"
                )
                continue
            if not isinstance(target_node, int) or not (0 <= target_node < len(nodes)):
                errors.append(
                    f"animation[{anim_idx}].channels[{channel_idx}] target.node is out of range"
                )
                continue

            if (
                root_motion_policy == "in_place"
                and root_node_index is not None
                and target_node == root_node_index
                and target_path == "translation"
            ):
                output_accessor_index = samplers[sampler_index].get("output")
                if not isinstance(output_accessor_index, int):
                    errors.append(
                        f"animation[{anim_idx}].channels[{channel_idx}] root translation output accessor is invalid"
                    )
                    continue
                extents = _sample_translation_extents(accessors, output_accessor_index)
                if extents is None:
                    warnings.append(
                        f"animation[{anim_idx}] ('{clip_name if _is_non_empty_string(clip_name) else '<unnamed>'}') "
                        f"channels[{channel_idx}] sampler[{sampler_index}] root translation lacks min/max metadata; "
                        "cannot statically verify in-place policy"
                    )
                    continue
                x_extent, z_extent = extents
                if x_extent > root_motion_epsilon or z_extent > root_motion_epsilon:
                    errors.append(
                        f"animation[{anim_idx}] ('{clip_name if _is_non_empty_string(clip_name) else '<unnamed>'}') "
                        f"channels[{channel_idx}] sampler[{sampler_index}] root translation violates in_place policy: "
                        f"|x|={x_extent:.6f}, |z|={z_extent:.6f}, epsilon={root_motion_epsilon:.6f}"
                    )

    required_clips = {"idle", "walk"}
    if require_action_clip:
        required_clips.add("action")

    if not clip_names:
        errors.append("Missing required animation clips: idle, walk, and action")
    else:
        for name in sorted(required_clips - clip_names):
            errors.append(f"Missing required baseline clip: {name}")
        if len(clip_names) < 3:
            errors.append("Missing required third clip: expected idle, walk, plus one additional clip")

    if sidecar_path is not None:
        try:
            sidecar = _read_json(sidecar_path)
            _validate_motion_sidecar(sidecar, clip_names, errors)
        except json.JSONDecodeError as exc:
            errors.append(f"motion sidecar JSON parse failure: {exc}")

    return errors, warnings


def _collect_validation_summary(
    asset_path: Path, root_joint_name: str | None
) -> tuple[list[str], str]:
    clip_names: list[str] = []
    resolved_root = "<unresolved>"
    try:
        doc = _load_gltf_or_glb(asset_path)
        animations = doc.get("animations", [])
        if isinstance(animations, list):
            for animation in animations:
                if not isinstance(animation, dict):
                    continue
                name = animation.get("name")
                if _is_non_empty_string(name):
                    clip_names.append(name.strip())
        clip_names = sorted(set(clip_names))

        nodes = doc.get("nodes", [])
        root_index = _resolve_root_node_index(doc, root_joint_name)
        if root_index is not None and isinstance(nodes, list) and 0 <= root_index < len(nodes):
            node = nodes[root_index]
            if isinstance(node, dict):
                name = node.get("name")
                if _is_non_empty_string(name):
                    resolved_root = f"{name} (node={root_index})"
                else:
                    resolved_root = f"<unnamed> (node={root_index})"
    except ValidationError:
        pass
    return clip_names, resolved_root


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate PMX motion-converted glTF clips for isla Phase 6")
    parser.add_argument("asset", type=Path, help="Path to motion clip .gltf or .glb")
    parser.add_argument(
        "--sidecar",
        type=Path,
        default=None,
        help="Path to motion sidecar JSON (default: <asset>.motion.json, optional)",
    )
    parser.add_argument(
        "--root-joint",
        type=str,
        default=None,
        help="Root joint name to apply root motion checks against (default: first skin joint)",
    )
    parser.add_argument(
        "--root-motion-policy",
        choices=["in_place", "allow"],
        default="in_place",
        help="Root motion validation policy",
    )
    parser.add_argument(
        "--root-motion-epsilon",
        type=float,
        default=1.0e-4,
        help="Maximum allowed absolute root translation on X/Z for in_place policy",
    )
    parser.add_argument(
        "--allow-missing-action",
        action="store_true",
        help="Allow clip set without an explicit action clip",
    )
    args = parser.parse_args()

    if not args.asset.exists():
        print(f"ERROR: Asset not found: {args.asset}")
        return 2

    sidecar = args.sidecar
    sidecar_resolution = "explicit"
    if sidecar is None:
        candidate = args.asset.with_suffix(".motion.json")
        sidecar = candidate if candidate.exists() else None
        sidecar_resolution = "auto-discovered" if sidecar is not None else "none"

    if sidecar is not None:
        print(f"INFO: using motion sidecar '{sidecar}' ({sidecar_resolution})")
    else:
        print("INFO: no motion sidecar found (optional)")

    errors, warnings = validate_motion_package(
        args.asset,
        sidecar,
        args.root_joint,
        args.root_motion_policy,
        args.root_motion_epsilon,
        require_action_clip=not args.allow_missing_action,
    )

    for w in warnings:
        print(f"WARN: {w}")
    for e in errors:
        print(f"ERROR: {e}")

    if errors:
        print(f"FAILED: {len(errors)} error(s), {len(warnings)} warning(s)")
        return 1

    clip_names, resolved_root = _collect_validation_summary(args.asset, args.root_joint)
    print(
        "INFO: motion validation summary "
        f"root_joint={resolved_root}, policy={args.root_motion_policy}, "
        f"clip_count={len(clip_names)}, clips={clip_names}"
    )
    print(f"OK: Phase 6 motion validation passed with {len(warnings)} warning(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
