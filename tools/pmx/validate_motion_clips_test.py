import unittest
import subprocess
import sys
import tempfile
from pathlib import Path

from tools.pmx import validate_motion_clips


def _runfile(relpath: str) -> Path:
    base = Path(__file__).resolve().parent
    rel = relpath
    prefix = "tools/pmx/"
    if rel.startswith(prefix):
        rel = rel[len(prefix):]
    path = base / rel
    if not path.exists():
        raise FileNotFoundError(f"Runfile not found: {relpath} (resolved: {path})")
    return path


class ValidateMotionClipsTest(unittest.TestCase):
    def _run_validator(self, asset_rel: str, sidecar_rel: str | None = None):
        asset = _runfile(asset_rel)
        sidecar = _runfile(sidecar_rel) if sidecar_rel is not None else None
        return validate_motion_clips.validate_motion_package(
            asset,
            sidecar,
            root_joint_name="Root",
            root_motion_policy="in_place",
            root_motion_epsilon=1.0e-4,
            require_action_clip=True,
        )

    def _run_cli(self, *args: str):
        script = _runfile("tools/pmx/validate_motion_clips.py")
        command = [sys.executable, str(script), *args]
        return subprocess.run(command, check=False, capture_output=True, text=True)

    def test_allows_animated_non_joint_hierarchy(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            asset = Path(tmpdir) / "motion_non_joint.gltf"
            asset.write_text(
                """
{
  "asset": {"version": "2.0"},
  "accessors": [
    {"count": 2},
    {"count": 2},
    {"count": 2},
    {"count": 2},
    {"count": 2},
    {"count": 2}
  ],
  "nodes": [
    {"name": "RigRoot"},
    {"name": "Root", "translation": [0, 0, 0]}
  ],
  "skins": [{"joints": [1]}],
  "animations": [
    {
      "name": "idle",
      "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"}],
      "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]
    },
    {
      "name": "walk",
      "samplers": [{"input": 0, "output": 2, "interpolation": "LINEAR"}],
      "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]
    },
    {
      "name": "action",
      "samplers": [{"input": 0, "output": 3, "interpolation": "STEP"}],
      "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]
    }
  ]
}
""".strip(),
                encoding="utf-8",
            )

            errors, warnings = validate_motion_clips.validate_motion_package(
                asset,
                sidecar_path=None,
                root_joint_name="Root",
                root_motion_policy="in_place",
                root_motion_epsilon=1.0e-4,
                require_action_clip=True,
            )
            self.assertFalse(errors, msg=f"errors: {errors}, warnings: {warnings}")

    def test_passes_valid_motion_fixture(self):
        errors, warnings = self._run_validator(
            "tools/pmx/testdata/motion/pass_motion.gltf",
            "tools/pmx/testdata/motion/pass_motion.motion.json",
        )
        self.assertFalse(errors, msg=f"errors: {errors}, warnings: {warnings}")

    def test_fails_on_cubicspline(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/motion/fail_motion_cubicspline.gltf",
        )
        self.assertTrue(errors)
        self.assertIn("Unsupported interpolation: CUBICSPLINE", "\n".join(errors))

    def test_fails_on_root_motion_violation(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/motion/fail_motion_root_translation.gltf",
        )
        self.assertTrue(errors)
        self.assertIn("violates in_place policy", "\n".join(errors))

    def test_fails_when_action_clip_missing(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/motion/fail_motion_missing_action.gltf",
        )
        self.assertTrue(errors)
        self.assertIn("Missing required baseline clip: action", "\n".join(errors))

    def test_fails_on_invalid_motion_sidecar_schema_version(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/motion/pass_motion.gltf",
            "tools/pmx/testdata/motion/fail_motion_schema_version.motion.json",
        )
        self.assertTrue(errors)
        self.assertIn("motion sidecar schema_version '1.0.1' is unsupported", "\n".join(errors))

    def test_cli_uses_default_sidecar_path(self):
        asset = _runfile("tools/pmx/testdata/motion/pass_motion.gltf")
        result = self._run_cli(str(asset))
        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        self.assertIn("OK: Phase 6 motion validation passed", result.stdout)

    def test_cli_uses_contract_named_default_sidecar_path(self):
        asset = _runfile("tools/pmx/testdata/motion/pass_contract_named.motion.gltf")
        result = self._run_cli(str(asset))
        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        self.assertIn("using motion sidecar", result.stdout)
        self.assertIn("pass_contract_named.motion.json", result.stdout)
        self.assertIn("OK: Phase 6 motion validation passed", result.stdout)

    def test_cli_allow_missing_action_flag(self):
        asset = _runfile("tools/pmx/testdata/motion/fail_motion_missing_action.gltf")
        result = self._run_cli(str(asset), "--allow-missing-action")
        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        self.assertIn("OK: Phase 6 motion validation passed", result.stdout)

    def test_cli_reports_invalid_root_joint(self):
        asset = _runfile("tools/pmx/testdata/motion/pass_motion.gltf")
        result = self._run_cli(str(asset), "--root-joint", "MissingRoot")
        self.assertEqual(result.returncode, 1, msg=result.stdout + result.stderr)
        self.assertIn("requested root joint 'MissingRoot' was not found", result.stdout)

    def test_cli_allow_root_motion_policy_bypasses_in_place_error(self):
        asset = _runfile("tools/pmx/testdata/motion/fail_motion_root_translation.gltf")
        result = self._run_cli(str(asset), "--root-motion-policy", "allow")
        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        self.assertIn("OK: Phase 6 motion validation passed", result.stdout)

    def test_fails_on_motion_sidecar_missing_required_fields(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/motion/pass_motion.gltf",
            "tools/pmx/testdata/motion/fail_motion_sidecar_missing_fields.motion.json",
        )
        self.assertTrue(errors)
        self.assertIn("motion sidecar missing required field: clips", "\n".join(errors))

    def test_fails_on_motion_sidecar_malformed_clip_entry(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/motion/pass_motion.gltf",
            "tools/pmx/testdata/motion/fail_motion_sidecar_malformed_clip.motion.json",
        )
        self.assertTrue(errors)
        self.assertIn("motion sidecar clips[0].clip_name missing or empty", "\n".join(errors))

    def test_fails_on_motion_sidecar_invalid_timestamp(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/motion/pass_motion.gltf",
            "tools/pmx/testdata/motion/fail_motion_sidecar_bad_timestamp.motion.json",
        )
        self.assertTrue(errors)
        self.assertIn("converter.timestamp_utc missing or invalid RFC3339", "\n".join(errors))

    def test_fails_on_motion_sidecar_unknown_clip_name(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/motion/pass_motion.gltf",
            "tools/pmx/testdata/motion/fail_motion_sidecar_unknown_clip.motion.json",
        )
        self.assertTrue(errors)
        self.assertIn("motion sidecar clip 'jump' not found", "\n".join(errors))

    def test_warns_when_root_translation_has_no_minmax_metadata(self):
        errors, warnings = self._run_validator(
            "tools/pmx/testdata/motion/warn_motion_root_no_minmax.gltf",
        )
        self.assertFalse(errors, msg=f"errors: {errors}, warnings: {warnings}")
        self.assertIn("root translation lacks min/max metadata", "\n".join(warnings))

    def test_fails_on_duplicate_clip_names(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/motion/fail_motion_duplicate_clip_names.gltf",
        )
        self.assertTrue(errors)
        self.assertIn("duplicate animation clip name: idle", "\n".join(errors))

    def test_fails_on_empty_clip_name(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/motion/fail_motion_empty_clip_name.gltf",
        )
        self.assertTrue(errors)
        self.assertIn("missing non-empty name", "\n".join(errors))

    def test_fails_action_root_motion_without_sidecar_override(self):
        errors, _warnings = self._run_validator(
            "tools/pmx/testdata/motion/fail_motion_action_root_translation.gltf",
        )
        self.assertTrue(errors)
        all_errors = "\n".join(errors)
        self.assertIn("('action')", all_errors)
        self.assertIn("violates in_place policy", all_errors)
        self.assertIn("source=cli_default", all_errors)

    def test_passes_action_root_motion_with_sidecar_clip_allow_override(self):
        errors, warnings = self._run_validator(
            "tools/pmx/testdata/motion/fail_motion_action_root_translation.gltf",
            "tools/pmx/testdata/motion/pass_motion_action_allow_override.motion.json",
        )
        self.assertFalse(errors, msg=f"errors: {errors}, warnings: {warnings}")


if __name__ == "__main__":
    unittest.main()
