# PMX Tools

## Phase 7 Runtime Intake Notes

At app startup, when both `ISLA_ANIMATED_GLTF_ASSET` and `ISLA_MESH_ASSET` are unset, runtime now
scans `models/` and applies deterministic intake selection:

- preferred names first: `model.glb`, `model.gltf`, `model.pmx`
- then remaining candidates by extension priority (`.glb`, `.gltf`, `.pmx`) and filename order

Behavior by selected candidate:

- `.glb` / `.gltf`: loaded directly
- `.pmx`: auto-converted to `models/.isla_converted/<stem>.auto.glb`, then loaded

PMX conversion caching:

- cache metadata: `models/.isla_converted/<stem>.auto.cache`
- cache key inputs: PMX source size + mtime, converter command, converter version

Converter command configuration:

- default command template (used when env unset):
  - `pmx2gltf --input {input} --output {output}`
- optional override env vars:
  - `ISLA_PMX_CONVERTER_COMMAND`
  - `ISLA_PMX_CONVERTER_VERSION`

Important dependency note:

- Runtime orchestrates PMX conversion but does not include a native PMX converter implementation.
- A compatible converter command (default `pmx2gltf` or custom override) must be available for
  `.pmx` auto-convert to succeed.

## Troubleshooting PMX Auto-Convert

If `.pmx` startup conversion fails:

- Verify converter command availability:
  - `pmx2gltf --help`
  - if this fails, install the converter or set `ISLA_PMX_CONVERTER_COMMAND` to a valid command.
- Verify command template token usage:
  - expected tokens are `{input}` and `{output}`
  - example:
    - `ISLA_PMX_CONVERTER_COMMAND=pmx2gltf --input {input} --output {output}`
- Check conversion output/cache paths:
  - output: `models/.isla_converted/<stem>.auto.glb`
  - cache: `models/.isla_converted/<stem>.auto.cache`
- Force a reconvert when debugging:
  - delete `models/.isla_converted/<stem>.auto.glb`
  - delete `models/.isla_converted/<stem>.auto.cache`
- Confirm converter process exits successfully:
  - runtime warnings include converter command exit code (non-zero indicates failure).

## Phase 1 Validator

Run:

```bash
python tools/pmx/validate_converted_gltf.py <path-to-asset.gltf-or.glb>
```

Optional explicit sidecar path:

```bash
python tools/pmx/validate_converted_gltf.py <asset> --sidecar <asset.physics.json>
```

The validator enforces Phase 1 runtime compatibility checks and reports warnings for recommended quality checks.

Run validator tests:

```bash
bazel test //tools/pmx:validate_converted_gltf_test
```

## Phase 6 Motion Validator

Run:

```bash
python tools/pmx/validate_motion_clips.py <path-to-motion-asset.gltf-or.glb>
```

Optional checks:

```bash
python tools/pmx/validate_motion_clips.py <asset> \
  --sidecar <asset.motion.json> \
  --root-joint Root \
  --root-motion-policy in_place
```

The motion validator enforces Phase 6 clip requirements:
- baseline clip names (`idle`, `walk`, `action`)
- interpolation compatibility (`LINEAR`/`STEP` only)
- key count consistency (`input.count == output.count`)
- optional root-motion policy enforcement (`in_place` or `allow`)

Run motion validator tests:

```bash
bazel test //tools/pmx:validate_motion_clips_test
```
