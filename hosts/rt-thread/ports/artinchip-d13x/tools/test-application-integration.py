#!/usr/bin/env python3
"""Host regressions for SDK selection and application source dependency safety."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import portenv as pe


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, pe.TOOLS_DIR / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


build = module("firmware_test", "build-firmware.py")
overlay = module("overlay_test", "apply-sdk.py")


class ApplicationIntegration(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.sdk = Path(self.tmp.name) / "sdk"
        self.repo = self.sdk / "application/rt-thread/pocketjs-smoke/third_party/pocketjs"
        self.repo.mkdir(parents=True)
        (self.sdk / "SConstruct").touch()
        (self.sdk / "bsp/artinchip").mkdir(parents=True)

    def test_nested_sdk_precedes_stale_machine_pin(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(pe, "repo_root", return_value=self.repo):
            self.assertEqual(pe.sdk_root(), self.sdk.resolve())
            self.assertTrue(pe.sdk_submodule_mode())

    def test_explicit_sdk_override_is_honored(self):
        other = Path(self.tmp.name) / "other"
        with patch.dict(os.environ, {"POCKETJS_AIC_SDK_ROOT": str(other)}):
            self.assertEqual(pe.sdk_root(), other.resolve())

    def test_plain_edits_and_gitlink_changes_are_not_hidden(self):
        status = " M bsp/artinchip/driver.c\n M application/rt-thread/pocketjs-smoke/third_party/pocketjs\n"
        with patch.object(pe, "sdk_root", return_value=self.sdk), patch.object(pe, "_git", return_value=status):
            self.assertEqual(pe.sdk_dirty_entries(), status.splitlines())

    def test_overlay_cannot_overwrite_a_submodule(self):
        (self.repo / ".git").write_text("gitdir: unrelated\n")
        with patch.object(pe, "sdk_root", return_value=self.sdk), patch("sys.argv", ["apply-sdk.py"]):
            self.assertEqual(overlay.main(), 2)
        self.assertFalse((self.repo / "src").exists())

    def test_wrong_sdk_branch_rejected_before_build(self):
        with patch.object(pe, "sdk_root", return_value=self.sdk), \
             patch.object(pe, "sdk_branch", return_value="codex/hangcha-zc-202620085"), \
             patch.object(pe, "port_branch", return_value="codex/port-pocketjs"), \
             patch.object(build, "build_rust") as rust, patch("sys.argv", ["build-firmware.py"]):
            self.assertEqual(build.main(), 2)
            rust.assert_not_called()

    def test_other_application_defconfig_rejected(self):
        with patch.object(pe, "sdk_root", return_value=self.sdk), \
             patch.object(pe, "sdk_branch", return_value="codex/port-pocketjs"), \
             patch.object(pe, "port_branch", return_value="codex/port-pocketjs"), \
             patch.object(build, "build_rust") as rust, \
             patch("sys.argv", ["build-firmware.py", "--defconfig", "product_defconfig"]):
            self.assertEqual(build.main(), 2)
            rust.assert_not_called()

    def test_source_dependency_archives_are_not_copied(self):
        archive = self.repo / ".pocket-build/libtest.a"
        archive.parent.mkdir()
        archive.write_bytes(b"archive fixture")
        with patch.object(pe, "sdk_submodule_mode", return_value=True), \
             patch.object(build, "rust_artifact", return_value=archive):
            self.assertEqual(build.stage_rust(), [archive] * len(build.ARCHIVES))
        self.assertFalse((self.repo / "lib").exists())


if __name__ == "__main__":
    unittest.main()
