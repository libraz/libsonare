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
    project.set_sample_rate(48000.0)
    return project


def test_all_parts_constant():
    assert PART_RIG_ALL_PARTS == 0xFF


def test_absent_entry_reads_none():
    with _project() as project:
        assert project.get_part_rig(DESTINATION, 0) is None


def test_set_get_clear_round_trip():
    with _project() as project:
        project.set_part_rig(DESTINATION, 0, mode="none")
        assert project.get_part_rig(DESTINATION, 0) == ("none", None)

        # A bank entry is kept explicitly so it can override the destination default.
        project.set_part_rig(DESTINATION, PART_RIG_ALL_PARTS, mode="bank")
        assert project.get_part_rig(DESTINATION, PART_RIG_ALL_PARTS) == ("bank", None)

        project.clear_part_rig(DESTINATION, 0)
        assert project.get_part_rig(DESTINATION, 0) is None
        assert project.get_part_rig(DESTINATION, PART_RIG_ALL_PARTS) is not None


def test_chain_round_trip():
    with _project() as project:
        project.set_part_rig(DESTINATION, 1, mode="chain", inserts=CHAIN)
        mode, inserts = project.get_part_rig(DESTINATION, 1)
        assert mode == "chain"
        assert [i["processor"] for i in inserts] == ["saturation.softClipper"]
        assert inserts[0]["params"] == {}


def test_params_accepts_json_string():
    with _project() as project:
        project.set_part_rig(
            DESTINATION,
            1,
            mode="chain",
            inserts=[{"processor": "saturation.softClipper", "params": "{}"}],
        )
        assert project.get_part_rig(DESTINATION, 1)[0] == "chain"


def test_undo_restores():
    with _project() as project:
        project.set_part_rig(DESTINATION, 0, mode="none")
        project.set_part_rig(DESTINATION, 0, mode="bank")
        project.undo()
        assert project.get_part_rig(DESTINATION, 0) == ("none", None)
        project.undo()
        assert project.get_part_rig(DESTINATION, 0) is None
        project.redo()
        assert project.get_part_rig(DESTINATION, 0) == ("none", None)


def test_invalid_input_is_refused():
    with _project() as project:
        with pytest.raises(SonareValueError, match="mode"):
            project.set_part_rig(DESTINATION, 0, mode="wet")
        with pytest.raises(SonareValueError, match="processor"):
            project.set_part_rig(DESTINATION, 0, mode="chain", inserts=[{"params": {}}])
        with pytest.raises(SonareValueError, match="part"):
            project.set_part_rig(DESTINATION, 256, mode="none")

        with pytest.raises(SonareError) as bad_part:
            project.set_part_rig(DESTINATION, 16, mode="none")
        assert bad_part.value.code == int(ErrorCode.INVALID_PARAMETER)

        with pytest.raises(SonareError) as empty_chain:
            project.set_part_rig(DESTINATION, 0, mode="chain", inserts=[])
        assert empty_chain.value.code == int(ErrorCode.INVALID_PARAMETER)

        with pytest.raises(SonareError) as bad_json:
            project.set_part_rig(
                DESTINATION,
                0,
                mode="chain",
                inserts=[{"processor": "saturation.softClipper", "params": "{not json"}],
            )
        assert bad_json.value.code == int(ErrorCode.INVALID_PARAMETER)
        assert project.get_part_rig(DESTINATION, 0) is None


def test_engine_refuses_builtin_instrument():
    with RealtimeEngine() as engine:
        engine.prepare(48000.0, 128, 16, 16)
        engine.set_builtin_instrument(BuiltinSynthConfig(), DESTINATION)
        with pytest.raises(SonareError) as refusal:
            engine.set_part_rig(DESTINATION, 0, mode="none")
        assert refusal.value.code == int(ErrorCode.NOT_SUPPORTED)


def test_engine_refuses_unbound_destination():
    with RealtimeEngine() as engine:
        engine.prepare(48000.0, 128, 16, 16)
        with pytest.raises(SonareError) as refusal:
            engine.set_part_rig(DESTINATION, 0, mode="none")
        assert refusal.value.code == int(ErrorCode.INVALID_PARAMETER)
        with pytest.raises(SonareValueError, match="mode"):
            engine.set_part_rig(DESTINATION, 0, mode="wet")
