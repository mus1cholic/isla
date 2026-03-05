import unittest
import tempfile
from pathlib import Path

from tools.pmx import validate_converted_gltf


def _runfile(relpath: str) -> Path:
    # Package-local fixtures are available next to this test under runfiles.
    base = Path(__file__).resolve().parent
    rel = relpath
    prefix = "tools/pmx/"
    if rel.startswith(prefix):
        rel = rel[len(prefix):]
    path = base / rel
    if not path.exists():
        raise FileNotFoundError(f"Runfile not found: {relpath} (resolved: {path})")
    return path


class ValidateConvertedGltfTest(unittest.TestCase):
    def _run_validator(self, asset_rel: str, sidecar_rel: str):
        asset = _runfile(asset_rel)
        sidecar = _runfile(sidecar_rel)
        return validate_converted_gltf.validate_package(asset, sidecar)

    def test_allows_animated_non_joint_hierarchy(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            asset = root / "animated_non_joint.gltf"
            sidecar = root / "animated_non_joint.physics.json"
            asset.write_text(
                """
{
  "asset": {"version": "2.0"},
  "accessors": [
    {"count": 3},
    {"count": 3},
    {"count": 3},
    {"count": 3},
    {"count": 2},
    {"count": 2},
    {"count": 2},
    {"count": 2},
    {"count": 2}
  ],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2}, "indices": 3}]}
  ],
  "nodes": [
    {"name": "RigRoot"},
    {"name": "Root", "translation": [1, 0, 0]},
    {"mesh": 0, "skin": 0}
  ],
  "skins": [{"joints": [1]}],
  "animations": [
    {
      "name": "idle",
      "samplers": [{"input": 4, "output": 5, "interpolation": "LINEAR"}],
      "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]
    },
    {
      "name": "walk",
      "samplers": [{"input": 6, "output": 7, "interpolation": "LINEAR"}],
      "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]
    },
    {
      "name": "action",
      "samplers": [{"input": 6, "output": 8, "interpolation": "STEP"}],
      "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]
    }
  ]
}
""".strip(),
                encoding="utf-8",
            )
            sidecar.write_text(
                """
{
  "schema_version": "1.0.0",
  "converter": {
    "name": "conv",
    "version": "1",
    "command": "x",
    "timestamp_utc": "2026-03-01T00:00:00Z"
  },
  "collision_layers": [{"index": 0, "name": "default"}],
  "colliders": [
    {
      "id": "c0",
      "bone_name": "Root",
      "shape": "sphere",
      "offset": [0, 0, 0],
      "rotation_euler_deg": [0, 0, 0],
      "is_trigger": false,
      "layer": 1,
      "mask": 1,
      "radius": 0.2
    }
  ],
  "constraints": []
}
""".strip(),
                encoding="utf-8",
            )

            errors, warnings = validate_converted_gltf.validate_package(asset, sidecar)
            self.assertFalse(errors, msg=f"errors: {errors}, warnings: {warnings}")

    def test_passes_valid_fixture(self):
        errors, warnings = self._run_validator(
            "tools/pmx/testdata/pass_minimal.gltf",
            "tools/pmx/testdata/pass_minimal.physics.json",
        )
        self.assertFalse(errors, msg=f"errors: {errors}, warnings: {warnings}")

    def test_fails_cubicspline_fixture(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/fail_cubicspline.gltf",
            "tools/pmx/testdata/fail_cubicspline.physics.json",
        )
        self.assertTrue(errors)
        self.assertIn("Unsupported interpolation: CUBICSPLINE", "\n".join(errors))

    def test_fails_schema_version_mismatch(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/pass_minimal.gltf",
            "tools/pmx/testdata/fail_schema_version.physics.json",
        )
        self.assertTrue(errors)
        self.assertIn("schema_version '1.0.1' is unsupported", "\n".join(errors))

    def test_fails_non_object_sidecar_root(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/pass_minimal.gltf",
            "tools/pmx/testdata/fail_top_level_array.physics.json",
        )
        self.assertTrue(errors)
        self.assertIn(
            "physics sidecar top-level JSON value must be an object",
            "\n".join(errors),
        )

    def test_fails_naive_converter_timestamp(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/pass_minimal.gltf",
            "tools/pmx/testdata/fail_naive_timestamp.physics.json",
        )
        self.assertTrue(errors)
        self.assertIn(
            "converter.timestamp_utc missing or invalid RFC3339 date-time",
            "\n".join(errors),
        )

    def test_fails_collider_layer_out_of_range(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/pass_minimal.gltf",
            "tools/pmx/testdata/fail_layer_out_of_range.physics.json",
        )
        self.assertTrue(errors)
        self.assertIn(
            "collider[0] layer must be int in [0, 31]",
            "\n".join(errors),
        )

    def test_returns_error_instead_of_raising_for_no_skin(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/fail_no_skin.gltf",
            "tools/pmx/testdata/pass_minimal.physics.json",
        )
        self.assertTrue(errors)
        self.assertIn("glTF has no skin", "\n".join(errors))

    def test_warns_for_malformed_joints_min_metadata(self):
        errors, warnings = self._run_validator(
            "tools/pmx/testdata/warn_malformed_joints_min_metadata.gltf",
            "tools/pmx/testdata/pass_minimal.physics.json",
        )
        self.assertFalse(errors, msg=f"errors: {errors}, warnings: {warnings}")
        all_warnings = "\n".join(warnings)
        self.assertIn("JOINTS_0 accessor min metadata is malformed", all_warnings)
        self.assertNotIn("JOINTS_0 accessor lacks max metadata", all_warnings)


if __name__ == "__main__":
    unittest.main()
