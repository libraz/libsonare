"""The kit-internal relations: what a drum kit is, as opposed to what is in it."""

from __future__ import annotations

import math

import numpy as np

# --------------------------------------------------------------------------
# What a kit is, as opposed to what is in it.
#
# Every term above reduces one hit to numbers taken against that hit's own
# reference row. A kit is not a collection of independent instruments: six toms
# are one series of sizes, three hi-hats are one mechanism at three openings,
# and what a listener notices wrong first is the relation rather than the
# member. Nothing measured a note at a time can hold a relation.
#
# The gap is not merely one of emphasis, because the per-row terms are capped.
# Measured on this kit: the reference's six toms run 71 to 184 Hz and
# libsonare's key-track from 100 to 167, so every one of the six is more than
# MODE_CENTS_CAP out and `modes` returns its cap on all six. Widening the series
# — the one repair that would fix it — changes that term by nothing at all until
# the members come back under the cap, so the objective has no gradient in the
# direction of the fix. A saturated term is not a weak signal; it is no signal.
#
# A relation term does not saturate that way, because it measures CONTRAST:
# each member against its own group, so wherever the group as a whole sits
# cancels — that part is already priced by `modes`, `env` and `level` — and what
# is left is only the shape of the set.
#
# The comparison is between SORTED contrast vectors, which is what makes this a
# statement about the group rather than about its members. Two consequences,
# both wanted. It is immune to the layout question a drum capture always has:
# this kit lays its six toms out as 45, 47, 48, 50, 41, 43 against GM's order,
# and a set has no order to disagree about, so the term needs no note map and
# cannot charge for one. And it is blind to a permuted group — a model with the
# open hi-hat's decay on the closed key scores here exactly as one with them the
# right way round. That is deliberate: placement is what a per-note term is good
# at, and `env` charges a swapped hi-hat pair some eight units per row.
#
# Every relation is expressed in doublings — a factor of two in frequency, in
# milliseconds or in amplitude — so one cap serves all four and a unit means
# roughly the same amount of audible wrongness whichever one produced it.

#: Members that have to survive on both sides before a relation is scored. Two
#: is the floor by construction: one member has no interior, and its contrast
#: against itself is zero on both sides whatever either side did.
KIT_MIN_MEMBERS = 2

#: How far apart the REFERENCE's own members have to sit, in doublings, before
#: a relation counts as one.
#:
#: Measured rather than declared, for the reason `measure_band_edge` measures
#: its own edge: a group holds some relations and not others, and which is a
#: property of the instrument. This kit's six toms are level-matched to within
#: 1.2 dB — 0.19 doublings — so there is no level relation among them to
#: reproduce, and scoring one would charge the model for the reference's own
#: strike-to-strike variation. The same group's pitch spans 1.37 doublings and
#: its decay 0.98, which are relations. A capture whose groups were declared
#: with the relations they hold would drift from what the rows say the day
#: either changed; this cannot.
KIT_MIN_SPREAD = 0.25

#: Per-member cap, in doublings. Wide on purpose — the whole point of the term
#: is to keep a gradient where the per-row terms have none, so a cap tight
#: enough to bind on an ordinary error would reintroduce exactly the saturation
#: this was written to escape. Three doublings is past any relation a kit holds.
KIT_DOUBLING_CAP = 3.0

#: dB per doubling of amplitude, so a level relation lands in the same unit as
#: a frequency or a duration ratio.
KIT_DB_PER_DOUBLING = 20.0 * math.log10(2.0)

#: (name, row field, the field is already in dB, oracle field that disqualifies
#: a member). Four relations, each naming a different repair.
#:
#: `level` reads `peak_dbfs` rather than the row's own `level_db`, for the
#: reason `_level_terms` does: that field is measured on audio no normalisation
#: has touched, and it is also the one field of the four that a probe can be
#: missing. A run that measured only a normalised render then scores no level
#: relation, which is the right answer rather than a match.
#:
#: `decay` drops a member whose ORACLE row hit the analysis ceiling: that row's
#: number is the window rather than the instrument, and a contrast taken against
#: it measures how long the capture ran. The model side is NOT dropped for the
#: same reason, and the asymmetry is the honest direction — a capped model
#: decay is a lower bound on a render that rang past the window, and a charge
#: computed from a lower bound can only understate what is really there, where
#: dropping it would hide a model that rings too long altogether.
KIT_RELATIONS = (
    ("pitch", "tone_f0_hz", False, None),
    ("decay", "decay_ms", False, "decay_capped"),
    ("colour", "centroid_hz", False, None),
    ("level", "peak_dbfs", True, None),
)


def _kit_value(row: dict, field: str, in_db: bool) -> float | None:
    """One member's value in doublings, or None where the row has no usable one."""
    value = row.get(field)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return None
    value = float(value)
    if not math.isfinite(value):
        return None
    if in_db:
        return value / KIT_DB_PER_DOUBLING
    return math.log2(value) if value > 0.0 else None


def _kit_relation(
    model_rows: list[dict],
    oracle_rows_: list[dict],
    indices: list[int],
    field: str,
    in_db: bool,
    guard: str | None,
) -> tuple[list[float], list[float]] | None:
    """(model contrasts, oracle contrasts) for one relation of one group.

    Both sides are read over exactly the same members: a contrast taken over two
    different sets is not a comparison of anything. Each side is then centred on
    its OWN median and sorted, so what survives is the spread and the spacing of
    the set and not where the set sits or which member holds which place.

    None when the members do not support the relation, or when the REFERENCE
    does not itself hold it — see KIT_MIN_SPREAD.
    """
    pairs = []
    for i in indices:
        if guard is not None and oracle_rows_[i].get(guard):
            continue
        m = _kit_value(model_rows[i], field, in_db)
        o = _kit_value(oracle_rows_[i], field, in_db)
        if m is not None and o is not None:
            pairs.append((m, o))
    if len(pairs) < KIT_MIN_MEMBERS:
        return None
    oracle_c = sorted(v - float(np.median([b for _, b in pairs])) for _, v in pairs)
    if oracle_c[-1] - oracle_c[0] < KIT_MIN_SPREAD:
        return None
    model_c = sorted(v - float(np.median([a for a, _ in pairs])) for v, _ in pairs)
    return model_c, oracle_c


def _kit_scored(
    model_rows: list[dict], oracle_rows_: list[dict], groups: dict[str, list[int]] | None
):
    """Yield (family, relation, velocity, model contrasts, oracle contrasts).

    Groups arrive as ORACLE note numbers, since a family is a fact about the
    captured kit, and are resolved to row indices — so whatever pairing the
    caller established between the two sides stands here unchanged.

    One relation per velocity, because two members of a family struck at
    different velocities are not in a relation; they are two measurements.
    """
    if not groups:
        return
    index: dict[tuple, int] = {}
    for i, row in enumerate(oracle_rows_):
        index.setdefault((row.get("note"), row.get("velocity")), i)
    velocities = sorted({row.get("velocity") for row in oracle_rows_}, key=lambda v: (v is None, v))
    for family, notes in groups.items():
        for velocity in velocities:
            members = [index[(n, velocity)] for n in notes if (n, velocity) in index]
            if len(members) < KIT_MIN_MEMBERS:
                continue
            for name, field, in_db, guard in KIT_RELATIONS:
                found = _kit_relation(model_rows, oracle_rows_, members, field, in_db, guard)
                if found is not None:
                    yield family, name, velocity, found[0], found[1]


def _kit_terms(
    model_rows: list[dict], oracle_rows_: list[dict], groups: dict[str, list[int]] | None
) -> tuple[float, int]:
    """How far the model's kit-internal relations sit from the reference's.

    A mean, so it dilutes: the shipped kit resolves to 790 member-relations
    across thirteen families, and one family badly wrong moves the number by a
    fortieth of its own charge. That is the same arithmetic `band` has over its
    twenty-five bands and it is left alone here rather than patched, because the
    two things that read this already answer it. A fit names the family it is
    working on (`--notes 41,43,45,47,48,50`), and then the term is that family
    and dilutes by nothing; a report calls `kit_report`, which never pools
    families together at all.
    """
    total = 0.0
    scored = 0
    for _family, _name, _v, model_c, oracle_c in _kit_scored(model_rows, oracle_rows_, groups):
        total += sum(min(abs(a - b), KIT_DOUBLING_CAP) for a, b in zip(model_c, oracle_c))
        scored += len(oracle_c)
    return (total / scored, scored) if scored else (0.0, 0)


def kit_report(
    model_rows: list[dict], oracle_rows_: list[dict], groups: dict[str, list[int]] | None
) -> list[dict]:
    """One readable line per family and relation, pooled over the velocities.

    The scalar term answers whether the kit's relations are right; this answers
    which of them are not, which is what someone reading a comparison table
    needs and what a single number can never say. Same measurements — a family
    absent here is one no relation could be taken over, not one that passed.

    `spread` is the reference's own range in doublings and `model_spread` the
    model's over the same members; `charge` is what the term is being fed. A
    model spread far under the reference's is a family collapsed towards one
    instrument, which is the failure this was written for.
    """
    pooled: dict[tuple[str, str], dict[str, list[float]]] = {}
    for family, name, _v, model_c, oracle_c in _kit_scored(model_rows, oracle_rows_, groups):
        acc = pooled.setdefault(
            (family, name), {"spread": [], "model_spread": [], "charge": [], "members": []}
        )
        acc["spread"].append(oracle_c[-1] - oracle_c[0])
        acc["model_spread"].append(model_c[-1] - model_c[0])
        acc["charge"].append(
            sum(min(abs(a - b), KIT_DOUBLING_CAP) for a, b in zip(model_c, oracle_c))
            / len(oracle_c)
        )
        acc["members"].append(float(len(oracle_c)))
    out = []
    for (family, name), acc in pooled.items():
        out.append(
            {
                "family": family,
                "relation": name,
                "spread": float(np.median(acc["spread"])),
                "model_spread": float(np.median(acc["model_spread"])),
                "charge": float(np.median(acc["charge"])),
                "members": int(max(acc["members"])),
                "velocities": len(acc["charge"]),
            }
        )
    return sorted(out, key=lambda r: -r["charge"])
