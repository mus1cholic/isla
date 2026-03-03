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
