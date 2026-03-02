# PMX to glTF Animation + Physics Phased Plan

## Context

`isla` currently has:

- Static mesh loading for `.obj/.gltf/.glb`
- A bgfx-based renderer path
- Transparent overlay window behavior
- Initial animated glTF foundation (skin/joint/clip loading and pose evaluation)

Target outcome:

- Use PMX assets by converting them to glTF
- Preserve basic character animation playback
- Preserve basic physics semantics needed for runtime behavior

## Recommendation

Implement PMX support as an external conversion pipeline plus incremental runtime import/playback in `isla`, not as native PMX runtime parsing.

Rationale:

- PMX ecosystem tooling is conversion-oriented.
- glTF is already partially supported in runtime.
- This de-risks format complexity and keeps runtime focused.

> [!NOTE]
> **Current status (2026-03-02):** Phase 0 is complete. Phases 1+ are pending.

## Phase 0: Animated glTF Runtime Foundation (Completed)

### Goal

Add core runtime structures and pose evaluation needed before full playback/rendering.

### Implemented

- New animated glTF module:
  - `engine/src/render/include/animated_gltf.hpp`
  - `engine/src/render/animated_gltf.cpp`
- Added:
  - skeleton/joint representation
  - inverse bind matrix import
  - clip/track/keyframe import (TRS channels)
  - pose sampling (vec3 interpolation + quat slerp)
  - skin matrix generation
- Added tests:
  - `engine/src/render/animated_gltf_test.cpp`

### Exit Criteria

- Runtime can load skinned glTF animation data and evaluate joint/skin matrices.

## Phase 1: PMX to glTF Conversion Contract

### Goal

Define a deterministic PMX conversion contract so runtime requirements are explicit.

### Scope

- Choose and pin converter workflow (CLI + version).
- Define required conversion outputs:
  - skinned mesh data (`JOINTS_0`, `WEIGHTS_0`)
  - skeleton hierarchy
  - animation clips (idle/walk/test clip minimum)
  - material + texture references
- Define required metadata for physics carryover (via `extras` and/or sidecar JSON):
  - per-bone collider hints (sphere/capsule/box)
  - optional joint constraints
  - layer/mask hints
- Add validation checklist for converted assets.

### Contract Details (In-Plan)

#### A. Conversion Toolchain Contract

- Conversion must be deterministic and scripted (no manual editor-only steps).
- Converter invocation must be pinned by version and checked into docs/scripts.
- Input package baseline:
  - `model.pmx`
  - optional `motions/*.vmd`
  - textures used by PMX materials
- Output package baseline:
  - `model.glb` or `model.gltf + bin + textures`
  - optional sidecar `model.physics.json` (if physics is not embedded in glTF `extras`)

#### B. Required glTF Runtime Fields

The converted glTF is considered valid only if all required fields below are present.

- Mesh/Skin:
  - at least 1 mesh primitive with triangle topology
  - `POSITION`
  - `JOINTS_0`
  - `WEIGHTS_0`
  - one valid `skin` with non-empty `joints`
  - valid inverse bind matrices (or converter emits defaults)
- Animation:
  - at least one clip for validation assets (`idle` minimum)
  - channels targeting translation/rotation/scale are allowed
  - clip timestamps must be monotonic per sampler
- Materials/Textures:
  - base color texture references must resolve to files in package
  - missing textures must be explicit and handled by runtime fallback

#### C. Coordinate/Units Contract

- Runtime target convention:
  - units: `1.0 = 1 meter`
  - up-axis: `+Y`
  - handedness/conversion must be consistent for mesh + skeleton + animation
- Conversion must preserve relative bone hierarchy transforms after axis conversion.
- Root transform policy must be explicit:
  - either baked into skeleton root node, or standardized via one root correction node.

#### D. Physics Metadata Contract (Basic Preservation)

Basic PMX physics intent must be carried via one of:

- glTF node `extras.isla_physics`, or
- sidecar `model.physics.json` keyed by node/bone name.

Minimum supported metadata schema:

```json
{
  "colliders": [
    {
      "bone": "Spine",
      "shape": "capsule",
      "radius": 0.08,
      "half_height": 0.22,
      "center": [0.0, 0.12, 0.0],
      "layer": "character",
      "mask": ["world", "character"]
    }
  ],
  "constraints": [
    {
      "type": "parent",
      "bone_a": "UpperArm_L",
      "bone_b": "LowerArm_L"
    }
  ]
}
```

Supported in-scope fields for Phase 4:

- `shape`: `sphere`, `capsule`, `box`
- `center`, primitive dimensions
- `layer`, `mask`
- minimal parent/attachment-style constraint hints

Out-of-scope fields (must be safely ignored with warning):

- full soft-body semantics
- advanced spring/joint motor parameters
- engine-specific PMX rigid-body flags with no runtime mapping

#### E. Naming and Clip Contract

- Bone names must be stable and preserved from conversion output.
- Motion clip naming convention:
  - lowercase snake case (for example: `idle`, `walk_fwd`, `run`, `jump_start`)
- Required regression clip set for test assets:
  - `idle`
  - `walk_fwd`
- Optional root motion:
  - if present, must be tagged in metadata so runtime can choose in-place vs root-driven playback.

#### F. Validation Checklist (Pass/Fail)

A converted asset fails contract validation if any of these are true:

- no `skin` or no joints
- missing `JOINTS_0` or `WEIGHTS_0`
- joint indices out of range for skeleton
- weights are all zero for any vertex and cannot be normalized to fallback
- no animations for required validation assets
- unresolved required texture paths
- physics metadata parse error (if metadata file exists)

A converted asset passes with warnings if:

- optional physics fields are unsupported but ignorable
- optional clips are missing beyond required baseline

#### G. Runtime Fallback Policy

- Missing optional physics metadata:
  - continue load, no physics proxies created, log warning
- Unsupported physics field:
  - continue load, ignore field, log warning
- Invalid core skinning data:
  - hard fail load for animated path
- Missing animation clips:
  - allow static skinned pose if skin is valid; playback APIs return clip-not-found

### Deliverables

- This section (Phase 1 contract details) is the single source of truth.
- Example converted asset set under `engine/src/render/testdata/pmx_pipeline/`.
- A conversion script entrypoint (to be added) that emits contract-compliant output.

### Exit Criteria

- PMX conversion is reproducible and outputs runtime-consumable glTF + physics metadata.

## Phase 2: Runtime Clip Playback System

### Goal

Play animation clips in runtime using the Phase 0 pose evaluator.

### Scope

- Add animation playback state:
  - clip index
  - local time
  - play/pause/loop
  - speed scalar
- Advance clip time each tick (fixed-step or frame-time policy, documented).
- Evaluate pose each frame/tick and cache skin matrices for rendering.
- Add minimal API for selecting clips.

### Files (expected)

- `engine/include/isla/engine/render/render_world.hpp` (or adjacent animation state)
- `engine/src/render/*` animation playback integration
- `client/src/client_app.cpp` startup/demo clip selection

### Exit Criteria

- A converted skinned glTF character visibly plays a looped clip.

## Phase 3: GPU Skinning Render Path

### Goal

Render skinned meshes using evaluated joint matrices.

### Scope

- Extend mesh manager vertex format for skinned attributes:
  - joints (u16/u8 packed)
  - weights (float/normalized)
- Add skinning-capable shader variant(s).
- Upload/bind joint matrix palette per draw.
- Keep static mesh path intact.

### Risks

- Uniform limits for large skeletons
- Shader/backend compatibility differences

### Exit Criteria

- Character deforms correctly during animation (no rigid-only motion).

## Phase 4: Basic Physics Preservation from PMX Conversion

### Goal

Preserve basic PMX physics intent through glTF metadata ingestion.

### Scope

- Read converter-emitted physics metadata (extras/sidecar).
- Map to runtime basic physics constructs:
  - simple colliders per mapped bone/root
  - optional parented rigid attachments
  - conservative constraints subset (if present)
- Define fallback behavior for unsupported physics fields.
- Keep physics feature scope minimal and stable.

### Exit Criteria

- Imported character gets basic collider/physics proxies aligned with skeleton.

## Phase 5: PMX Motion Pipeline (VMD to glTF Clip Workflow)

### Goal

Make PMX motion assets usable by converting motion data into glTF clips.

### Scope

- Define VMD to glTF clip conversion workflow.
- Resolve bone naming/retarget mapping policy.
- Verify clip timing and root motion policy.
- Add regression assets for idle/walk/action.

### Exit Criteria

- PMX model + converted motion clips play reliably in runtime through the same clip system.

## Phase 6: Tooling, Validation, and CI

### Goal

Make the pipeline maintainable and testable.

### Scope

- Add asset validation script/checklist for converted glTF packages.
- Add automated tests for:
  - loader failures (missing skin/joints/weights)
  - pose eval determinism
  - shader contract for skinning path
  - physics metadata parsing fallbacks
- Add CI target(s) for animation/physics pipeline tests.

### Exit Criteria

- Pipeline regressions are detected automatically in CI.

## Cross-Phase Testing Strategy

1. Keep unit tests at module boundaries (loader, pose sampling, metadata parser).
2. Add small canonical converted assets for deterministic assertions.
3. Prefer structural assertions (joint counts, matrix validity, clip duration) over brittle visual checks.
4. Add a smoke runtime test for one animated skinned character path.

## Suggested Delivery Milestones

1. First usable PMX pipeline point: after Phase 2 (runtime clip playback).
2. First visually correct character deformation point: after Phase 3 (GPU skinning).
3. First basic PMX parity point (animation + basic physics): after Phase 4/5.
