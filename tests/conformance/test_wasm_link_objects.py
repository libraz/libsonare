"""Unit tests for wasm_link_objects.module_objects over hand-built build trees."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from wasm_link_objects import module_objects


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class ModuleObjectsTest(unittest.TestCase):
    def _tree(self, root: Path, *, response_file: bool) -> Path:
        build = root / "build"
        objdir = build / "sub" / "CMakeFiles"
        module_inputs = "CMakeFiles/mod.dir/a.cpp.o CMakeFiles/mod.dir/b.cpp.o ../lib/liblib.a"
        if response_file:
            _write(build / "sub" / "CMakeFiles" / "mod.dir" / "objects1.rsp", module_inputs + "\n")
            module_inputs = "@CMakeFiles/mod.dir/objects1.rsp"
        _write(
            objdir / "mod.dir" / "link.txt",
            f"em++ -O3 --bind -o ../bin/module.js {module_inputs}\n",
        )
        _write(
            objdir / "lib.dir" / "link.txt",
            "emar qc ../lib/liblib.a CMakeFiles/lib.dir/c.cpp.o CMakeFiles/lib.dir/d.cpp.o\n"
            "emranlib ../lib/liblib.a\n",
        )
        return build

    def _names(self, objects: list[Path]) -> list[str]:
        return [f"{obj.parent.name}/{obj.name}" for obj in objects]

    def test_archive_inputs_resolve_to_their_members_in_link_order(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = self._tree(Path(tmp), response_file=False)
            self.assertEqual(
                self._names(module_objects(build)),
                ["mod.dir/a.cpp.o", "mod.dir/b.cpp.o", "lib.dir/c.cpp.o", "lib.dir/d.cpp.o"],
            )

    def test_a_response_file_is_spliced_into_the_link_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = self._tree(Path(tmp), response_file=True)
            self.assertEqual(
                self._names(module_objects(build)),
                ["mod.dir/a.cpp.o", "mod.dir/b.cpp.o", "lib.dir/c.cpp.o", "lib.dir/d.cpp.o"],
            )

    def test_a_tree_without_exactly_one_module_link_line_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            _write(build / "CMakeFiles" / "lib.dir" / "link.txt", "emar qc liblib.a c.cpp.o\n")
            with self.assertRaises(SystemExit) as raised:
                module_objects(build)
            self.assertIn("found 0", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
