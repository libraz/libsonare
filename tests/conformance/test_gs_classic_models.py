"""Stdlib regression tests for the GS classic model generator's input boundary."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

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
    @staticmethod
    def git_fixture() -> tuple[Path, Path]:
        root = Path(tempfile.mkdtemp())
        source = root / "src" / "soundings" / "render" / "graph.py"
        source.parent.mkdir(parents=True)
        source.write_text("VERSION = 1\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(root), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(root), "add", str(source)], check=True)
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
        return root, source

    def test_loaded_soundings_source_is_included_in_dirty_input_check(self) -> None:
        root, source = self.git_fixture()
        archive = classic.Archive.__new__(classic.Archive)
        archive.root = root
        archive.read = set()
        module = types.ModuleType("soundings.render.graph")
        module.__file__ = str(source)
        outside = types.ModuleType("soundings.render.third_party")
        outside.__file__ = str(Path(tempfile.mkdtemp()) / "third_party.py")

        with mock.patch.dict(
            sys.modules,
            {
                "soundings.render.graph": module,
                "soundings.render.third_party": outside,
            },
        ):
            archive.record_imported_sources()

        self.assertIn(source.resolve(), archive.read)
        self.assertNotIn(Path(outside.__file__).resolve(), archive.read)
        clean = classic.archive_revision(root, sorted(archive.read))
        self.assertFalse(clean["archive_inputs_dirty"])
        source.write_text("VERSION = 2\n", encoding="utf-8")
        dirty = classic.archive_revision(root, sorted(archive.read))
        self.assertTrue(dirty["archive_inputs_dirty"])

    def test_only_python_sources_inside_archive_root_are_recorded(self) -> None:
        root, source = self.git_fixture()
        archive = classic.Archive.__new__(classic.Archive)
        archive.root = root
        archive.read = set()
        package = types.ModuleType("soundings")
        package.__file__ = str(source)
        escaped = types.ModuleType("soundings.render.escaped")
        escaped.__file__ = str(root.parent / "outside.py")
        extension = types.ModuleType("soundings.render.extension")
        extension.__file__ = str(root / "src" / "soundings" / "render" / "native.so")

        with mock.patch.dict(
            sys.modules,
            {
                "soundings": package,
                "soundings.render.escaped": escaped,
                "soundings.render.extension": extension,
            },
        ):
            archive.record_imported_sources()

        self.assertEqual(archive.read, {source.resolve()})


if __name__ == "__main__":
    unittest.main()
