"""Self-tests for the shared TypeScript declaration walk.

Every check built on this module reports a clean repository, so what has to be
established is that the walk could report otherwise -- and, just as much, that it
reaches what it claims to. Both of its failure modes are silent and both land on
"nothing found": resolving a leaf against a block that is not its parent, and
stopping at a type expression it cannot follow while the caller still counts the
path as compared.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ts_surface_walk as walk

SURFACE = """
export type Band = 'sub' | 'low' | 'lowMid';
export type Occupancy = Record<Band, number>;
export type OpenBag = Record<string, unknown>;

export interface Inner { value: number; mode: string }
export interface Other { mode: string }
export interface Derived extends Inner { extra: string }

export interface Root {
  inner: Inner;
  either?: boolean | { nested: number };
  occupancy: Occupancy;
  bag: OpenBag;
  items: Inner[];
}
"""


def _root() -> str:
    return walk.named_type_bodies(SURFACE, "Root")[0]


class Camel(unittest.TestCase):
    def test_snake_case_becomes_the_surface_spelling(self):
        self.assertEqual(walk.camel("band0_gain_db"), "band0GainDb")
        self.assertEqual(walk.camel("bpm"), "bpm")


class NamedTypes(unittest.TestCase):
    def test_an_absent_type_yields_nothing_rather_than_a_false_body(self):
        self.assertEqual(walk.named_type_bodies(SURFACE, "Missing"), [])

    def test_an_interface_carries_the_members_it_extends(self):
        bodies = walk.named_type_bodies(SURFACE, "Derived")
        self.assertTrue(any(walk.declares_leaf(body, "extra") for body in bodies))
        self.assertTrue(any(walk.declares_leaf(body, "value") for body in bodies))
        self.assertFalse(any(walk.declares_leaf(body, "nested") for body in bodies))

    def test_a_property_resolves_through_a_named_type(self):
        bodies = walk.property_bodies(_root(), "inner", SURFACE)
        self.assertEqual(len(bodies), 1)
        self.assertTrue(walk.declares_leaf(bodies[0], "value"))

    def test_a_leaf_is_not_matched_outside_the_block_that_declares_it(self):
        """`mode` is declared by two interfaces; only one of them is `inner`."""
        self.assertTrue(
            walk.declares_leaf(walk.named_type_bodies(SURFACE, "Other")[0], "mode")
        )
        self.assertFalse(walk.declares_leaf(_root(), "mode"))

    def test_a_union_member_object_is_followed(self):
        bodies = walk.property_bodies(_root(), "either", SURFACE)
        self.assertTrue(any(walk.declares_leaf(body, "nested") for body in bodies))

    def test_an_array_property_resolves_to_its_element_type(self):
        bodies = walk.property_bodies(_root(), "items", SURFACE)
        self.assertTrue(any(walk.declares_leaf(body, "value") for body in bodies))


class MappedTypes(unittest.TestCase):
    def test_a_literal_keyed_record_expands_to_its_keys(self):
        bodies = walk.property_bodies(_root(), "occupancy", SURFACE)
        self.assertEqual(len(bodies), 1)
        for key in ("sub", "low", "lowMid"):
            self.assertTrue(walk.declares_leaf(bodies[0], key), key)
        self.assertFalse(walk.declares_leaf(bodies[0], "high"))

    def test_a_string_keyed_record_resolves_to_nothing(self):
        """An open key set is not decidable, so the caller must not count it."""
        self.assertEqual(walk.property_bodies(_root(), "bag", SURFACE), [])

    def test_union_literals_follow_one_alias_hop(self):
        self.assertEqual(walk.union_literals(SURFACE, "Band"), ["sub", "low", "lowMid"])
        self.assertEqual(walk.union_literals(SURFACE, "string"), [])


class BraceMatching(unittest.TestCase):
    def test_a_nested_block_does_not_end_its_parent(self):
        text = "interface A { a: { b: { c: number } }; d: number }"
        body = walk.named_type_bodies(text, "A")[0]
        self.assertTrue(walk.declares_leaf(body, "d"))

    def test_trivia_between_union_members_is_skipped(self):
        text = "interface A { x: /* note */ | { y: number } }"
        bodies = walk.property_bodies(text, "x", text)
        self.assertTrue(any(walk.declares_leaf(body, "y") for body in bodies))


if __name__ == "__main__":
    unittest.main()
