#!/usr/bin/env python3
"""Tooling validation, including isolated dirty/wrong-SHA/URL negative fixtures."""
import configparser
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("dependencies", ROOT / "tools/dependency_metadata.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class DependencyVerificationTest(unittest.TestCase):
    def test_real_gitlinks(self):
        self.assertEqual(len(MODULE.verify(ROOT)), 3)

    def test_negative_fixtures(self):
        # Shared local clones reuse pinned objects; no network or commits needed.
        with tempfile.TemporaryDirectory(prefix="ros2-bag-deps-") as temporary:
            fixture = Path(temporary)
            subprocess.run(["git", "init", "-q", str(fixture)], check=True)
            config = configparser.ConfigParser()
            for name, url in MODULE.DEPENDENCIES.items():
                relative = f"third_party/{name}"
                subprocess.run(["git", "clone", "--quiet", "--shared", "--no-hardlinks",
                                str(ROOT / relative), str(fixture / relative)], check=True)
                sha = MODULE.git(fixture / relative, "rev-parse", "HEAD")
                MODULE.git(fixture, "update-index", "--add", "--cacheinfo", f"160000,{sha},{relative}")
                config[f'submodule "{relative}"'] = {"path": relative, "url": url}
            with (fixture / ".gitmodules").open("w") as stream:
                config.write(stream)
            self.assertEqual(len(MODULE.verify(fixture)), 3)
            readme = fixture / "third_party/json/README.md"
            original = readme.read_bytes()
            readme.write_bytes(original + b"\nfixture mutation\n")
            with self.assertRaisesRegex(ValueError, "dirty dependency"):
                MODULE.verify(fixture)
            readme.write_bytes(original)
            MODULE.git(fixture, "update-index", "--cacheinfo",
                       f"160000,{'1' * 40},third_party/json")
            with self.assertRaisesRegex(ValueError, "checkout does not match"):
                MODULE.verify(fixture)
            sha = MODULE.git(fixture / "third_party/json", "rev-parse", "HEAD")
            MODULE.git(fixture, "update-index", "--cacheinfo", f"160000,{sha},third_party/json")
            config['submodule "third_party/json"']["url"] = "https://invalid.example/wrong.git"
            with (fixture / ".gitmodules").open("w") as stream:
                config.write(stream)
            with self.assertRaisesRegex(ValueError, "upstream URL"):
                MODULE.verify(fixture)

    def test_lock_generation(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "third_party.lock"
            subprocess.run(["bash", str(ROOT / "tools/generate-third-party-lock.sh"),
                            str(destination)], check=True, stdout=subprocess.PIPE)
            data = json.loads(destination.read_text())
            self.assertEqual(data["dependencies"], MODULE.verify(ROOT))


if __name__ == "__main__":
    unittest.main()
