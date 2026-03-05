# Native PMX Runtime Phased Plan

## Context

`isla` currently has:

- Static mesh loading for `.obj/.gltf/.glb`
- A bgfx-based renderer path
- Animated glTF runtime foundation (skin/joint/clip loading and pose evaluation)
- GPU skinning support with deterministic large-skeleton partition/remap
- PMX model intake orchestration that still depends on external PMX-to-glTF conversion

Target outcome:

- Load `.pmx` assets directly at runtime without conversion to `.gltf/.glb`
- Render native PMX characters with usable skinned deformation in `isla`
- Resolve PMX textures/material partitions into current runtime material semantics
- Support startup intake of native `.pmx` assets from existing environment/model-directory workflows
- Add regression coverage and CI guardrails comparable to current glTF runtime paths

Non-goals for this plan:

- Full MMD/VMD motion parity
- PMX physics parity
- Exact MMD toon/sphere/edge rendering parity
- Full morph authoring/editor workflows

## Recommendation

Implement native PMX support as a parallel runtime path backed by `saba`, not by replacing the
existing glTF runtime and not by attempting to build a custom PMX/MMD runtime from scratch.

Rationale:

- `saba` already covers the hard PMX/MMD-specific parsing/runtime semantics that are expensive to
  reproduce correctly.
- `isla` already has a working renderer, `RenderWorld` abstraction, and skinning/material upload
  path that can be reused.
- A parallel PMX path keeps current glTF support stable while native PMX support is introduced
  incrementally.

Operational interpretation:

- glTF/GLB remains a supported runtime input path.
- Native PMX support is added alongside glTF, not as a transitional conversion step.
- `saba` is used as the PMX/MMD backend and adapter source, while `isla` remains the renderer and
  application runtime owner.

> [!NOTE]
> **Current status (2026-03-05):**
> - Native PMX startup selection does not exist yet; `.pmx` startup handling still routes through
>   `pmx2gltf` conversion orchestration.
> - Animated runtime loading is glTF-specific.
> - Static mesh loading is `.obj/.gltf/.glb`-specific.
> - PMX-sidecar work completed so far assumes converted glTF runtime assets, not direct PMX runtime
>   loading.
> - This document is an alternative forward plan for native PMX runtime support scoped only to
>   direct PMX loading, skinned rendering, texture/material mapping, startup integration, and
>   regression hardening.

### Changelog

- 2026-03-05: added initial native PMX runtime alternative plan scoped to direct PMX loading via
  `saba`, skinned rendering integration, PMX material/texture mapping baseline, startup intake, and
  regression coverage.

## Phase 0: Saba Integration + Native PMX Runtime Boundary

### Goal

Introduce `saba` cleanly into the repo and define the native PMX runtime boundary without
destabilizing the existing glTF path.

### Scope

- Vendor or otherwise integrate `saba` into the build with an explicit version pin.
- Decide the integration shape:
  - preferred: `saba` as a third-party PMX/MMD backend library consumed by new `isla` adapter code
  - avoid: folding `saba` rendering/viewer code into `isla`
- Add build wiring for required `saba` dependencies and isolate them from unrelated runtime modules.
- Define a new PMX-native runtime module boundary in `engine/src/render`, parallel to current
  glTF-specific loaders, instead of widening `animated_gltf` into a mixed-format loader.
- Define the first native PMX support matrix:
  - direct PMX mesh/skeleton/material load
  - bind-pose display
  - usable skinned deformation path for supported PMX skinning cases
  - explicit fallback/logging behavior for unsupported PMX features
- Define coordinate-system, naming, and path-normalization policy for PMX runtime ingestion.
- Reuse existing `RenderWorld`, material upload, and skin-palette plumbing where possible.
- Preserve current glTF paths unchanged while PMX-native modules are brought up.

### Exit Criteria

- Repo builds with `saba` integrated and isolated behind new PMX runtime modules.
- A clear PMX-native loader/runtime boundary exists without broad glTF module churn.
- The first supported/unsupported PMX feature matrix is documented in code/comments/tests.

## Phase 1: Native PMX Asset Loading + Skeleton Bridge

### Goal

Load a PMX asset directly into `isla` runtime structures with correct mesh partitions, skeleton
topology, and stable diagnostics.

### Scope

- Add native PMX loader module(s), for example:
  - `engine/src/render/include/pmx_asset_loader.hpp`
  - `engine/src/render/pmx_asset_loader.cpp`
  - supporting PMX asset/runtime type headers as needed
- Use `saba` to parse PMX data and extract:
  - vertices
  - index buffers
  - material/submesh boundaries
  - bone hierarchy
  - bind-pose or initial local/global transforms
  - vertex skinning influences
- Normalize PMX names/strings into stable UTF-8 runtime strings for bones/materials/textures.
- Normalize PMX-relative texture paths against the PMX asset directory with the same hardening
  expectations already applied to runtime asset-relative texture loading.
- Establish deterministic PMX load-time validation and diagnostics for:
  - malformed PMX data
  - missing textures
  - unsupported feature encounters
  - invalid bone/material references
- Preserve source identity metadata needed for later startup/material diagnostics.
- Keep PMX loading separate from startup/world-population code so the loader remains unit-testable.

### Exit Criteria

- A native `.pmx` file can be parsed directly into stable runtime asset structures.
- Skeleton and submesh/material partition data are available to downstream runtime population code.
- Failure behavior is explicit and diagnosable rather than silent or converter-dependent.

## Phase 2: PMX Deformation + Skinned Rendering Path

### Goal

Make native PMX characters render as deforming skinned assets through `isla`'s runtime mesh path.

### Scope

- Bridge native PMX geometry/skeleton data into current `RenderWorld` skinned mesh structures.
- Reuse existing `MeshData::set_skinned_geometry(...)` and `set_skin_palette(...)` flow where
  compatible with PMX skinning data.
- Define the initial PMX deformation authority policy:
  - use the existing GPU skinning path for PMX skinning representations that fit current runtime
    assumptions
  - provide a deterministic CPU deformation fallback for PMX cases that cannot be represented
    safely in the current shader-side skinning contract
- Reuse current large-skeleton partition/remap helpers where possible instead of introducing a
  separate PMX-only draw partitioning strategy.
- Add client/runtime population path(s) for PMX skinned meshes without routing through
  `animated_gltf`.
- Ensure bind-pose display works first before adding any richer PMX runtime pose/deform update
  behavior.
- Add runtime summaries/logging for:
  - skinned primitive counts
  - partition counts
  - fallback-to-CPU-deform decisions
  - unsupported PMX deformation features encountered
- Keep this phase scoped to usable PMX model deformation/display, not VMD playback or PMX physics.

### Exit Criteria

- A native PMX character renders in `isla` with usable skinned deformation behavior.
- Existing renderer-side static/skinned program selection remains stable.
- Unsupported PMX deformation semantics fail clearly or use explicit fallback behavior.

## Phase 3: PMX Material + Texture Mapping Baseline

### Goal

Map PMX material and texture intent into `isla`'s current runtime material model with stable,
predictable results.

### Scope

- Convert PMX material partitions into per-submesh runtime material slots.
- Map the baseline PMX material properties that fit current runtime semantics:
  - base/diffuse color
  - alpha/opacity
  - cull policy
  - primary albedo texture path
- Resolve PMX texture references directly from the PMX package without converter-generated sidecars.
- Preserve multi-material PMX partition boundaries through runtime world population.
- Define baseline handling for PMX material features not yet represented in the current renderer:
  - toon textures
  - sphere textures
  - edge rendering parameters
  - material morph-driven overrides
- Prefer explicit logging/omission over ad hoc approximations for unsupported channels.
- Reuse existing material diagnostics patterns so PMX and glTF startup logs remain comparable.
- Evaluate whether the existing texture-remap override path should be generalized for PMX-native
  assets, but keep direct PMX texture resolution as the default baseline.

### Exit Criteria

- Native PMX assets render with stable material partitioning and primary texture hookup.
- Missing/unsupported PMX material channels are surfaced clearly in logs/tests.
- Multi-material PMX assets no longer require glTF conversion to preserve basic material intent.

## Phase 4: Startup Intake + Native PMX Asset Selection

### Goal

Allow users to place a `.pmx` file in the existing startup/intake flow and have it load directly
without conversion.

### Scope

- Extend model intake/startup loading to distinguish:
  - direct native PMX runtime path
  - existing direct glTF/GLB runtime path
  - optional legacy conversion path, if retained temporarily for fallback or comparison
- Update `models/` directory selection policy so native `.pmx` loading is an explicit first-class
  runtime choice instead of always triggering conversion.
- Update startup loader wiring so PMX-native assets bypass glTF-only loader entry points.
- Preserve current environment-variable startup flows while adding a PMX-native path.
- Add clear startup diagnostics for:
  - direct PMX selection
  - PMX-native load success/failure
  - fallback behavior when PMX-native loading is unavailable or disabled
- Keep glTF startup behavior stable and deterministic while adding PMX-native selection logic.
- Remove converter dependence from the primary `.pmx` startup path for this milestone.

### Exit Criteria

- Dropping `model.pmx` into `models/` can load a character directly without generating `.auto.glb`.
- Startup logs make it clear when the runtime is using the native PMX path versus glTF.
- Existing glTF startup behavior remains intact.

## Phase 5: Native PMX Regression Coverage + CI

### Goal

Harden the native PMX runtime path with regression coverage comparable to current loader/runtime
subsystems.

### Scope

- Add PMX-native unit/regression tests covering:
  - PMX load success/failure paths
  - skeleton hierarchy import
  - submesh/material partition preservation
  - texture path resolution/hardening
  - startup model selection with native `.pmx`
  - runtime population of skinned PMX meshes
  - deterministic fallback behavior for unsupported PMX deformation/material features
- Add representative PMX fixtures that are small enough for deterministic tests:
  - single-material skinned PMX
  - multi-material textured PMX
  - negative fixtures for malformed/missing-path cases
- Add client/runtime smoke coverage for direct PMX startup loading similar to current animated
  startup coverage.
- Wire new PMX-native tests into Bazel and CI alongside existing render/runtime tests.
- Keep tests structural and diagnostic-oriented rather than visual-only:
  - mesh/material counts
  - bone counts/names
  - skin-palette validity
  - resolved texture sources
  - startup-source labels/log messages where appropriate
- Add explicit regression coverage for coexistence with the current glTF runtime path.

### Exit Criteria

- Native PMX runtime loading, world population, and startup selection are covered by automated
  regression tests.
- CI exercises the native PMX path as a supported runtime input, not only the conversion workflow.
- Native PMX support is maintainable without depending on manual viewer inspection.

## Phase 6: Native PMX Support Readiness

### Goal

Close the first native PMX milestone with a clear, supportable runtime baseline.

### Scope

- Publish the supported native PMX baseline after implementation lands:
  - direct PMX loading
  - usable skinned rendering
  - primary texture/material mapping
  - startup intake support
- Publish the intentionally deferred items:
  - VMD playback
  - PMX physics
  - exact MMD material/shading parity
  - broader morph/runtime editing support
- Document the runtime operator path for using native PMX assets in the app.
- Keep sidecar/conversion-era documentation accurate where both native PMX and conversion-based
  flows coexist.

### Exit Criteria

- Native PMX support is explicitly documented as a first-class runtime path with clear boundaries.
- Users can load supported `.pmx` assets directly in `isla` without converter setup.
- Remaining PMX/MMD parity work is clearly separated from the first usable native PMX milestone.

## Cross-Phase Testing Strategy

1. Keep PMX-native loaders and adapters unit-testable outside startup/client orchestration.
2. Prefer small canonical PMX fixtures with stable counts, names, and texture references.
3. Assert structure and diagnostics first: skeleton topology, submesh counts, texture resolution,
   material mapping, and startup source selection.
4. Add at least one client/runtime smoke test that loads a native PMX asset through the same startup
   flow end users will use.
5. Maintain coexistence coverage so native PMX work does not regress the existing glTF runtime path.

## Suggested Delivery Milestones

1. First direct PMX parse milestone: after Phase 1.
2. First visible native PMX character milestone: after Phase 2.
3. First materially usable native PMX visual baseline: after Phase 3.
4. First end-user direct PMX startup milestone: after Phase 4.
5. First maintainable native PMX runtime baseline: after Phase 5/6.
