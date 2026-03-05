import subprocess
import sys
import unittest
from pathlib import Path


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


class ValidateMotionClipsSmokeTest(unittest.TestCase):
    def test_cli_end_to_end_pass_fixture(self):
        script = _runfile("tools/pmx/validate_motion_clips.py")
        asset = _runfile("tools/pmx/testdata/motion/pass_motion.gltf")
        sidecar = _runfile("tools/pmx/testdata/motion/pass_motion.motion.json")

        result = subprocess.run(
            [sys.executable, str(script), str(asset), "--sidecar", str(sidecar)],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        self.assertIn("OK: Phase 6 motion validation passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
