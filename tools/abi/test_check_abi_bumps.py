"""Stdlib self-tests for the ABI bump check's declaration comparison."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_abi_bumps as check

TAG = """
#define SONARE_X_ABI_VERSION 2u
#define SONARE_X_MAX 8
typedef struct SonareThing SonareThing;
/// Options.
typedef struct {
  int a;  // first
  float b;
} SonareXOptions;
typedef enum { SONARE_X_A = 0, SONARE_X_B = 1 } SonareXMode;
SonareError sonare_x_run(SonareThing* thing, const SonareXOptions* options);
"""


def _changed(tree: str, tag: str = TAG) -> list[str]:
    return check.changed([tag], [tag], [tree])


class ChangedTest(unittest.TestCase):
    def test_comments_formatting_additions_and_counter_do_not_count(self) -> None:
        tree = TAG.replace("2u", "3u").replace("int a;  // first", "int   a;") + (
            "SonareError sonare_x_new(void);\n"
        )
        self.assertEqual(_changed(tree), [])

    def test_a_struct_field_change_counts(self) -> None:
        self.assertEqual(len(_changed(TAG.replace("float b;", "float b;\n  int c;"))), 1)

    def test_a_signature_change_or_removal_counts(self) -> None:
        tree = TAG.replace("const SonareXOptions* options", "int flags")
        self.assertEqual(len(_changed(tree)), 1)
        self.assertEqual(len(_changed(TAG.replace("SonareError sonare_x_run", "// "))), 1)

    def test_an_enum_may_grow_at_its_end_only(self) -> None:
        grown = TAG.replace("SONARE_X_B = 1 }", "SONARE_X_B = 1, SONARE_X_C = 2 }")
        self.assertEqual(_changed(grown), [])
        renumbered = TAG.replace("SONARE_X_B = 1", "SONARE_X_B = 2")
        self.assertEqual(_changed(renumbered), ["enum SonareXMode"])

    def test_a_renamed_opaque_handle_is_still_a_pointer(self) -> None:
        tree = TAG.replace("SonareThing", "SonareWidget")
        self.assertEqual(_changed(tree), [])

    def test_a_define_constant_change_counts(self) -> None:
        self.assertEqual(len(_changed(TAG.replace("MAX 8", "MAX 16"))), 1)

    def test_a_declaration_moved_to_another_header_is_found_there(self) -> None:
        moved = "SonareError sonare_x_run(SonareThing* thing, const SonareXOptions* options);"
        rest = TAG.replace(moved, "")
        self.assertEqual(check.changed([TAG], [TAG], [rest, moved]), [])

    def test_cxx_namespaces_and_inline_bodies_are_looked_through(self) -> None:
        tag = "namespace a::b {\nstruct C { int x; };\nconstexpr int f(int v) { return v; }\n}\n"
        body = tag.replace("return v;", "return v + 1;")
        self.assertEqual(check.changed([tag], [tag], [body]), [])
        field = tag.replace("int x;", "long x;")
        self.assertEqual(len(check.changed([tag], [tag], [field])), 1)

    def test_every_public_header_has_one_owner(self) -> None:
        root = Path(__file__).resolve().parents[2]
        for header in (root / "include/sonare").glob("*.h"):
            self.assertIsNotNone(check._owner(str(header.relative_to(root))))
        self.assertEqual(check._owner("include/sonare/sonare_c_project_edit.h"), "project")
        self.assertEqual(check._owner("include/sonare/sonare_c_mixing.h"), "feature")


if __name__ == "__main__":
    unittest.main()
