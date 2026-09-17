"""Prove every guard the public-integer-domain gate trusts actually refuses.

``tests/conformance/check_public_integer_domains.py`` is a static reader: it
decides that a published argument *reaches* a guard, never that the guard works.
Those are different claims and the second is the load-bearing one -- a
vocabulary of thirty-odd names that all silently returned their input would leave
the gate green over a binding with no domain checks at all. The gate cannot
answer it, because answering it means calling the guards, which means the
package and its dylib; so it is answered here, where both already are.

The probe population is **the vocabulary the gate derived**, imported from the
checker rather than restated: every name it found must be either probed or on the
skip list with a reason, and a name in neither is a failure. Add a guard to the
narrowing home and this file demands a probe for it the same day the gate starts
crediting it.

Each probe is paired with a **control** in the same domain that must be
ACCEPTED. Without one, a guard that raised on everything would read as rigorous,
and so would a probe that had drifted onto an argument the guard rejects for the
wrong reason -- a misspelled keyword raises too.

The out-of-domain value differs by family because the domain does. The integer
guards get a fraction (``valid + 0.5``), which is the value the C conversion
truncates; the ``c_float`` guards get a double no 32-bit float holds, because a
fraction is a perfectly legal float and saturation rather than truncation is what
they exist to refuse; the ``c_double`` guard gets an infinity, the only thing a
double cannot represent as a quantity.
"""

from __future__ import annotations

import importlib
import importlib.util
import math
from pathlib import Path

import numpy as np
import pytest

_CHECKER = (
    Path(__file__).resolve().parents[3]
    / "tests"
    / "conformance"
    / "check_public_integer_domains.py"
)
_SPEC = importlib.util.spec_from_file_location("check_public_integer_domains", _CHECKER)
assert _SPEC and _SPEC.loader
gate = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(gate)

_runtime = importlib.import_module("libsonare._runtime")
_narrowing = importlib.import_module("libsonare._narrowing")

# Where a guard's implementation is looked up: the gate's GUARD_HOME as module
# objects rather than file names.
HOMES = (_runtime, _narrowing)

# The refusals the family is spelled with. `SonareValueError` derives from
# `ValueError`, so naming it separately is what keeps the assertion readable; the
# builtins are here because a guard reaching an enum class or a NumPy cast can
# raise one on the way to the same outcome, and admitting only `SonareValueError`
# would fail on a refusal that is correct.
REFUSALS = (_runtime.SonareValueError, ValueError, TypeError, OverflowError)

# `guard -> (out-of-domain arguments, in-domain arguments)`. The second half is
# the control: it must be accepted, or the probe proves nothing.
PROBES: dict[str, tuple[tuple[object, ...], tuple[object, ...]]] = {
    # The root predicates.
    "_narrow_int": ((1.5, "probe", 0, 100), (7, "probe", 0, 100)),
    "_narrow_float": ((1e40, "probe"), (1.5, "probe")),
    "_narrow_double": ((math.inf, "probe"), (1.5, "probe")),
    # The conversion readers. Every integer width takes the same fraction: the
    # value its C type would truncate into a different legal setting.
    "_to_c_int": ((1.5, "probe"), (7, "probe")),
    "_to_c_int32": ((1.5, "probe"), (7, "probe")),
    "_to_c_int64": ((1.5, "probe"), (7, "probe")),
    "_to_c_uint": ((1.5, "probe"), (7, "probe")),
    "_to_c_uint32": ((1.5, "probe"), (7, "probe")),
    "_to_c_uint16": ((1.5, "probe"), (7, "probe")),
    "_to_c_uint8": ((1.5, "probe"), (7, "probe")),
    "_to_c_size_t": ((1.5, "probe"), (7, "probe")),
    "_to_c_float": ((1e40, "probe"), (1.5, "probe")),
    "_to_c_double": ((math.inf, "probe"), (1.5, "probe")),
    # The array refusals, where the fraction is in an element rather than in the
    # argument.
    "_reject_unrepresentable_int32": ((np.array([1.5]), "probe"), (np.array([1, 2]), "probe")),
    "_to_c_int_array": (([1.5], "probe"), ([1, 2], "probe")),
    # The field and option validators, which add a domain on top of the range.
    "_validate_c_int_field": (("probe", 1.5, "probe"), ("probe", 7, "probe")),
    "_validate_hpss_kernel": (("probe", 31.5, "probe"), ("probe", 31, "probe")),
    "_validate_stft_n_fft": (("probe", 2048.5), ("probe", 2048)),
    "_validate_effect_fft_options": (("probe", 2048.5, 512), ("probe", 2048, 512)),
    "_validate_scalar": (("probe", math.nan, "probe"), ("probe", 1.5, "probe")),
    "_validate_samples": (("probe", [math.nan]), ("probe", [0.1, 0.2])),
    "_require_power_of_two": ((1024.5, "probe"), (1024, "probe")),
    # The closed-domain resolvers. A fraction is neither an ordinal nor a name,
    # so it leaves the domain the way an unknown spelling does.
    "_resolve_enum": ((1.5, {"alpha": 3}, "probe"), ("alpha", {"alpha": 3}, "probe")),
    "_synth_enum_value": ((1.5, {"alpha": 3}, "probe"), ("alpha", {"alpha": 3}, "probe")),
    "_pan_mode_value": ((1.5,), ("balance",)),
    "_pan_law_value": ((1.5,), ("-3db",)),
    "_curve_value": ((1.5,), ("linear",)),
    "_meter_tap_value": ((1.5,), ("post-fader",)),
    "_send_timing_value": ((1.5,), ("post-fader",)),
    "_warp_mode_value": ((1.5,), ("off",)),
    "_profile_value": (("not-a-key-profile",), ("krumhansl",)),
    "_mode_values": (("not-a-mode",), ("major-minor",)),
}

# Guards with no integer domain to leave, so no probe of this shape exists. Not
# a gap: each coerces a buffer of 32-bit floats, where a fraction is the ordinary
# case and `valid + 0.5` is a legal sample rather than a refusable value. Their
# own refusals -- an input NumPy cannot make a numeric buffer from -- answer a
# different question and belong to a different check.
SKIPS: dict[str, str] = {
    "_to_c_float_array": (
        "coerces a float32 sample buffer; every finite element is in domain, and "
        "no integer becomes a C integer here"
    ),
    "_to_c_float_array_owned": "as _to_c_float_array, over a fresh writable copy",
}


@pytest.fixture(scope="module")
def vocabulary() -> frozenset[str]:
    """The guard set the static gate derived, read from the gate itself."""
    return frozenset(gate.vocabulary(gate.Tree(gate.BINDING)))


def _guard(name: str):
    for home in HOMES:
        found = getattr(home, name, None)
        if found is not None:
            return found
    pytest.fail(f"{name} is in the derived vocabulary but resolves in neither home module")


def test_every_derived_guard_is_probed(vocabulary: frozenset[str]) -> None:
    """The probe population IS the vocabulary, so a guard with no probe fails."""
    unprobed = sorted(vocabulary - set(PROBES) - set(SKIPS))
    assert unprobed == [], (
        "these guards are in the vocabulary the gate derived, with no probe and no "
        f"skip reason, so nothing proves they refuse anything: {unprobed}"
    )
    stale = sorted((set(PROBES) | set(SKIPS)) - vocabulary)
    assert stale == [], (
        "these probes name a guard the vocabulary no longer has, so they assert a "
        f"decision about a name nothing holds: {stale}"
    )
    print(f"vocabulary: {len(vocabulary)}  probed: {len(PROBES)}  skipped: {len(SKIPS)}")


def test_skips_carry_a_reason() -> None:
    """A skip is a decision, so it says what it decided, and it is counted."""
    unreasoned = sorted(name for name, reason in SKIPS.items() if not reason.strip())
    assert unreasoned == [], unreasoned
    print(f"skipped with a reason: {len(SKIPS)} -> {sorted(SKIPS)}")


@pytest.mark.parametrize("name", sorted(PROBES))
def test_guard_refuses_out_of_domain(name: str) -> None:
    """The load-bearing half: the guard raises on the value it exists for."""
    out_of_domain, _ = PROBES[name]
    with pytest.raises(REFUSALS):
        _guard(name)(*out_of_domain)


@pytest.mark.parametrize("name", sorted(PROBES))
def test_guard_accepts_in_domain(name: str) -> None:
    """The control. A guard that refused everything would pass the probe above."""
    _, in_domain = PROBES[name]
    _guard(name)(*in_domain)
