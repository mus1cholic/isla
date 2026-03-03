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

Operational interpretation:

- Runtime render input remains glTF/GLB.
- PMX support is provided through conversion orchestration workflows, not native PMX render/runtime parsing.

> [!NOTE]
> **Current status (2026-03-03):**
> - Phase 0 is complete and substantially hardened.
> - Phase 1 is complete (contract + schema + validator + validator tests).
> - Phase 2 is complete (runtime clip playback + controller + temporary CPU skinning path).
> - Phase 2.5 is complete (in-place CPU skinning updates + workspace reuse + deferred bounds recompute).
> - Phase 3 is complete (authoritative GPU skinning path for current fixed palette budget).
> - Phase 3.5 is pending (large-skeleton GPU skinning support beyond current fixed palette budget).
> - Phases 4-9 remain pending runtime/tooling expansion.
> - Model intake automation (`models/` directory + PMX auto-convert-on-launch) is planned for Phase 6 and finalized in Phase 9.
>
> Phase 3 design constraint (current):
> - Authoritative GPU skinning is complete for current scope, but uses a fixed 64-joint palette budget per skinned draw.
> - Large-skeleton (>64 referenced joint index) support is explicitly deferred to Phase 3.5.
>
> Phase 1 artifacts:
> - `docs/pmx/pmx_to_gltf_conversion_contract.md`
> - `docs/pmx/schemas/pmx_physics_metadata.schema.json`
> - `tools/pmx/validate_converted_gltf.py`
> - `tools/pmx/validate_converted_gltf_test.py` (`bazel test //tools/pmx:validate_converted_gltf_test`)
>
> Phase 2 artifacts:
> - `engine/src/render/include/animation_playback_controller.hpp`
> - `engine/src/render/animation_playback_controller.cpp`
> - `client/src/animated_mesh_skinning.hpp`
> - `client/src/animated_mesh_skinning.cpp`
> - `client/src/client_app.cpp` (runtime wiring + env controls)
>
> Phase 2.5 artifacts:
> - `engine/include/isla/engine/render/render_world.hpp` (`edit_triangles_without_recompute_bounds(...)`)
> - `client/src/animated_mesh_skinning.hpp` (per-primitive workspace + in-place APIs)
> - `client/src/animated_mesh_skinning.cpp` (topology/workspace build + in-place skin writes)
> - `client/src/client_app.hpp` / `client/src/client_app.cpp` (binding workspace + in-place tick updates)
> - `engine/src/render/render_world_test.cpp` (`MeshData` no-bounds-edit regression coverage)
> - `client/src/animated_mesh_skinning_test.cpp` (multi-tick churn/capacity guards)
> - `client/src/client_app_animation_test.cpp` (runtime multi-frame storage stability smoke test)
> - `.github/workflows/ci.yml` (windows smoke includes `//engine/src/render:render_world_tests`)
>
> Phase 3 artifacts:
> - `engine/include/isla/engine/render/render_world.hpp` (skinned geometry + skin palette storage)
> - `engine/src/render/include/bgfx_mesh_manager.hpp` / `engine/src/render/bgfx_mesh_manager.cpp` (dual static/skinned upload path + validation guards)
> - `engine/src/render/include/bgfx_shader_manager.hpp` / `engine/src/render/bgfx_shader_manager.cpp` (skinned shader program cache/resolve path)
> - `engine/src/render/shaders/vs_mesh_skinned.sc` / `engine/src/render/shaders/varying.def.sc` / `engine/src/render/shaders/BUILD` (skinning shader variant + build wiring)
> - `engine/include/isla/engine/render/model_renderer.hpp` / `engine/src/render/model_renderer.cpp` (skinned draw selection + palette upload)
> - `engine/src/render/include/model_renderer_skinning_utils.hpp` / `engine/src/render/model_renderer_skinning_utils.cpp` (program-path and palette helper logic)
> - `client/src/client_app.hpp` / `client/src/client_app.cpp` (GPU-authoritative animation updates + CPU fallback)
> - `engine/src/render/model_renderer_skinning_utils_test.cpp`
> - `engine/src/render/bgfx_mesh_manager_test.cpp`
> - `engine/src/render/shader_contract_test.cpp` / `engine/src/render/shader_binary_runtime_test.cpp`
> - `.github/workflows/ci.yml` (windows smoke includes `//engine/src/render:bgfx_mesh_manager_tests`, `//engine/src/render:model_renderer_skinning_utils_tests`, `//engine/src/render:shader_binary_runtime_tests`)

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
- Phase 1 now codifies these runtime constraints in conversion contract + validation tooling.
- Phase 2 now consumes this runtime evaluator through a system-level playback controller.

## Phase 1: PMX to glTF Conversion Contract (Completed)

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

### Implemented (2026-03-03)

- Published contract and schema:
  - `docs/pmx/pmx_to_gltf_conversion_contract.md`
  - `docs/pmx/schemas/pmx_physics_metadata.schema.json`
- Added example sidecar payload:
  - `docs/pmx/examples/sample.physics.json`
- Added validator script:
  - `tools/pmx/validate_converted_gltf.py`
- Added validator tests + fixtures:
  - `tools/pmx/validate_converted_gltf_test.py`
  - `tools/pmx/testdata/*`
  - Bazel target: `//tools/pmx:validate_converted_gltf_test`
- Hardened validator behavior to align with contract:
  - strict sidecar schema version (`1.0.0`)
  - strict baseline clip requirements (`idle`, `walk`, + one additional clip) as pass/fail errors
  - strict top-level sidecar type handling (structured errors instead of crash)
  - stricter RFC3339 timestamp validation (timezone required)
  - improved JOINTS_0 metadata diagnostics for malformed `min`/`max`

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
- Current GPU skinning caveat (Phase 3 implementation):
  - authoritative GPU palette currently supports joint indices in `[0, 63]` per skinned draw
  - >64-joint referenced primitive support is tracked in Phase 3.5

### Known Phase 1 Limits (Intentional)

- Validator does not decode raw accessor buffer values for `JOINTS_0`; bounds checks are static and metadata-dependent.
- Validator enforces conversion contract constraints, but does not execute rendering/runtime playback paths.
- Validator does not currently enforce Phase 3 fixed GPU palette budget constraints (`JOINTS_0 <= 63` on rendered primitive path).

### Exit Criteria

- PMX conversion is reproducible and outputs runtime-consumable glTF + physics metadata that respects current runtime constraints.
- Satisfied as of 2026-03-03.
- Phase 2 runtime playback path now uses Phase 1 outputs as expected baseline input.

## Phase 2: Runtime Clip Playback System (Completed)

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
- Consume Phase 1-validated assets as the expected input contract for runtime playback.

### Implemented (2026-03-03)

- Added animation playback controller module:
  - `engine/src/render/include/animation_playback_controller.hpp`
  - `engine/src/render/animation_playback_controller.cpp`
- Added system-level playback state and API:
  - clip index
  - local time
  - play/pause
  - speed scalar
  - playback mode (`Loop`/`Clamp`)
  - seek + immediate evaluate support
- Hardened controller semantics:
  - transactional `set_asset(...)` (failed set leaves cleared state)
  - local time normalization aligned with sampled pose semantics (loop/clamp-consistent state introspection)
- Added runtime/client wiring:
  - frame-time tick advancement using SDL nanosecond clock
  - animated glTF startup load path via env (`ISLA_ANIMATED_GLTF_ASSET`)
  - optional clip/mode env controls (`ISLA_ANIM_CLIP`, `ISLA_ANIM_PLAYBACK_MODE`)
  - per-tick pose evaluation + temporary CPU skinning into render mesh triangles
- Added logging for runtime observability:
  - startup selection logs (clip/mode)
  - invalid mode warnings
  - zero-playable-mesh warning for animated package load
  - throttled playback telemetry
- Added tests:
  - `engine/src/render/animation_playback_controller_test.cpp`
  - `client/src/animated_mesh_skinning_test.cpp`
  - `client/src/client_app_animation_test.cpp`
- Added CI/smoke wiring updates:
  - `.github/workflows/ci.yml` windows smoke includes:
    - `//engine/src/render:animation_playback_controller_tests`
    - `//client/src:animated_mesh_skinning_test`
    - `//client/src:client_app_animation_test`

### Known Phase 2 Limits (Intentional / Deferred)

- This CPU path is functional for visible playback bring-up but not intended as final deformation architecture.
- CPU deformation remains a temporary compatibility path until Phase 3 GPU skinning is authoritative.

### Exit Criteria

- A converted skinned glTF character visibly plays selected clips with predictable loop/clamp behavior.
- Satisfied as of 2026-03-03 via runtime playback controller + temporary CPU skinning.

## Phase 2.5: CPU Skinning Update Path Optimization (Completed)

### Goal

Reduce per-frame CPU and allocation overhead in the temporary CPU-skinned playback path introduced during Phase 2.

### Scope

- Replace per-frame triangle vector reconstruction/replacement with in-place updates:
  - avoid `MeshData::set_triangles(...)` per tick for animated meshes
  - prefer pre-sized storage and `MeshData::edit_triangles(...)` mutation
- Reuse per-primitive working buffers (for skinned vertex positions and/or triangle writes) across ticks.
- Avoid unnecessary per-frame bounds recomputation for animated meshes:
  - either defer bounds recompute to a lower frequency, or
  - gate by runtime need (if bounds are not currently consumed for culling/physics in this path).
- Keep this optimization scoped to the temporary CPU deformation path; do not block Phase 3 GPU skinning rollout.

### Rationale

Phase 2 currently skins by rebuilding and replacing triangle lists every frame, which implies:

- repeated allocations and copies
- repeated bounds recomputation
- avoidable CPU overhead that scales with mesh/primitive size

This is acceptable for initial bring-up, but should be tightened before relying on larger PMX assets in playback-heavy scenarios.

### Implemented (2026-03-03)

- Replaced per-tick triangle-list replacement for animated meshes with in-place mutation:
  - animated mesh tick path now uses `MeshData::edit_triangles_without_recompute_bounds(...)`
  - no per-frame `MeshData::set_triangles(...)` calls on the animated update loop
- Added reusable per-primitive CPU skinning workspace:
  - cached triangle topology indices for valid primitive triangles
  - reused skinned-position buffer across ticks
- Updated startup animated mesh population to prebuild triangle storage + topology workspace once.
- Deferred bounds recompute on animated tick path to a lower-frequency interval (instead of every frame).
- Added defensive workspace topology validation/rebuild in `skin_primitive_in_place(...)`:
  - detects stale/uninitialized/mismatched topology against primitive valid triangle stream
  - logs throttled warning when rebuild is triggered
- Reset animation bounds-recompute tick counter in `populate_world_from_animated_asset()` for deterministic behavior across runtime and test-hook call paths.
- Added low-verbosity telemetry (`VLOG(1)`) when deferred animated mesh bounds recompute is executed.
- Added regression/perf-oriented test coverage:
  - in-place skinning storage/capacity stability over many ticks
  - client animation smoke test validating stable triangle storage across many frames
  - `MeshData` edit-without-bounds-recompute contract test
  - deferred-bounds interval boundary behavior in client animation tick tests
  - stale workspace and uninitialized workspace rebuild safety tests

### Known Limits (Post-Phase 2.5)

- CPU skinning remains a temporary deformation path until Phase 3 GPU skinning is authoritative.
- Bounds recompute is deferred (not per-frame), which is acceptable for current usage because this path does not currently rely on per-frame bounds for culling/physics decisions.

### Exit Criteria

- Animated CPU-skinning path no longer performs full triangle-list reallocation/replacement each tick.
- Runtime behavior is unchanged functionally (same visible animation output as Phase 2 baseline).
- Added regression/perf-oriented test coverage for the in-place update path (correctness first, plus basic allocation/churn guard where practical).
- Satisfied as of 2026-03-03.

## Phase 3: GPU Skinning Render Path (Completed; Phase 3.5 Follow-up Pending)

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
- Validate GPU path against Phase 1 contract outputs (`JOINTS_0`, `WEIGHTS_0`, baseline clip presence).
- Retire or bypass Phase 2 temporary CPU skinning deformation updates once GPU skinning is authoritative.
- Keep Phase 2.5 CPU path as an optional fallback/debug path during rollout, but treat GPU skinning as the single source of visual truth once validated.
- Continue consuming converted glTF/GLB as runtime render input; PMX intake automation is handled in later tooling phases.

### Implemented (2026-03-03)

- Added skinned mesh render data to `RenderWorld`:
  - skinned vertex stream (`position/normal/uv/joints/weights`)
  - skinned index stream
  - per-mesh skin palette storage
- Added bgfx dual upload path (static + skinned) with dedicated skinned vertex layout.
- Added skinning-capable shader variant and runtime loading path:
  - `vs_mesh_skinned.sc`
  - shader manager skinned program resolution
- Added renderer draw-path wiring:
  - per-draw static-vs-skinned program selection
  - per-draw joint palette upload (`u_joint_palette[64]`)
- Added `ClientApp` GPU-authoritative animation tick path:
  - per-tick palette updates without per-frame CPU geometry deformation
  - retained Phase 2.5 CPU skinning path as fallback/debug path
- Added Phase 3 safety hardening:
  - shader-side index clamp into `[0, 63]`
  - skinned upload rejection for invalid primitive indices
  - skinned upload rejection when vertex joint indices exceed current palette budget
- Added focused tests and CI coverage for:
  - skinned shader source/binary contracts
  - skinned upload churn/guard behavior
  - GPU program-path decision and palette helper logic
  - GPU-authoritative runtime update stability

### Known Limits (Post-Phase 3)

- Current GPU skinning palette budget is fixed at 64 joint matrices per skinned draw.
- Assets/primitives referencing joint indices above this budget are not yet supported by the current Phase 3 upload path.
- Large-skeleton support is deferred to Phase 3.5 (joint remap and/or draw partition strategy).

### Risks

- Uniform limits for large skeletons
- Shader/backend compatibility differences
- Large-skeleton mitigation beyond fixed palette budget is tracked in Phase 3.5.

### Exit Criteria

- Character deforms correctly during animation (no rigid-only motion).
- Satisfied as of 2026-03-03 for the current fixed-palette budget path.

## Phase 3.5: Large-Skeleton GPU Skinning Support (>64 Joint Indices)

### Goal

Support real character assets where skinned primitives reference more than 64 joints without falling back to non-authoritative deformation behavior.

### Scope

- Remove strict runtime dependency on a single fixed 64-matrix palette per skinned draw.
- Introduce a deterministic joint remap workflow for GPU skinning:
  - build per-primitive (or per-submesh partition) local joint palettes
  - rewrite uploaded skinned joint indices to local palette space
  - upload only remapped palette entries per draw
- Define and document draw splitting policy when a primitive cannot be represented in one palette budget.
- Keep Phase 3 shader/runtime contracts intact for assets already within the 64-joint budget.
- Preserve static mesh and non-skinned paths unchanged.
- Add regression coverage for:
  - primitives with joints spanning above 64 global indices
  - remap correctness (same deformation output as CPU/reference path)
  - deterministic behavior across repeated loads/ticks

### Risks

- Additional CPU preprocessing cost for remap/partition build
- Complexity in draw splitting and material/primitive bookkeeping
- Potential mismatch bugs between remapped indices and uploaded palette entries

### Exit Criteria

- GPU skinning path supports converted assets whose skinned primitives reference more than 64 joints, with correct deformation and without silent GPU-path rejection.
- Runtime no longer treats `joint_index > 63` as a hard incompatibility for otherwise valid assets.

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
- Use Phase 1 sidecar schema (`schema_version: 1.0.0`) as ingestion baseline.
- Integrate against established Phase 2/3 playback + skeleton runtime state (avoid introducing a parallel animation/deformation state path).

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
- Maintain Phase 1 baseline clip naming/acceptance requirements.
- Target Phase 2 playback controller API as the runtime clip control surface.
- Validate converted clips against the Phase 3 GPU skinning runtime path (not CPU fallback-only behavior).

### Exit Criteria

- PMX model + converted motion clips play reliably in runtime through the same clip system.

## Phase 6: Tooling, Validation, and CI

### Goal

Make the pipeline maintainable and testable.

### Scope

- Expand the Phase 1 validator/checklist into broader CI/runtime coverage (do not replace it).
- Add model intake orchestration for app/runtime workflows:
  - define a default `models/` directory scan/input policy
  - accept both `.pmx` and `.gltf/.glb` files in that intake directory
  - if input is `.pmx`, run conversion automatically at app launch (or first-use) to produce runtime glTF/GLB output
  - if input is `.gltf/.glb`, load directly without conversion
  - cache converted outputs and avoid unnecessary reconversion when source + converter version are unchanged
  - produce clear logs/errors for conversion failures and fallback/skip behavior
  - define deterministic model selection policy when multiple candidates exist
- Add automated tests for:
  - loader failures (missing skin/joints/weights)
  - pose eval determinism
  - interpolation mode handling (`LINEAR`/`STEP` + rejection paths)
  - playback mode behavior (`Loop`/`Clamp`)
  - shader contract for skinning path
  - GPU skinning guard/fallback behavior around current palette/index limits
  - physics metadata parsing fallbacks
  - model intake orchestration (`models/` scan, PMX auto-convert trigger, converted-output cache hit path)
- Add CI target(s) for animation/physics pipeline tests.
- Add CI wiring for `//tools/pmx:validate_converted_gltf_test`.
- Extend CI/smoke wiring from current baseline that already includes:
  - `//engine/src/render:animation_playback_controller_tests`
  - `//engine/src/render:render_world_tests`
  - `//client/src:animated_mesh_skinning_test`
  - `//client/src:client_app_animation_test`
  - `//engine/src/render:bgfx_mesh_manager_tests`
  - `//engine/src/render:model_renderer_skinning_utils_tests`
  - `//engine/src/render:shader_binary_runtime_tests`

### Exit Criteria

- Pipeline regressions are detected automatically in CI.
- App/tooling flow supports `models/` intake where `.pmx` inputs are auto-converted to runtime glTF/GLB and then loaded for display, while `.gltf/.glb` inputs load directly.

## Phase 7: Full Node-Hierarchy Animation Support

### Goal

Remove current hierarchy caveats by evaluating full glTF node graph semantics.

### Scope

- Evaluate animation channels on non-joint nodes.
- Compose full node hierarchy before skin extraction.
- Replace/retire `bind_prefix_matrices` workaround where appropriate.
- Support rigs with non-joint skeleton roots and animated intermediate nodes correctly.
- Revisit Phase 1 conversion caveats once full hierarchy animation support lands.
- Preserve Phase 2 controller semantics for clip/state APIs while changing evaluation internals.
- Keep compatibility with Phase 3/3.5 GPU skinning data flow while evaluation internals evolve.

### Exit Criteria

- Assets with animated non-joint hierarchy segments evaluate correctly without conversion-side workarounds.

## Phase 8: Interpolation + Sampling Completeness

### Goal

Close remaining runtime animation fidelity/performance gaps.

### Scope

- Add `CUBICSPLINE` interpolation support (including tangent semantics).
- Add cached key index walking for monotonic playback (`amortized O(1)` sampling).
- Expand regression tests for cubic edge cases and seek behavior.
- Update Phase 1 contract + validator rules when `CUBICSPLINE` transitions from unsupported to supported.
- Maintain Phase 2 state/pose consistency guarantees (reported local time aligns with sampled pose semantics).
- Maintain Phase 3 deformation consistency guarantees (GPU-skinned output remains aligned with evaluated pose semantics).

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
- Version and publish contract/schema migration guidance beyond Phase 1 (`schema_version` evolution).
- Explicitly document temporary-vs-final deformation path transitions:
  - Phase 2 temporary CPU skinning
  - Phase 2.5 optimized temporary CPU skinning (in-place + deferred bounds)
  - Phase 3+ authoritative GPU skinning
  - Phase 3.5 large-skeleton GPU skinning extension beyond fixed palette budget
- Publish final end-user intake workflow docs:
  - where to place models (`models/`)
  - supported input extensions (`.pmx`, `.gltf`, `.glb`)
  - PMX auto-convert behavior, cache location/invalidation policy, and troubleshooting steps

### Exit Criteria

- PMX conversion + runtime playback + basic physics flow is production-ready with documented limits and regression coverage.
- End-user can place a model in `models/` (`.pmx` or `.gltf/.glb`), run the app, and see it displayed; PMX inputs auto-convert through the defined pipeline.

## Cross-Phase Testing Strategy

1. Keep unit tests at module boundaries (loader, pose sampling, metadata parser).
2. Add small canonical converted assets for deterministic assertions.
3. Prefer structural assertions (joint counts, matrix validity, clip duration) over brittle visual checks.
4. Add a smoke runtime test for one animated skinned character path.
5. Keep `//tools/pmx:validate_converted_gltf_test` as the conversion-gate baseline and extend coverage incrementally.

## Suggested Delivery Milestones

1. First deterministic PMX conversion gate: after Phase 1 (contract + schema + validator).
2. First usable PMX runtime playback point: after Phase 2 (runtime clip playback).
3. First visually correct character deformation point: after Phase 3 (GPU skinning, achieved for current fixed-palette-budget content).
4. First large-rig-ready deformation point (>64-joint referenced primitives): after Phase 3.5.
5. First basic PMX parity point (animation + basic physics): after Phase 4/5.
6. First integrated model-intake UX point (`models/` directory + PMX auto-convert + load/display): after Phase 6.
7. First hierarchy-fidelity point for complex rigs: after Phase 7.
8. First interpolation-complete point: after Phase 8.
