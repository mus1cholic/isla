import unittest
from pathlib import Path

from tools.pmx import validate_converted_gltf


def _runfile(relpath: str) -> Path:
    # Package-local fixtures are available next to this test under runfiles.
    base = Path(__file__).resolve().parent
    rel = relpath.replace("tools/pmx/", "")
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


if __name__ == "__main__":
    unittest.main()
