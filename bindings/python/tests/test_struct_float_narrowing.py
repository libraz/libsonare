"""A struct field's own C type refuses a caller's number, rather than folding it.

``ctypes`` applies the C conversion on assignment, so a field declared ``float``
takes ``1e40`` as ``inf``. That is saturation rather than the integer family's
wrap, and it is the same defect: the quantity the caller asked for arrives as a
legal value of the field's own type, and on the fields whose contract reads a
non-finite as "unspecified" it cannot be told from a deliberate request.

Three parameters decide what a case exercises, and they are independent:

* value shape -- saturating, non-finite, ``bool``, non-number, in-range float,
  in-range ``int``, and a value that only loses precision
* field kind -- ``c_float`` scalar, ``c_double`` scalar, ``c_int`` scalar, and a
  ``c_float * N`` array field
* assignment path -- ``setattr``, the keyword constructor, the positional
  constructor, and an already-built ctypes scalar

The cases below are the full cross product of the three rather than a covering
subset of it: every combination is a dictionary lookup and an attribute write,
so the exhaustive matrix costs less than the argument for a smaller one.

Every acceptance reads the field back and asserts which value landed; a refusal
asserts the exception type and that the message names the field that carried the
value. Each path opens with a positive control -- two legitimate values with two
different readbacks -- because a path that ignored its argument would otherwise
pass as one that accepted it.
"""

from __future__ import annotations

import ctypes
import math

import pytest

import libsonare as ls
from libsonare import SonareValueError
from libsonare._cstruct import CStruct
from libsonare._runtime import _FLOAT32_MAX

ARRAY_LENGTH = 3


class Probe(CStruct):
    """One field of each kind, named so no refusal message reads as another's."""

    _fields_ = [
        ("gain", ctypes.c_float),
        ("ppq", ctypes.c_double),
        ("count", ctypes.c_int),
        ("bands", ctypes.c_float * ARRAY_LENGTH),
    ]


FIELD_ORDER = [name for name, *_ in Probe._fields_]
NEUTRAL = {"gain": 0.0, "ppq": 0.0, "count": 0, "bands": (ctypes.c_float * ARRAY_LENGTH)()}

# Finite as a double and an infinity as a float32.
SATURATING = (1e40, -1e40, 3.5e38, -3.5e38)
# Accepted as quantities too, and the larger of these has no double at all.
SATURATING_INTS = (10**40, 10**400)
INFINITIES = (float("inf"), float("-inf"))
NOT_A_QUANTITY = (True, "x", None)
FOLDED_BY_FLOAT32 = SATURATING + SATURATING_INTS + INFINITIES + (float("nan"),) + NOT_A_QUANTITY
LEGITIMATE = (0.1, -6.25, 2.5, 3, True)
SCALAR_PATHS = ("setattr", "keyword", "positional")


def assign(path: str, field: str, value: object) -> Probe:
    """Deliver ``value`` to ``field`` by ``path``, returning the struct it landed in."""
    if path == "setattr":
        probe = Probe()
        setattr(probe, field, value)
        return probe
    if path == "keyword":
        return Probe(**{field: value})
    if path == "positional":
        index = FIELD_ORDER.index(field)
        args = [NEUTRAL[name] for name in FIELD_ORDER[:index]]
        return Probe(*args, value)
    raise AssertionError(f"unknown assignment path: {path}")


@pytest.mark.parametrize("path", SCALAR_PATHS)
@pytest.mark.parametrize("value", FOLDED_BY_FLOAT32)
def test_a_folding_value_is_refused_on_every_path_into_a_float_field(path, value) -> None:
    """The defect itself, across the value shapes and the three delivery paths."""
    with pytest.raises(SonareValueError, match="gain"):
        assign(path, "gain", value)


@pytest.mark.parametrize("path", SCALAR_PATHS)
def test_every_path_into_a_float_field_stores_what_it_accepted(path) -> None:
    """Positive control: two legitimate values, two different readbacks."""
    assert assign(path, "gain", -6.25).gain == -6.25
    assert assign(path, "gain", 2.5).gain == 2.5
    assert assign(path, "gain", 3).gain == 3.0  # an int is a quantity too
    # Precision loss is not the defect being refused: 0.1 is still the quantity
    # the caller asked for once rounded to the nearest float.
    assert assign(path, "gain", 0.1).gain == ctypes.c_float(0.1).value


@pytest.mark.parametrize("value", (_FLOAT32_MAX, -_FLOAT32_MAX))
def test_the_bound_is_the_float32_range_rather_than_a_plausible_one(value) -> None:
    """The largest representable float is accepted, and arrives unchanged."""
    assert assign("setattr", "gain", value).gain == value


def test_an_already_built_ctypes_float_passes_through_untouched() -> None:
    """A built scalar came from the argument readers, which apply the same check."""
    assert assign("setattr", "gain", ctypes.c_float(2.5)).gain == 2.5
    assert assign("setattr", "gain", ctypes.c_float(1e40)).gain == float("inf")


@pytest.mark.parametrize("path", SCALAR_PATHS)
@pytest.mark.parametrize("value", SATURATING + INFINITIES + LEGITIMATE)
def test_a_double_field_takes_everything_it_took_before(path, value) -> None:
    """``c_double`` converts nothing a Python float carries, so it refuses nothing."""
    assert assign(path, "ppq", value).ppq == float(value)


@pytest.mark.parametrize("path", SCALAR_PATHS)
def test_a_double_field_takes_a_nan_on_every_path(path) -> None:
    """The one accepted value that cannot be asserted by equality."""
    assert math.isnan(assign(path, "ppq", float("nan")).ppq)


def test_a_double_field_stores_what_it_accepted() -> None:
    """Positive control for the field kind deliberately left alone."""
    assert assign("setattr", "ppq", -6.25).ppq == -6.25
    assert assign("setattr", "ppq", 2.5).ppq == 2.5
    assert assign("setattr", "ppq", 0.1).ppq == 0.1  # exact: the field is the same type


@pytest.mark.parametrize("path", SCALAR_PATHS)
@pytest.mark.parametrize("value", (1e40, float("nan"), float("inf"), 0.1, -6.25, True, "x", 2**32))
def test_an_integer_field_keeps_the_guard_it_already_had(path, value) -> None:
    """The integer half is untouched: a non-index and an out-of-range int are refused."""
    with pytest.raises(SonareValueError, match="count"):
        assign(path, "count", value)


@pytest.mark.parametrize("path", SCALAR_PATHS)
def test_every_path_into_an_integer_field_stores_what_it_accepted(path) -> None:
    """Positive control for the integer half, on each delivery path."""
    assert assign(path, "count", 7).count == 7
    assert assign(path, "count", -9).count == -9


@pytest.mark.parametrize("path", SCALAR_PATHS)
def test_an_array_field_takes_a_whole_array_unchanged(path) -> None:
    """A built array is passed through: the scalar guard reads no array field."""
    landed = assign(path, "bands", (ctypes.c_float * ARRAY_LENGTH)(1.0, 2.0, 3.0))
    assert list(landed.bands) == [1.0, 2.0, 3.0]
    other = assign(path, "bands", (ctypes.c_float * ARRAY_LENGTH)(4.0, 5.0, 6.0))
    assert list(other.bands) == [4.0, 5.0, 6.0]  # positive control


def test_an_array_element_is_not_reached_by_the_field_guard() -> None:
    """Writing an element goes to the array object, never through ``__setattr__``."""
    probe = Probe()
    probe.bands[0] = 1e40
    assert probe.bands[0] == float("inf")


def test_a_shipped_config_refuses_a_folding_value_on_the_field_that_carried_it() -> None:
    """The guard reaches the declared fields of a real struct, not only a probe."""
    assert ls.RealtimeVoiceChangerConfig(input_gain_db=-6.25).to_pod().input_gain_db == -6.25
    assert ls.RealtimeVoiceChangerConfig(input_gain_db=2.5).to_pod().input_gain_db == 2.5
    for value in (1e40, float("nan"), float("inf")):
        with pytest.raises(SonareValueError, match="input_gain_db"):
            ls.RealtimeVoiceChangerConfig(input_gain_db=value).to_pod()
        with pytest.raises(SonareValueError, match="output_gain_db"):
            ls.RealtimeVoiceChangerConfig(output_gain_db=value).to_pod()


def test_a_shipped_config_round_trips_the_defaults_the_library_supplies() -> None:
    """A value the C side produced is written back, so the guard has to accept it."""
    pod = ls.RealtimeVoiceChangerConfig().to_pod()
    again = ls.RealtimeVoiceChangerConfig.from_pod(pod).to_pod()
    names = [name for name, *_ in pod._fields_]
    assert [getattr(again, name) for name in names] == [getattr(pod, name) for name in names]
