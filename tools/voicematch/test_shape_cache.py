"""Tests for what a candidate costs, rather than for what it measures.

`test_shape.py` grades the comparison; this grades the machinery underneath it
-- which notes a candidate has to re-render, and whether skipping the rest
changes the answer. The two halves have to be read together and the second is
the load-bearing one: a cache that is fast and wrong is indistinguishable from
one that is fast, because every score it returns is a plausible number.

Everything runs against a renderer that obeys the scoping rule by construction,
and against one that violates it. The library's own compliance is not testable
here -- that is `identity.py --isolate`, which needs a built dylib.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from shape.loss import ShapeLoss  # noqa: E402
from shape.render import DRUM_SCOPE, read_overrides, scope_overrides  # noqa: E402
from shape.spectro import Spectro  # noqa: E402
from test_shape import synth  # noqa: E402


# --- override scoping ----------------------------------------------------

def test_scope_keeps_a_notes_own_keys_and_everything_addressed_to_no_note():
    ov = ("d042.percussion.strike_r=0.4,d049.percussion.plate_gain=2.0,"
          "cymbal_voice.kWash=0.5")
    assert scope_overrides(ov, 42) == \
        "d042.percussion.strike_r=0.4,cymbal_voice.kWash=0.5"
    assert scope_overrides(ov, 49) == \
        "d049.percussion.plate_gain=2.0,cymbal_voice.kWash=0.5"
    # A piece with nothing addressed to it still reads the unscoped constant.
    assert scope_overrides(ov, 51) == "cymbal_voice.kWash=0.5"
    assert scope_overrides("", 42) == ""


def test_scope_keeps_a_melodic_patch_field_for_every_note():
    """A melodic key carries its PATCH's name, and a patch voices a keyboard.

    So there is nothing in it to scope by and the whole string survives every
    note -- which is what makes the cache correct on a piano and worthless on
    one, both at once.
    """
    ov = "violin.bowed_string.bow_force=1.2,fam0.piano.brightness=0.8"
    assert scope_overrides(ov, 40) == ov
    assert scope_overrides(ov, 76) == ov


class _Scoped:
    """A renderer that obeys the scoping rule the per-note cache rests on.

    Each note answers the keys addressed to it and the keys addressed to no
    note, which is what `scope_overrides` claims the library does. `leaky` makes
    a note read its neighbours' keys as well: that is the shape of failure a
    cache keyed on the scoped string would serve stale results under, and the
    reason the equivalence test has to be shown able to fail.
    """

    def __init__(self, leaky: bool = False):
        self.leaky = leaky

    def _tilt(self, note: int, ov: str) -> float:
        keys = read_overrides(ov)
        if self.leaky:
            return sum(keys.values())
        return sum(v for k, v in keys.items()
                   if not DRUM_SCOPE.match(k) or int(k[1:4]) == note)

    def __call__(self, pairs, ov: str = "", ref: bool = False) -> dict:
        out = {}
        for p in pairs:
            n, v = tuple(p)
            x = 0.0 if ref else self._tilt(n, ov)
            out[(n, v)] = synth(n, seconds=10.0,
                                level=[(-6.0 + x) * k for k in range(8)])
        return out


#: One coordinate moved at a time, which is what a descent does.
_CANDIDATES = ("", "d042.x=3.0", "d042.x=6.0", "d042.x=6.0,d044.y=2.0",
               "d044.y=2.0", "d044.y=2.0,d046.z=1.0")


def _cache_pair(leaky: bool = False):
    """A cached loss and an uncached one over the same struck grid."""
    notes, vels = (42, 44, 46), (64, 100)
    kw = dict(spectro=Spectro(seconds=10.0), velocities=vels, pitched=False)
    hot = ShapeLoss(signals=_Scoped(leaky=leaky), **kw)
    cold = ShapeLoss(signals=_Scoped(leaky=leaky), cache_mb=0.0, **kw)
    return notes, hot, cold


def test_the_note_cache_scores_a_candidate_exactly_as_an_uncached_loss_does():
    notes, hot, cold = _cache_pair()
    got = [hot.score(o, notes=notes).total for o in _CANDIDATES]
    want = [cold.score(o, notes=notes).total for o in _CANDIDATES]
    assert got == want
    # Not one number six times over, or the equality above is arithmetic rather
    # than evidence.
    assert len(set(want)) == len(want)
    # Six pairs per candidate uncached. Cached, the first candidate pays for the
    # whole grid and each of the others re-renders only the piece it moved --
    # except the fifth, whose two pieces are both already in the cache at those
    # settings, which is the case a descent spends most of its time in.
    assert cold.rendered == 36
    assert hot.rendered == 14


def test_the_note_cache_is_stale_when_a_piece_reads_its_neighbours_key():
    """The control that makes the equivalence worth having.

    Keyed on the scoped string, the cache serves note 42 from before note 44
    moved. If that were wrong about the library -- if one piece's constant could
    reach another piece's render -- nothing in a correctly scoped test would
    ever say so. Against a renderer that leaks, the same comparison separates,
    so the equality above is a property of the scoping and not of the loss.
    """
    notes, hot, cold = _cache_pair(leaky=True)
    got = [hot.score(o, notes=notes).total for o in _CANDIDATES]
    want = [cold.score(o, notes=notes).total for o in _CANDIDATES]
    assert got != want


def test_an_unscoped_key_re_renders_every_piece():
    notes, hot, _ = _cache_pair()
    hot.score("", notes=notes)
    assert hot.rendered == 6
    # Addressed to no note, so no note's cached render can answer for it.
    hot.score("cymbal_voice.kWash=0.5", notes=notes)
    assert hot.rendered == 12


def test_a_budget_too_small_for_two_grids_is_raised_to_two_grids():
    """Anything less evicts the notes the next candidate is about to ask for.

    The cache then costs an analysis per note and saves none, which is worse
    than not caching and looks exactly like caching. So the floor wins over the
    budget rather than the other way round, and the result stays identical.
    """
    notes, _, cold = _cache_pair()
    loss = ShapeLoss(signals=_Scoped(), spectro=Spectro(seconds=10.0),
                     velocities=(64, 100), pitched=False, cache_mb=1e-6)
    got = [loss.score(o, notes=notes).total for o in _CANDIDATES]
    want = [cold.score(o, notes=notes).total for o in _CANDIDATES]
    assert got == want
    assert loss.cache_limit(6) == 12 and len(loss._packs) == 12
