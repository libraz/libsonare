"""Stdlib regression tests for the GS classic model generator's input boundary."""

from __future__ import annotations

import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "gs_classic_models_under_test", ROOT / "tools" / "gs" / "classic_models.py"
)
assert SPEC is not None and SPEC.loader is not None
classic = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(classic)


def shaper(oversample):
    return {
        "id": "s",
        "kind": "shaper",
        "input": "in_l",
        "curve": "tanh",
        "drive": {"value": 0.0},
        "oversample": oversample,
    }


class ClassicModelVocabularyTest(unittest.TestCase):
    def test_shaper_oversample_must_be_the_supported_non_bool_integer_one(self) -> None:
        for oversample in (True, 1.0, 0, 2, 15):
            with self.subTest(oversample=oversample), self.assertRaises(SystemExit):
                classic.check_vocabulary(
                    {"nodes": [shaper(oversample)]}, "synthetic", overlay=False
                )

        classic.check_vocabulary({"nodes": [shaper(1)]}, "synthetic", overlay=False)

    def test_converter_rejects_unsupported_shaper_oversampling(self) -> None:
        class Context:
            def __init__(self) -> None:
                self.accept = {(0, 0): (0, 127)}
                self.power_on = {0: [0] * classic.SLOTS}

        converter = classic.Converter(Context(), classic.Pools(), 0, {}, {})
        converter.signals = {"in_l": 0}
        for oversample in (True, 1.0, 2, 15):
            with self.subTest(oversample=oversample), self.assertRaises(SystemExit):
                converter.node(shaper(oversample))

        converter.node(shaper(1))


class ArchiveRevisionTest(unittest.TestCase):
    def git_fixture(self) -> Path:
        root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        files = {
            "src/soundings/__init__.py": "VERSION = 1\n",
            "src/soundings/render/graph.py": "VERSION = 1\n",
            "src/soundings/render/native.so": "binary\n",
            "tools/helper.py": "VERSION = 1\n",
            ".venv/lib/soundings/installed.py": "VERSION = 1\n",
        }
        for name, text in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        subprocess.run(["git", "-C", str(root), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(root), "add", "-f", "."], check=True)
        subprocess.run(
            [
                "git",
                "-C",
                str(root),
                "-c",
                "core.hooksPath=/dev/null",
                "-c",
                "commit.gpgsign=false",
                "-c",
                "user.name=classic-test",
                "-c",
                "user.email=classic-test@example.invalid",
                "commit",
                "-qm",
                "fixture",
            ],
            check=True,
        )
        return root

    def dirty(self, root: Path, read: set[Path]) -> bool:
        return classic.archive_revision(root, sorted(read))["archive_inputs_dirty"]

    def test_tracked_package_sources_are_the_dirty_input_set(self) -> None:
        root = self.git_fixture()
        package = root / "src" / "soundings"
        read = classic.package_sources(root, package)

        self.assertEqual(
            read,
            {
                (package / "__init__.py").resolve(),
                (package / "render" / "graph.py").resolve(),
            },
        )
        self.assertFalse(self.dirty(root, read))
        (package / "render" / "graph.py").write_text("VERSION = 2\n", encoding="utf-8")
        self.assertTrue(self.dirty(root, read))

    def test_files_outside_the_package_are_not_counted(self) -> None:
        root = self.git_fixture()
        read = classic.package_sources(root, root / "src" / "soundings")
        (root / "tools" / "helper.py").write_text("VERSION = 2\n", encoding="utf-8")

        self.assertNotIn((root / "tools" / "helper.py").resolve(), read)
        self.assertFalse(self.dirty(root, read))

    def test_a_package_under_venv_or_outside_the_root_is_not_archive_input(self) -> None:
        root = self.git_fixture()
        installed = root / ".venv" / "lib" / "soundings"
        elsewhere = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, elsewhere, ignore_errors=True)
        (installed / "installed.py").write_text("VERSION = 2\n", encoding="utf-8")

        self.assertEqual(classic.package_sources(root, installed), set())
        self.assertEqual(classic.package_sources(root, elsewhere), set())
        read = classic.package_sources(root, root / "src" / "soundings") | classic.package_sources(
            root, installed
        )
        self.assertFalse(self.dirty(root, read))


if __name__ == "__main__":
    unittest.main()
