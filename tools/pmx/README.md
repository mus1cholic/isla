# PMX Tools

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
