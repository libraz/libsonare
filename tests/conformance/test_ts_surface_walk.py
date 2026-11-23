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
  single: [Other];
  pair: [first: Inner, Derived];
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

    def test_a_tuple_property_resolves_to_each_element_type(self):
        single = walk.property_bodies(_root(), "single", SURFACE)
        self.assertTrue(any(walk.declares_leaf(body, "mode") for body in single))
        pair = walk.property_bodies(_root(), "pair", SURFACE)
        self.assertTrue(any(walk.declares_leaf(body, "value") for body in pair))
        self.assertTrue(any(walk.declares_leaf(body, "extra") for body in pair))


class IndexedPaths(unittest.TestCase):
    def test_a_numeric_segment_under_an_array_property_is_the_element(self):
        bodies = walk.resolve_blocks(SURFACE, ["items", "0"])
        self.assertTrue(any(walk.declares_leaf(body, "value") for body in bodies))

    def test_a_numeric_segment_under_a_non_array_property_is_unreached(self):
        self.assertEqual(walk.resolve_blocks(SURFACE, ["inner", "0"]), [])

    def test_a_leading_numeric_segment_is_unreached(self):
        self.assertEqual(walk.resolve_blocks(SURFACE, ["0", "items"]), [])

    def test_array_typed_leaves_are_told_from_scalars(self):
        body = "{ list?: number[]; other?: Array<string>; one?: number }"
        self.assertTrue(walk.declares_array_leaf(body, "list"))
        self.assertTrue(walk.declares_array_leaf(body, "other"))
        self.assertFalse(walk.declares_array_leaf(body, "one"))
        self.assertFalse(walk.declares_array_leaf(body, "absent"))

    def test_a_path_without_indices_resolves_as_before(self):
        bodies = walk.resolve_blocks(SURFACE, ["inner"])
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


WRAPPED = """
export interface Inner { value: number; mode: string }
export interface Other { mode: string; tail: number }
export type Stage = ({ stage: 'a' } & Omit<Inner, 'mode'>) | ({ stage: 'b' } & Other);
export type ByMode = { [M in 'x' | 'y']: { slot: string; mode: M } }['x' | 'y'];
export interface Root {
  required: Required<Inner>;
  stages: Stage[];
  narrowed: Omit<Inner, 'mode'> | Other;
  indexed: Other['mode'] | Holder['other'];
  mapped: ByMode[];
}
export interface Holder { other: { picked: number } }
"""


def _wrapped_root() -> str:
    return walk.named_type_bodies(WRAPPED, "Root")[0]


class TypeOperators(unittest.TestCase):
    def test_an_identity_wrapper_resolves_to_its_argument(self):
        bodies = walk.property_bodies(_wrapped_root(), "required", WRAPPED)
        self.assertTrue(any(walk.declares_leaf(body, "value") for body in bodies))

    def test_a_parenthesized_intersection_contributes_each_member(self):
        bodies = walk.property_bodies(_wrapped_root(), "stages", WRAPPED)
        self.assertTrue(any(walk.declares_leaf(body, "stage") for body in bodies))
        self.assertTrue(any(walk.declares_leaf(body, "tail") for body in bodies))

    def test_a_key_removing_operator_resolves_to_nothing_and_the_union_continues(self):
        """`Omit` would hand back the keys it removes; the next alternative still counts."""
        bodies = walk.property_bodies(_wrapped_root(), "narrowed", WRAPPED)
        self.assertFalse(any(walk.declares_leaf(body, "value") for body in bodies))
        self.assertTrue(any(walk.declares_leaf(body, "tail") for body in bodies))


    def test_an_indexed_access_resolves_to_that_member_alone(self):
        bodies = walk.property_bodies(_wrapped_root(), "indexed", WRAPPED)
        self.assertTrue(any(walk.declares_leaf(body, "picked") for body in bodies))
        self.assertFalse(any(walk.declares_leaf(body, "other") for body in bodies))

    def test_a_mapped_type_resolves_to_its_value_type(self):
        bodies = walk.property_bodies(_wrapped_root(), "mapped", WRAPPED)
        self.assertTrue(any(walk.declares_leaf(body, "slot") for body in bodies))


class LeafLevel(unittest.TestCase):
    def test_a_leaf_of_a_nested_object_is_not_a_leaf_of_its_parent(self):
        self.assertFalse(walk.declares_leaf(_root(), "nested"))
        self.assertTrue(walk.declares_leaf(_root(), "either"))

    def test_a_name_inside_a_comment_or_a_signature_is_not_a_leaf(self):
        body = "{ /** e.g. hidden: 1 */ run(arg: number): void; // gone: 2\n kept: number }"
        self.assertFalse(walk.declares_leaf(body, "hidden"))
        self.assertFalse(walk.declares_leaf(body, "arg"))
        self.assertFalse(walk.declares_leaf(body, "gone"))
        self.assertTrue(walk.declares_leaf(body, "kept"))


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
