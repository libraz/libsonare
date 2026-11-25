"""Track fader/pan automation id lifetimes; mirrors tests/api/sonare_c_engine_strip_test.cpp."""

import pytest

from libsonare import EngineClip, RealtimeEngine, SonareError, SonareValueError

_BLOCK = 256
_FRAMES = _BLOCK * 80


def _alive(engine: RealtimeEngine, param_id: int) -> bool:
    try:
        engine.parameter_info(param_id)
    except SonareError:
        return False
    return True


def _settled(engine: RealtimeEngine, blocks: int = 30) -> float:
    out = [[0.0] * _BLOCK]
    for _ in range(blocks):
        out = engine.process([[0.0] * _BLOCK])
    return out[0][-1]


@pytest.mark.parametrize(
    ("method", "args"),
    [
        ("resolve_strip_automation_id", ("faderDb",)),
        ("resolve_strip_insert_automation_id", (0, "levelDb")),
        ("set_strip_pan", (0.0,)),
    ],
)
def test_missing_strip_names_the_callers_ref(method: str, args: tuple) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        with pytest.raises(SonareValueError) as refused:
            getattr(engine, method)(99, *args)
        assert str(refused.value) == (
            f"RealtimeEngine.{method}: ref must be the id of a track in this engine"
        )


def test_fader_id_keeps_its_track_across_a_reorder_and_retires_with_it() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as engine:
        # Only track 20 sounds, so the output reads track 20's fader alone.
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    track_id=20,
                    channels=[[1.0] * _FRAMES],
                    start_ppq=0.0,
                    length_samples=_FRAMES,
                )
            ]
        )
        engine.set_track_lanes([{"track_id": 10}, {"track_id": 20}])
        fader10 = engine.resolve_strip_automation_id(10, "faderDb")
        assert engine.resolve_strip_automation_id(10, "pan") != fader10
        with pytest.raises(SonareError):
            engine.resolve_strip_automation_id(30, "faderDb")
        with pytest.raises(SonareError):
            engine.resolve_strip_automation_id(10, "width")

        engine.set_track_lanes([{"track_id": 20}, {"track_id": 10}])
        assert engine.resolve_strip_automation_id(10, "faderDb") == fader10
        engine.set_parameter(fader10, -60.0)
        engine.play()
        assert _settled(engine) == pytest.approx(1.0, abs=1e-3)

        engine.set_track_lanes([{"track_id": 20}])
        assert not _alive(engine, fader10)
        engine.set_parameter(fader10, -60.0)
        assert _settled(engine) == pytest.approx(1.0, abs=1e-3)
        engine.set_track_lanes([{"track_id": 20}, {"track_id": 10}])
        assert engine.resolve_strip_automation_id(10, "faderDb") != fader10


@pytest.mark.parametrize(
    ("ref", "error", "match"),
    [
        ("bus", SonareValueError, "track id or 'master'"),
        (None, TypeError, "ref"),
        (True, TypeError, "ref"),
        (1.5, SonareValueError, "ref"),
    ],
)
def test_a_strip_reference_is_a_track_id_or_master(
    ref: object, error: type[Exception], match: str
) -> None:
    """``None`` is not a spelling of the master strip; only ``"master"`` is."""
    with RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        with pytest.raises(error, match=match):
            engine.resolve_strip_automation_id(ref, "faderDb")  # type: ignore[arg-type]


@pytest.mark.parametrize(
    "call",
    [
        lambda e: e.set_strip_pan("master", 0.0),
        lambda e: e.set_strip_pan_law("master", "const3Db"),
        lambda e: e.set_strip_pan_mode("master", "balance"),
        lambda e: e.set_strip_dual_pan("master", -1.0, 1.0),
        lambda e: e.set_strip_surround_pan("master", azimuth=30.0),
        lambda e: e.set_strip_channel_delay_samples("master", 8),
    ],
)
def test_track_only_strip_setters_refuse_the_master_strip(call) -> None:
    with (
        RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as engine,
        pytest.raises(SonareValueError, match="master strip"),
    ):
        call(engine)


def test_width_resolves_on_the_master_strip_only() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        assert engine.resolve_strip_automation_id("master", "width") > 0
        with pytest.raises(
            SonareValueError,
            match="kind must be the name of a track strip parameter: faderDb or pan",
        ):
            engine.resolve_strip_automation_id(10, "width")
        with pytest.raises(SonareValueError, match="kind"):
            engine.resolve_strip_automation_id("master", "gain")  # type: ignore[arg-type]


def test_set_strip_fader_db_drives_the_track_fader() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    track_id=20,
                    channels=[[1.0] * _FRAMES],
                    start_ppq=0.0,
                    length_samples=_FRAMES,
                )
            ]
        )
        engine.set_track_lanes([{"track_id": 20}])
        engine.set_strip_fader_db(20, -60.0)
        engine.play()
        assert _settled(engine) == pytest.approx(0.001, abs=1e-4)
        with pytest.raises(SonareError):
            engine.set_strip_fader_db(30, 0.0)
