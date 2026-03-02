# PMX to glTF Conversion Contract (Phase 1)

Last updated: 2026-03-02

## Purpose

This document defines the deterministic conversion contract for PMX assets that must be consumed by the current `isla` animated glTF runtime (`engine/src/render/animated_gltf.cpp`).

The contract targets runtime compatibility as of 2026-03-02 (Phase 0 complete; Phases 2+ pending).

## Normative Terms

- MUST: required for runtime compatibility.
- SHOULD: recommended; not required for baseline import.
- MAY: optional.

## Converter Pinning

A PMX conversion package MUST record the converter identity and version in package metadata.

Required metadata fields:
- `converter.name` (string)
- `converter.version` (string)
- `converter.command` (string, full CLI invocation used)
- `converter.timestamp_utc` (RFC3339 string)

This metadata MUST be emitted in sidecar JSON (`<asset>.physics.json`), even if no physics records are present.

## Output Package Layout

For a converted character package rooted at `<package_dir>`:

- `<character>.gltf` or `<character>.glb` (required)
- Referenced texture/image files (required when referenced by materials)
- `<character>.physics.json` (required; may contain empty arrays)

## glTF Core Requirements

The converted glTF MUST satisfy all of the following:

1. Skin and joints
- At least one `skin` entry MUST exist.
- The selected target skin (index 0 in current runtime) MUST contain at least one joint.
- Joint nodes MUST be authored as TRS (no matrix-authored joint nodes).

2. Skinned primitives
- At least one triangle primitive MUST be attached to a node that references the selected skin.
- Each skinned primitive MUST provide:
  - `POSITION`
  - `JOINTS_0`
  - `WEIGHTS_0`
- All `JOINTS_0` indices MUST resolve to joints in the selected skin.

3. Animation clips
- At least one animation clip SHOULD exist for validation assets.
- Required baseline clips for conversion acceptance: `idle`, `walk`, and one additional test/action clip.
- For any sampled channel used by runtime (`translation`, `rotation`, `scale`):
  - Interpolation MUST be `LINEAR` or `STEP`.
  - `CUBICSPLINE` MUST NOT be emitted.
  - Sampler `input.count` MUST equal `output.count`.

4. Hierarchy compatibility caveat
- Static non-joint ancestors are currently baked at load.
- Animated non-joint hierarchy segments are not fully supported yet.
- Converter SHOULD bake or avoid animation on non-joint chains that affect skinned joints.

5. Materials/textures
- Materials and texture references SHOULD be preserved from conversion output.
- Missing material/texture data does not block skeletal import, but fails package quality checks.

## Physics Metadata Contract (Sidecar)

Physics metadata MUST be provided in sidecar file `<character>.physics.json` using schema:
- `docs/pmx/schemas/pmx_physics_metadata.schema.json`

Top-level required fields:
- `schema_version`
- `converter`
- `colliders`
- `constraints`
- `collision_layers`

### Bone Mapping Rules

- Collider and constraint references MUST use `bone_name` (or `bone_a_name`/`bone_b_name`) that match glTF joint node names exactly.
- Duplicate bone names SHOULD be avoided in conversion output.

### Collider Rules

Each collider entry MUST define:
- `id` (stable string)
- `bone_name` (string)
- `shape` in `{ "sphere", "capsule", "box" }`
- `offset` (xyz)
- `rotation_euler_deg` (xyz)
- `is_trigger` (bool)
- `layer` and `mask` (uint32)

Shape parameters:
- Sphere: `radius`
- Capsule: `radius`, `height`
- Box: `size` (xyz full extents)

### Constraint Rules

Each constraint entry MUST define:
- `id`
- `bone_a_name`
- `bone_b_name`
- `type` in `{ "fixed", "hinge", "cone_twist" }`
- `limit` object (optional fields allowed)

Unsupported fields MAY be included only under `extensions`.

## Determinism Rules

- Converter invocation and version metadata MUST be present.
- Re-running conversion with identical inputs and converter version SHOULD produce byte-identical sidecar JSON after stable key ordering.
- Animation key times SHOULD be monotonically increasing per channel.

## Validation Checklist

A converted package passes Phase 1 validation when all required checks pass:

1. File/package checks
- glTF/GLB exists.
- Sidecar physics JSON exists and schema-valid.

2. Runtime compatibility checks
- At least one skin with at least one joint.
- At least one triangle primitive attached to node using selected skin.
- Required skinned attributes present (`POSITION`, `JOINTS_0`, `WEIGHTS_0`).
- Joint nodes are TRS-authored (no matrix joints).
- No `CUBICSPLINE` samplers.
- Animation sampler key counts match (`input.count == output.count`).

3. Content quality checks
- At least one animation clip exists.
- Clip name set includes: `idle`, `walk`, plus at least one additional clip.
- Material/image references resolve to files for `.gltf` packages.
- Collider/constraint bone names resolve to skin joint names.

## Current Runtime Limits (Informative)

These are known and expected as of 2026-03-02:
- Matrix-authored joints are rejected.
- `CUBICSPLINE` is rejected.
- Animated non-joint hierarchy segments are not fully supported.
- Primitive dedup in runtime is pointer-based (per-node primitive instance transforms are not represented yet).

## Acceptance for Phase 1

Phase 1 is considered complete when:
- This contract is published.
- Sidecar schema is published.
- Validation is automatable (see `tools/pmx/validate_converted_gltf.py`).

Reference example sidecar: `docs/pmx/examples/sample.physics.json`
