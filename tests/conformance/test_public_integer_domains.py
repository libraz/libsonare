"""Stdlib self-tests for the public-integer-domain checker.

Every case drives :func:`check_public_integer_domains.evaluate` -- the function
the shipping entry point calls -- rather than restating its rules, because a
class asserted through a reimplementation would only ever agree with itself.

The non-vacuity requirement is per class, not per run: each class is provoked on
its own and must produce exactly one report, so a green run means every class
still fires for its own reason instead of one loud class covering for the rest.
A floor on the real tree is pinned too, because every check passes vacuously on
an empty population.

The second half is the one a reach check most needs: a *reported* class proves
the check can fail, and proves nothing about whether it fails for the right
input. So each mechanism has a companion case on the same synthetic tree with
the guard put back, which must report nothing -- the two together are what say
the verdict tracks the code rather than the shape of the tree.

Stdlib only, and no ``libsonare`` import: this file runs under system python3
beside the checker, off the built dylib and off rye. Proving the guards it
trusts actually refuse a value is a runtime question and lives with the binding's
own suite, in ``bindings/python/tests/test_public_integer_domains.py``, which
reads its probe population out of this checker rather than restating it.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_public_integer_domains",
    Path(__file__).resolve().parent / "check_public_integer_domains.py",
)
assert _SPEC and _SPEC.loader
domains = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(domains)

NO_FLOOR: dict[str, int] = {}
NO_EXCLUSIONS: dict[str, dict[str, tuple[str, str]]] = {}

# The narrowing home a synthetic tree needs, because the vocabulary is derived
# from the tree rather than listed: with no `_runtime.py` there are no guards,
# and every case would report `unreached` for the same uninteresting reason.
HOME = """\
class SonareValueError(ValueError):
    pass


def _narrow_int(value, name, low, high):
    raise SonareValueError(name)


def _to_c_int(value, name):
    return _narrow_int(value, name, 0, 1)


def _to_c_int64(value, name):
    return _narrow_int(value, name, 0, 1)


def _resolve_enum(value, names, what, *, validate_int=True):
    raise SonareValueError(what)
"""


class _SyntheticTree(unittest.TestCase):
    """Cases that need a surface and an implementation of their own."""

    def tree(self, stub: str, implementation: str, *, home: str = HOME) -> Path:
        root = Path(tempfile.mkdtemp())
        (root / "m.pyi").write_text(stub, encoding="utf-8")
        (root / "m.py").write_text(implementation, encoding="utf-8")
        (root / "_runtime.py").write_text(home, encoding="utf-8")
        (root / "_narrowing.py").write_text("", encoding="utf-8")
        return root

    def evaluate(self, root: Path, *, exclusions=None, floor=None):
        report = domains.Report(
            root, exclusions=NO_EXCLUSIONS if exclusions is None else exclusions
        )
        return report, domains.evaluate(report, NO_FLOOR if floor is None else floor)

    def only(self, failures, fragment: str):
        """One class, and the one intended -- not merely at least the one."""
        self.assertEqual(
            len(failures),
            1,
            f"expected one failure class, got: {[heading for heading, _ in failures]}",
        )
        self.assertIn(fragment, failures[0][0])
        return failures[0][1]

    def both(self, failures, *fragments: str):
        """Exactly these classes, for the provocations that cannot fire one alone."""
        self.assertEqual(
            len(failures),
            len(fragments),
            f"expected {len(fragments)} failure classes, got: "
            f"{[heading for heading, _ in failures]}",
        )
        headings = [heading for heading, _ in failures]
        for fragment in fragments:
            self.assertTrue(
                any(fragment in heading for heading in headings),
                f"no heading carried {fragment!r}: {headings}",
            )
        return {
            fragment: lines
            for fragment in fragments
            for heading, lines in failures
            if fragment in heading
        }

    def clean(self, failures):
        self.assertEqual(
            failures, [], f"expected no report, got: {[h for h, _ in failures]}"
        )


class ShippingTreeTest(unittest.TestCase):
    """The binding itself, which is what the gate is for."""

    report: domains.Report

    @classmethod
    def setUpClass(cls) -> None:
        cls.report = domains.Report(domains.BINDING)

    def test_the_binding_reports_nothing(self) -> None:
        self.assertEqual(domains.evaluate(self.report), [])

    def test_the_binding_clears_the_floor_it_is_sized_for(self) -> None:
        counts = self.report.counts()
        for name, floor in domains.FLOORS.items():
            with self.subTest(population=name):
                self.assertGreaterEqual(counts.get(name, 0), floor)

    def test_every_published_argument_is_accounted_for(self) -> None:
        """The population's own arithmetic: no parameter falls out of the tally.

        A verdict this check forgot to count would shrink every class at once
        and leave each one individually plausible.
        """
        counts = self.report.counts()
        classes = (
            "guarded",
            "refused",
            "ctypes",
            "struct",
            "not_marshalled",
            "excused",
            "unreached",
        )
        self.assertEqual(sum(counts[name] for name in classes), counts["population"])

    def test_no_implementation_goes_unresolved(self) -> None:
        """An instrument that cannot find the code must not report cleanliness."""
        self.assertEqual(self.report.missing, [])
        self.assertEqual(self.report.resolved, self.report.counts()["functions"])


class FailureClassTest(_SyntheticTree):
    """One provocation per class, each reporting that class and no other."""

    def test_an_unguarded_argument_is_the_only_report(self) -> None:
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "def f(n):\n    return n + 1\n",
        )
        lines = self.only(self.evaluate(root)[1], "reach no guard")
        self.assertIn("m.f(n)", lines[0])

    def test_a_guarded_argument_reports_nothing(self) -> None:
        """The same tree with the guard put back, so the verdict tracks the code."""
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(n):\n"
            "    return _to_c_int(n, 'n')\n",
        )
        self.clean(self.evaluate(root)[1])

    def test_a_fold_in_front_of_the_only_guard_reports_both_classes(self) -> None:
        """Both, and not by accident: for a scalar the two classes are one defect.

        `_to_c_int(int(n))` leaves `n` reaching no mechanism *because* the fold is
        in the way, so the fold class and the unreached class are the same finding
        read twice. A provocation engineered to fire one alone would be asserting
        a separation the check does not have; the separable shape is a record
        field guarded at one marshalling site and folded at another, below.
        """
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(n):\n"
            "    return _to_c_int(int(n), 'n')\n",
        )
        reported = self.both(
            self.evaluate(root)[1], "reach no guard", "can no longer fail"
        )
        self.assertIn("int(...) on `n`", reported["can no longer fail"][0])
        self.assertIn("m.f(n)", reported["can no longer fail"][0])

    def test_an_undominated_fold_at_a_second_reader_is_the_only_report(self) -> None:
        """The separable shape: one site guards the field, another folds it first."""
        root = self.tree(
            "class R:\n    def __init__(self, n: int) -> None: ...\n",
            "import dataclasses\n\nfrom ._runtime import _to_c_int\n\n\n"
            "@dataclasses.dataclass(frozen=True)\nclass R:\n    n: int\n\n\n"
            "def pack_folded(record: R):\n    return _to_c_int(int(record.n), 'n')\n\n\n"
            "def pack_guarded(record: R):\n    return _to_c_int(record.n, 'n')\n",
        )
        report, failures = self.evaluate(root)
        lines = self.only(failures, "can no longer fail")
        self.assertIn("int(...) on `record.n`", lines[0])
        self.assertEqual([v.verdict for v in report.verdicts], ["guarded"])

    def test_a_fold_beside_a_guard_reports_nothing(self) -> None:
        """The value itself reaches the guard, so the fold beside it is dominated."""
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(n):\n"
            "    kept = int(n)\n    return _to_c_int(n, 'n'), kept\n",
        )
        report, failures = self.evaluate(root)
        self.clean(failures)
        self.assertEqual(report.counts()["fold_sites"], 1)

    def test_an_unresolved_implementation_reports_both_classes(self) -> None:
        """Coupled deliberately: a stub whose code is missing fails twice.

        Once as the function whose implementation could not be found, and once as
        every argument that was therefore never asked the question -- because the
        alternative is an argument dropping out of the tally, which is the shape
        that reads as cleanliness.
        """
        root = self.tree("def f(n: int) -> None: ...\n", "x = 1\n")
        reported = self.both(
            self.evaluate(root)[1],
            "no implementation this check could resolve",
            "reach no guard",
        )
        self.assertIn("m.f", reported["no implementation this check could resolve"][0])

    def test_a_guard_outside_the_home_is_the_only_report(self) -> None:
        """A guard the vocabulary cannot see would leave its callers unmeasured."""
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\n"
            "def _narrow_frames(value, name):\n    raise ValueError(name)\n\n\n"
            "def f(n):\n    return _to_c_int(n, 'n')\n",
        )
        lines = self.only(self.evaluate(root)[1], "outside the home")
        self.assertIn("_narrow_frames", lines[0])

    def test_a_stale_exclusion_is_the_only_report(self) -> None:
        """An entry excusing nothing goes on asserting a decision about a name."""
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(n):\n"
            "    return _to_c_int(n, 'n')\n",
        )
        exclusions = {"m.f": {"n": ("python_only", "no longer true")}}
        lines = self.only(
            self.evaluate(root, exclusions=exclusions)[1], "excused nothing"
        )
        self.assertIn("m.f(n)", lines[0])

    def test_an_exclusion_outside_the_category_set_is_the_only_report(self) -> None:
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "def f(n):\n    return n + 1\n",
        )
        exclusions = {"m.f": {"n": ("because-i-said-so", "a reason of a new shape")}}
        lines = self.only(
            self.evaluate(root, exclusions=exclusions)[1], "excused nothing"
        )
        self.assertIn("is not one of", lines[-1])

    def test_a_live_exclusion_reports_nothing(self) -> None:
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "def f(n):\n    return n + 1\n",
        )
        exclusions = {"m.f": {"n": ("python_only", "never crosses the FFI")}}
        report, failures = self.evaluate(root, exclusions=exclusions)
        self.clean(failures)
        self.assertEqual(report.counts()["excused"], 1)

    def test_a_shrunken_population_is_the_only_report(self) -> None:
        """A surface that shrank, which is a different finding from an unread root.

        The tree is readable and clean, so the floor is the only thing left to
        fire -- an empty tree would trip the unreadable-root check first, which
        answers "there is nothing here" rather than "there is less than there
        was".
        """
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(n):\n"
            "    return _to_c_int(n, 'n')\n",
        )
        lines = self.only(
            self.evaluate(root, floor={"population": 2})[1],
            "no longer finds the population",
        )
        self.assertIn("population: found 1", lines[0])

    def test_a_dead_reach_class_is_the_only_report(self) -> None:
        """A class going to zero is how a predicate stops working silently."""
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(n):\n"
            "    return _to_c_int(n, 'n')\n",
        )
        lines = self.only(
            self.evaluate(root, floor={"struct": 1})[1],
            "no longer finds the population",
        )
        self.assertIn("struct: found 0", lines[0])


class MechanismTest(_SyntheticTree):
    """Each reach class fires for its own input, and for nothing weaker."""

    def verdict(self, root: Path) -> str:
        report, _ = self.evaluate(root)
        self.assertEqual(len(report.verdicts), 1)
        return report.verdicts[0].verdict

    def test_a_value_handed_to_a_foreign_call_is_reached_by_ctypes(self) -> None:
        """argtypes refuses a fraction inside the call, with nothing spelled at it."""
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "def f(n):\n    lib = _get_lib()\n    return lib.sonare_thing(n)\n",
        )
        self.assertEqual(self.verdict(root), "ctypes")

    def test_a_struct_field_assignment_is_reached_by_struct(self) -> None:
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._cstruct import CStruct\n\n\n"
            "class S(CStruct):\n    _fields_ = [('a', 'c_int32')]\n\n\n"
            "def f(n):\n    raw = S()\n    raw.a = n\n    return raw\n",
        )
        self.assertEqual(self.verdict(root), "struct")

    def test_a_struct_array_element_is_reached_by_struct(self) -> None:
        """The spelling the marshalling loops use, whose callee is a BinOp."""
        root = self.tree(
            "def f(ns: list[int]) -> None: ...\n",
            "from ._cstruct import CStruct\n\n\n"
            "class S(CStruct):\n    _fields_ = [('a', 'c_int32')]\n\n\n"
            "def f(ns):\n    raw = (S * len(ns))()\n"
            "    for i, n in enumerate(ns):\n        raw[i].a = n\n    return raw\n",
        )
        self.assertEqual(self.verdict(root), "struct")

    def test_an_integrality_test_is_reached_by_refused(self) -> None:
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from numbers import Integral\n\n\ndef f(n):\n"
            "    if not isinstance(n, Integral):\n        raise ValueError('n')\n    return n\n",
        )
        self.assertEqual(self.verdict(root), "refused")

    def test_a_range_test_is_not_a_refusal(self) -> None:
        """`n <= 0` refuses a domain violation and says nothing about a fraction."""
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "def f(n):\n    if n <= 0:\n        raise ValueError('n')\n    return n\n",
        )
        self.assertEqual(self.verdict(root), "unreached")

    def test_an_unaccepted_record_is_not_marshalled(self) -> None:
        """A record nothing takes becomes no C type, so there is nothing to refuse."""
        root = self.tree(
            "class R:\n    def __init__(self, n: int) -> None: ...\n",
            "import dataclasses\n\n\n@dataclasses.dataclass(frozen=True)\n"
            "class R:\n    n: int\n",
        )
        self.assertEqual(self.verdict(root), "not_marshalled")

    def test_an_accepted_record_is_answered_where_its_field_is_read(self) -> None:
        root = self.tree(
            "class R:\n    def __init__(self, n: int) -> None: ...\n",
            "import dataclasses\n\nfrom ._runtime import _to_c_int\n\n\n"
            "@dataclasses.dataclass(frozen=True)\nclass R:\n    n: int\n\n\n"
            "def pack(record: R):\n    return _to_c_int(record.n, 'n')\n",
        )
        self.assertEqual(self.verdict(root), "guarded")

    def test_a_field_no_reader_reads_is_not_marshalled(self) -> None:
        """Keyed on (record, field): the record travels, this field does not."""
        root = self.tree(
            "class R:\n    def __init__(self, n: int, unread: int) -> None: ...\n",
            "import dataclasses\n\nfrom ._runtime import _to_c_int\n\n\n"
            "@dataclasses.dataclass(frozen=True)\nclass R:\n    n: int\n    unread: int\n\n\n"
            "def pack(record: R):\n    return _to_c_int(record.n, 'n')\n",
        )
        report, _ = self.evaluate(root)
        answers = {v.parameter.name: v.verdict for v in report.verdicts}
        self.assertEqual(answers, {"n": "guarded", "unread": "not_marshalled"})


class FlowFidelityTest(_SyntheticTree):
    """What the value is followed through, and what it deliberately is not."""

    def verdict(self, root: Path) -> str:
        report, _ = self.evaluate(root)
        self.assertEqual(len(report.verdicts), 1)
        return report.verdicts[0].verdict

    def test_a_value_is_followed_into_a_local_helper(self) -> None:
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\n"
            "def _pack(value):\n    return _to_c_int(value, 'value')\n\n\n"
            "def f(n):\n    return _pack(n)\n",
        )
        self.assertEqual(self.verdict(root), "guarded")

    def test_a_value_is_followed_through_super(self) -> None:
        root = self.tree(
            "class B:\n    def __init__(self, n: int) -> None: ...\n"
            "class D(B):\n    def __init__(self, n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\n"
            "class B:\n    def __init__(self, n):\n        _to_c_int(n, 'n')\n\n\n"
            "class D(B):\n    def __init__(self, n):\n        super().__init__(n)\n",
        )
        report, _ = self.evaluate(root)
        self.assertEqual({v.verdict for v in report.verdicts}, {"guarded"})

    def test_a_value_is_followed_out_of_a_container(self) -> None:
        """Sound: a loop body applies the same code to every element."""
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(n):\n"
            "    fields = {'n': n}\n"
            "    return {k: _to_c_int(v, k) for k, v in fields.items()}\n",
        )
        self.assertEqual(self.verdict(root), "guarded")

    def test_a_value_is_followed_through_a_helper_that_returns_it(self) -> None:
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\n"
            "def _passthrough(value):\n    return value\n\n\n"
            "def f(n):\n    return _to_c_int(_passthrough(n), 'n')\n",
        )
        self.assertEqual(self.verdict(root), "guarded")

    def test_a_value_past_a_splat_has_no_position(self) -> None:
        """Attribution ends where the positions stop being knowable."""
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\n"
            "def _pack(*values):\n    return [_to_c_int(v, 'v') for v in values]\n\n\n"
            "def f(n):\n    return _pack(*[n])\n",
        )
        self.assertEqual(self.verdict(root), "unreached")

    def test_an_unbounded_enum_resolver_is_not_a_guard(self) -> None:
        """`validate_int=False` removes the bound rather than widening it."""
        root = self.tree(
            "def f(n: int | str) -> None: ...\n",
            "from ._runtime import _resolve_enum\n\n\ndef f(n):\n"
            "    return _resolve_enum(n, {'a': 1}, 'thing', validate_int=False)\n",
        )
        self.assertEqual(self.verdict(root), "unreached")

    def test_a_bounded_enum_resolver_is_a_guard(self) -> None:
        """The companion: the same call with the bound left on."""
        root = self.tree(
            "def f(n: int | str) -> None: ...\n",
            "from ._runtime import _resolve_enum\n\n\ndef f(n):\n"
            "    return _resolve_enum(n, {'a': 1}, 'thing')\n",
        )
        self.assertEqual(self.verdict(root), "guarded")


class PopulationTest(_SyntheticTree):
    """Which annotations admit a caller-supplied integer, and which do not."""

    def admitted(self, annotation: str) -> bool:
        root = self.tree(
            f"def f(n: {annotation}) -> None: ...\n",
            "def f(n):\n    return n\n",
        )
        return (
            domains.Report(root, exclusions=NO_EXCLUSIONS).counts()["population"] == 1
        )

    def test_the_annotations_an_integer_can_be_passed_at(self) -> None:
        for annotation in (
            "int",
            "int | None",
            "Optional[int]",
            "Sequence[int]",
            "list[int]",
            "Iterable[int]",
            "int | str",
            "'int'",
            "Mapping[str, int]",
        ):
            with self.subTest(annotation=annotation):
                self.assertTrue(self.admitted(annotation))

    def test_the_annotations_it_cannot(self) -> None:
        for annotation in ("float", "str", "bool", "Point", "Sequence[float]", "None"):
            with self.subTest(annotation=annotation):
                self.assertFalse(self.admitted(annotation))


class HandoffAttributionTest(_SyntheticTree):
    """Where a credit was earned, and whose guard is allowed to earn it.

    These are the cases a one-function synthetic tree cannot express, and they
    are the ones that discriminate: with a module-wide key, a guard anywhere in
    the file answers for an argument anywhere else in it, and every per-class
    case above still passes. So each of them pairs a reach with a *severed*
    variant that must go unreached, and asserts the hop distance -- the number
    that says whether a reach count is a measurement or an upper bound.

    Modelled on `_engine_pages.py`: `FileClipPageProvider.supply_page` has no
    guard of its own and hands its `page_index` to the inherited
    `ClipPageProvider.supply`, which does.
    """

    HOME_STUB = (
        "class Base:\n"
        "    def supply(self, page_index: int) -> None: ...\n"
        "class Derived(Base):\n"
        "    def supply_page(self, page_index: int) -> bool: ...\n"
    )

    def impl(self, tail: str) -> str:
        return (
            "from ._runtime import _to_c_int64\n\n\n"
            "class Base:\n"
            "    def supply(self, page_index):\n"
            "        return _to_c_int64(page_index, 'page_index')\n\n\n"
            "class Derived(Base):\n"
            "    def supply_page(self, page_index):\n"
            "        page = page_index\n" + tail
        )

    def answers(self, tail: str) -> dict[str, tuple[str, int]]:
        root = self.tree(self.HOME_STUB, self.impl(tail))
        report = domains.Report(root, exclusions=NO_EXCLUSIONS)
        return {v.parameter.qualname: (v.verdict, v.hops) for v in report.verdicts}

    def test_a_subclass_argument_is_credited_only_through_the_call(self) -> None:
        """Reached, and reported as one hop away rather than as a local guard."""
        answers = self.answers("        return self.supply(page)\n")
        self.assertEqual(answers["Base.supply"], ("guarded", 0))
        self.assertEqual(answers["Derived.supply_page"], ("guarded", 1))

    def test_severing_the_call_leaves_the_subclass_argument_unreached(self) -> None:
        """The discriminator: the base's guard is untouched, only the call is gone.

        With the guard lookup keyed on the module rather than on this function,
        `Base.supply`'s guard would still answer for `Derived.supply_page` here
        and this case would read exactly like the one above.
        """
        answers = self.answers("        return self.supply(0)\n")
        self.assertEqual(answers["Base.supply"], ("guarded", 0))
        self.assertEqual(answers["Derived.supply_page"][0], "unreached")

    def test_renaming_the_argument_does_not_change_the_credit(self) -> None:
        """The other discriminator: the credit follows the call, not the spelling."""
        stub = (
            "class Base:\n"
            "    def supply(self, page_index: int) -> None: ...\n"
            "class Derived(Base):\n"
            "    def supply_page(self, requested_page: int) -> bool: ...\n"
        )
        implementation = (
            "from ._runtime import _to_c_int64\n\n\n"
            "class Base:\n"
            "    def supply(self, page_index):\n"
            "        return _to_c_int64(page_index, 'page_index')\n\n\n"
            "class Derived(Base):\n"
            "    def supply_page(self, requested_page):\n"
            "        return self.supply(requested_page)\n"
        )
        root = self.tree(stub, implementation)
        report = domains.Report(root, exclusions=NO_EXCLUSIONS)
        answers = {v.parameter.qualname: (v.verdict, v.hops) for v in report.verdicts}
        self.assertEqual(answers["Derived.supply_page"], ("guarded", 1))

    def test_a_sibling_functions_struct_holder_does_not_lend_its_guard(self) -> None:
        """A name is not an owner: `raw` in one function is not `raw` in another."""
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._cstruct import CStruct\n\n\n"
            "class S(CStruct):\n    _fields_ = [('a', 'c_int32')]\n\n\n"
            "def other():\n    raw = S()\n    return raw\n\n\n"
            "def f(n):\n    raw.a = n\n",
        )
        report = domains.Report(root, exclusions=NO_EXCLUSIONS)
        self.assertEqual([v.verdict for v in report.verdicts], ["unreached"])

    def test_a_module_level_struct_holder_does_lend_its_guard(self) -> None:
        """The companion: a module-level binding IS in scope for the function."""
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._cstruct import CStruct\n\n\n"
            "class S(CStruct):\n    _fields_ = [('a', 'c_int32')]\n\n\n"
            "raw = S()\n\n\n"
            "def f(n):\n    raw.a = n\n",
        )
        report = domains.Report(root, exclusions=NO_EXCLUSIONS)
        self.assertEqual([v.verdict for v in report.verdicts], ["struct"])

    def test_a_same_named_argument_in_a_sibling_function_is_not_credited(self) -> None:
        """The (qualname, parameter) discriminator, at its smallest.

        Two module-level functions, one parameter name, one guard. A lookup keyed
        on the bare name would let `guarded_one`'s guard answer for
        `unguarded_one` and report nothing at all; every one-function case in this
        file would still pass, which is exactly why this one exists.
        """
        root = self.tree(
            "def guarded_one(page_index: int) -> None: ...\n"
            "def unguarded_one(page_index: int) -> None: ...\n",
            "from ._runtime import _to_c_int64\n\n\n"
            "def guarded_one(page_index):\n"
            "    return _to_c_int64(page_index, 'page_index')\n\n\n"
            "def unguarded_one(page_index):\n    return page_index + 1\n",
        )
        report, failures = self.evaluate(root)
        answers = {v.parameter.qualname: v.verdict for v in report.verdicts}
        self.assertEqual(
            answers, {"guarded_one": "guarded", "unguarded_one": "unreached"}
        )
        lines = self.only(failures, "reach no guard")
        self.assertEqual(len(lines), 1)
        self.assertIn("m.unguarded_one(page_index)", lines[0])

    def test_a_same_named_argument_on_a_sibling_method_is_not_credited(self) -> None:
        """The same discriminator one level in: two methods of one class."""
        root = self.tree(
            "class K:\n"
            "    def guarded_one(self, page_index: int) -> None: ...\n"
            "    def unguarded_one(self, page_index: int) -> None: ...\n",
            "from ._runtime import _to_c_int64\n\n\n"
            "class K:\n"
            "    def guarded_one(self, page_index):\n"
            "        return _to_c_int64(page_index, 'page_index')\n\n"
            "    def unguarded_one(self, page_index):\n"
            "        return page_index + 1\n",
        )
        report, _ = self.evaluate(root)
        answers = {v.parameter.qualname: v.verdict for v in report.verdicts}
        self.assertEqual(
            answers, {"K.guarded_one": "guarded", "K.unguarded_one": "unreached"}
        )

    def test_a_same_named_argument_on_an_unrelated_class_is_not_credited(self) -> None:
        """And across classes, where no inheritance or call path exists at all."""
        root = self.tree(
            "class Guarded:\n"
            "    def take(self, page_index: int) -> None: ...\n"
            "class Unrelated:\n"
            "    def take(self, page_index: int) -> None: ...\n",
            "from ._runtime import _to_c_int64\n\n\n"
            "class Guarded:\n"
            "    def take(self, page_index):\n"
            "        return _to_c_int64(page_index, 'page_index')\n\n\n"
            "class Unrelated:\n"
            "    def take(self, page_index):\n        return page_index + 1\n",
        )
        report, _ = self.evaluate(root)
        answers = {v.parameter.qualname: v.verdict for v in report.verdicts}
        self.assertEqual(
            answers, {"Guarded.take": "guarded", "Unrelated.take": "unreached"}
        )

    def test_removing_a_body_guard_moves_the_distance_counters(self) -> None:
        """The signature of the real-tree ablation, pinned as a test.

        Removing `FileClipPageProvider.supply_page`'s own guard leaves the class
        totals unchanged -- correctly, because the value still reaches the
        inherited guard unfolded, one call later -- so the totals are not where
        that edit shows up. It shows up in the distance counters, and those are
        the ones an ablation of this shape has to be read against.
        """
        guarded_body = (
            "        page = _to_c_int64(page_index, 'page_index')\n"
            "        return self.supply(page)\n"
        )
        raw_body = "        page = page_index\n        return self.supply(page)\n"

        def counts(body: str) -> dict[str, int]:
            root = self.tree(
                self.HOME_STUB,
                "from ._runtime import _to_c_int64\n\n\n"
                "class Base:\n"
                "    def supply(self, page_index):\n"
                "        return _to_c_int64(page_index, 'page_index')\n\n\n"
                "class Derived(Base):\n"
                "    def supply_page(self, page_index):\n" + body,
            )
            return domains.Report(root, exclusions=NO_EXCLUSIONS).counts()

        before, after = counts(guarded_body), counts(raw_body)
        for name in ("guarded", "unreached", "population"):
            with self.subTest(counter=name):
                self.assertEqual(before[name], after[name])
        self.assertEqual(before["answered_in_body"] - after["answered_in_body"], 1)
        self.assertEqual(
            after["answered_via_handoff"] - before["answered_via_handoff"], 1
        )


class KeyedMemberEntryTest(_SyntheticTree):
    """The category for a parameter whose integers arrive at keys, not at names.

    Kept apart from `guarded_past_the_ffi` because the two record different
    facts, and one of them is checkable here: an entry of this category is
    excused whichever side of the FFI its members are refused on, because what
    puts them out of reach is the key, not the guard's address. Both sides are
    pinned, so a future edit cannot collapse the categories without a red test.
    """

    def tree_with(self, annotation: str, body: str) -> Path:
        return self.tree(
            f"def f(n: {annotation}) -> None: ...\n",
            body,
        )

    def test_an_entry_is_printed_and_counted_apart_from_the_other_category(
        self,
    ) -> None:
        root = self.tree_with("dict[str, int]", "def f(n):\n    return n\n")
        exclusions = {
            "m.f": {
                "n": (
                    "integral_member_is_a_key",
                    "key nFft, refused past the FFI by assign_int_param in src/x.h; measured",
                )
            }
        }
        report, failures = self.evaluate(root, exclusions=exclusions)
        self.clean(failures)
        counts = report.counts()
        self.assertEqual(counts["keyed_member_entries"], 1)
        self.assertEqual(counts["past_the_ffi_entries"], 0)
        self.assertEqual(counts["bag_members"], 1)
        entry = report.keyed_member_entries()[0]
        self.assertIn("m.f(n)", entry)
        self.assertIn("src/x.h", entry)

    def test_the_two_categories_do_not_answer_for_each_other(self) -> None:
        """A `guarded_past_the_ffi` entry is never counted as a keyed member."""
        root = self.tree_with("int", "def f(n):\n    return n + 1\n")
        exclusions = {
            "m.f": {"n": ("guarded_past_the_ffi", "refused in src/x.h; measured")}
        }
        report, failures = self.evaluate(root, exclusions=exclusions)
        self.clean(failures)
        counts = report.counts()
        self.assertEqual(counts["past_the_ffi_entries"], 1)
        self.assertEqual(counts["keyed_member_entries"], 0)
        self.assertEqual(counts["bag_members"], 0)

    def test_moving_one_entry_between_categories_swaps_both_counters(self) -> None:
        """The transition, which asserting each category alone does not reach.

        Two entries of different categories on different fixtures agree with a
        pair of counters that never talk to each other. Re-filing the SAME entry
        is what shows they are one partition: one count falls by exactly one as
        the other rises by exactly one, off an entry that stays live throughout,
        so neither category can answer in the other's place.
        """
        root = self.tree(
            "def f(n: dict[str, int]) -> None: ...\n",
            "def f(n):\n    return n\n",
        )
        filed = {
            category: domains.Report(
                root, exclusions={"m.f": {"n": (category, "measured; see the note")}}
            ).counts()
            for category in ("integral_member_is_a_key", "guarded_past_the_ffi")
        }
        keyed = filed["integral_member_is_a_key"]
        ffi = filed["guarded_past_the_ffi"]

        self.assertEqual(
            (keyed["keyed_member_entries"], keyed["past_the_ffi_entries"]), (1, 0)
        )
        self.assertEqual(
            (ffi["keyed_member_entries"], ffi["past_the_ffi_entries"]), (0, 1)
        )
        self.assertEqual(
            ffi["keyed_member_entries"] - keyed["keyed_member_entries"], -1
        )
        self.assertEqual(ffi["past_the_ffi_entries"] - keyed["past_the_ffi_entries"], 1)
        # Live on both sides, so the swap is a reclassification and not one
        # entry going stale while an unrelated one appears.
        self.assertEqual(keyed["excused"], 1)
        self.assertEqual(ffi["excused"], 1)
        self.assertEqual(keyed["unreached"], 0)
        self.assertEqual(ffi["unreached"], 0)

    def test_an_entry_retires_when_the_member_stops_being_a_key(self) -> None:
        """Its stated retirement condition, and the only one that can satisfy it.

        Not a guard moving to either side -- the bag gaining a named parameter
        this check can carry a question about.
        """
        exclusions = {
            "m.f": {
                "n_fft": (
                    "integral_member_is_a_key",
                    "key nFft, refused past the FFI by assign_int_param in src/x.h",
                )
            }
        }
        named = self.tree(
            "def f(n_fft: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(n_fft):\n"
            "    return _to_c_int(n_fft, 'n_fft')\n",
        )
        lines = self.only(
            self.evaluate(named, exclusions=exclusions)[1], "excused nothing"
        )
        self.assertIn("m.f(n_fft)", lines[0])

    def test_a_bag_with_no_entry_is_still_reported(self) -> None:
        """The category is a decision per entry, never a pass on the shape."""
        root = self.tree_with("dict[str, int]", "def f(n):\n    return n\n")
        report, failures = self.evaluate(root)
        self.assertEqual(report.counts()["keyed_member_entries"], 0)
        self.only(failures, "reach no guard")


class BypassTest(_SyntheticTree):
    """A guard that covers the body it sits in, and one that does not.

    Two-sided on purpose, and the second side is the one that decides whether the
    rule is usable: a body routinely opens with an early exit that reads the
    value (`if value is None: continue`, `if count <= 0: return []`) and bypasses
    nothing, because no value survives it. Only a branch that WRITES the value
    out and leaves is a detour around the guard. A rule that cannot separate
    those two reports the whole tree and is worth nothing.
    """

    def bag(self, branch: str) -> Path:
        return self.tree(
            "def f(options: dict[str, int]) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(options):\n"
            "    out = {}\n"
            "    for name, value in options.items():\n"
            + branch
            + "        out[name] = _to_c_int(value, name)\n"
            "    return out\n",
        )

    def test_a_branch_that_writes_the_value_out_is_a_failure(self) -> None:
        root = self.bag(
            "        if isinstance(value, bool):\n"
            "            out[name] = value\n"
            "            continue\n"
        )
        report, failures = self.evaluate(root)
        self.assertEqual(report.counts()["bypass"], 1)
        lines = self.only(failures, "do not cover every path")
        self.assertIn("isinstance(value, bool)", lines[0])
        self.assertIn("out[name] = value", lines[0])
        # Still `guarded`: the guard is there, it just does not cover this path.
        self.assertEqual([v.verdict for v in report.verdicts], ["guarded"])

    def test_a_branch_that_consumes_nothing_is_not_a_failure(self) -> None:
        """`if value is None: continue` -- read, exited, and nothing written out."""
        root = self.bag("        if value is None:\n            continue\n")
        report, failures = self.evaluate(root)
        self.assertEqual(report.counts()["conditional"], 1)
        self.assertEqual(report.counts()["bypass"], 0)
        self.clean(failures)

    def test_an_early_return_that_consumes_nothing_is_not_a_failure(self) -> None:
        """The other spelling the tree uses: `if count <= 0: return []`."""
        root = self.tree(
            "def f(count: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(count):\n"
            "    if count <= 0:\n        return []\n"
            "    return _to_c_int(count, 'count')\n",
        )
        report, failures = self.evaluate(root)
        self.assertEqual(report.counts()["conditional"], 1)
        self.assertEqual(report.counts()["bypass"], 0)
        self.clean(failures)

    def test_a_branch_that_forwards_the_value_is_a_failure(self) -> None:
        """Handed to a call rather than assigned: the same detour, another spelling."""
        root = self.bag(
            "        if isinstance(value, bool):\n"
            "            out[name] = _flag(value)\n"
            "            continue\n"
        )
        report, failures = self.evaluate(root)
        self.assertEqual(report.counts()["bypass"], 1)
        self.only(failures, "do not cover every path")

    def test_every_earlier_branch_is_examined_not_the_first(self) -> None:
        """The benign branch above the detour must not hide it.

        A body that opens with `if value is None: continue` and then writes the
        value out on a second branch is the real shape, and a search that stops
        at the first match reads the harmless one and reports nothing -- which is
        how this rule first measured zero on a tree that had a bypass in it.
        """
        root = self.bag(
            "        if value is None:\n            continue\n"
            "        if isinstance(value, bool):\n"
            "            out[name] = value\n"
            "            continue\n"
        )
        report, failures = self.evaluate(root)
        self.assertEqual(report.counts()["conditional"], 2)
        self.assertEqual(report.counts()["bypass"], 1)
        self.only(failures, "do not cover every path")

    def test_a_branch_below_the_guard_is_not_a_bypass(self) -> None:
        """Nothing above the guard to leave from, so there is nothing to report."""
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(n):\n"
            "    narrowed = _to_c_int(n, 'n')\n"
            "    if n > 10:\n        return narrowed\n    return None\n",
        )
        report, failures = self.evaluate(root)
        self.assertEqual(report.counts()["conditional"], 0)
        self.clean(failures)

    def test_a_dead_branch_search_is_the_only_report(self) -> None:
        """`bypass: 0` has to mean nothing was found, not nothing was looked at."""
        root = self.bag("        if value is None:\n            continue\n")
        lines = self.only(
            self.evaluate(root, floor={"conditional": 2})[1],
            "no longer finds the population",
        )
        self.assertIn("conditional: found 1", lines[0])


class FailureTextTest(_SyntheticTree):
    """What the unreached heading is allowed to assert.

    It used to say the value "is folded onto another legal one instead of
    refused", which was measured false for every mapping parameter here: their
    members are refused, by name, where this check cannot read. A failure text
    that asserts something its author measured as untrue is worse than one that
    says nothing, so the heading now states what was not found and names both
    places an answer can be hiding.
    """

    def heading(self) -> str:
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "def f(n):\n    return n + 1\n",
        )
        failures = self.evaluate(root)[1]
        self.assertEqual(len(failures), 1)
        return failures[0][0]

    def test_the_heading_does_not_assert_a_fold(self) -> None:
        self.assertNotIn(
            "is folded onto another legal one instead of refused", self.heading()
        )

    def test_the_heading_says_what_was_not_found_and_where_else_to_look(self) -> None:
        heading = self.heading()
        self.assertIn("THAT THIS CHECK CAN SEE", heading)
        self.assertIn("past the FFI", heading)
        self.assertIn("mapping key", heading)


class UnreadableRootTest(_SyntheticTree):
    """An instrument that read nothing must not report cleanliness."""

    def test_a_missing_root_is_the_only_report(self) -> None:
        report = domains.Report(Path("/nonexistent-root-for-this-test"), exclusions={})
        lines = self.only(
            domains.evaluate(report, NO_FLOOR), "read no published surface at all"
        )
        self.assertIn("not a directory", lines[0])

    def test_a_root_without_stubs_is_the_only_report(self) -> None:
        root = Path(tempfile.mkdtemp())
        (root / "m.py").write_text("def f(n):\n    return n\n", encoding="utf-8")
        report = domains.Report(root, exclusions={})
        lines = self.only(
            domains.evaluate(report, NO_FLOOR), "read no published surface at all"
        )
        self.assertIn("holds no *.pyi", lines[0])

    def test_a_root_without_implementations_is_the_only_report(self) -> None:
        root = Path(tempfile.mkdtemp())
        (root / "m.pyi").write_text("def f(n: int) -> None: ...\n", encoding="utf-8")
        report = domains.Report(root, exclusions={})
        lines = self.only(
            domains.evaluate(report, NO_FLOOR), "read no published surface at all"
        )
        self.assertIn("holds no *.py", lines[0])

    def test_stubs_declaring_no_integer_argument_are_the_only_report(self) -> None:
        """Read, parsed, and empty: a different finding from an unread root."""
        root = Path(tempfile.mkdtemp())
        (root / "m.pyi").write_text("def f(x: float) -> None: ...\n", encoding="utf-8")
        (root / "m.py").write_text("def f(x):\n    return x\n", encoding="utf-8")
        report = domains.Report(root, exclusions={})
        lines = self.only(
            domains.evaluate(report, NO_FLOOR), "read no published surface at all"
        )
        self.assertIn("declare no parameter", lines[0])

    def test_an_empty_root_fails_with_the_shipping_floors_too(self) -> None:
        """Belt and braces: the floors would have caught it, and still do."""
        report = domains.Report(Path("/nonexistent-root-for-this-test"), exclusions={})
        self.assertNotEqual(domains.evaluate(report), [])


class AliasAndBagTest(_SyntheticTree):
    """The population's own reach: what an unresolved alias hides, and what a bag is.

    An annotation naming a `TypeAlias` mentions no `int` until the alias is
    followed, so the parameter is simply absent -- the population under-reaches
    and reports the smaller number as if it were the surface. These cases pin
    both halves: the alias is followed, and what it resolves to decides the class.
    """

    def population(self, annotation: str, aliases: str = "") -> list[tuple[str, str]]:
        root = self.tree(
            "from typing import TypeAlias\n"
            + aliases
            + f"def f(n: {annotation}) -> None: ...\n",
            "def f(n):\n    return n\n",
        )
        report = domains.Report(root, exclusions=NO_EXCLUSIONS)
        return [(v.parameter.name, v.verdict) for v in report.verdicts]

    def test_an_alias_is_followed_into_the_population(self) -> None:
        """Without this the parameter is absent, and absence reads as clean."""
        self.assertEqual(
            self.population("Ref | None", "Ref: TypeAlias = int | str\n"),
            [("n", "unreached")],
        )

    def test_an_alias_that_admits_no_integer_stays_out(self) -> None:
        """The companion, so the resolution is not simply sweeping everything in."""
        self.assertEqual(self.population("Ref", "Ref: TypeAlias = float | str\n"), [])

    def test_an_alias_chain_is_followed_transitively(self) -> None:
        self.assertEqual(
            self.population(
                "Bag | None",
                "Value: TypeAlias = float | int | bool\nBag: TypeAlias = dict[str, Value]\n",
            ),
            [("n", "unreached")],
        )

    def test_a_bag_is_reported_rather_than_passed_on_its_shape(self) -> None:
        """The whole point of the bag count being informational.

        A mapping-shaped annotation says this check cannot ask about the keys. It
        does not say the keys are guarded, and passing on it would answer for
        every bag written from now on without anyone deciding so -- which is the
        reach problem this check exists to refuse, wearing a derived class.
        """
        root = self.tree(
            "def f(n: dict[str, int]) -> None: ...\n",
            "def f(n):\n    return n\n",
        )
        report, failures = self.evaluate(root)
        self.assertEqual([v.verdict for v in report.verdicts], ["unreached"])
        self.assertEqual(report.counts()["bag_members"], 1)
        self.only(failures, "reach no guard")

    def test_a_named_integer_beside_a_bag_is_not_counted_as_a_bag(self) -> None:
        """The predicate strips mappings and asks again, so a name still counts."""
        root = self.tree(
            "def f(n: int | dict[str, int]) -> None: ...\n",
            "def f(n):\n    return n\n",
        )
        report, _ = self.evaluate(root)
        self.assertEqual(report.counts()["bag_members"], 0)

    def test_a_bag_reached_by_a_guard_reports_nothing(self) -> None:
        """Counted as a bag and still answered: the two are independent."""
        root = self.tree(
            "def f(n: dict[str, int]) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(n):\n"
            "    return _to_c_int(n, 'n')\n",
        )
        report, failures = self.evaluate(root)
        self.assertEqual([v.verdict for v in report.verdicts], ["guarded"])
        self.assertEqual(report.counts()["bag_members"], 1)
        self.clean(failures)

    def test_an_alias_two_stubs_disagree_on_is_the_only_report(self) -> None:
        """Resolved to whichever was read first, the population would be a guess."""
        root = self.tree(
            "from typing import TypeAlias\nRef: TypeAlias = int\n"
            "def f(n: Ref) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(n):\n"
            "    return _to_c_int(n, 'n')\n",
        )
        (root / "other.pyi").write_text(
            "from typing import TypeAlias\nRef: TypeAlias = str\n", encoding="utf-8"
        )
        (root / "other.py").write_text("x = 1\n", encoding="utf-8")
        report = domains.Report(root, exclusions=NO_EXCLUSIONS)
        lines = self.only(domains.evaluate(report, NO_FLOOR), "declared differently")
        self.assertIn("Ref:", lines[0])


class PastTheFfiEntryTest(_SyntheticTree):
    """The category that records a guard this instrument cannot see."""

    def test_an_entry_is_printed_as_well_as_counted(self) -> None:
        root = self.tree(
            "def f(n: int) -> None: ...\n",
            "def f(n):\n    return n + 1\n",
        )
        exclusions = {
            "m.f": {
                "n": (
                    "guarded_past_the_ffi",
                    "refused by assign_int_param in src/x.h; measured",
                )
            }
        }
        report, failures = self.evaluate(root, exclusions=exclusions)
        self.clean(failures)
        entries = report.past_the_ffi_entries()
        self.assertEqual(len(entries), 1)
        self.assertIn("m.f(n)", entries[0])
        self.assertIn("src/x.h", entries[0])
        self.assertEqual(report.counts()["past_the_ffi_entries"], 1)

    def test_an_entry_retires_when_the_python_side_guards(self) -> None:
        """The fix it retires on, and the one it does not, are different edits.

        A C++ fix leaves the Python route identical and the entry keeps excusing;
        only a narrowing here makes the parameter reached and the entry stale.
        """
        exclusions = {
            "m.f": {"n": ("guarded_past_the_ffi", "refused in src/x.h; measured")}
        }
        guarded = self.tree(
            "def f(n: int) -> None: ...\n",
            "from ._runtime import _to_c_int\n\n\ndef f(n):\n"
            "    return _to_c_int(n, 'n')\n",
        )
        lines = self.only(
            self.evaluate(guarded, exclusions=exclusions)[1], "excused nothing"
        )
        self.assertIn("m.f(n)", lines[0])


if __name__ == "__main__":
    unittest.main()
