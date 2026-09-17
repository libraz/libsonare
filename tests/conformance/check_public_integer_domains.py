#!/usr/bin/env python3
"""Hold every public integer argument of the Python binding to the C type it becomes.

The question is not whether a module contains a conversion spelling -- that is
``check_python_narrowing_scope.py``, which reads the implementation and asks
where its conversions live. This one starts from the **published surface** and
asks the opposite question, per argument: a caller reads ``n_fft: int`` in the
``.pyi``, passes ``4096.5``, and something has to refuse it, because every route
that does not refuse it folds the value onto another legal one -- a ctypes
integer constructor wraps, a ``c_float`` saturates to an infinity, an
``astype(np.int32)`` truncates, and the C ABI cannot tell any of the three from
a number the caller meant.

A scope check cannot answer it. A file can be full of ``_to_c_int`` calls and
still hand one argument straight to a foreign call; the argument is what is
exposed, so the argument is the unit.

THE POPULATION IS DISCOVERED, THE VOCABULARY IS DERIVED
-------------------------------------------------------
Both halves are read out of the tree rather than listed here, because a list is
what goes stale the day a facade grows a function:

* **Population.** Every parameter of every ``def`` in ``*.pyi`` whose annotation
  admits a caller-supplied integer -- ``int``, ``int | None``, ``Sequence[int]``,
  and any union or generic containing ``int``, with ``TypeAlias`` names followed
  transitively, because the facade declares most of its unions once and refers to
  them by name. A new facade argument enters the population the moment its stub
  is written.
* **Vocabulary.** Every ``def`` in the narrowing home matching the project's own
  guard naming (see ``GUARD_PREFIXES``). A guard added there joins the
  vocabulary without this file being touched, and a guard named *outside* those
  families is not silently trusted: a parameter routed only through one reaches
  nothing this check recognises, so it reports as ``unreached`` and fails.

REACH IS AN OUTPUT AND UN-REACH IS A FAILURE
--------------------------------------------
An instrument that cannot find the code reports its blind spot as cleanliness,
so nothing here is allowed to drop quietly:

* A ``.pyi`` function whose implementation cannot be resolved is a failure, not
  an omission -- ``implementations_missing`` is required to be 0.
* A parameter reaching none of the mechanisms below is a failure, named as
  ``module.qualname(param)``.
* Every count the verdict rests on is printed, so a predicate that narrowed past
  a whole family shows up as a number that moved rather than as continued green.

The mechanisms, all of which end at a refusal rather than at a conversion:

1. **guard** -- the value flows into a vocabulary guard.
2. **refused** -- the value is handed to ``operator.index``, or tested for
   integrality by a branch that raises. Narrow on purpose: a range test refuses
   a domain violation and says nothing about ``4096.5``, so only a test that
   decides integrality counts, that being the property a fold destroys without
   leaving a trace.
3. **ctypes** -- the value is handed to a ``lib.<symbol>(...)`` call, where the
   declared ``argtypes`` refuses a float outright. (Refuses a *float*; it does
   not refuse an out-of-range integer, which is what the narrowing-scope check's
   Scan C is for. Here it counts as reached because the argument cannot arrive
   as a fraction.)
4. **struct** -- the value is assigned to, or constructed into, a field of a
   ``CStruct`` subclass -- one instance or an element of an array of them --
   whose ``__setattr__`` narrows against the field's own declared C type.
5. **record** -- the parameter belongs to a frozen record's generated
   ``__init__``, where it becomes a field rather than an argument; the question
   moves to where that field is read, and is answered at the marshalling site.

The value is followed to those mechanisms through what the tree actually does
with it: assignment, a conditional or ``or`` default, ``min``/``max``/``abs``, a
container it is put into and iterated out of, a helper that returns it
unchanged, and a hand-off to a module-level function, a ``self`` method, a
``super()`` method or a function imported inside the caller to break a cycle.
Anything further is not followed, and what is not followed reads as
``unreached`` -- a failure -- rather than as answered.

WHERE A CREDIT WAS EARNED IS PART OF THE ANSWER
-----------------------------------------------
A guard found in the declared function's own body and one found three calls away
are both real refusals -- the value crosses each hand-off by identity, so the
fraction is still there when the guard sees it -- but they are claims about
paths of different length, and a total that merges them is an upper bound
wearing a measurement's clothes. So ``answered_in_body`` and
``answered_via_handoff`` are reported apart, with ``deepest_handoff``.

Attribution is keyed on the resolved function, never on a name. Two things
follow, and the second is the one worth stating: a guard on an argument that
*happens to share a name* with this one answers nothing here, and neither does a
struct holder bound in a sibling function -- but a guard genuinely reached by
passing this value into another function does answer, even when that function is
on another class.

Which makes an ablation of this shape readable only against the distance
counters. Deleting `FileClipPageProvider.supply_page`'s own guard leaves
`guarded` and `unreached` exactly where they were, and that is the right answer:
`supply_page` hands its `page_index` to the inherited `ClipPageProvider.supply`,
which narrows it, so the value is still refused -- one call later. What the edit
moves is `answered_in_body` down by one and `answered_via_handoff` up by one. A
reader who takes the class totals as the whole output will read that edit as
having changed nothing.

WHAT THIS CHECK DOES NOT ASK, AND CANNOT
----------------------------------------
Four limits, stated because a reader should not have to infer any of them from
a count that looks complete.

**Ordering.** Whether the guard runs *before the value is used*. The question
here is whether a value the C type cannot hold is refused rather than folded, and
a guard at the end of a function answers it. It does not answer whether the
arithmetic, the seek and the read above it ran on an unchecked number first.

**A fold that happens past the FFI.** This reads Python. A value marshalled out
as a ``double`` and folded onto an integer by a ``static_cast<int>`` in C++ is
invisible here in both directions: the fold does not appear in the fold
inventory, and neither does the guard that later replaces it. So a fix landing in
the core does not retire a ``guarded_past_the_ffi`` entry, and never will --
only a narrowing on the Python side does, which is what the category's own note
says. An entry there is a statement about this instrument's reach.

**A parameter that is a bag rather than a name.** The unit is the declared
parameter. ``params: MasteringParams | None`` is one population member carrying a
whole key space (``nFft``, ``kernelSize``, ``truePeakOversample``, ``stages``, …),
and there is no declared parameter for any of those keys to ask a question
about. ``bag_members`` counts them, so the size of what cannot be asked is an
output -- but it is a count of *bags*, not of the fields inside them, and it
decides nothing. A bag is answered like anything else or it is reported, and each
one that is excused carries a measurement of its own: passing on the *shape*
would turn "I cannot see a guard" into "there is no finding" for every bag
written from now on, which is this check's own reach problem wearing a derived
class.

**A bypass written in the caller rather than beside the guard.** The flow asks
whether the value reaches a mechanism on *some* path, not on every path, and
what closes most of that gap is ``bypass``: an earlier branch in the guard's own
body that writes the value out and leaves is a failure, because the guard then
answers for every value except the ones that took the branch. The shape it
catches::

    if isinstance(value, bool):
        params[key] = value          # written out, unnarrowed
        continue
    params[key] = _validate_c_int_field(fn_name, value, name)

``True`` is an ``int`` subclass, so it never fails a range check -- which is why
the narrowing family refuses a ``bool`` outright -- and in that order it never
reaches the narrowing at all.

The limit is that the branch search is **body-local**. It reads the frame the
mechanism was found in, so a value written out and handed on one frame up, in the
caller, is outside it: the credit crosses the hand-off and the branch that
skipped the guard is never examined. What is checked is that a guard covers the
body it sits in, not that the value took no other route to C on the way there.

The first two compound: the mastering assistant's ``nFft``, ``hopLength`` and
``true_peak_oversample`` were cast with ``static_cast<int>`` for as long as the
mixing assistant's were, three lines below a full integrality check on
``targetPlatform`` that spelled out why truncating was wrong. This check reaches
the entry points that carry them -- they are ``param_bag`` members, which is how
a reader is pointed at the right functions -- and it cannot reach the keys.

A FOLD MUST BE DOMINATED
------------------------
``int(x)``, ``round(x)``, ``x // n``, ``math.floor(x)``, ``np.rint(x)`` and
``.astype(...)`` each produce an integer from whatever they were given, so a
guard placed *after* one is handed a value that can no longer fail it: the
fraction is already gone. Every fold applied to a population parameter is
therefore required to have a guard on the **same parameter** -- not on the fold's
result -- somewhere in the same function. ``undominated`` must be 0.

WHAT IS DERIVED RATHER THAN EXCUSED
-----------------------------------
A record class no function anywhere accepts is never marshalled, so its
``__init__`` parameter becomes no C type at all and there is nothing for a guard
to refuse. That is a fact about the tree, not a decision about an entry, so it
is *derived* and reported in its own counter (``not_marshalled``) rather than
written into EXCLUSIONS -- an entry would keep excusing the parameter after the
record started being accepted. Deriving it needs its own vacuity guard, because
a reader analysis that resolved nothing would reclassify the whole population as
"never marshalled" and go green: the floors below pin the reader population's
own size.
"""

from __future__ import annotations

import argparse
import ast
import sys
import typing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BINDING = ROOT / "bindings" / "python" / "src" / "libsonare"

# The narrowing home. Not a filter on the vocabulary -- the vocabulary is every
# matching `def` in these files -- but the answer to "where does a guard live",
# which is what lets a guard defined anywhere else be reported (see
# `misplaced_guards`) instead of silently trusted. The CLI uses `_reject_*` for
# refusals that have nothing to do with a C type, which is why the home is a
# place rather than a naming rule alone.
GUARD_HOME = ("_runtime.py", "_narrowing.py")

# The project's own naming for a value-refusing helper, read off the home rather
# than asserted: the conversion readers (`_to_c_*`), the root predicates
# (`_narrow_*`), the array refusals (`_reject_*`), the field and option
# validators (`_validate_*`, `_require_*`) and the closed-domain resolvers
# (`_resolve_enum`, and the `_*_value` / `_*_values` family that wraps it).
#
# A guard named outside these families is invisible here, and that is contained
# rather than trusted: a parameter routed only through an unnamed guard reaches
# nothing this check recognises, so it reports as `unreached` and FAILS. The
# naming convention is therefore self-enforcing -- an addition either takes a
# family name or turns the gate red.
GUARD_PREFIXES = (
    "_to_c_",
    "_narrow_",
    "_reject_",
    "_validate_",
    "_require_",
    "_resolve_",
)
GUARD_SUFFIXES = ("_value", "_values")

# The prefixes a guard defined outside the home is *reported* under. Only the
# two families that exist for nothing but a C conversion, because the rest are
# ordinary English: the CLI layer spells an argparse refusal `_reject_*` and
# `_validate_*`, and neither converts anything. So this check is narrower than
# the vocabulary on purpose, and the difference is covered by the same
# containment as an unnamed guard -- a value routed through a resolver written
# outside the home reaches nothing recognised, so it reports as `unreached`.
HOME_ONLY_PREFIXES = ("_to_c_", "_narrow_")

# `_resolve_enum`'s own escape hatch. It bounds an integer against the ordinals
# its table spells; `validate_int=False` removes that bound rather than widening
# it, so a resolver handed it is not a guard and neither is a call site that
# passes it. Nothing in the tree does today -- this is here so the day one does,
# the credit stops instead of the check staying green.
UNBOUNDED_ENUM = ("validate_int", False)

# The mapping spellings a param bag is written as. A value inside one of these
# reaches an integral C field through a string key, which is not a declared
# parameter and so is not a unit this check can ask a question about.
MAPPING_TYPES = ("dict", "Dict", "Mapping", "MutableMapping")

# The struct base whose `__setattr__` narrows an integer or `c_float` field
# against the field's own declared type. Mechanism 3.
STRUCT_BASE = "CStruct"

# How the loaded shared library is spelled at a call site. A call through one of
# these reaches the declared `argtypes`, which refuses a non-integral value.
LIB_NAMES = ("lib", "_lib")

# Builtins that turn a caller's number into an integer, silently. A guard behind
# one of these is handed a value that already fits.
FOLD_BUILTINS = ("int", "round")
FOLD_ATTRIBUTES = ("floor", "ceil", "trunc", "rint", "astype", "round_", "fix")

# Calls that cannot change the answer to this check's question: none of them can
# make a non-integral value integral, or bring an out-of-range one into range, so
# a guard reached through one still sees what it was put there to refuse.
PASSTHROUGH_CALLS = ("min", "max", "abs")

# Calls that build a container out of a value. Tracked because a value put into
# one and taken out again by a loop is guarded at the loop, and the loop applies
# the same code to every element.
SEQUENCE_BUILDERS = ("tuple", "list", "asarray", "array", "asanyarray", "fromiter")

# `operator.index` is a refusal in itself: it raises on anything that is not an
# integer, so a value handed to it cannot arrive as a fraction. Read as
# `operator.index(...)` rather than as a bare `index`, which is a list method.
INDEX_CALL = ("operator", "index")

# What makes a `raise` behind an `if` an answer to *this* question rather than to
# some other one. `n_fft <= 0` refuses a domain violation and says nothing about
# `4096.5`; a test naming one of these decides integrality, which is what the
# fold would otherwise destroy silently.
INTEGRALITY_TESTS = (
    "Integral",
    "SupportsIndex",
    "is_integer",
    "index",
    "int",
    "integer",
)

# The exception classes a refusal is spelled with.
REFUSAL_ERRORS = ("SonareValueError", "ValueError", "TypeError", "OverflowError")

# How deep a value is followed through local helper calls. Three hops covers
# every route in this tree (facade -> options builder -> marshaller); the limit
# exists so a mutually recursive pair terminates, and a value that outruns it is
# reported unreached rather than assumed fine.
MAX_HOPS = 3

# Floors on the populations this check's verdict rests on. Two empty sets agree
# perfectly: with the resolver broken every parameter reads as "implementation
# missing", but with the *record reader* analysis broken every record parameter
# reads as the benign `not_marshalled`, and the gate would go green over a
# surface it stopped measuring. Each number is what was there when the floor was
# written, so a family disappearing fails instead of shrinking quietly.
# A floor per reach class as well as per population, because a class going to
# zero is how a predicate stops working without the total moving: the parameters
# it used to answer for get answered by another class, or by an exclusion, and
# the verdict stays green over a mechanism nothing exercises.
#
# `ctypes` has no floor because it is legitimately 0: on this tree every public
# integer argument is refused before the foreign call, so nothing relies on
# `argtypes` alone. A zero counter is a class no run exercises, so it is proven
# elsewhere -- an ablation that hands a value to `lib.<symbol>(...)` raw is
# classified `ctypes`, which is what shows the class is live rather than dead
# code. If the count ever rises off 0 that is a finding, not an improvement.
FLOORS: dict[str, int] = {
    "population": 900,
    "functions": 300,
    "vocabulary": 12,
    "guarded": 500,
    "refused": 8,
    "struct": 40,
    "bag_members": 8,
    # Sized on what is there now. A body-local branch search that stopped
    # matching would take this to 0 with no total moving and no finding lost,
    # and `bypass: 0` would then mean "nothing was looked at" rather than
    # "nothing was found" -- the same shape the reach floors above are sized
    # against. `bypass` itself carries no floor: 0 is the correct answer on this
    # tree and any value above it is a failure, not a population.
    "conditional": 8,
    "marshalled_records": 40,
    "record_field_readers": 30,
    "fold_sites": 10,
}

# A category is the shape of the reason, so a reason that fits none of them is
# real drift rather than a new kind of excuse.
CATEGORIES = (
    # The argument names a Python-side quantity that never crosses the FFI.
    "python_only",
    # The argument is validated against a closed domain (an enum, a table key, a
    # length derived from another argument) before any conversion.
    "domain_checked",
    # The surface is not a caller entry point: an internal packer or a callback
    # signature the library itself fills in.
    "not_caller_supplied",
    # A NAMED argument whose refusal is real and was measured, but happens in
    # C++, past the FFI, where a Python AST reader cannot see it. Never silent:
    # every entry is printed on every run with the measurement and the C++ site
    # behind it, because "the core checks it" is the easiest unfalsifiable excuse
    # to write and the only thing separating this category from one is evidence.
    #
    # It retires on the fix that would make it unnecessary -- a narrowing on the
    # Python side, which is what every other named integer argument on this
    # surface has -- and not on the C++ fix, which leaves the Python route
    # identical. That asymmetry is the point: an entry here records a reach this
    # instrument does not have, not a defect waiting on someone.
    "guarded_past_the_ffi",
    # The parameter is a mapping, and the integers it carries arrive at string
    # KEYS rather than at names. This check's unit is the declared parameter, so
    # there is no unit for `nFft` or `stages` to be; the member cannot be
    # expressed here at all, let alone answered.
    #
    # Which is a different fact from where its guard lives, and the distinction
    # is the whole reason this category is not the one above: the bags excused
    # under it are refused on BOTH sides of the FFI -- the mastering processor
    # keys in C++, `targetPlatform` in Python -- and every one of them is
    # invisible for the same reason, which is neither. An entry therefore states
    # what refuses each member AND where, per entry and per measurement; "it is a
    # mapping" is the reason it cannot be asked, never the reason it is fine.
    #
    # It retires when the member stops arriving at a key -- when the bag gains
    # named parameters this check can carry a question about. No guard moving to
    # either side retires it, and an entry claiming otherwise would state a
    # retirement condition nothing can satisfy.
    "integral_member_is_a_key",
)

# The three reasons the mastering bags carry, named once because each is shared
# by the entry points that reach the same C++ reader. Written out rather than
# generated: the audit retires an entry, and an entry has to be a decision
# someone made about a named surface.
_BAG = (
    "integral_member_is_a_key",
    "keys fftSize / kernelSize / stages and the rest of the processor set; each "
    "refused PAST the FFI, by detail::assign_field -> assign_int_param in "
    "src/mastering/api/param_field_tables.h; measured on eq.linearPhase "
    "kernelSize (127.5) and effects.modulation.phaser stages (4.5, 2**40)",
)
_PROFILE_BAG = (
    "integral_member_is_a_key",
    "keys nFft / hopLength / truePeakOversample; each refused PAST the FFI, by "
    "assign_int_param in src/mastering/assistant/config_from_params.h; measured "
    "on nFft (512.5) and truePeakOversample (2.5)",
)
_ASSISTANT_BAG = (
    "integral_member_is_a_key",
    "its one integral key is targetPlatform, refused BEFORE the FFI, in Python, "
    "by _assistant_params in bindings/python/src/libsonare/_mastering_offline.py "
    "(it takes a delivery-target name, so 1, 1.5 and 999 are all refused alike); "
    "measured. Filed here rather than under guarded_past_the_ffi because the "
    "guard is on this side: what keeps the member out of reach is that it is a "
    "key, not where it is checked",
)

# Exclusions, as data. Each entry must excuse something: an entry that excused
# nothing still asserts a reviewed decision about a name, so the next argument
# to take that name inherits the blessing unexamined.
#
# An excused parameter leaves the population, which takes its folds with it --
# the fold inventory is collected while following the population, so a value
# nothing follows contributes no fold site. That is deliberate: the reason an
# entry gives is a claim about the whole route the value travels, folds
# included, so recording the same decision twice would let the two copies part.
EXCLUSIONS: dict[str, dict[str, tuple[str, str]]] = {
    "analyzer.Mixer.__init__": {
        "handle": (
            "not_caller_supplied",
            (
                "an opaque native handle `from_scene_json` obtained from the core, not a "
                "quantity; an arbitrary integer here is a bad pointer, which no integer "
                "domain check can answer for"
            ),
        ),
        "sample_rate": (
            "not_caller_supplied",
            (
                "`from_scene_json` is the constructor a caller uses and narrows this with "
                "`_to_c_int` before handing it on; the copy kept here is only read back "
                "into MixerStereoResult.sample_rate"
            ),
        ),
        "block_size": (
            "not_caller_supplied",
            (
                "narrowed by `from_scene_json` as above; the copy kept here is only read as "
                "the Python-side bound on a process block's frame count"
            ),
        ),
    },
    # THE MASTERING PARAM BAGS.
    #
    # `params: MasteringParams | None` is `dict[str, float | int | bool]`, so it
    # admits an integer -- at a string key, not at a name. The keys carry integral
    # C fields (`fftSize`, `kernelSize`, `stages`, `nFft`, `truePeakOversample`),
    # and every one of them is assigned through
    # `mastering::api::assign_int_param` / `detail::assign_field`, which refuses a
    # fraction and an out-of-range value by name. Past the FFI, so invisible here.
    #
    # Not excused on that shape -- a mapping-shaped annotation is not evidence of
    # anything, and passing on it would answer for every bag written from now on
    # without anyone deciding so. One entry per published entry point, each
    # against a measurement:
    #
    #   eq.linearPhase   kernelSize  control 63 vs 127 DISCRIMINATES
    #                                127.5 -> "kernelSize must be a whole number"
    #                    fftSize     4096.5 -> "fftSize must be a whole number"
    #   effects.modulation.phaser
    #                    stages      control 4 vs 8 DISCRIMINATES
    #                                4.5   -> "stages must be a whole number"
    #                                2**40 -> "stages is out of range"
    #   audio profile    nFft        control 512 vs 1024 DISCRIMINATES
    #                                512.5 -> "nFft must be a whole number"
    #                    truePeakOversample  2.5 -> "must be a whole number"
    #
    # And the control that decides how to read an ACCEPTED: unknown keys are
    # dropped by design, so `multiband.compressor` takes `fftSize=4096.5` happily
    # while its control does not move. A probe picked there would have read
    # "accepted, therefore folded" off a key the processor never looks at. Which
    # is also why a refusal is the stronger observation of the two here:
    # `eq.linearPhase`'s own `fftSize` control is inert over the first 96 samples,
    # and the refusal is what says the key is read at all.
    "analyzer.mastering_process": {
        "params": _BAG,
    },
    "analyzer.mastering_process_stereo": {
        "params": _BAG,
    },
    "analyzer.mastering_pair_process": {
        "params": _BAG,
    },
    "analyzer.mastering_pair_analyze": {
        "params": _BAG,
    },
    "analyzer.mastering_stereo_analyze": {
        "params": _BAG,
    },
    "audio.Audio.mastering_process": {
        "params": _BAG,
    },
    "analyzer.mastering_audio_profile": {
        "params": _PROFILE_BAG,
    },
    "analyzer.mastering_audio_profile_stereo": {
        "params": _PROFILE_BAG,
    },
    "analyzer.mastering_assistant_suggest": {
        "params": _ASSISTANT_BAG,
    },
    "analyzer.mastering_assistant_suggest_stereo": {
        "params": _ASSISTANT_BAG,
    },
    "types.ClipPageRequest.__init__": {
        "sample": (
            "domain_checked",
            (
                "`supply_request` divides it by the page size before `_narrow_int` sees the "
                "quotient, and `//` returns a float when given one -- measured: 1000.5 // 512 "
                "is 1.0, which `_narrow_int` refuses for not being SupportsIndex. The fold "
                "moves the value without removing the property the guard keys on"
            ),
        ),
    },
}


class Parameter(typing.NamedTuple):
    """One published integer argument: where it is declared and what it is called."""

    module: str
    qualname: str
    name: str
    annotation: str
    line: int
    # True when every integer this parameter admits arrives as a value in a
    # mapping rather than at a name. Computed with the surface, where the alias
    # table is, and read after the flow has had its say.
    bag_only: bool

    @property
    def key(self) -> str:
        return f"{self.module}.{self.qualname}"

    @property
    def display(self) -> str:
        return f"{self.module}.{self.qualname}({self.name})"


class Reach(typing.NamedTuple):
    """How a parameter's value was answered for, by what, and how far away.

    `hops` is 0 when the mechanism is in the declared function's own body, and
    counts the hand-offs crossed otherwise. It is reported, because the
    difference decides whether a reach count is a measurement or an upper bound:
    a credit earned several calls away is still a real refusal -- the value
    crosses each hand-off unfolded, by identity -- but it is a claim about a
    longer path, and a reader is owed the distinction rather than one total.
    """

    verdict: str
    detail: str
    hops: int = 0
    # The line the mechanism was found on, so a branch can be asked whether it
    # sits above it. Stamped by `_direct`; a hand-off keeps the callee's line,
    # which is the frame the branch search runs in.
    line: int = 0


UNREACHED = Reach("unreached", "")


def admits_integer(
    annotation: ast.expr | None, aliases: dict[str, ast.expr] | None = None
) -> bool:
    """Whether a caller may pass an integer at this annotation.

    Keyed on the *names* the annotation mentions rather than its source text, so
    a parameter typed `Point` is not swept in by the letters in its name, and a
    string annotation is parsed rather than matched.

    `TypeAlias` names are followed, transitively, because the facade declares
    most of its unions once and refers to them by name: without this,
    `params: MasteringParams` mentions no `int` and the parameter is absent from
    a population that claims to be discovered. A cycle stops at the name that
    closes it rather than recursing.
    """
    if annotation is None:
        return False
    aliases = aliases or {}
    names: set[str] = set()
    seen: set[str] = set()

    def walk(node: ast.AST) -> None:
        if isinstance(node, ast.Name):
            names.add(node.id)
            if node.id in aliases and node.id not in seen:
                seen.add(node.id)
                walk(aliases[node.id])
        elif isinstance(node, ast.Attribute):
            names.add(node.attr)
        elif isinstance(node, ast.Constant) and isinstance(node.value, str):
            try:
                walk(ast.parse(node.value, mode="eval").body)
            except SyntaxError:
                return
        for child in ast.iter_child_nodes(node):
            walk(child)

    walk(annotation)
    return "int" in names


def _alias_table(
    trees: dict[str, ast.Module],
) -> tuple[dict[str, ast.expr], list[str]]:
    """Every `TypeAlias` the stubs declare, and any name two of them disagree on.

    One table across the stubs rather than one per module, because the facade
    declares the same alias in several of them (`FloatSamples` in three) and a
    parameter annotated with one is the same population member wherever it sits.
    A name two stubs define *differently* is reported instead of resolved: with
    one table there would be no way to say which definition a signature meant,
    and picking whichever was read first is the mistake the population's own
    keying exists to avoid.
    """
    table: dict[str, ast.expr] = {}
    source: dict[str, tuple[str, str]] = {}
    conflicts: list[str] = []
    for module, tree in sorted(trees.items()):
        for node in tree.body:
            if isinstance(node, ast.AnnAssign):
                if not isinstance(node.target, ast.Name) or node.value is None:
                    continue
                if ast.unparse(node.annotation) != "TypeAlias":
                    continue
                name, value = node.target.id, node.value
            elif isinstance(node, ast.Assign) and isinstance(
                node.value, (ast.Subscript, ast.BinOp, ast.Name)
            ):
                targets = [t for t in node.targets if isinstance(t, ast.Name)]
                if len(targets) != 1:
                    continue
                name, value = targets[0].id, node.value
            else:
                continue
            spelling = ast.unparse(value)
            if name in source and source[name][1] != spelling:
                conflicts.append(
                    f"{name}: {source[name][0]}.pyi says {source[name][1]}, "
                    f"{module}.pyi says {spelling}"
                )
                continue
            table[name] = value
            source[name] = (module, spelling)
    return table, conflicts


def admits_only_through_a_bag(
    annotation: ast.expr | None, aliases: dict[str, ast.expr] | None = None
) -> bool:
    """Whether the only integer this annotation admits is a *value in a mapping*.

    `params: MasteringParams | None`, where `MasteringParams` is
    `dict[str, float | int | bool]`, admits an integer -- but not at a name. The
    fields it carries (`nFft`, `stages`, `truePeakOversample`) are string keys,
    so this check's unit, the declared parameter, has one member standing for a
    whole key space and no way to ask a question about any key in it.

    Decided by removing the mappings and asking again: if what is left admits no
    integer, every integer this parameter can carry arrives through a bag.
    """
    if annotation is None or not admits_integer(annotation, aliases):
        return False
    return not admits_integer(_without_mappings(annotation, aliases or {}), aliases)


def _without_mappings(annotation: ast.expr, aliases: dict[str, ast.expr]) -> ast.expr:
    """`annotation` with every mapping subscript replaced by a name nothing matches."""

    class Strip(ast.NodeTransformer):
        def visit_Subscript(self, node: ast.Subscript) -> ast.expr:  # noqa: N802
            if _callee_name(node.value) in MAPPING_TYPES:
                return ast.Name(id="__stripped__", ctx=ast.Load())
            return typing.cast(ast.expr, self.generic_visit(node))

        def visit_Name(self, node: ast.Name) -> ast.expr:  # noqa: N802
            alias = aliases.get(node.id)
            if alias is None:
                return node
            return Strip().visit(_copy(alias))

    return Strip().visit(_copy(annotation))


def _copy(node: ast.expr) -> ast.expr:
    return typing.cast(ast.expr, ast.parse(ast.unparse(node), mode="eval").body)


def _parameters(node: ast.FunctionDef | ast.AsyncFunctionDef) -> list[ast.arg]:
    args = node.args
    every = [*args.posonlyargs, *args.args, *args.kwonlyargs]
    if args.vararg is not None:
        every.append(args.vararg)
    if args.kwarg is not None:
        every.append(args.kwarg)
    return [arg for arg in every if arg.arg not in ("self", "cls")]


class Surface:
    """The published surface: every `.pyi` function and its integer parameters."""

    def __init__(self, binding: Path) -> None:
        self.binding = binding
        self.parameters: list[Parameter] = []
        self.functions: set[tuple[str, str]] = set()
        # Kept, so an empty population can be told apart from an unread root.
        self.stubs: list[Path] = sorted(binding.glob("*.pyi"))
        trees = {
            path.stem: ast.parse(path.read_text(encoding="utf-8"))
            for path in self.stubs
        }
        self.aliases, self.alias_conflicts = _alias_table(trees)
        for module, tree in trees.items():
            self._visit(module, tree, "")

    def _visit(self, module: str, node: ast.AST, prefix: str) -> None:
        for child in getattr(node, "body", []):
            if isinstance(child, ast.ClassDef):
                self._visit(module, child, f"{prefix}{child.name}.")
            elif isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
                admitted = [
                    arg
                    for arg in _parameters(child)
                    if admits_integer(arg.annotation, self.aliases)
                ]
                if not admitted:
                    continue
                qualname = f"{prefix}{child.name}"
                self.functions.add((module, qualname))
                for arg in admitted:
                    self.parameters.append(
                        Parameter(
                            module,
                            qualname,
                            arg.arg,
                            ast.unparse(arg.annotation) if arg.annotation else "",
                            arg.lineno,
                            admits_only_through_a_bag(arg.annotation, self.aliases),
                        )
                    )


class Tree:
    """The implementation tree, with the two lookups the resolver needs.

    Names are resolved through each module's own imports rather than through a
    flat index of the whole package: two modules may define the same helper name,
    and a flat index would answer with whichever was read first and credit a
    guard the facade never reaches.
    """

    def __init__(self, binding: Path) -> None:
        self.binding = binding
        self.modules: dict[str, ast.Module] = {}
        for path in sorted(binding.glob("*.py")):
            self.modules[path.stem] = ast.parse(path.read_text(encoding="utf-8"))
        self._namespaces: dict[str, dict[str, object]] = {}
        self.struct_classes = self._struct_classes()

    def namespace(self, module: str) -> dict[str, object]:
        """`name -> node | (module, name)` for everything the module exposes."""
        cached = self._namespaces.get(module)
        if cached is not None:
            return cached
        space: dict[str, object] = {}
        self._namespaces[module] = space
        tree = self.modules.get(module)
        if tree is None:
            return space
        # The facade modules re-export with `import *`, so a star has to be
        # followed or the whole analysis surface resolves to nothing -- and
        # "nothing" is the shape that would read as a clean sweep.
        stars: list[str] = []
        for node in tree.body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                space[node.name] = (module, node)
            elif isinstance(node, ast.ImportFrom) and node.level == 1 and node.module:
                for alias in node.names:
                    if alias.name == "*":
                        stars.append(node.module)
                    else:
                        space[alias.asname or alias.name] = (node.module, alias.name)
        for target in stars:
            for name, entry in self.namespace(target).items():
                space.setdefault(name, entry)
        return space

    def resolve(
        self, module: str, name: str, _seen: frozenset[str] = frozenset()
    ) -> object | None:
        """The definition `name` refers to inside `module`, following re-exports."""
        if module in _seen:
            return None
        entry = self.namespace(module).get(name)
        if entry is None:
            return None
        target, payload = typing.cast(tuple, entry)
        if isinstance(payload, str):
            return self.resolve(target, payload, _seen | {module})
        return (target, payload)

    def method(self, module: str, owner: str, name: str) -> tuple[str, ast.AST] | None:
        """A method on `owner`, searched through its base classes."""
        found = self.resolve(module, owner)
        if found is None:
            return None
        home, node = typing.cast(tuple, found)
        if not isinstance(node, ast.ClassDef):
            return None
        for child in node.body:
            if (
                isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef))
                and child.name == name
            ):
                return (home, child)
        for base in node.bases:
            base_name = (
                base.id if isinstance(base, ast.Name) else getattr(base, "attr", None)
            )
            if base_name is None or base_name == owner:
                continue
            inherited = self.method(home, base_name, name)
            if inherited is not None:
                return inherited
        return None

    def inherited(
        self, module: str, owner: str, name: str
    ) -> tuple[str, ast.AST] | None:
        """A method `super().<name>()` reaches: the bases only, never `owner` itself."""
        found = self.resolve(module, owner)
        if found is None:
            return None
        home, node = typing.cast(tuple, found)
        if not isinstance(node, ast.ClassDef):
            return None
        for base in node.bases:
            base_name = (
                base.id if isinstance(base, ast.Name) else getattr(base, "attr", None)
            )
            if base_name is None or base_name == owner:
                continue
            reached = self.method(home, base_name, name)
            if reached is not None:
                return reached
        return None

    def record(self, module: str, owner: str) -> tuple[str, ast.ClassDef] | None:
        """`owner` as a dataclass whose `__init__` the decorator generates."""
        found = self.resolve(module, owner)
        if found is None:
            return None
        home, node = typing.cast(tuple, found)
        if not isinstance(node, ast.ClassDef):
            return None
        decorated = any(
            "dataclass" in ast.unparse(decorator) for decorator in node.decorator_list
        )
        return (home, node) if decorated else None

    def _struct_classes(self) -> frozenset[str]:
        """Every class whose bases reach STRUCT_BASE, so a field write narrows."""
        bases: dict[str, set[str]] = {}
        for tree in self.modules.values():
            for node in ast.walk(tree):
                if isinstance(node, ast.ClassDef):
                    bases.setdefault(node.name, set()).update(
                        base.id
                        if isinstance(base, ast.Name)
                        else getattr(base, "attr", "")
                        for base in node.bases
                    )
        reached = {STRUCT_BASE}
        changed = True
        while changed:
            changed = False
            for name, parents in bases.items():
                if name not in reached and parents & reached:
                    reached.add(name)
                    changed = True
        return frozenset(reached)


def _unbounds_enum(node: ast.AST) -> bool:
    """Whether this function hands `_resolve_enum` its own bound-removing flag."""
    name, disabled = UNBOUNDED_ENUM
    return any(
        isinstance(child, ast.Call)
        and any(
            keyword.arg == name
            and isinstance(keyword.value, ast.Constant)
            and keyword.value.value is disabled
            for keyword in child.keywords
        )
        for child in ast.walk(node)
    )


def _unbounds_enum_call(call: ast.Call) -> bool:
    """The same flag, read at one call rather than over a whole function."""
    name, disabled = UNBOUNDED_ENUM
    return any(
        keyword.arg == name
        and isinstance(keyword.value, ast.Constant)
        and keyword.value.value is disabled
        for keyword in call.keywords
    )


def vocabulary(tree: Tree) -> dict[str, ast.FunctionDef]:
    """Every guard the narrowing home defines, keyed by name."""
    found: dict[str, ast.FunctionDef] = {}
    for module in (Path(name).stem for name in GUARD_HOME):
        for node in tree.modules.get(module, ast.Module(body=[], type_ignores=[])).body:
            if not isinstance(node, ast.FunctionDef):
                continue
            named = node.name.startswith(GUARD_PREFIXES) or node.name.endswith(
                GUARD_SUFFIXES
            )
            if named and not _unbounds_enum(node):
                found[node.name] = node
    return found


def misplaced_guards(tree: Tree) -> list[str]:
    """A guard defined outside the home, which the vocabulary cannot see."""
    home = {Path(name).stem for name in GUARD_HOME}
    return [
        f"{module}.py:{node.lineno}  def {node.name}"
        for module, parsed in sorted(tree.modules.items())
        if module not in home
        for node in ast.walk(parsed)
        if isinstance(node, ast.FunctionDef)
        and node.name.startswith(HOME_ONLY_PREFIXES)
    ]


class Fold(typing.NamedTuple):
    """One integer-producing expression applied to a value a caller supplied.

    Carries the function and the tracked spelling as well as its own line,
    because domination is a question about *that* value in *that* function -- a
    guard on something else nearby does not answer it.
    """

    module: str
    function_line: int
    tracked: str
    line: int
    spelling: str
    # Which published argument the fold was found while following. Carried so a
    # fold can be attributed to the surface it belongs to, and so an excused
    # argument takes its own folds out of the inventory with it.
    subject: str

    @property
    def display(self) -> str:
        return (
            f"{self.module}.py:{self.line}  {self.spelling} on `{self.tracked}`"
            f"  <- {self.subject}"
        )


def _fold_spelling(node: ast.AST) -> str | None:
    """The fold `node` performs, or None. Keyed on the spelling, not the result."""
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.FloorDiv):
        return "//"
    if isinstance(node, ast.Call):
        if isinstance(node.func, ast.Name) and node.func.id in FOLD_BUILTINS:
            return f"{node.func.id}(...)"
        if isinstance(node.func, ast.Attribute) and node.func.attr in FOLD_ATTRIBUTES:
            return f".{node.func.attr}(...)"
    return None


class Conditional(typing.NamedTuple):
    """A credit an earlier branch in the same body can leave before reaching.

    `stores` is the whole distinction: `if value is None: continue` reads the
    value and exits, and bypasses nothing, because no value survives it. `if
    isinstance(value, bool): params[key] = value; continue` writes the value out
    and skips what was going to narrow it, so the guard below answers for every
    value except the ones that took this branch.
    """

    subject: str
    module: str
    branch_line: int
    mechanism_line: int
    test: str
    stores: bool
    source: str

    @property
    def display(self) -> str:
        first = "; ".join(self.source.splitlines()[:2])
        return (
            f"{self.module}.py:{self.branch_line} `if {self.test}: {first}` "
            f"leaves before the guard at :{self.mechanism_line}  <- {self.subject}"
        )


def _callee_name(callee: ast.AST) -> str | None:
    if isinstance(callee, ast.Name):
        return callee.id
    if isinstance(callee, ast.Attribute):
        return callee.attr
    return None


def _is_index_call(callee: ast.AST) -> bool:
    """Whether this is `operator.index(...)`, which refuses a non-integer itself."""
    holder, attribute = INDEX_CALL
    return (
        isinstance(callee, ast.Attribute)
        and callee.attr == attribute
        and isinstance(callee.value, ast.Name)
        and callee.value.id == holder
    )


def _raised_name(node: ast.Raise) -> str:
    """The class a `raise` names, whether it is constructed or re-raised bare."""
    if node.exc is None:
        return ""
    if isinstance(node.exc, ast.Call):
        return _callee_name(node.exc.func) or ""
    return _callee_name(node.exc) or ""


def _iterated(iterator: ast.expr) -> tuple[str, int | None] | None:
    """The sequence a loop walks, and which tuple slot of its target gets an element.

    A slot of None means the target *is* the element. The three spellings here
    are the ones the marshalling loops in this tree are written as, and a loop
    the resolver cannot read leaves its element unaliased -- which reports as
    unreached rather than as answered.
    """
    if isinstance(iterator, ast.Name):
        return (iterator.id, None)
    if isinstance(iterator, ast.Call):
        func = iterator.func
        if isinstance(func, ast.Name) and func.id == "enumerate" and iterator.args:
            inner = _iterated(iterator.args[0])
            return (inner[0], 1) if inner is not None else None
        if isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name):
            if func.attr == "values":
                return (func.value.id, None)
            if func.attr == "items":
                return (func.value.id, 1)
    return None


def _bound_slot(target: ast.expr, slot: int | None) -> set[str]:
    """The names a loop target binds at `slot`."""
    if slot is None:
        return _bound_names(target)
    if isinstance(target, (ast.Tuple, ast.List)) and slot < len(target.elts):
        return _bound_names(target.elts[slot])
    return set()


def _loops(function: ast.AST):
    """Every `for` and every comprehension clause, as (iterator, target)."""
    for node in _own_body(function):
        if isinstance(node, (ast.For, ast.comprehension)):
            yield node.iter, node.target


class Scope(typing.NamedTuple):
    """What a call site needs: where it is, what `self` is, what structs it holds."""

    module: str
    local: dict[str, str]
    owner: str | None
    # Names in scope for THIS function that hold a narrowing struct. Per function
    # rather than per module, so a sibling's local cannot lend its guard.
    struct_holders: frozenset[str]


class Flow:
    """Follows one published value through the implementation.

    The value is tracked by its **spelling** rather than by a bare name, because
    a record field arrives as `clip.id` and an argument as `n_fft`, and both have
    to be followed by the same machinery or the two halves of the population
    drift apart.

    An alias is only ever an *identity* alias: a name still holding the caller's
    value. That is what makes the fold check mean something -- `int(x)` does not
    extend the set, so a guard written behind one is not credited to `x`, which
    is correct, because by then the fraction a guard would refuse is gone.
    """

    def __init__(self, tree: Tree, guards: frozenset[str]) -> None:
        self.tree = tree
        self.guards = guards
        self.folds: set[Fold] = set()
        # (module, function line, tracked spelling) for every value a mechanism
        # answered for in that function's own body. This is what a fold site is
        # checked against: the guard has to see the value the fold folded, not
        # the fold's result and not a guard one hop away.
        self.answered: set[tuple[str, int, str]] = set()
        # Credits an earlier branch in the same body can leave before reaching.
        self.conditional: set[Conditional] = set()

    def follow(
        self,
        module: str,
        function: ast.AST,
        root: str,
        subject: str,
        hops: int = 0,
        seen: frozenset[tuple[str, int, str]] = frozenset(),
        owner: str | None = None,
    ) -> Reach:
        """How `root` is answered for inside `function`, following local calls."""
        marker = (module, getattr(function, "lineno", 0), root)
        if marker in seen or hops > MAX_HOPS:
            return UNREACHED
        seen = seen | {marker}
        scope = Scope(
            module,
            _local_imports(function),
            owner,
            self._struct_holder_names(module, function),
        )
        aliases, containers = self._aliases(scope, function, root)
        body = list(_own_body(function))

        # Folds are collected over the whole body before any verdict is taken.
        # Returning at the first mechanism found would stop the walk, and the
        # fold check would then measure only what happens to sit above a guard.
        for node in body:
            spelling = _fold_spelling(node)
            if spelling is not None and self._touches(node, aliases):
                self.folds.add(
                    Fold(module, marker[1], root, node.lineno, spelling, subject)
                )

        deferred: list[tuple[str, ast.AST, str, str | None]] = []
        for node in body:
            reach = self._direct(scope, node, aliases)
            if reach is not UNREACHED:
                self.answered.add(marker)
                self._record_branches(module, function, aliases, reach, subject)
                return reach
            if isinstance(node, ast.Call):
                deferred.extend(self._handoffs(scope, node, aliases, containers))

        # A hop counts towards domination too, and only because of what crosses
        # it: the hand-off is by identity, so the value the helper guards is the
        # unfolded one this function was handed. A guard past a fold would not
        # have been reached by an identity alias in the first place.
        for home, node, name, next_owner in deferred:
            reach = self.follow(home, node, name, subject, hops + 1, seen, next_owner)
            if reach is not UNREACHED:
                self.answered.add(marker)
                return reach._replace(hops=reach.hops + 1)
        return UNREACHED

    def _record_branches(
        self,
        module: str,
        function: ast.AST,
        aliases: frozenset[str],
        reach: Reach,
        subject: str,
    ) -> None:
        """Every earlier branch that reads this value and leaves before the guard.

        Every one of them, not the first: a body routinely opens with a benign
        `if value is None: continue`, and stopping there would read the harmless
        branch and never reach the one that writes the value out. Which is how
        this was first measured at zero on a tree that had one.
        """
        for node in _own_body(function):
            if not isinstance(node, ast.If) or node.lineno >= reach.line:
                continue
            if not self._touches(node.test, aliases):
                continue
            for branch in (node.body, node.orelse):
                if not any(
                    isinstance(child, (ast.Continue, ast.Return, ast.Break))
                    for statement in branch
                    for child in ast.walk(statement)
                ):
                    continue
                self.conditional.add(
                    Conditional(
                        subject,
                        module,
                        node.lineno,
                        reach.line,
                        ast.unparse(node.test)[:90],
                        self._stores(branch, aliases),
                        "\n".join(ast.unparse(statement) for statement in branch)[:160],
                    )
                )

    def _stores(self, branch, aliases: frozenset[str]) -> bool:
        """Whether the branch writes the value out or hands it on before leaving."""
        for statement in branch:
            for node in ast.walk(statement):
                if isinstance(node, ast.Assign) and self._is_identity(
                    node.value, aliases
                ):
                    return True
                if isinstance(node, ast.Return) and self._is_identity(
                    node.value, aliases
                ):
                    return True
                if isinstance(node, ast.Call) and self._passes(node, aliases):
                    return True
        return False

    def _aliases(
        self, scope: Scope, function: ast.AST, root: str
    ) -> tuple[frozenset[str], frozenset[str]]:
        """Spellings that still hold the value, by fixpoint over assignment.

        A value put into a container and taken out again by a loop is followed
        through, and soundly: a loop body applies the same code to every element,
        so a guard on the loop variable is a guard on whichever element this one
        was. The container is tracked separately from the value, so the element
        is credited and the container itself is not.
        """
        aliases = {root}
        # The value is its own container: a `Sequence[int]` parameter is walked
        # directly, without being copied into anything first. Harmless for a
        # scalar, which nothing iterates.
        containers: set[str] = {root}
        for _ in range(5):
            grown, grown_containers = set(aliases), set(containers)
            for node in _own_body(function):
                if isinstance(node, ast.Assign):
                    targets, value = node.targets, node.value
                elif isinstance(node, (ast.AnnAssign, ast.AugAssign, ast.NamedExpr)):
                    targets, value = [node.target], node.value
                else:
                    continue
                if value is None:
                    continue
                if self._is_identity(value, aliases, scope):
                    for target in targets:
                        if isinstance(target, (ast.Name, ast.Attribute)):
                            grown.add(ast.unparse(target))
                elif self._is_container_of(value, aliases, containers):
                    for target in targets:
                        if isinstance(target, ast.Name):
                            grown_containers.add(target.id)
            for iterator, target in _loops(function):
                walked = _iterated(iterator)
                if walked is not None and walked[0] in grown_containers:
                    grown |= _bound_slot(target, walked[1])
            if (grown, grown_containers) == (aliases, containers):
                break
            aliases, containers = grown, grown_containers
        return frozenset(aliases), frozenset(containers)

    def _is_container_of(
        self, value: ast.expr, aliases: set[str] | frozenset[str], containers: set[str]
    ) -> bool:
        """Whether `value` builds a container holding the tracked value.

        Both spellings the tree uses: a literal the value is written into, and a
        sequence constructor applied to a sequence the value already is.
        """
        if isinstance(value, (ast.List, ast.Tuple, ast.Set)):
            return any(self._is_identity(element, aliases) for element in value.elts)
        if isinstance(value, ast.Dict):
            return any(self._is_identity(element, aliases) for element in value.values)
        if (
            isinstance(value, ast.Call)
            and _callee_name(value.func) in SEQUENCE_BUILDERS
        ):
            return any(
                self._is_identity(argument, aliases)
                or (isinstance(argument, ast.Name) and argument.id in containers)
                for argument in value.args
            )
        if isinstance(value, ast.IfExp):
            return self._is_container_of(
                value.body, aliases, containers
            ) or self._is_container_of(value.orelse, aliases, containers)
        return False

    def _is_identity(
        self,
        value: ast.expr | None,
        aliases: frozenset[str] | set[str],
        scope: Scope | None = None,
    ) -> bool:
        """Whether `value` is one of `aliases` unchanged.

        A conditional and an `or` default both hand the value through on one
        branch, which is enough: that branch is the one a fraction travels on.
        `min`/`max`/`abs` pass through for a narrower reason -- none of them can
        make a non-integral value integral or bring an out-of-range one into
        range, so a guard behind one still sees what it was put there to refuse.

        A helper that returns the value it was handed passes it through too, and
        that is the shape the enum resolvers here are written in:
        `_to_c_int(_resolve_preset_ordinal(preset))` narrows the caller's number,
        because the resolver hands an integer straight back. Following it needs
        the scope, so it is only asked where the scope is known.
        """
        if value is None:
            return False
        if isinstance(value, (ast.Name, ast.Attribute)):
            return ast.unparse(value) in aliases
        if (
            isinstance(value, ast.Call)
            and _callee_name(value.func) in PASSTHROUGH_CALLS
        ):
            return any(
                self._is_identity(argument, aliases, scope) for argument in value.args
            )
        if isinstance(value, ast.Call) and scope is not None:
            return self._returns_argument(scope, value, aliases)
        if isinstance(value, ast.IfExp):
            return self._is_identity(value.body, aliases, scope) or self._is_identity(
                value.orelse, aliases, scope
            )
        if isinstance(value, ast.BoolOp):
            return any(self._is_identity(part, aliases, scope) for part in value.values)
        return False

    def _returns_argument(
        self, scope: Scope, call: ast.Call, aliases: frozenset[str] | set[str]
    ) -> bool:
        """Whether `call` hands one of `aliases` straight back out.

        One level only, and by `return <parameter>` in the callee's own body: a
        helper that returns a value it computed is not this shape, and a chain of
        them is followed as a chain of hand-offs instead.
        """
        resolved = self._resolve_callee(scope, call)
        if resolved is None:
            return False
        _, node, _ = resolved
        landed = {
            parameter
            for parameter, argument in _attribute(node, call)
            if self._is_identity(argument, aliases)
        }
        if not landed:
            return False
        return any(
            isinstance(statement, ast.Return)
            and self._is_identity(statement.value, landed)
            for statement in _own_body(node)
        )

    def _touches(self, node: ast.AST, aliases: frozenset[str]) -> bool:
        return any(
            isinstance(child, (ast.Name, ast.Attribute))
            and ast.unparse(child) in aliases
            for child in ast.walk(node)
        )

    def _direct(self, scope: Scope, node: ast.AST, aliases: frozenset[str]) -> Reach:
        """The mechanisms decided inside the function being read, with their line."""
        found = self._mechanism(scope, node, aliases)
        if found is UNREACHED or found.line:
            return found
        return found._replace(line=getattr(node, "lineno", 0))

    def _mechanism(self, scope: Scope, node: ast.AST, aliases: frozenset[str]) -> Reach:
        if isinstance(node, ast.Call):
            named = _callee_name(node.func)
            if self._passes(node, aliases, scope):
                if named in self.guards and not _unbounds_enum_call(node):
                    return Reach("guarded", f"{named}(...)")
                if _is_index_call(node.func):
                    return Reach("refused", "operator.index(...)")
                if self._is_lib_call(node.func):
                    return Reach("ctypes", f"lib.{named}(...)")
                if named in self.tree.struct_classes:
                    return Reach("struct", f"{named}(...)")
        if isinstance(node, ast.Assign) and self._is_identity(
            node.value, aliases, scope
        ):
            for target in node.targets:
                if isinstance(target, ast.Attribute) and self._is_struct_field(
                    scope, target
                ):
                    return Reach("struct", f"{ast.unparse(target)} = ...")
        if isinstance(node, ast.If) and self._is_integrality_test(node, aliases):
            return Reach("refused", f"if ... raise (line {node.lineno})")
        return UNREACHED

    def _is_integrality_test(self, node: ast.If, aliases: frozenset[str]) -> bool:
        """A branch that refuses the value for not being an integer.

        Narrow on purpose. A range test refuses a domain violation and says
        nothing about a fraction, so only a test that decides *integrality*
        counts -- the property a fold destroys without a trace.
        """
        if not self._touches(node.test, aliases):
            return False
        mentioned = {
            _callee_name(child) or ""
            for child in ast.walk(node.test)
            if isinstance(child, (ast.Name, ast.Attribute))
        } | {
            argument.id
            for child in ast.walk(node.test)
            if isinstance(child, ast.Call)
            for argument in child.args
            if isinstance(argument, ast.Name)
        }
        if not mentioned & set(INTEGRALITY_TESTS):
            return False
        return any(
            isinstance(child, ast.Raise) and _raised_name(child) in REFUSAL_ERRORS
            for branch in (node.body, node.orelse)
            for statement in branch
            for child in ast.walk(statement)
        )

    def _passes(
        self, call: ast.Call, aliases: frozenset[str], scope: Scope | None = None
    ) -> bool:
        """Whether an alias is an argument of `call`, by value rather than inside one."""
        for argument in [*call.args, *(keyword.value for keyword in call.keywords)]:
            inner = argument.value if isinstance(argument, ast.Starred) else argument
            if self._is_identity(inner, aliases, scope):
                return True
            # A guard applied elementwise to a sequence the value is part of.
            if isinstance(inner, (ast.List, ast.Tuple)) and any(
                self._is_identity(element, aliases, scope) for element in inner.elts
            ):
                return True
        return False

    def _is_lib_call(self, callee: ast.AST) -> bool:
        if not isinstance(callee, ast.Attribute):
            return False
        holder = callee.value
        if isinstance(holder, ast.Name):
            return holder.id in LIB_NAMES
        return isinstance(holder, ast.Attribute) and holder.attr in LIB_NAMES

    def _is_struct_field(self, scope: Scope, target: ast.Attribute) -> bool:
        """Whether the assigned object is a CStruct instance, not an arbitrary one.

        Decided from the constructor the holder was bound to, so the credit is
        owed to a struct whose `__setattr__` actually narrows rather than to any
        attribute that happens to be named like a field.

        Keyed on (function, holder) rather than on the holder's name, because a
        name is not an owner: with a module-wide key, `raw = SonareThing()` in one
        function would make `raw.field = n` in a *sibling* function read as a
        narrowing write, and the sibling's argument would be credited for a guard
        that is not on its path. Module-level bindings are in scope for every
        function and so are included; a sibling function's local is not.
        """
        holder = target.value
        if isinstance(holder, ast.Subscript):
            holder = holder.value
        if not isinstance(holder, ast.Name):
            return False
        return holder.id in scope.struct_holders

    def _constructs_struct(self, callee: ast.expr) -> bool:
        """A narrowing struct, one instance or a whole array of them.

        `(SonareEngineTrackLane * n)()` reaches the same `__setattr__` per
        element as a single instance does, and the marshalling loops use it far
        more often than the scalar form.
        """
        if _callee_name(callee) in self.tree.struct_classes:
            return True
        if isinstance(callee, ast.BinOp) and isinstance(callee.op, ast.Mult):
            return any(
                _callee_name(side) in self.tree.struct_classes
                for side in (callee.left, callee.right)
            )
        return False

    def _struct_holder_names(self, module: str, function: ast.AST) -> frozenset[str]:
        """Names bound to a narrowing struct and in scope for `function`.

        Two sources, and no third: the function's own body, and the module's top
        level. A sibling function's local is deliberately absent -- see
        `_is_struct_field`.
        """
        names: set[str] = set()
        parsed = self.tree.modules.get(module)
        if parsed is not None:
            names |= self._bound_to_struct(parsed.body)
        names |= self._bound_to_struct(_own_body(function))
        return frozenset(names)

    def _bound_to_struct(self, nodes) -> set[str]:
        names: set[str] = set()
        for node in nodes:
            if not isinstance(node, ast.Assign) or not isinstance(node.value, ast.Call):
                continue
            if not self._constructs_struct(node.value.func):
                continue
            for bound in node.targets:
                if isinstance(bound, ast.Name):
                    names.add(bound.id)
        return names

    def _resolve_callee(
        self, scope: Scope, call: ast.Call
    ) -> tuple[str, ast.FunctionDef | ast.AsyncFunctionDef, str | None] | None:
        """The function a call reaches, its module, and the owner to carry on.

        Four routes, because the tree uses all four: a module-level helper, a
        sibling method as `self.<name>` (which a module-level lookup answers None
        for), a base-class method as `super().<name>`, and a facade function
        pulled in by an import written *inside* the function to break a cycle.

        The owner travels with the hop only when the hop lands on a method of
        that owner: past a module-level helper `self` means nothing, and carrying
        the name on would resolve a sibling call against the wrong class.
        """
        name = _callee_name(call.func)
        if name is None:
            return None
        target: object | None = None
        next_owner: str | None = None
        if _is_self_call(call.func) and scope.owner is not None:
            target = self.tree.method(scope.module, scope.owner, name)
            next_owner = scope.owner if target is not None else None
        elif _is_super_call(call.func) and scope.owner is not None:
            target = self.tree.inherited(scope.module, scope.owner, name)
        if target is None and name in scope.local:
            target = self.tree.resolve(scope.local[name], name)
        if target is None:
            target = self.tree.resolve(scope.module, name)
        if target is None:
            return None
        home, node = typing.cast(tuple, target)
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            return None
        return (home, node, next_owner)

    def _handoffs(
        self,
        scope: Scope,
        call: ast.Call,
        aliases: frozenset[str],
        containers: frozenset[str],
    ) -> list[tuple[str, ast.AST, str, str | None]]:
        """A local helper the value is handed to, and the parameter it lands on.

        A container carrying the value is followed as well as the value itself:
        the options bags here are built at the facade and unpacked one hop in,
        and the guard is on the unpacked element.
        """
        if _callee_name(call.func) in self.guards:
            return []
        resolved = self._resolve_callee(scope, call)
        if resolved is None:
            return []
        home, node, next_owner = resolved
        return [
            (home, node, parameter, next_owner)
            for parameter, argument in _attribute(node, call)
            if self._is_identity(argument, aliases)
            or self._is_container_of(argument, aliases, set(containers))
            or (isinstance(argument, ast.Name) and argument.id in containers)
        ]


def _attribute(
    function: ast.FunctionDef | ast.AsyncFunctionDef, call: ast.Call
) -> list[tuple[str, ast.expr]]:
    """`(parameter, argument)` for every argument whose landing place is nameable.

    Attribution ends at a splat: nothing past one has a position this check can
    name, so it is left out and reports as unreached rather than as answered
    somewhere past the splat.
    """
    positional = [*function.args.posonlyargs, *function.args.args]
    if positional and positional[0].arg in ("self", "cls"):
        positional = positional[1:]
    landed: list[tuple[str, ast.expr]] = []
    for index, argument in enumerate(call.args):
        if isinstance(argument, ast.Starred) or index >= len(positional):
            continue
        landed.append((positional[index].arg, argument))
    for keyword in call.keywords:
        if keyword.arg is not None:
            landed.append((keyword.arg, keyword.value))
    return landed


def _is_self_call(callee: ast.AST) -> bool:
    return (
        isinstance(callee, ast.Attribute)
        and isinstance(callee.value, ast.Name)
        and callee.value.id == "self"
    )


def _is_super_call(callee: ast.AST) -> bool:
    return (
        isinstance(callee, ast.Attribute)
        and isinstance(callee.value, ast.Call)
        and isinstance(callee.value.func, ast.Name)
        and callee.value.func.id == "super"
    )


def _local_imports(function: ast.AST) -> dict[str, str]:
    """`name -> module` for imports written inside the function, to break a cycle.

    A facade pulled in this way is invisible to the module's own namespace, so
    the hand-off to it would not resolve and its parameter would read unreached.
    """
    found: dict[str, str] = {}
    for node in _own_body(function):
        if isinstance(node, ast.ImportFrom) and node.level == 1 and node.module:
            for alias in node.names:
                found[alias.asname or alias.name] = node.module
    return found


def _own_body(function: ast.AST):
    """Every node of `function` except a nested function's own.

    A value handed into a closure is followed as a call argument instead, so
    walking into the closure here would credit an outer parameter for a guard on
    a name that merely shadows it.
    """
    for child in ast.iter_child_nodes(function):
        if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
            continue
        yield child
        yield from _own_body(child)


class Records:
    """Where a frozen record's fields are read, so a generated `__init__` can be answered.

    A record parameter is not an argument to anything: `@dataclass` turns it into
    a field, and the conversion happens wherever the field is read. The reader is
    found by annotation -- a function taking `clips: Sequence[EngineClip]` reads
    `clip.id` -- so the credit is keyed on (record, field) rather than on the
    field name alone, which several unrelated records share.
    """

    def __init__(self, tree: Tree, flow: Flow) -> None:
        self.tree = tree
        self.flow = flow
        self.readers: dict[str, list[tuple[str, ast.AST, frozenset[str]]]] = {}
        self._collect()

    def _collect(self) -> None:
        for module, parsed in self.tree.modules.items():
            for node in ast.walk(parsed):
                if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    continue
                for arg in _parameters(node):
                    for record in _annotated_records(arg.annotation):
                        holders = _holder_names(node, arg.arg)
                        self.readers.setdefault(record, []).append(
                            (module, node, holders)
                        )

    def marshalled(self) -> frozenset[str]:
        return frozenset(self.readers)

    def reach(self, record: str, field: str, subject: str) -> Reach:
        """How `record.field` is answered for at the sites that read it.

        Keyed on (record, field), not on the record alone: a record can be
        accepted somewhere and still have a field nothing on the far side reads
        back, and that field becomes no C type however the record travels. So a
        field no reader touches is `not_marshalled` on its own, which is a fact
        about the tree rather than a decision about an entry.
        """
        touched = False
        for module, node, holders in self.readers.get(record, []):
            names = _field_aliases(node, holders, field)
            if not self._reads(node, holders, field):
                continue
            touched = True
            for name in names:
                answer = self.flow.follow(module, node, name, subject)
                if answer is not UNREACHED:
                    return Reach(answer.verdict, f"{record}.{field} -> {answer.detail}")
        if not touched:
            return Reach("not_marshalled", f"nothing reads {record}.{field}")
        return UNREACHED

    def _reads(self, function: ast.AST, holders: frozenset[str], field: str) -> bool:
        """Whether this reader touches `<holder>.<field>` at all."""
        return any(
            isinstance(node, ast.Attribute)
            and node.attr == field
            and isinstance(node.value, ast.Name)
            and node.value.id in holders
            for node in ast.walk(function)
        )


def _annotated_records(annotation: ast.expr | None) -> list[str]:
    """Class names an annotation mentions, which a record lookup then filters."""
    if annotation is None:
        return []
    found: list[str] = []
    for node in ast.walk(annotation):
        if isinstance(node, ast.Name) and node.id[:1].isupper():
            found.append(node.id)
        elif isinstance(node, ast.Attribute) and node.attr[:1].isupper():
            found.append(node.attr)
    return found


def _holder_names(function: ast.AST, parameter: str) -> frozenset[str]:
    """Names inside `function` that hold one record: the parameter, or a loop item.

    A sequence-annotated parameter becomes a single record either by being
    iterated -- including through `enumerate`, which is how most of the
    marshalling loops here are written -- or by being subscripted.
    """
    holders = {parameter}
    for _ in range(3):
        grown = set(holders)
        for iterator, target in _loops(function):
            walked = _iterated(iterator)
            if walked is not None and walked[0] in grown:
                grown |= _bound_slot(target, walked[1])
        for node in ast.walk(function):
            if isinstance(node, ast.Assign) and isinstance(node.value, ast.Subscript):
                holder = node.value.value
                if isinstance(holder, ast.Name) and holder.id in grown:
                    for target in node.targets:
                        grown |= _bound_names(target)
        if grown == holders:
            break
        holders = grown
    return frozenset(holders)


def _bound_names(target: ast.expr) -> set[str]:
    return {node.id for node in ast.walk(target) if isinstance(node, ast.Name)}


def _field_aliases(function: ast.AST, holders: frozenset[str], field: str) -> list[str]:
    """Local names bound to `holder.field`, which the flow analysis can then follow.

    A read used inline (`raw.id = clip.id`) is answered by the flow analysis
    seeing the attribute directly; this covers the other spelling, where the
    field is lifted into a local first.
    """
    found = [f"{holder}.{field}" for holder in sorted(holders)]
    for node in ast.walk(function):
        if not isinstance(node, ast.Assign):
            continue
        value = node.value
        if (
            isinstance(value, ast.Attribute)
            and value.attr == field
            and isinstance(value.value, ast.Name)
            and value.value.id in holders
        ):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    found.append(target.id)
    return found


_FUNCTIONS = (ast.FunctionDef, ast.AsyncFunctionDef)


class Verdict(typing.NamedTuple):
    """One published parameter's answer, and where the answer was found."""

    parameter: Parameter
    verdict: str
    detail: str
    hops: int


class Report:
    """Every count the gate's verdict rests on, and the findings behind them."""

    def __init__(
        self,
        binding: Path,
        exclusions: dict[str, dict[str, tuple[str, str]]] | None = None,
    ) -> None:
        # Injected rather than read from the module constant, so a self-test can
        # drive one failure class over a tree of its own without the shipping
        # table's entries all reporting as stale on it.
        self.exclusions = EXCLUSIONS if exclusions is None else exclusions
        self.surface = Surface(binding)
        self.tree = Tree(binding)
        self.guards = vocabulary(self.tree)
        self.flow = Flow(self.tree, frozenset(self.guards))
        self.records = Records(self.tree, self.flow)
        self.misplaced = misplaced_guards(self.tree)
        self.verdicts: list[Verdict] = []
        self.missing: list[str] = []
        self.resolved = 0
        self.used_exclusions: set[tuple[str, str]] = set()
        # Published arguments an exclusion answered for. A fold found while
        # following one belongs to the route that exclusion describes, so it
        # leaves the inventory with its argument rather than being reported
        # on its own and needing a second entry of its own.
        self.excused_subjects: set[str] = set()
        self._classify()

    def _classify(self) -> None:
        implementations: dict[tuple[str, str], object] = {}
        for module, qualname in sorted(self.surface.functions):
            found = self._implementation(module, qualname)
            implementations[(module, qualname)] = found
            if found is None:
                self.missing.append(f"{module}.{qualname}")
            else:
                self.resolved += 1

        # The exclusion is consulted AFTER the verdict, never before it. Asked
        # first it would be marked used for every argument merely named in the
        # table, so an entry would go on excusing a parameter that had since
        # been guarded and the unused-exclusion audit would never retire it.
        # Asked here, an entry is used only where there was something to excuse,
        # which is what makes the audit the retirement mechanism.
        for parameter in self.surface.parameters:
            found = implementations[(parameter.module, parameter.qualname)]
            if found is None:
                reach = Reach("unreached", "no implementation resolved")
            else:
                kind, home, node, owner = typing.cast(tuple, found)
                if kind == "function":
                    reach = self.flow.follow(
                        home, node, parameter.name, parameter.display, owner=owner
                    )
                else:
                    reach = self._record_reach(parameter)
            excuse = self.exclusions.get(parameter.key, {}).get(parameter.name)
            if reach.verdict == "unreached" and excuse is not None:
                self.used_exclusions.add((parameter.key, parameter.name))
                self.excused_subjects.add(parameter.display)
                reach = Reach("excused", excuse[1])
            self.verdicts.append(
                Verdict(parameter, reach.verdict, reach.detail, reach.hops)
            )

    def _record_reach(self, parameter: Parameter) -> Reach:
        """A generated `__init__` parameter, answered where its field is read."""
        owner = parameter.qualname.rsplit(".", 1)[0]
        if owner not in self.records.marshalled():
            return Reach("not_marshalled", f"{owner} is accepted by nothing")
        return self.records.reach(owner, parameter.name, parameter.display)

    def _implementation(self, module: str, qualname: str) -> object | None:
        """The code a stub stands for: a written function, or a generated `__init__`."""
        if "." not in qualname:
            found = self.tree.resolve(module, qualname)
            if found is None:
                return None
            home, node = typing.cast(tuple, found)
            return (
                ("function", home, node, None) if isinstance(node, _FUNCTIONS) else None
            )
        owner, name = qualname.rsplit(".", 1)
        written = self.tree.method(module, owner, name)
        if written is not None:
            return ("function", written[0], written[1], owner)
        record = self.tree.record(module, owner) if name == "__init__" else None
        if record is not None:
            return ("record", record[0], record[1], owner)
        return None

    def counts(self) -> dict[str, int]:
        tally: dict[str, int] = {
            "population": len(self.surface.parameters),
            "functions": len(self.surface.functions),
            "vocabulary": len(self.guards),
            "implementations_resolved": self.resolved,
            "implementations_missing": len(self.missing),
            "marshalled_records": len(self.records.marshalled()),
            "record_field_readers": sum(len(v) for v in self.records.readers.values()),
            "guarded": 0,
            "refused": 0,
            "ctypes": 0,
            "struct": 0,
            "not_marshalled": 0,
            "excused": 0,
            "unreached": 0,
        }
        for verdict in self.verdicts:
            tally[verdict.verdict] = tally.get(verdict.verdict, 0) + 1
        answered = [
            verdict
            for verdict in self.verdicts
            if verdict.verdict in ("guarded", "refused", "ctypes", "struct")
        ]
        tally["answered_in_body"] = sum(1 for v in answered if v.hops == 0)
        tally["answered_via_handoff"] = sum(1 for v in answered if v.hops > 0)
        tally["deepest_handoff"] = max((v.hops for v in answered), default=0)
        # Informational, never a verdict. How many published arguments carry a
        # whole key space rather than a value is the size of what this check
        # cannot ask about, and a reader is owed that number -- but passing on it
        # would turn "I cannot see a guard" into "there is no finding", for every
        # bag written from now on, without anyone deciding so.
        tally["bag_members"] = sum(1 for p in self.surface.parameters if p.bag_only)
        tally["conditional"] = len(self.flow.conditional)
        tally["bypass"] = len(self.bypass())
        tally["fold_sites"] = len(self.flow.folds)
        # Folds the domination check never judges, because their argument was
        # excused and the exclusion's reason covers the whole route. Printed
        # apart from `undominated` so the two are never one number.
        tally["folds_excused"] = sum(
            1 for fold in self.flow.folds if fold.subject in self.excused_subjects
        )
        tally["undominated"] = len(self.undominated())
        tally["past_the_ffi_entries"] = len(self.past_the_ffi_entries())
        tally["keyed_member_entries"] = len(self.keyed_member_entries())
        return tally

    def entries_for(self, category: str) -> list[str]:
        """Every live entry of one category, with the reason it carries.

        One accessor for both reported categories, because two written apart
        drift: the counter and the printed block would stop describing the same
        set, and only one of them would be wrong at a time.
        """
        return sorted(
            f"{key}({name}): {reason}"
            for key, entries in self.exclusions.items()
            for name, (category_name, reason) in entries.items()
            if category_name == category and (key, name) in self.used_exclusions
        )

    def past_the_ffi_entries(self) -> list[str]:
        """Named arguments whose guard sits past the FFI, where this cannot read."""
        return self.entries_for("guarded_past_the_ffi")

    def keyed_member_entries(self) -> list[str]:
        """Mapping parameters whose integers arrive at keys this has no unit for."""
        return self.entries_for("integral_member_is_a_key")

    def unreached(self) -> list[Verdict]:
        return [verdict for verdict in self.verdicts if verdict.verdict == "unreached"]

    def bypass(self) -> list[Conditional]:
        """Credits whose guard an earlier branch writes the value out and skips."""
        return sorted(entry for entry in self.flow.conditional if entry.stores)

    def undominated(self) -> list[Fold]:
        """Folds whose own value no mechanism in the same function also saw."""
        return sorted(
            fold
            for fold in self.flow.folds
            if (fold.module, fold.function_line, fold.tracked) not in self.flow.answered
            and fold.subject not in self.excused_subjects
        )

    def unreadable(self) -> list[str]:
        """Why the root carries no surface, named by path, or empty if it does.

        Separate from the floors and checked before them: a floor answers "is
        this smaller than it was", and the answer here is "there is nothing
        here", which is a different finding with a different fix.
        """
        root = self.surface.binding
        if not root.is_dir():
            return [f"  {root}: not a directory"]
        if not self.surface.stubs:
            return [f"  {root}: holds no *.pyi, so there is no published surface"]
        if not self.tree.modules:
            return [f"  {root}: holds no *.py, so no implementation can be resolved"]
        if not self.surface.parameters:
            return [
                f"  {root}: {len(self.surface.stubs)} stub(s) declare no parameter "
                "admitting a caller-supplied integer"
            ]
        return []

    def stale_exclusions(self) -> list[str]:
        """Exclusions that excused nothing, and entries outside the category set."""
        stale = [
            f"{key}({name})"
            for key, entries in self.exclusions.items()
            for name in entries
            if (key, name) not in self.used_exclusions
        ]
        wrong = [
            f"{key}({name}): category '{category}' is not one of {CATEGORIES}"
            for key, entries in self.exclusions.items()
            for name, (category, _) in entries.items()
            if category not in CATEGORIES
        ]
        return sorted(stale) + sorted(wrong)


def evaluate(
    report: Report, floors: dict[str, int] | None = None
) -> list[tuple[str, list[str]]]:
    """Every failure class, as (heading, lines). Empty means the surface holds.

    Kept apart from ``main`` so a self-test can revert one class at a time and
    require exactly that class to fire.
    """
    failures: list[tuple[str, list[str]]] = []
    counts = report.counts()

    # Asked first, and on its own, because the diagnosis differs. Nine floors
    # all reading zero says "the surface shrank"; what actually happened is that
    # the root held no surface to read, and only the path can say so. An
    # instrument pointed at the wrong directory reports its own blindness here
    # rather than reporting the tree as clean.
    unreadable = report.unreadable()
    if unreadable:
        return [
            (
                "This check read no published surface at all, so every population "
                "below it is empty and every check over it would agree with nothing",
                unreadable,
            )
        ]

    short = [
        f"  {name}: found {counts.get(name, 0)}, floor is {minimum}"
        for name, minimum in (FLOORS if floors is None else floors).items()
        if counts.get(name, 0) < minimum
    ]
    if short:
        failures.append(
            (
                (
                    "The scan no longer finds the population it is sized for, so "
                    "every check over it agrees with an empty set"
                ),
                short,
            )
        )

    if report.missing:
        failures.append(
            (
                (
                    "These published functions have no implementation this check "
                    "could resolve, so their arguments were never asked the question"
                ),
                [f"  {name}" for name in sorted(report.missing)],
            )
        )

    if report.surface.alias_conflicts:
        failures.append(
            (
                "These type aliases are declared differently in two stubs, so an "
                "annotation naming one cannot be resolved -- and a parameter this "
                "check cannot resolve the annotation of is absent from a population "
                "that reports itself as discovered",
                [f"  {line}" for line in report.surface.alias_conflicts],
            )
        )

    if report.misplaced:
        failures.append(
            (
                (
                    f"These narrowing guards live outside the home ({', '.join(GUARD_HOME)}), "
                    "so the derived vocabulary cannot see them and a value routed "
                    "through one reads as unguarded"
                ),
                [f"  {line}" for line in report.misplaced],
            )
        )

    unreached = report.unreached()
    if unreached:
        failures.append(
            (
                (
                    "These published integer arguments reach no guard, no ctypes "
                    "declaration and no narrowing struct field THAT THIS CHECK CAN "
                    "SEE. A value the C type cannot hold is therefore unaccounted "
                    "for: either folded onto another legal one, or refused somewhere "
                    "this check does not read -- past the FFI, or at a mapping key "
                    "that is not a declared parameter. Which of the two it is has to "
                    "be measured; it is not stated here, because a failure text that "
                    "asserts the first is wrong every time it is the second"
                ),
                [
                    f"  {verdict.parameter.display}  [{verdict.parameter.annotation}]"
                    for verdict in sorted(unreached, key=lambda v: v.parameter.display)
                ],
            )
        )

    bypassed = report.bypass()
    if bypassed:
        failures.append(
            (
                "These guards do not cover every path their value can take: an "
                "earlier branch in the same body writes the value out and leaves "
                "before reaching the guard, so the guard answers for every value "
                "except the ones that took that branch. A `bool` is the one that "
                "matters most -- it is an `int` subclass, so it can never fail a "
                "range check, which is exactly why the narrowing family refuses it",
                [f"  {entry.display}" for entry in bypassed],
            )
        )

    undominated = report.undominated()
    if undominated:
        failures.append(
            (
                (
                    "These folds turn a caller's number into an integer with nothing "
                    "in the same function having checked that number first, so the "
                    "guard behind them is handed a value that can no longer fail"
                ),
                [f"  {fold.display}" for fold in undominated],
            )
        )

    stale = report.stale_exclusions()
    if stale:
        failures.append(
            (
                (
                    "These exclusions excused nothing, or carry a category outside the "
                    "closed set. An exclusion that suppresses nothing still asserts a "
                    "reviewed decision about an argument, so the next one to take that "
                    "name inherits it unexamined"
                ),
                [f"  {line}" for line in stale],
            )
        )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Public integer arguments vs their C types."
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=BINDING,
        help="the binding package to read (a copy under /tmp, for an ablation)",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="print the derived vocabulary and every fold site",
    )
    args = parser.parse_args()

    report = Report(args.root)
    counts = report.counts()
    for name in (
        "population",
        "functions",
        "vocabulary",
        "implementations_resolved",
        "implementations_missing",
        "guarded",
        "refused",
        "ctypes",
        "struct",
        "not_marshalled",
        "excused",
        "unreached",
        "bag_members",
        "conditional",
        "bypass",
        "answered_in_body",
        "answered_via_handoff",
        "deepest_handoff",
        "marshalled_records",
        "record_field_readers",
        "fold_sites",
        "folds_excused",
        "undominated",
        "past_the_ffi_entries",
        "keyed_member_entries",
    ):
        print(f"{name}: {counts.get(name, 0)}")

    # Printed on every run, green or not. A reach this instrument does not have
    # is the one thing an exit code cannot carry, so it goes to stderr where a CI
    # log keeps it rather than into a count nobody reads twice.
    for heading, entries in (
        (
            "Named arguments whose guard sits past the FFI, with the measurement "
            "behind each",
            report.past_the_ffi_entries(),
        ),
        (
            "Mapping parameters whose integers arrive at keys this check has no "
            "unit for, with what refuses each member and where",
            report.keyed_member_entries(),
        ),
    ):
        if entries:
            print(
                f"\n{heading}:",
                *(f"  {line}" for line in entries),
                sep="\n",
                file=sys.stderr,
            )

    if args.list:
        print("\nderived guard vocabulary:")
        for name in sorted(report.guards):
            print(f"    {name}")
        print("\nfold sites:")
        for fold in sorted(report.flow.folds):
            marker = (fold.module, fold.function_line, fold.tracked)
            if marker in report.flow.answered:
                verdict = "dominated"
            elif fold.subject in report.excused_subjects:
                verdict = "UNDOMINATED, excused with its argument"
            else:
                verdict = "UNDOMINATED"
            print(f"    {fold.display}  [{verdict}]")

    failures = evaluate(report)
    for heading, lines in failures:
        print(f"\n{heading}:", *lines, sep="\n", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
