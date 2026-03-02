# PMX to glTF Animation + Physics Phased Plan

## Context

`isla` currently has:

- Static mesh loading for `.obj/.gltf/.glb`
- A bgfx-based renderer path
- Transparent overlay window behavior
- Animated glTF runtime foundation (skin/joint/clip loading and pose evaluation)

Target outcome:

- Use PMX assets by converting them to glTF
- Preserve basic character animation playback
- Preserve basic physics semantics needed for runtime behavior

## Recommendation

Implement PMX support as an external conversion pipeline plus incremental runtime import/playback in `isla`, not as native PMX runtime parsing.

Rationale:

- PMX ecosystem tooling is conversion-oriented.
- glTF is already integrated in runtime.
- This de-risks format complexity and keeps runtime focused.

> [!NOTE]
> **Current status (2026-03-02):** Phase 0 is complete and substantially hardened.  
> Phase 1 contract artifacts are now drafted:
> - `docs/pmx/pmx_to_gltf_conversion_contract.md`
> - `docs/pmx/schemas/pmx_physics_metadata.schema.json`
> - `tools/pmx/validate_converted_gltf.py`
> Runtime integration phases (2-9) remain pending.

## Phase 0: Animated glTF Runtime Foundation (Completed)

### Goal

Add robust runtime structures and pose evaluation needed before full playback/rendering.

### Implemented

- New animated glTF module:
  - `engine/src/render/include/animated_gltf.hpp`
  - `engine/src/render/animated_gltf.cpp`
- Added core runtime data:
  - skeleton/joint representation
  - inverse bind matrix import
  - clip/track/keyframe import (TRS channels)
  - per-track interpolation modes (`LINEAR`, `STEP`)
  - pose sampling (vec3 interpolation + quat slerp)
  - skin matrix generation
- Added playback time mode support in pose evaluation:
  - `Loop` (default; wraps into `[0, duration)`)
  - `Clamp` (end-sticky; exact end hits final key)
- Hierarchy robustness:
  - non-topological skin joint order handling via DFS parent resolution
  - parent cycle detection during pose evaluation
  - non-joint ancestor transform baking (`bind_prefix_matrices`)
- Loader robustness:
  - selects all triangle primitives attached to nodes using selected skin (instead of first global primitive)
  - strict keyframe decode failures for translation/rotation/scale channels
  - strict input/output key count validation per channel
  - explicit rejection of unsupported interpolation (`CUBICSPLINE`)
  - explicit rejection of matrix-authored joint nodes (`has_matrix`)
- Added/expanded tests:
  - `engine/src/render/animated_gltf_test.cpp`
  - loader tests, hierarchy tests, interpolation tests (translation/rotation/scale, linear/step, exact-key behavior), playback mode tests
  - fixture-based test organization (`TEST_F`) and hardened temp directory creation

### Known Phase 0 Limits (Intentional)

- Matrix-authored joints are hard-failed.
- `CUBICSPLINE` animation interpolation is hard-failed.
- Non-joint node animation channels are not evaluated in hierarchy composition.
- Primitive dedup is pointer-based; per-node primitive instances/transforms are not yet represented.
- Sampling uses binary search per sample (`O(log N)`), not cached index walking (`amortized O(1)`).

### Exit Criteria

- Runtime can load skinned glTF animation data and evaluate joint/skin matrices deterministically with explicit failure behavior for unsupported/invalid content.

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

### Contract Notes Updated From Phase 0

- Required runtime-compatible animation interpolation:
  - supported: `LINEAR`, `STEP`
  - unsupported: `CUBICSPLINE` (currently hard fail)
- Joint authoring requirement:
  - joints must be TRS-authored (`translation/rotation/scale`)
  - matrix-authored joint nodes are currently unsupported and rejected
- Channel count requirement:
  - `input.count == output.count` must hold for runtime-supported samplers
- Skin primitive association requirement:
  - skinned primitives must be referenced by nodes that use the target skin
- Hierarchy caveat:
  - static non-joint ancestors are baked; animated non-joint chains are not fully supported yet

### Exit Criteria

- PMX conversion is reproducible and outputs runtime-consumable glTF + physics metadata that respects current runtime constraints.

## Phase 2: Runtime Clip Playback System

### Goal

Play animation clips in runtime using the Phase 0 pose evaluator.

### Scope

- Add animation playback state:
  - clip index
  - local time
  - play/pause/loop
  - speed scalar
  - playback mode (`Loop`/`Clamp`) wiring at system level
- Advance clip time each tick (fixed-step or frame-time policy, documented).
- Evaluate pose each frame/tick and cache skin matrices for rendering.
- Add minimal API for selecting clips and playback mode.

### Exit Criteria

- A converted skinned glTF character visibly plays selected clips with predictable loop/clamp behavior.

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
- Ensure multi-primitive skinned assets render all attached primitives.

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
- Ensure converted clips avoid unsupported sampler output until cubic support lands.

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
  - interpolation mode handling (`LINEAR`/`STEP` + rejection paths)
  - playback mode behavior (`Loop`/`Clamp`)
  - shader contract for skinning path
  - physics metadata parsing fallbacks
- Add CI target(s) for animation/physics pipeline tests.

### Exit Criteria

- Pipeline regressions are detected automatically in CI.

## Phase 7: Full Node-Hierarchy Animation Support

### Goal

Remove current hierarchy caveats by evaluating full glTF node graph semantics.

### Scope

- Evaluate animation channels on non-joint nodes.
- Compose full node hierarchy before skin extraction.
- Replace/retire `bind_prefix_matrices` workaround where appropriate.
- Support rigs with non-joint skeleton roots and animated intermediate nodes correctly.

### Exit Criteria

- Assets with animated non-joint hierarchy segments evaluate correctly without conversion-side workarounds.

## Phase 8: Interpolation + Sampling Completeness

### Goal

Close remaining runtime animation fidelity/performance gaps.

### Scope

- Add `CUBICSPLINE` interpolation support (including tangent semantics).
- Add cached key index walking for monotonic playback (`amortized O(1)` sampling).
- Expand regression tests for cubic edge cases and seek behavior.

### Exit Criteria

- Runtime matches expected glTF animation interpolation semantics for all targeted modes with stable performance.

## Phase 9: PMX Pipeline Readiness and Sign-Off

### Goal

Finalize end-to-end PMX-to-runtime readiness with explicit support matrix.

### Scope

- Publish supported/unsupported feature matrix (PMX -> conversion -> glTF -> runtime).
- Freeze converter/runtime compatibility versions.
- Produce representative demo assets and validation reports.
- Document operational runbook for adding new PMX characters/clips.

### Exit Criteria

- PMX conversion + runtime playback + basic physics flow is production-ready with documented limits and regression coverage.

## Cross-Phase Testing Strategy

1. Keep unit tests at module boundaries (loader, pose sampling, metadata parser).
2. Add small canonical converted assets for deterministic assertions.
3. Prefer structural assertions (joint counts, matrix validity, clip duration) over brittle visual checks.
4. Add a smoke runtime test for one animated skinned character path.

## Suggested Delivery Milestones

1. First usable PMX pipeline point: after Phase 2 (runtime clip playback).
2. First visually correct character deformation point: after Phase 3 (GPU skinning).
3. First basic PMX parity point (animation + basic physics): after Phase 4/5.
4. First hierarchy-fidelity point for complex rigs: after Phase 7.
5. First interpolation-complete point: after Phase 8.
