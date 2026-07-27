#!/usr/bin/env python3
"""Regression tests for YAL HoppieHelper release packaging."""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path


SCRIPT_PATH = Path(__file__).with_name("package_hoppiehelper_release.py")
SPEC = importlib.util.spec_from_file_location("package_hoppiehelper_release", SCRIPT_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Could not load release packager from {SCRIPT_PATH}")
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)


class ReleasePackageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.root = Path(self.tempdir.name) / "repo"
        self.output = Path(self.tempdir.name) / "dist"
        self.root.mkdir()

        (self.root / "src").mkdir()
        (self.root / "deploy/YAL_HoppieHelper/64").mkdir(parents=True)

        (self.root / "src/YAL_hoppiehelper.cpp").write_text(
            'constexpr const char* kPluginVersion = "2.1b1";\n',
            encoding="utf-8",
        )
        (self.root / "README.md").write_text(
            "Current plugin version: 2.1b1\n",
            encoding="utf-8",
        )
        (self.root / "INSTALL.md").write_text("install\n", encoding="utf-8")
        (self.root / "LICENSE").write_text("license\n", encoding="utf-8")
        (self.root / "deploy/YAL_HoppieHelper/64/mac.xpl").write_bytes(b"mac plugin\n")
        (self.root / "deploy/YAL_HoppieHelper/64/lin.xpl").write_bytes(b"lin plugin\n")
        (self.root / "deploy/YAL_HoppieHelper/64/win.xpl").write_bytes(b"win plugin\n")

    def build(self, channel: str = "beta", version: str = "2.1b1"):
        return PACKAGER.build_release_assets(self.root, self.output, channel, version)

    def test_assets_match_manifest_and_checksums(self) -> None:
        zip_path, text_manifest_path, json_manifest_path, checksum_path = self.build()
        manifest = json.loads(json_manifest_path.read_text(encoding="utf-8"))

        self.assertEqual("wahltho.yal-hoppiehelper", manifest["packageId"])
        self.assertEqual("2.1b1", manifest["packageVersion"])
        self.assertEqual("r2.1b1", manifest["releaseTag"])
        self.assertEqual("beta", manifest["channel"])
        self.assertEqual("Resources/plugins/YAL_HoppieHelper", manifest["targetPath"])
        self.assertEqual(["zibo-737ng"], manifest["supportedProducts"])
        self.assertTrue(manifest["restartRequired"])
        self.assertEqual([], manifest["protectedPaths"])

        archive_metadata = manifest["archive"]
        self.assertEqual(zip_path.name, archive_metadata["fileName"])
        self.assertEqual("YAL_HoppieHelper", archive_metadata["rootPath"])
        self.assertEqual(zip_path.stat().st_size, archive_metadata["size"])
        self.assertEqual(hashlib.sha256(zip_path.read_bytes()).hexdigest(), archive_metadata["sha256"])

        expected_paths = [target for _, target in PACKAGER.collect_files(self.root)]
        actual_paths = [entry["path"] for entry in manifest["files"]]
        self.assertEqual(expected_paths, actual_paths)

        with zipfile.ZipFile(zip_path, "r") as archive:
            self.assertEqual(
                {f"YAL_HoppieHelper/{path}" for path in expected_paths},
                {info.filename for info in archive.infolist() if not info.is_dir()},
            )
            for entry in manifest["files"]:
                payload = archive.read(f"YAL_HoppieHelper/{entry['path']}")
                self.assertEqual(len(payload), entry["size"])
                self.assertEqual(hashlib.sha256(payload).hexdigest(), entry["sha256"])

        checksums = checksum_path.read_text(encoding="utf-8").splitlines()
        self.assertEqual(3, len(checksums))
        self.assertTrue(any(zip_path.name in line for line in checksums))
        self.assertTrue(any(text_manifest_path.name in line for line in checksums))
        self.assertTrue(any(json_manifest_path.name in line for line in checksums))

    def test_verifier_rejects_changed_file_hash(self) -> None:
        zip_path, _, json_manifest_path, _ = self.build()
        manifest = json.loads(json_manifest_path.read_text(encoding="utf-8"))
        manifest["files"][0]["sha256"] = "0" * 64
        json_manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                PACKAGER.verify_release_package(zip_path, json_manifest_path, "beta", "2.1b1")

    def test_stable_channel_requires_stable_version(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                PACKAGER.validate_versions(self.root, "stable", "2.1b1")


if __name__ == "__main__":
    unittest.main()
