"""Part-rig binding tests: the project set/get/clear round trip with undo, the
refusals that have to stay distinguishable, and the engine's direct call on an
instrument that has no part rigs.
"""

from __future__ import annotations

import pytest

from libsonare import (
    PART_RIG_ALL_PARTS,
    BuiltinSynthConfig,
    ErrorCode,
    PartRigEntry,
    Project,
    RealtimeEngine,
    SonareError,
    SonareValueError,
)

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

DESTINATION = 3
CHAIN = [{"processor": "saturation.softClipper", "params": {}}]


def _project() -> Project:
    project = Project()
    project.set_sample_rate(48000)
    return project


def test_all_parts_constant():
    assert PART_RIG_ALL_PARTS == 0xFF


def test_absent_entry_reads_none():
    with _project() as project:
        assert project.part_rig(DESTINATION, 0) is None


def test_set_get_clear_round_trip():
    with _project() as project:
        project.set_part_rig(DESTINATION, 0, mode="none")
        assert project.part_rig(DESTINATION, 0) == PartRigEntry("none", None)

        # A bank entry is kept explicitly so it can override the destination default.
        project.set_part_rig(DESTINATION, PART_RIG_ALL_PARTS, mode="bank")
        assert project.part_rig(DESTINATION, PART_RIG_ALL_PARTS) == PartRigEntry("bank", None)

        project.clear_part_rig(DESTINATION, 0)
        assert project.part_rig(DESTINATION, 0) is None
        assert project.part_rig(DESTINATION, PART_RIG_ALL_PARTS) is not None


def test_chain_round_trip():
    with _project() as project:
        project.set_part_rig(DESTINATION, 1, mode="chain", inserts=CHAIN)
        entry = project.part_rig(DESTINATION, 1)
        assert isinstance(entry, PartRigEntry)
        with pytest.raises(TypeError):
            iter(entry)
        assert entry.mode == "chain"
        inserts = entry.inserts
        assert inserts is not None
        assert [i["processor"] for i in inserts] == ["saturation.softClipper"]
        assert inserts[0]["params"] == {}


def test_params_json_string_is_refused():
    with _project() as project:
        with pytest.raises(SonareValueError, match=r"inserts\[0\]\.params must be an object"):
            project.set_part_rig(
                DESTINATION,
                1,
                mode="chain",
                inserts=[{"processor": "saturation.softClipper", "params": "{}"}],
            )
        assert project.part_rig(DESTINATION, 1) is None


def test_undo_restores():
    with _project() as project:
        project.set_part_rig(DESTINATION, 0, mode="none")
        project.set_part_rig(DESTINATION, 0, mode="bank")
        project.undo()
        assert project.part_rig(DESTINATION, 0) == PartRigEntry("none", None)
        project.undo()
        assert project.part_rig(DESTINATION, 0) is None
        project.redo()
        assert project.part_rig(DESTINATION, 0) == PartRigEntry("none", None)


def test_invalid_input_is_refused():
    with _project() as project:
        with pytest.raises(SonareValueError, match="mode"):
            project.set_part_rig(DESTINATION, 0, mode="wet")
        with pytest.raises(TypeError, match="mode"):
            project.set_part_rig(DESTINATION, 0, mode=1)  # type: ignore[arg-type]
        # Refused by the facade before any document reaches the library.
        with pytest.raises(SonareValueError, match="processor"):
            project.set_part_rig(DESTINATION, 0, mode="chain", inserts=[{"params": {}}])
        with pytest.raises(SonareValueError, match="part"):
            project.set_part_rig(DESTINATION, 256, mode="none")

        with pytest.raises(SonareValueError) as bad_part:
            project.set_part_rig(DESTINATION, 16, mode="none")
        assert bad_part.value.code == int(ErrorCode.INVALID_PARAMETER)

        with pytest.raises(SonareValueError, match="inserts must hold 1 to 8 entries") as empty:
            project.set_part_rig(DESTINATION, 0, mode="chain", inserts=[])
        assert empty.value.code == int(ErrorCode.INVALID_PARAMETER)

        with pytest.raises(SonareValueError, match=r"inserts\[0\]\.params") as bad_params:
            project.set_part_rig(
                DESTINATION,
                0,
                mode="chain",
                inserts=[{"processor": "saturation.softClipper", "params": "{not json"}],
            )
        assert bad_params.value.code == int(ErrorCode.INVALID_PARAMETER)
        assert project.part_rig(DESTINATION, 0) is None


@pytest.mark.parametrize(
    ("mode", "inserts", "predicate"),
    [
        ("chain", None, "must not be None when mode is chain"),
        ("bank", CHAIN, "must be None unless mode is chain"),
    ],
)
def test_inserts_presence_refusal_says_none(mode, inserts, predicate):
    with _project() as project:
        with pytest.raises(SonareValueError) as exc:
            project.set_part_rig(DESTINATION, 0, mode=mode, inserts=inserts)
        assert predicate in str(exc.value)
        assert "NULL" not in str(exc.value)


def test_engine_refuses_builtin_instrument():
    with RealtimeEngine(sample_rate=48000) as engine:
        engine.prepare(48000, 128, 16, 16)
        engine.set_builtin_instrument(BuiltinSynthConfig(), DESTINATION)
        with pytest.raises(SonareError) as refusal:
            engine.set_part_rig(DESTINATION, 0, mode="none")
        assert refusal.value.code == int(ErrorCode.NOT_SUPPORTED)


def test_engine_refuses_unbound_destination():
    with RealtimeEngine(sample_rate=48000) as engine:
        engine.prepare(48000, 128, 16, 16)
        with pytest.raises(SonareValueError) as refusal:
            engine.set_part_rig(DESTINATION, 0, mode="none")
        assert refusal.value.code == int(ErrorCode.INVALID_PARAMETER)
        with pytest.raises(SonareValueError, match="mode"):
            engine.set_part_rig(DESTINATION, 0, mode="wet")
