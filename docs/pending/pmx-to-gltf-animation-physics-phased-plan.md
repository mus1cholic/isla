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
> **Current status (2026-03-05):**
> - Phase 0 is complete and substantially hardened.
> - Phase 1 is complete (contract + schema + validator + validator tests).
> - Phase 2 is complete (runtime clip playback + controller + temporary CPU skinning path).
> - Phase 2.5 is complete (in-place CPU skinning updates + workspace reuse + deferred bounds recompute).
> - Phase 3 is complete (authoritative GPU skinning path for current fixed palette budget).
> - Phase 3.5 is complete (large-skeleton GPU skinning support beyond current fixed palette budget via deterministic remap/partition).
> - Phase 4 is complete (static glTF visual-fidelity baseline: authored normals/material ingestion, `MASK` semantics hardening, and static-fallback parity guardrails).
> - Phase 4.1 is complete (static glTF per-primitive material preservation + deterministic aggregate transform + fallback parity + additional loader hardening and contract test refactors).
> - Phase 4.5 is complete (Windows DirectComposition-backed transparent overlay path with visible 3D rendering).
> - Phase 4.6 is complete (coordinate system mirroring documentation + Alpha Blend depth sorting assertions).
> - Phase 5 is complete (basic PMX physics sidecar ingestion + skeleton-aligned collider proxy runtime path + parser hardening guardrails).
> - Phase 6 is complete (motion contract/schema + validator + regression fixtures/tests + CLI smoke + effective root-motion policy precedence + CI wiring).
> - Phase 7 is in progress (runtime model-intake orchestration landed and hardened: deterministic
>   `models/` scan policy, `.gltf/.glb` direct load, `.pmx` auto-convert trigger with cache
>   metadata/invalidation, fallback diagnostics, shell-less converter process execution
>   (no `std::system`), robust converter-template token handling, and regression coverage via
>   `//client/src:model_intake_test`).
> - Phases 8-10 remain pending runtime/tooling expansion.
> - Phase 7.5 (runtime material/primitive introspection + deterministic texture-remap override path) remains pending.
> - Model intake automation (`models/` directory + PMX auto-convert-on-launch) is now partially implemented in Phase 7 and finalized in Phase 10.
> - PMX conversion remains orchestration-driven (external converter command), not native PMX runtime parsing.
>
> Phase 3/3.5 design constraint (current):
> - Shader-side per-draw GPU palette budget remains fixed at 64 joints (`u_joint_palette[64]`).
> - Large-skeleton support is provided by deterministic primitive partitioning + local joint remap, so assets can reference >64 global joints overall while each draw remains within the 64-joint local budget.
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
>
> Phase 3.5 artifacts:
> - `engine/src/render/include/model_renderer_skinning_utils.hpp` / `engine/src/render/model_renderer_skinning_utils.cpp` (deterministic GPU skinning partition/remap utilities)
> - `client/src/client_app.hpp` / `client/src/client_app.cpp` (partitioned GPU mesh population + per-partition remapped skin palette updates)
> - `engine/src/render/model_renderer_skinning_utils_test.cpp` (remap equivalence + determinism + split/no-split + invalid-input guard coverage)
> - `client/src/client_app_animation_test.cpp` (large-skeleton GPU-authoritative partitioning + repeated populate stability coverage)
>
> Phase 4 artifacts:
> - `engine/include/isla/engine/render/render_types.hpp` (static triangle per-vertex normal payload for authored-normal preservation)
> - `engine/src/render/include/mesh_asset_loader.hpp` / `engine/src/render/mesh_asset_loader.cpp` (static glTF base color/alpha/alpha-cutoff/texture ingestion + URI hardening)
> - `engine/include/isla/engine/render/render_world.hpp` (neutral default object color baseline + material alpha cutoff field)
> - `engine/src/render/bgfx_mesh_manager.cpp` (authored-normal consume path with deterministic face-normal fallback when missing)
> - `engine/src/render/model_renderer.cpp` / `engine/src/render/shaders/fs_mesh.sc` (cutout shader + render-state semantics for `MASK`)
> - `client/src/client_app.cpp` (animated-load fallback-to-static fidelity path + static material summary logging)
> - `engine/src/render/mesh_asset_loader_test.cpp` / `client/src/client_app_animation_test.cpp` / `engine/src/render/shader_contract_test.cpp` / `engine/src/render/render_world_test.cpp` (Phase 4 regression coverage)
>
> Phase 4.1 artifacts:
> - `engine/src/render/include/mesh_asset_loader.hpp` / `engine/src/render/mesh_asset_loader.cpp` (per-primitive static load contract + primitive-material preservation + URI hardening edge-case fixes)
> - `client/src/client_app.cpp` (per-primitive static fallback world population + deterministic aggregate model transform via mesh-bounds union)
> - `engine/src/render/include/model_renderer_skinning_utils.hpp` / `engine/src/render/model_renderer_skinning_utils.cpp` (material render-path decision helper extracted from renderer path)
> - `engine/src/render/model_renderer.cpp` (material render-path helper integration + lower-noise draw diagnostics)
> - `engine/src/render/mesh_asset_loader_test.cpp` / `client/src/client_app_animation_test.cpp` / `engine/src/render/model_renderer_skinning_utils_test.cpp` (Phase 4.1 regression + determinism + contract coverage)
>
> Phase 4.5 artifacts:
> - `engine/src/render/model_renderer.cpp` (DirectComposition presenter creation + premultiplied-alpha composition swapchain + external backbuffer bgfx path + transparent presentation reset contract)
> - `client/src/win32_layered_overlay.cpp` / `client/src/win32_layered_overlay.hpp` (authoritative non-layered Windows overlay style path for DirectComposition + compatibility fallback guardrails)
> - `client/src/client_app.cpp` (startup/resize wiring for the Windows alpha-composited overlay mode)
> - `engine/src/render/overlay_transparency_contract_test.cpp` / `engine/include/isla/engine/render/overlay_transparency.hpp` (transparent clear-color contract for compositor presentation path)
> - `engine/src/render/windows_composition_contract_test.cpp` (Windows composition policy/contract guardrails for style/swapchain/bgfx platform-data wiring)
> - `engine/src/render/BUILD` / `client/src/BUILD` (Windows composition/render link dependencies, including GNU-vs-MSVC linkopts split)
>
> Phase 4.6 artifacts:
> - `engine/include/isla/engine/render/render_world.hpp` / `engine/src/render/include/mesh_asset_loader.hpp` (documented Left-Handed vs Right-Handed mirroring + CCW cull requirement)
> - `engine/src/render/model_renderer.cpp` (documented Right-Handed camera `-Z` mirroring + static assertion preventing Alpha Blend intra-mesh depth sorting regressions)
>
> Phase 6 artifacts:
> - `tools/pmx/validate_motion_clips.py` (Phase 6 motion validator + per-clip effective root-motion policy resolution)
> - `tools/pmx/validate_motion_clips_test.py` / `tools/pmx/validate_motion_clips_smoke_test.py` (unit + CLI smoke coverage)
> - `tools/pmx/testdata/motion/*` (idle/walk/action baseline + sidecar-policy + failure/diagnostic fixtures)
> - `docs/pmx/schemas/pmx_motion_metadata.schema.json` / `docs/pmx/examples/sample.motion.json`
> - `docs/pmx/pmx_to_gltf_conversion_contract.md` / `tools/pmx/README.md` (Phase 6 contract + usage updates)
> - `tools/pmx/BUILD` / `.github/workflows/ci.yml` (Bazel + CI wiring for motion validator tests and smoke)
>
> Phase 7 artifacts (in progress):
> - `client/src/model_intake.hpp` / `client/src/model_intake.cpp` (deterministic `models/` intake +
>   PMX auto-convert orchestration + cache metadata)
> - `client/src/client_app.cpp` (runtime startup intake integration + startup selection diagnostics)
> - `client/src/model_intake_test.cpp` (intake/conversion/cache/injection-hardening regression suite)
> - `client/src/BUILD` / `.github/workflows/ci.yml` (Bazel target + Windows smoke inclusion for
>   `//client/src:model_intake_test`)
> - `tools/pmx/README.md` (Phase 7 intake behavior + troubleshooting updates)

### Changelog

- 2026-03-05 (Phase 7 hardening): replaced shell-based PMX converter invocation with shell-less
  argv process execution (`_spawnvp`/`fork+execvp`) to mitigate command-injection risk, added
  regression coverage for dangerous filenames and shell-metacharacter templates, fixed Windows path
  backslash handling in converter-template parsing, added incomplete-token template handling
  diagnostics (`{input}`/`{output}`) with positional-arg fallback, and expanded startup/intake
  observability logging.
- 2026-03-05 (Phase 7 partial): added runtime `models/` intake orchestration with deterministic candidate selection (`model.glb`/`model.gltf`/`model.pmx` preference + stable extension/name ordering), direct `.gltf/.glb` startup load path, `.pmx` auto-convert trigger into `models/.isla_converted/`, cache metadata-based reconversion guard (source size/mtime + converter command/version), conversion failure diagnostics/fallback behavior, new model-intake regression suite (`//client/src:model_intake_test`), and Windows smoke CI inclusion for that test target.
- 2026-03-05 (Phase 6 close-out): finalized motion contract/schema + validator implementation, added Phase 6 fixture/test matrix (including CLI smoke), wired PMX validation tests into CI, aligned default motion sidecar auto-discovery with contract naming (`<character>.motion.gltf/.glb` -> `<character>.motion.json`) with legacy fallback, and hardened root-motion policy evaluation to use effective per-clip precedence (`clip override` -> `sidecar default` -> `CLI fallback`) with actionable diagnostics.

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

### Known Limits (Phase 0, Intentional)

- Matrix-authored joints are hard-failed.
- `CUBICSPLINE` animation interpolation is hard-failed.
- Non-joint node animation channels are not evaluated in hierarchy composition.
- Primitive dedup is pointer-based; per-node primitive instances/transforms are not yet represented.
- Sampling uses binary search per sample (`O(log N)`), not cached index walking (`amortized O(1)`).

### Exit Criteria

- Runtime can load skinned glTF animation data and evaluate joint/skin matrices deterministically with explicit failure behavior for unsupported/invalid content.
- Phase 1 now codifies these runtime constraints in conversion contract + validation tooling.
- Phase 2 now consumes this runtime evaluator through a system-level playback controller.
- Phase 6 now extends this contract enforcement to motion clip packages (`tools/pmx/validate_motion_clips.py`) while retaining current runtime limits.

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
  - collider `layer` now validated as collision-layer index in `[0,31]` (mask remains uint32 bitmask)

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
- Current GPU skinning contract note (Phase 3.5 implementation):
  - per-draw GPU palette remains `[0, 63]` local joint index space
  - primitives referencing >64 global joints are supported through deterministic partition/remap preprocessing before draw submission
- Static-fidelity and security compatibility note (Phase 4 implementation):
  - runtime static fallback now consumes `alphaMode: MASK` cutoff semantics and treats surviving cutout fragments as opaque
  - absolute/traversal image URI paths are rejected in static glTF texture path resolution; conversion outputs should keep texture URIs package-local
- Physics-layer semantics note (Phase 5 hardening):
  - collider `layer` is treated as collision-layer index space (`0..31`)
  - collider `mask` remains uint32 bitmask semantics

### Known Limits (Phase 1, Intentional)

- Validator does not decode raw accessor buffer values for `JOINTS_0`; bounds checks are static and metadata-dependent.
- Validator enforces conversion contract constraints, but does not execute rendering/runtime playback paths.
- Validator does not currently execute/validate runtime partition-remap behavior for large-skeleton GPU skinning.
- Validator does not currently enforce static-loader URI hardening behavior (absolute/traversal image URI rejection is runtime-side).
- Motion-specific validation now lives in the Phase 6 validator (`tools/pmx/validate_motion_clips.py`) and is intentionally separate from this baseline Phase 1 package gate.

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
- Runtime startup observability was later expanded (Phase 6 follow-up) to log selected clip name/duration plus GPU-authoritative and physics-sidecar state in `client/src/client_app.cpp`.

### Known Limits (Phase 2, Intentional / Deferred)

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

### Known Limits (Phase 2.5, Post-Implementation)

- CPU skinning remains a temporary deformation path until Phase 3 GPU skinning is authoritative.
- Bounds recompute is deferred (not per-frame), which is acceptable for current usage because this path does not currently rely on per-frame bounds for culling/physics decisions.

### Exit Criteria

- Animated CPU-skinning path no longer performs full triangle-list reallocation/replacement each tick.
- Runtime behavior is unchanged functionally (same visible animation output as Phase 2 baseline).
- Added regression/perf-oriented test coverage for the in-place update path (correctness first, plus basic allocation/churn guard where practical).
- Satisfied as of 2026-03-03.

## Phase 3: GPU Skinning Render Path (Completed)

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

### Known Limits (Phase 3, Post-Baseline)

- Current GPU skinning palette budget is fixed at 64 joint matrices per skinned draw.
- This baseline fixed-budget behavior is extended by Phase 3.5 remap/partition support for large-skeleton primitives.

### Risks

- Uniform limits for large skeletons
- Shader/backend compatibility differences
- Large-skeleton mitigation beyond fixed palette budget is implemented in Phase 3.5; residual risk remains around partition overhead and bookkeeping complexity.

### Exit Criteria

- Character deforms correctly during animation (no rigid-only motion).
- Satisfied as of 2026-03-03 for the current fixed-palette budget path.

## Phase 3.5: Large-Skeleton GPU Skinning Support (>64 Joint Indices) (Completed)

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

### Implemented (2026-03-03)

- Added deterministic GPU partition/remap helper logic:
  - first-fit triangle partitioning into <=64-joint local palettes
  - per-partition local joint index rewrite for skinned vertices
  - input validation guards for malformed index/vertex buffers
- Updated runtime GPU-authoritative mesh population:
  - build one render mesh per partition (instead of assuming one mesh per primitive)
  - store local-to-global palette mapping per GPU mesh binding
  - upload per-partition remapped skin palettes on animation ticks
- Preserved existing 64-joint shader contract (`u_joint_palette[64]`) while lifting global primitive joint-count incompatibility through partitioning/remap.
- Added logging and observability:
  - partition-build failure diagnostics include source vertex/index counts
  - split summary logs include partition count and per-partition palette sizes
  - warning for unexpected empty remapped GPU partition palette
- Added regression coverage:
  - remap deformation equivalence (positions + normals)
  - split and no-split behavior across palette-budget boundaries
  - shared-topology and invalid-input guard cases
  - deterministic partition/remap output across repeated calls
  - client runtime large-skeleton partitioning and repeated populate stability

### Risks

- Additional CPU preprocessing cost for remap/partition build
- Complexity in draw splitting and material/primitive bookkeeping
- Potential mismatch bugs between remapped indices and uploaded palette entries

### Known Limits (Phase 3.5, Post-Implementation)

- Per-draw uniform palette budget remains fixed at 64 joints; large-skeleton support depends on preprocessing partition/remap correctness.
- Draw count and vertex duplication can increase for large-skeleton primitives that require multiple partitions.

### Exit Criteria

- GPU skinning path supports converted assets whose skinned primitives reference more than 64 joints, with correct deformation and without silent GPU-path rejection.
- Runtime no longer treats `joint_index > 63` as a hard incompatibility for otherwise valid assets.
- Satisfied as of 2026-03-03.

## Phase 4: Static glTF Visual Fidelity Path (Completed)

### Goal

Ensure static `.gltf/.glb` rendering preserves authored visual fidelity so loaded models do not appear materially/tessellation-degraded versus expected output.

### Scope

- Preserve imported surface shading inputs for static glTF path:
  - consume authored vertex normals where available (avoid forced flat-shaded recompute path)
  - keep deterministic fallback behavior when normals are missing
- Preserve core material appearance inputs for static fallback path:
  - neutral/default color policy that does not introduce global tint bias
  - base color and alpha ingestion where available in glTF
  - base color texture hookup for static loaded content where available
- Define and test behavior for animated-asset static fallback:
  - when animated glTF load succeeds but playback contract fails (e.g. no clips), static fallback render should still retain fidelity behavior
- Add regression coverage for:
  - static glTF normals/material fidelity expectations
  - fallback behavior consistency between startup env path and default-model path
  - `MASK` alpha-cutout semantics alignment (cutoff-driven discard + opaque surviving fragments)
  - safe texture URI resolution behavior (reject absolute and path-traversal image URIs)

### Implemented (2026-03-04)

- Preserved authored static normals end-to-end for static `.gltf/.glb`:
  - static triangle payload now carries per-vertex normals
  - static upload path consumes authored normals when present and falls back deterministically to face-normal recompute when missing
- Preserved static core material fidelity inputs in fallback path:
  - neutral default base color policy updated to remove tint bias
  - glTF material base color/alpha ingestion wired through static loader -> runtime material
  - glTF `MASK` alpha cutoff ingestion and shader-path handling added
- Hardened `MASK` rendering semantics:
  - alpha test uses authored cutoff
  - surviving cutout fragments are treated as opaque for output alpha
  - render-state selection keeps cutout materials on opaque/depth-write path (instead of blend-state fallback on `alpha < 1`)
- Strengthened animated-load fallback-to-static behavior:
  - when animated glTF load succeeds but playback setup fails (e.g. no clips), static fallback now retains Phase 4 fidelity behavior
- Added static-loader URI hardening:
  - rejects absolute image URI paths
  - rejects parent-traversal image URI paths escaping asset directory
- Added observability:
  - static-load material summary logs in client startup path
  - warnings for cutout materials missing resolvable texture sources
  - warning for current single-material static fallback collapse on multi-material assets (explicitly deferred to Phase 4.1)
- Added/expanded tests:
  - `engine/src/render/mesh_asset_loader_test.cpp` (normals/material ingestion, `MASK` cutoff ingestion, URI hardening, multi-material collapse contract)
  - `client/src/client_app_animation_test.cpp` (animated->static fallback fidelity + startup-path parity)
  - `engine/src/render/shader_contract_test.cpp` (alpha-cutout contract)
  - `engine/src/render/render_world_test.cpp` (neutral default material baseline)

### Known Limits (Phase 4, Post-Implementation)

- Static fallback multi-primitive/multi-material collapse limit is resolved in Phase 4.1.
- Static texture hookup currently depends on standards-compliant glTF `images/textures` references; converter outputs omitting those fields cannot be recovered runtime-side.

### Exit Criteria

- Static `.gltf/.glb` fallback no longer exhibits systemic yellow-tint/forced-faceted appearance for assets that include authored normals/material base inputs.
- Animated-load fallback-to-static path produces the same visual-fidelity baseline as direct static load.
- Satisfied as of 2026-03-04 for the current single-material static fallback baseline.

## Phase 4.1: Static glTF Per-Primitive Material Preservation

### Goal

Preserve authored material partitioning for static `.gltf/.glb` content so multi-material meshes render with per-primitive material intent instead of a single merged fallback material baseline.

### Dependencies

- Builds directly on completed Phase 4 static-fidelity foundations (authored normals, neutral color baseline, base color/alpha/texture ingestion, `MASK` semantics, URI hardening, and fallback-path consistency).
- Should be completed before Phase 4.5 Windows composition stabilization so transparent/cutout validation during overlay work uses material-faithful static fallback behavior.

### Scope

- Preserve static glTF primitive boundaries through runtime static fallback population:
  - avoid collapsing all static triangle primitives into one material assignment
  - keep one renderable static mesh/object binding per source primitive (or equivalent deterministic grouping that preserves material mapping)
- Preserve per-primitive material mapping inputs:
  - base color / alpha / alpha cutoff (`MASK`) semantics per primitive
  - cull mode (`doubleSided`) and blend mode behavior per primitive
  - base color texture path binding per primitive where available
- Define deterministic static model transform policy for multi-primitive fallback:
  - compute aggregate visibility fit bounds once
  - apply a consistent model-space transform to all primitive-backed render objects so assembled model alignment is preserved
- Add regression coverage for:
  - multi-primitive/multi-material static glTF load producing distinct material-preserving runtime objects
  - deterministic primitive-to-material mapping behavior across repeated loads
  - parity of per-primitive material preservation between direct static load and animated-load fallback-to-static path

### Implemented (2026-03-04)

- Preserved static glTF primitive boundaries and per-primitive materials through static fallback:
  - loader now emits primitive-scoped static chunks (`triangles + material`) for glTF inputs
  - client static fallback population now instantiates one mesh/object/material binding per primitive chunk
- Preserved per-primitive material fidelity in runtime static fallback:
  - base color/alpha/alpha-cutoff (`MASK`) semantics map per primitive
  - per-primitive cull/blend/texture binding paths are retained
- Added deterministic aggregate transform policy without triangle-buffer rebuild:
  - computes one aggregate visibility-fit transform across primitive meshes from existing mesh bounds
  - applies the same transform to all primitive-backed fallback render objects
- Hardened static loader URI handling while implementing Phase 4.1:
  - explicit rejection of Windows absolute-style image URIs (`C:/...`, UNC prefixes)
  - explicit rejection of parent traversal for parentless relative asset paths (`model.gltf` + `../...`)
- Refined render-state contract coverage:
  - moved per-material render-path decisions to a pure helper and unit-tested helper inputs/outputs (instead of brittle source-text checks)
- Added/updated regression tests:
  - loader coverage for per-primitive mapping, deterministic repeat-load mapping, default-vs-explicit material behavior, and URI hardening edge cases
  - client coverage for direct static load and animated->static fallback parity, mixed primitive robustness, deterministic repeated-load transforms, and per-object material mapping

### Known Limits (Phase 4.1, Post-Implementation)

- `MeshAssetLoadResult` currently retains legacy flattened `triangles` alongside per-primitive data for compatibility, which duplicates static triangle storage; this is explicitly tracked for later cleanup/deprecation of flattened legacy access.
- Static texture hookup still depends on standards-compliant glTF `images/textures` references.

### Exit Criteria

- Static multi-material `.gltf/.glb` fallback no longer degrades to a single-material baseline when source primitives encode distinct materials.
- Primitive-level alpha/cutout behavior (including hair-card style `MASK` materials) remains mapped per primitive in runtime static fallback.
- Satisfied as of 2026-03-04.

## Phase 4.5: Windows Transparent Overlay + 3D Composition Reliability

### Goal

Provide a stable Windows composition path where the desktop remains transparent while 3D content remains visible and correctly blended.

### Scope

- Replace fragile layered-window/color-key permutations with a dedicated alpha-composited presentation path on Windows:
  - DirectComposition-backed window composition
  - premultiplied-alpha swapchain/presentation path compatible with runtime renderer output
- Define a single authoritative Windows transparency mode for runtime:
  - transparent clear background
  - opaque model pixels
  - deterministic behavior across resize/reconfigure events
- Remove/retire ambiguous fallback combinations that produced inconsistent outcomes in current testing:
  - color key honored on some paths but not others
  - transparent-backbuffer path causing black/no-model presentation on current setup
- Add platform-focused diagnostics and tests:
  - compositor/swapchain init/result logging
  - regression test coverage for overlay contract and window reconfigure behavior
- Keep non-Windows behavior unchanged.

### Incident Summary (Resolved 2026-03-04)

- Runtime rendered the model reliably with non-transparent presentation (`reset_flags=0`) but the background remained black.
- Enabling transparent backbuffer caused black/no-model presentation despite successful draw submission and successful DWM API calls.
- This confirmed a Windows presentation/compositing integration gap, not a mesh/animation loading issue.

### Debug Timeline (Key Findings, 2026-03-03)

What was tried (and why), with observed outcomes:

- Layered + color-key path (`WS_EX_LAYERED` + `SetLayeredWindowAttributes(..., LWA_COLORKEY)`):
  - Goal: treat clear color as transparent while preserving rendered model pixels.
  - Outcome: model visible, but keyed background behavior was inconsistent and often remained opaque.
- Color-key debug switch to magenta:
  - Goal: verify whether Windows color-key transparency was actually being honored on the current path.
  - Outcome: full magenta background appeared with model visible, confirming key color was showing as raw clear color (not being composited out reliably in this runtime path).
- Transparent backbuffer path (`BGFX_RESET_TRANSPARENT_BACKBUFFER`) with alpha-clear `(0,0,0,0)`:
  - Goal: use per-pixel alpha composition instead of color-key behavior.
  - Outcome: black screen and/or missing model, despite renderer submitting draws.
- DWM overlay path (`DwmIsCompositionEnabled`, `DwmEnableBlurBehindWindow`, `DwmExtendFrameIntoClientArea`) with explicit HRESULT logging:
  - Goal: confirm compositor APIs were not failing.
  - Outcome: APIs consistently returned success (`hr=0x0`), but black/no-model behavior persisted on transparent-backbuffer attempts.
- Hybrid combinations (layered alpha + DWM + transparent backbuffer, SDL transparent flag toggles, color-key vs alpha variants):
  - Goal: find a compatible combination without major platform-path rewrite.
  - Outcome: no stable combination achieved transparent desktop + visible model simultaneously on current setup.
- Shader/visibility troubleshooting:
  - Forced mesh fragment alpha to `1.0` to ensure mesh pixels were not accidentally transparent.
  - Restored static mesh auto-fit transform so model stays in camera view.
  - Outcome: did not resolve black/no-model behavior under transparent-backbuffer mode.

Key evidence collected:

- Mesh/runtime loading is successful:
  - static fallback loads `models/model.glb`
  - renderer logs show `submitted_draws=1, world_objects=1, world_meshes=1`
- DWM calls succeed:
  - `dwm_composition_hr=0x0`, `blur_hr=0x0`, `extend_hr=0x0`
- Behavior splits by presentation path:
  - non-transparent bgfx reset (`reset_flags=0`): model visible, black background
  - transparent bgfx reset (`reset_flags=1048576`): black/no-model on current setup

What this rules out:

- Not primarily an asset conversion/load failure (model and shaders load; draws are submitted).
- Not primarily a single Win32 API call failure (DWM API success is verified).
- Not primarily mesh visibility from transform/alpha alone (forced-opaque fragment alpha + auto-fit still fail under transparent-backbuffer path).

Conclusion from diagnostics:

- The black-screen issue was a Windows presentation/compositing integration problem for this renderer path.
- A dedicated alpha-composited presentation implementation (DirectComposition + premultiplied-alpha swapchain path) is required for a reliable transparent-desktop + visible-3D result.

### Final Implementation (2026-03-04)

- Added a DirectComposition-backed presentation path for Windows runtime rendering:
  - D3D11 device/context creation for composition interop
  - premultiplied-alpha `IDXGISwapChain1` (`CreateSwapChainForComposition`)
  - DirectComposition target/visual attachment to the SDL/Win32 host window
- Switched runtime renderer initialization to an authoritative external-backbuffer path:
  - bgfx initializes against externally supplied D3D11 context + RTV/DSV
  - bgfx HWND swapchain ownership is disabled (`platformData.nwh = nullptr`) to avoid mixed presentation ownership
  - transparent backbuffer reset contract remains explicit (`BGFX_RESET_TRANSPARENT_BACKBUFFER` + clear alpha `0`)
- Finalized Windows overlay/window style policy for this composition path:
  - authoritative non-layered DirectComposition-compatible style path
  - compatibility fallback retained only for style-application failure cases
  - removed reliance on legacy DWM blur/frame-extension APIs for the composition path
- Finalized resize/refresh behavior to avoid no-op success masking:
  - `refresh_win32_alpha_composited_overlay(...)` now performs real re-application work for layered fallback mode
  - failure in layered fallback refresh path is surfaced instead of silently succeeding
- Finalized Windows toolchain compatibility for composition link dependencies:
  - GNU/MinGW lanes use `-ld3d11 -ldxgi -ldcomp -lole32`
  - MSVC lanes use `d3d11.lib dxgi.lib dcomp.lib ole32.lib`
- Retained/hardened platform diagnostics from bring-up:
  - compositor/style-selection logs
  - swapchain/init failure logs
  - draw-submission visibility logs for render/composition split debugging
- Added composition guardrail tests (temporary source-contract style):
  - transparent clear contract coverage (`overlay_transparency_contract_test`)
  - Windows composition source-contract coverage (`windows_composition_contract_test`) for swapchain alpha mode, external backbuffer ownership, style policy, and layered fallback refresh behavior
- Verified final observed runtime outcome on Windows:
  - transparent desktop/background with visible model in-session
  - stable runtime loop through repeated frames and clean shutdown event path

### Outcome / Exit Criteria

- On Windows, runtime shows desktop/background transparency and visible 3D model simultaneously in the same session, with stable behavior across startup and resize.
- Transparency behavior no longer depends on color-key hacks or mutually inconsistent overlay flag combinations.
- Status: satisfied as of 2026-03-04.
- Remaining follow-up:
  - current composition contract tests still include source-text guardrails; migrate to behavior-level unit seams/fakes in cleanup (tracked in Phase 10).

## Phase 5: Basic Physics Preservation from PMX Conversion

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
- Integrate against established Phase 2/3/3.5 playback + skeleton runtime state (avoid introducing a parallel animation/deformation state path).
- Preserve Phase 4.5 Windows presentation baseline while adding physics debug overlays/proxies (no reintroduction of layered/color-key-only transparency behavior).

### Implemented (2026-03-04)

- Added runtime Phase 5 sidecar ingestion module:
  - `engine/src/render/include/pmx_physics_sidecar.hpp`
  - `engine/src/render/pmx_physics_sidecar.cpp`
- Added runtime/client integration for sidecar-backed collider proxy population and per-tick updates:
  - `client/src/client_app.hpp` / `client/src/client_app.cpp`
  - loads sibling `.<asset>.physics.json` at animated-asset load time when present
  - maps sidecar collider `bone_name` to skeleton joints and creates parented proxy meshes
  - updates proxy transforms from authoritative animated joint pose state each tick
  - uses in-place triangle edits without per-frame bounds recompute, with periodic deferred bounds refresh aligned to existing animation tick cadence
- Added diagnostics/observability for Phase 5 runtime behavior:
  - sidecar load success/failure summaries
  - parse/validation warning surfacing
  - collider proxy creation/skip summaries
  - periodic proxy tick update summaries + invalid-binding throttled warnings
- Added parser hardening guardrails:
  - migrated sidecar parsing to `nlohmann::json` (replacing custom parser)
  - explicit sidecar file-size cap guard (`kMaxSidecarFileSizeBytes = 10MB`) with both metadata precheck (`file_size`) and bounded stream-read enforcement
  - explicit array-count caps (`collision_layers`, `colliders`, `constraints`)
  - explicit required-string length cap (`kMaxStringLengthBytes`)
  - specific top-level failure-reason propagation (distinguishes missing/invalid arrays vs exceeds-max failures)
  - improved warning diagnostics for unsupported constraint/collider types and unknown collider bone names (offending values included)
- Aligned physics contract/schema/validator/runtime semantics:
  - collider `layer` constrained to collision-layer index `[0,31]`
  - collider `mask` preserved as uint32 bitmask
  - updated:
    - `docs/pmx/schemas/pmx_physics_metadata.schema.json`
    - `docs/pmx/pmx_to_gltf_conversion_contract.md`
    - `tools/pmx/validate_converted_gltf.py` + fixtures/tests
- Added/updated regression tests:
  - `engine/src/render/pmx_physics_sidecar_test.cpp` (schema/version/shape/range/size/array-count/string-length/error handling + specific-failure-reason coverage)
  - `client/src/client_app_animation_test.cpp` (startup sidecar integration, missing/invalid sidecar resilience, proxy follow behavior, tick-storage stability, GPU-authoritative coexistence)
- Kept Phase 4.5 compositor behavior unchanged while adding physics proxies.

### Known Limits (Phase 5, Post-Implementation)

- Current `Capsule` visual proxy shape is intentionally approximated with a scaled box for lightweight debug rendering.
- Follow-up is tracked inline in `client/src/client_app.cpp` as a TODO to replace this with a low-poly capsule mesh (cylinder + hemispherical caps) when proxy-shape fidelity refinement is scheduled.

### Exit Criteria

- Imported character gets basic collider/physics proxies aligned with skeleton.
- Status: satisfied as of 2026-03-04 for baseline Phase 5 scope.

## Phase 6: PMX Motion Pipeline (VMD to glTF Clip Workflow)

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
- Validate converted clips against the Phase 3/3.5 GPU skinning runtime path (not CPU fallback-only behavior).
- Validate representative clip playback on the Phase 4.5 Windows composition path so animation bring-up does not regress transparent overlay behavior.

### Implemented (2026-03-05)

- Added Phase 6 motion validation tooling:
  - `tools/pmx/validate_motion_clips.py`
  - `tools/pmx/validate_motion_clips_test.py`
  - `tools/pmx/validate_motion_clips_smoke_test.py`
  - `tools/pmx/testdata/motion/*`
  - Bazel targets:
    - `//tools/pmx:validate_motion_clips_test`
    - `//tools/pmx:validate_motion_clips_smoke_test`
- Added motion metadata schema + example sidecar:
  - `docs/pmx/schemas/pmx_motion_metadata.schema.json`
  - `docs/pmx/examples/sample.motion.json`
- Updated conversion contract and tool docs with Phase 6 motion workflow/requirements:
  - `docs/pmx/pmx_to_gltf_conversion_contract.md`
  - `tools/pmx/README.md`
- Hardened Phase 6 validator semantics for real converter/package workflows:
  - sidecar schema_version type-first validation (clear non-string diagnostics)
  - actionable animation/sampler context in key-count mismatch errors
  - sidecar-aware effective root-motion policy precedence:
    - per-clip `root_motion_mode` override
    - sidecar-level `root_motion_policy`
    - CLI `--root-motion-policy` fallback
  - policy-aware root-motion diagnostics include effective policy + policy source (`clip_override`, `sidecar_default`, `cli_default`)
  - contract-aligned default sidecar discovery for `<character>.motion.gltf/.glb` -> `<character>.motion.json`
    with legacy compatibility fallback
  - expanded CLI observability logs:
    - explicit vs auto-discovered sidecar selection
    - validation summary (`root_joint`, `cli_policy`, clip inventory)
- Added CI wiring for PMX tooling gates:
  - `.github/workflows/ci.yml` runs
    - `//tools/pmx:validate_converted_gltf_test`
    - `//tools/pmx:validate_motion_clips_test`
    - `//tools/pmx:validate_motion_clips_smoke_test`

### Exit Criteria

- PMX model + converted motion clips play reliably in runtime through the same clip system.
- Status: satisfied for Phase 6 contract/tooling scope as of 2026-03-05.

## Phase 7: Tooling, Validation, and CI

### Goal

Make the pipeline maintainable and testable.

### Scope

- `[done]` Expand the Phase 1 validator/checklist into broader CI/runtime coverage (do not replace it).
- `[partial]` Add model intake orchestration for app/runtime workflows:
  - `[done]` define a default `models/` directory scan/input policy
  - `[done]` accept both `.pmx` and `.gltf/.glb` files in that intake directory
  - `[done]` if input is `.pmx`, run conversion automatically at app launch (or first-use) to produce runtime glTF/GLB output
  - `[done]` if input is `.gltf/.glb`, load directly without conversion
  - `[done]` cache converted outputs and avoid unnecessary reconversion when source + converter version are unchanged
  - `[done]` produce clear logs/errors for conversion failures and fallback/skip behavior
  - `[done]` define deterministic model selection policy when multiple candidates exist
  - `[pending]` remove external converter dependency (Phase 10 readiness/sign-off scope)
- `[done]` Add automated tests for:
  - `[done]` loader failures (missing skin/joints/weights)
  - `[done]` static-loader texture URI hardening behavior (absolute/traversal image URI rejection, including parentless-relative-path traversal and Windows absolute-style URI rejection)
  - `[done]` `MASK` cutout shader/render-state contract behavior
  - `[done]` static multi-primitive material-preservation behavior (Phase 4.1)
  - `[done]` deterministic aggregate static-fallback transform behavior across repeated loads
  - `[done]` material render-path helper contract behavior (blend/alpha/cutout/cull decision invariants)
  - `[done]` Windows composition policy contract behavior (DirectComposition swapchain alpha mode, external bgfx backbuffer ownership policy, overlay style fallback/refresh behavior)
  - `[done]` pose eval determinism
  - `[done]` interpolation mode handling (`LINEAR`/`STEP` + rejection paths)
  - `[done]` playback mode behavior (`Loop`/`Clamp`)
  - `[done]` shader contract for skinning path
  - `[done]` GPU skinning guard/fallback behavior and large-skeleton partition/remap correctness around palette/index limits
  - `[done]` physics metadata parsing fallbacks + parser/resource hardening limits (file-size cap, array-count caps, string-length caps, layer-index bounds)
  - `[done]` model intake orchestration (`models/` scan, PMX auto-convert trigger, converted-output cache hit path)
- `[done]` Add CI target(s) for animation/physics pipeline tests.
- `[done]` Add CI wiring for PMX conversion/motion validator tests:
  - `[done]` `//tools/pmx:validate_converted_gltf_test`
  - `[done]` `//tools/pmx:validate_motion_clips_test`
  - `[done]` `//tools/pmx:validate_motion_clips_smoke_test`
- `[done]` Extend CI/smoke wiring from current baseline that already includes:
  - `[done]` `//engine/src/render:animation_playback_controller_tests`
  - `[done]` `//engine/src/render:render_world_tests`
  - `[done]` `//client/src:animated_mesh_skinning_test`
  - `[done]` `//client/src:client_app_animation_test`
  - `[done]` `//engine/src/render:bgfx_mesh_manager_tests`
  - `[done]` `//engine/src/render:model_renderer_skinning_utils_tests`
  - `[done]` `//engine/src/render:shader_binary_runtime_tests`
  - `[done]` `//client/src:model_intake_test`

### Implemented (2026-03-05, In Progress)

- Added runtime `models/` intake orchestration:
  - deterministic discovery/selection policy (`model.glb`/`model.gltf`/`model.pmx` preference,
    then stable extension+filename ordering)
  - direct `.gltf/.glb` startup load path
  - `.pmx` auto-convert trigger into `models/.isla_converted/<stem>.auto.glb`
  - conversion cache metadata (`<stem>.auto.cache`) keyed by source size/mtime + converter
    command/version
- Added runtime startup diagnostics for resolved intake selection and conversion/cache outcomes.
- Added security hardening for converter invocation:
  - removed shell-based command execution (`std::system`)
  - shell-less process execution via argument vectors (`_spawnvp` on Windows, `fork+execvp` on POSIX)
  - regression coverage for dangerous filenames and shell-metacharacter command-template inputs
- Added converter-template robustness hardening:
  - Windows backslash path preservation in template tokenization
  - incomplete token template diagnostics with positional fallback for missing
    `{input}`/`{output}`
- Added/updated tests and CI wiring:
  - `//client/src:model_intake_test` (selection/auto-convert/cache/failure/security parser behavior)
  - Windows smoke includes `//client/src:model_intake_test`

### Known Limits (Phase 7, Current)

- PMX conversion remains orchestration-driven and depends on an external converter executable/toolchain
  (default command template targets `pmx2gltf`).
- Runtime currently does not ship a native PMX parser/exporter; missing converter binaries still
  produce conversion failure diagnostics and fallback/skip behavior.

### Exit Criteria

- Pipeline regressions are detected automatically in CI.
- App/tooling flow supports `models/` intake where `.pmx` inputs are auto-converted to runtime glTF/GLB and then loaded for display, while `.gltf/.glb` inputs load directly.
- Status: in progress as of 2026-03-05; model-intake + conversion orchestration + security-hardening
  sub-scope is implemented.

## Phase 7.5: Runtime Material Introspection + Texture Remap Overrides

### Goal

Enable deterministic texture assignment for converted/static runtime assets when source glTF/GLB texture references are missing or incomplete, without relying on ad-hoc trial-and-error primitive index guessing.

### Scope

- Expose runtime-inspectable primitive/material identity metadata for static glTF/GLB ingestion:
  - source mesh index
  - source primitive index
  - source material name (when present in asset)
  - resolved runtime material slot/index
- Add startup/runtime observability for texture targeting:
  - structured inventory logs for primitive/material slots
  - explicit texture-path presence/absence reporting per slot
- Add an optional deterministic runtime texture-remap sidecar/config path:
  - preferred keying by stable source material name
  - fallback keying by explicit source mesh/primitive index tuple
  - explicit precedence policy between embedded glTF texture refs and override mappings
  - clear diagnostics for unmapped keys, duplicate keys, and missing texture files
- Reuse existing static-loader URI hardening constraints for override texture paths (no absolute/traversal escapes outside approved asset/package boundaries).
- Keep the initial runtime-remap scope to static fallback/direct static load path; do not block Phase 8+ animation internals on first rollout.

### Proposed Sidecar Contract (Draft)

Suggested sibling file naming:

- `<asset_stem>.texturemap.json` (example: `model.texturemap.json`)

Draft payload:

```json
{
  "schema_version": "1.0.0",
  "policy": {
    "override_mode": "if_missing",
    "path_scope": "asset_relative_only"
  },
  "mappings": [
    {
      "id": "head_by_name",
      "target": {
        "material_name": "Head"
      },
      "albedo_texture": "textures/head.png"
    },
    {
      "id": "hair_fallback_by_index",
      "target": {
        "mesh_index": 0,
        "primitive_index": 12
      },
      "albedo_texture": "textures/hair_basecoloralpha.png",
      "alpha_cutoff": 0.5
    }
  ]
}
```

Target resolution rules (draft):

- Resolve `target.material_name` first when present.
- If no `material_name` match is found, allow explicit `mesh_index + primitive_index` lookup.
- Reject ambiguous matches (more than one primitive hit for a single mapping key) unless explicitly allowed by future policy extension.

Override policy semantics (draft):

- `override_mode: "if_missing"`:
  - apply sidecar texture only when source material has no resolvable albedo texture path.
- `override_mode: "always"`:
  - sidecar texture replaces source albedo texture path unconditionally.

Validation and diagnostics expectations:

- Hard-fail sidecar load on invalid schema version or malformed `mappings` structure.
- Per-entry warning+skip for:
  - unknown target key
  - missing texture file
  - rejected texture path (absolute/traversal escape)
  - duplicate mapping key collisions
- Emit startup inventory log with:
  - source mesh/primitive index
  - source material name
  - resolved texture source (`gltf`, `texturemap`, or `none`)

### Exit Criteria

- Users can list primitive/material targets deterministically from runtime logs and map external texture files without index trial-and-error.
- Static runtime path can apply named/indexed texture overrides deterministically with clear diagnostics and test coverage.

## Phase 8: Full Node-Hierarchy Animation Support

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
- Keep compatibility with Phase 4.5 Windows composition presentation contracts while hierarchy evaluation internals evolve.
- Keep compatibility with Phase 7 startup model-intake orchestration and converter-security behavior
  while hierarchy evaluation internals evolve.
- Update Phase 6 motion contract/validator caveats after landing:
  - revisit any conversion-side non-joint bake/avoid guidance
  - adjust validator expectations if non-joint animation channels become runtime-supported.

### Exit Criteria

- Assets with animated non-joint hierarchy segments evaluate correctly without conversion-side workarounds.

## Phase 9: Interpolation + Sampling Completeness

### Goal

Close remaining runtime animation fidelity/performance gaps.

### Scope

- Add `CUBICSPLINE` interpolation support (including tangent semantics).
- Add cached key index walking for monotonic playback (`amortized O(1)` sampling).
- Expand regression tests for cubic edge cases and seek behavior.
- Update Phase 1 contract + validator rules when `CUBICSPLINE` transitions from unsupported to supported.
- Maintain Phase 2 state/pose consistency guarantees (reported local time aligns with sampled pose semantics).
- Maintain Phase 3/3.5 deformation consistency guarantees (GPU-skinned output remains aligned with evaluated pose semantics).
- Maintain Phase 4/4.1 static-fidelity guarantees while sampling internals evolve (including unchanged `MASK` cutout semantics and per-primitive static fallback material behavior).
- Maintain Phase 4.5 compositor-facing alpha/presentation guarantees while interpolation/sampling internals evolve.
- Maintain Phase 7 intake/launch guarantees and converter-orchestration contracts while sampling
  internals evolve.
- Update Phase 6 motion validator contract checks when interpolation support expands:
  - once `CUBICSPLINE` is runtime-supported, remove/replace current hard-fail behavior in motion validation.

### Exit Criteria

- Runtime matches expected glTF animation interpolation semantics for all targeted modes with stable performance.

## Phase 10: PMX Pipeline Readiness and Sign-Off

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
  - Phase 3.5 completed large-skeleton GPU skinning extension beyond fixed palette budget
- Publish final end-user intake workflow docs:
  - where to place models (`models/`)
  - supported input extensions (`.pmx`, `.gltf`, `.glb`)
  - PMX auto-convert behavior, cache location/invalidation policy, and troubleshooting steps
- Execute final test-infrastructure cleanup for Windows composition regression coverage:
  - treat current source-contract (`StrContains`-style) tests as temporary guardrails
  - refactor composition/style invariants into testable policy/helper functions and assert behavior directly in unit tests
  - prefer call-level verification via thin platform abstractions/fakes where practical (style application, swapchain descriptor policy, bgfx platform-data policy)
  - add/retain at least one runtime-smoke-level composition path assertion where CI/platform constraints allow
  - remove or minimize text-fragile source assertions once behavioral tests cover the same invariants

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
4. First large-rig-ready deformation point (>64-joint referenced primitives): after Phase 3.5 (achieved 2026-03-03).
5. First static glTF visual-fidelity parity point (non-animated fallback path): after Phase 4 (achieved 2026-03-04).
6. First static multi-material parity point (primitive-level material preservation in fallback path): after Phase 4.1 (achieved 2026-03-04).
7. First stable transparent-overlay + visible-3D Windows composition point: after Phase 4.5 (achieved 2026-03-04).
8. First basic PMX parity point (animation + basic physics): after Phase 5/6.
9. First integrated model-intake UX point (`models/` directory + PMX auto-convert + load/display): after Phase 7 (in progress; orchestration path landed 2026-03-05, external converter dependency remains).
10. First deterministic runtime material/texture targeting point (primitive/material introspection + explicit texture remap overrides): after Phase 7.5.
11. First hierarchy-fidelity point for complex rigs: after Phase 8.
12. First interpolation-complete point: after Phase 9.
