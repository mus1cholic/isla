import unittest
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
