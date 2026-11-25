"""Tests for the stable surface snapshot projection and drift check."""

import contextlib
import io
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_surface_snapshot as snapshot


class SurfaceSnapshotTest(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        (self.root / "tools/api").mkdir(parents=True)
        self.rows = {surface: ["two", "one"] for surface in snapshot.SURFACES}

    def run_snapshot(self, *, check):
        with (
            contextlib.redirect_stdout(io.StringIO()) as out,
            contextlib.redirect_stderr(io.StringIO()) as err,
        ):
            status = snapshot.write_or_check(self.root, self.rows, check=check)
        return status, out.getvalue() + err.getvalue()

    def test_sorted_rows_and_read_only_diff(self):
        self.assertEqual(self.run_snapshot(check=False)[0], 0)
        path = self.root / "tools/api/surface-snapshot-node.txt"
        self.assertEqual(path.read_text(), snapshot.HEADER + "one\ntwo\n")
        self.assertEqual(self.run_snapshot(check=True)[0], 0)
        self.rows["node"] = ["one", "new"]
        status, output = self.run_snapshot(check=True)
        self.assertEqual(status, 1)
        self.assertIn("surface-snapshot-node.txt", output)
        self.assertIn("+new\n", output)
        self.assertIn("-two\n", output)
        self.assertIn("Run make surface-snapshot and review the diff.", output)
        self.assertEqual(path.read_text(), snapshot.HEADER + "one\ntwo\n")

    def test_empty_surface_fails_before_writing(self):
        self.rows["wasm"].clear()
        for check in (False, True):
            status, output = self.run_snapshot(check=check)
            self.assertEqual(status, 1)
            self.assertIn("wasm: no public surface items extracted", output)
            self.assertEqual(list((self.root / "tools/api").iterdir()), [])

    def test_public_type_fields_and_signature_defaults(self):
        row = {
            "kind": "interface",
            "sigs": [],
            "members": [
                {
                    "name": "samples",
                    "kind": "property",
                    "type": "Float32Array",
                    "static": False,
                    "optional": True,
                    "readonly": True,
                    "sigs": [],
                },
                {
                    "name": "slice",
                    "kind": "method",
                    "static": False,
                    "optional": False,
                    "readonly": False,
                    "sigs": [
                        {
                            "text": "(start?: number): Float32Array",
                            "params": [{"name": "start", "default": "0"}],
                        }
                    ],
                },
            ],
        }
        self.assertEqual(
            list(snapshot.type_rows("./analysis", row, "Audio")),
            [
                "./analysis Audio interface",
                "./analysis Audio.samples property optional readonly : Float32Array",
                "./analysis Audio.slice method required",
                "./analysis Audio.slice signature 01 (start?: number): Float32Array defaults (start = 0)",
            ],
        )

    def test_native_library_default_is_visible_through_option_wrappers(self):
        options = [
            (
                'described(library_default(with_domain(int_value("count-in-bars"),'
                ' between(0.0, 16.0)), "0"), "Help text")'
            )
        ]
        self.assertEqual(
            snapshot.inventory._native_snapshot_specs(options),
            {"count-in-bars": 'int_value("count-in-bars") library_default("0")'},
        )

    @unittest.skipUnless(
        shutil.which("node")
        and (snapshot.inventory.ROOT / "bindings/wasm/node_modules/typescript").is_dir(),
        "node or the WASM package's typescript is not installed",
    )
    def test_typescript_source_shape_and_literal_fallbacks(self):
        package = self.root / "bindings/probe"
        (package / "src").mkdir(parents=True)
        (package / "node_modules").symlink_to(
            snapshot.inventory.ROOT / "bindings/wasm/node_modules", target_is_directory=True
        )
        (self.root / "tools/api/ts_exports.cjs").symlink_to(
            snapshot.inventory.ROOT / "tools/api/ts_exports.cjs"
        )
        (package / "tsconfig.json").write_text('{"compilerOptions":{"strict":true,"types":[]}}')
        (package / "package.json").write_text("{}")
        source = package / "src/index.ts"
        text = """
export type Result = {
  /** Initial field comment. */
  value?: number;
};
export interface Request { value?: number }
export function analyze(request: Request): Result;
export function analyze(request: Request): Result {
  return { value: request.value ?? -0.5 };
}
"""
        source.write_text(text)
        rows = list(snapshot.typescript_rows(self.root, "probe", {".": "index.ts"}))
        self.assertIn(". Result.value property optional : number | undefined", rows)
        self.assertIn("default index.ts:analyze request.value = -0.5", rows)
        source.write_text(text.replace("Initial field comment.", "New comment."))
        self.assertEqual(
            list(snapshot.typescript_rows(self.root, "probe", {".": "index.ts"})), rows
        )
