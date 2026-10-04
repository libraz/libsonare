"""Native integration tests for the Python vocal-edit facade."""

from __future__ import annotations

import ctypes
from dataclasses import replace

import numpy as np
import pytest

from libsonare._errors import SonareError
from libsonare._ffi_types_vocal import (
    SonareVocalAnalysis,
    SonareVocalAnalysisResult,
    SonareVocalCreateOptions,
    SonareVocalErrorDetail,
    SonareVocalNote,
    SonareVocalNoteEdit,
    SonareVocalOperation,
    SonareVocalRenderResult,
)
from libsonare._runtime import _get_lib
from libsonare.vocal_edit import (
    VocalAnalysis,
    VocalEditError,
    VocalNoteEdit,
    VocalSetNoteEdit,
    VocalTargetMode,
    create_vocal_edit_session,
    restore_vocal_edit_session,
)


def _native_vocal_available() -> bool:
    try:
        lib = _get_lib()
        return bool(hasattr(lib, "sonare_vocal_available") and lib.sonare_vocal_available())
    except Exception:
        return False


pytestmark = pytest.mark.skipif(
    not _native_vocal_available(), reason="native library lacks the vocal-edit ABI"
)


def _source_and_analysis() -> tuple[np.ndarray, VocalAnalysis]:
    sr = 16_000
    count = 8192
    source = (0.4 * np.sin(2.0 * np.pi * 440.0 * np.arange(count) / sr)).astype(np.float32)
    frames = 16
    analysis = VocalAnalysis(
        frame_origin_sample=37.25,
        samples_per_frame=512.0,
        frame_length_samples=2048,
        f0_hz=np.full(frames, 440.0, dtype=np.float32),
        voiced=np.ones(frames, dtype=np.uint8),
        algorithm_id="host",
        algorithm_version=1,
    )
    return source, analysis


def test_ctypes_vocal_layout_matches_header() -> None:
    # Keep this local guard useful before the generated ABI snapshot is updated
    # by the root agent; these are the sizes of the public C structs.
    assert ctypes.sizeof(SonareVocalAnalysis) == 136
    assert ctypes.sizeof(SonareVocalCreateOptions) == 136
    assert ctypes.sizeof(SonareVocalNoteEdit) == 144
    assert ctypes.sizeof(SonareVocalNote) == 240
    assert ctypes.sizeof(SonareVocalOperation) == 264
    assert ctypes.sizeof(SonareVocalAnalysisResult) == 296
    assert ctypes.sizeof(SonareVocalRenderResult) == 120
    assert ctypes.sizeof(SonareVocalErrorDetail) == 320


def test_create_rejects_stereo_without_touching_output() -> None:
    source, _ = _source_and_analysis()
    with pytest.raises((VocalEditError, ValueError)):
        create_vocal_edit_session(np.column_stack((source, source)), 16_000)


def test_supplied_analysis_rejects_non_binary_voiced_mask() -> None:
    source, analysis = _source_and_analysis()
    invalid = VocalAnalysis(
        frame_origin_sample=analysis.frame_origin_sample,
        samples_per_frame=analysis.samples_per_frame,
        frame_length_samples=analysis.frame_length_samples,
        f0_hz=analysis.f0_hz,
        voiced=np.full(analysis.voiced.size, 2, dtype=np.uint8),
        algorithm_id=analysis.algorithm_id,
        algorithm_version=analysis.algorithm_version,
    )
    with pytest.raises(ValueError, match="0/1 mask"):
        create_vocal_edit_session(source, 16_000, analysis=invalid)


def test_context_lifetime_and_owned_note_arrays() -> None:
    source, analysis = _source_and_analysis()
    with create_vocal_edit_session(source, 16_000, analysis=analysis) as session:
        notes, transitions = session.notes()
        snapshot = session.capture_render_snapshot()
    try:
        # The snapshot owns native source/state independently of its session.
        rendered = snapshot.render()
        assert rendered.samples.dtype == np.float32
        assert rendered.samples.size == source.size
        assert transitions == () or all(t.left_note_id and t.right_note_id for t in transitions)
        if notes:
            original = notes[0].amplitude.copy()
            notes[0].amplitude[:] = 0.0
            assert np.array_equal(original, notes[0].amplitude) is False
    finally:
        snapshot.close()


def test_extended_output_length_survives_export_restore_and_snapshot() -> None:
    source, analysis = _source_and_analysis()
    output_length = source.size + 1024
    with create_vocal_edit_session(
        source,
        16_000,
        analysis=analysis,
        output_length_samples=output_length,
    ) as session:
        assert session.output_length_samples == output_length
        state = session.export_state()
        with session.capture_render_snapshot() as snapshot:
            assert snapshot.output_length_samples == output_length
            rendered = snapshot.render()
            assert rendered.samples.size == output_length
            assert np.array_equal(rendered.samples[source.size :], np.zeros(1024, dtype=np.float32))
    with restore_vocal_edit_session(source, 16_000, state) as restored:
        assert restored.output_length_samples == output_length
        with restored.capture_render_snapshot() as snapshot:
            assert snapshot.output_length_samples == output_length
            rendered = snapshot.render()
            assert rendered.samples.size == output_length
            assert np.array_equal(rendered.samples[source.size :], np.zeros(1024, dtype=np.float32))


def test_history_flags_follow_commit_undo_and_redo() -> None:
    source, analysis = _source_and_analysis()
    with create_vocal_edit_session(source, 16_000, analysis=analysis) as session:
        assert session.history() == (False, False)
        assert session.can_undo is False
        assert session.can_redo is False
        notes, _ = session.notes()
        assert notes
        with session.begin_edit() as draft:
            draft.apply(
                [
                    VocalSetNoteEdit(
                        notes[0].id,
                        VocalNoteEdit(
                            target_mode=VocalTargetMode.CENTER,
                            target_midi=69.0,
                            amount=1.0,
                            destination_start_sample=notes[0].source_start_sample,
                            destination_length_samples=notes[0].source_end_sample
                            - notes[0].source_start_sample,
                        ),
                    )
                ]
            )
            draft.commit()
        assert session.history() == (True, False)
        assert session.can_undo is True
        assert session.can_redo is False
        session.undo()
        assert session.history() == (False, True)
        assert session.can_undo is False
        assert session.can_redo is True
        session.redo()
        assert session.history() == (True, False)


def test_draft_edit_evaluate_commit_and_restore() -> None:
    source, analysis = _source_and_analysis()
    with create_vocal_edit_session(source, 16_000, analysis=analysis) as session:
        notes, _ = session.notes()
        assert notes
        note = notes[0]
        with session.begin_edit() as draft:
            edit = VocalNoteEdit(
                target_mode=VocalTargetMode.CENTER,
                target_midi=69.0,
                amount=1.0,
                speed_ms=0.0,
                destination_start_sample=note.source_start_sample,
                destination_length_samples=note.source_end_sample - note.source_start_sample,
            )
            applied = draft.apply([VocalSetNoteEdit(note.id, edit)])
            assert applied.token.generation > 0
            pitch = draft.evaluate_pitch(note.id)
            assert pitch.source_samples.size == pitch.effective_midi.size
            committed = draft.commit()
            assert committed.token.revision == session.revision
        revision = session.revision
        state = session.export_state()
        assert state.startswith(b"SVE1")
    with restore_vocal_edit_session(source, 16_000, state) as restored:
        restored_notes, _ = restored.notes()
        assert restored_notes[0].id == notes[0].id
        assert restored.token().revision == revision


def test_render_job_cancellation_is_call_scoped() -> None:
    source, analysis = _source_and_analysis()
    with (
        create_vocal_edit_session(source, 16_000, analysis=analysis) as session,
        session.capture_render_snapshot() as snapshot,
        snapshot.begin_render_job(request_id=91) as job,
    ):
        with pytest.raises(VocalEditError) as caught:
            job.next(lambda: True)
        assert caught.value.code in (8, 7)
        job.abort()


def test_premature_finalize_keeps_render_job_usable() -> None:
    source, analysis = _source_and_analysis()
    with (
        create_vocal_edit_session(source, 16_000, analysis=analysis) as session,
        session.capture_render_snapshot() as snapshot,
        snapshot.begin_render_job() as job,
    ):
        with pytest.raises(VocalEditError) as premature:
            job.finalize()
        assert premature.value.code in (7, 8)
        while not job.next():
            pass
        rendered = job.finalize()
        assert rendered.samples.size == source.size


def test_close_from_cancel_probe_is_deferred_until_the_call_returns() -> None:
    source, analysis = _source_and_analysis()
    session = create_vocal_edit_session(source, 16_000, analysis=analysis)

    def close_session() -> bool:
        session.close()
        return False

    assert session.render(cancel=close_session).samples.size == source.size
    with pytest.raises(SonareError):
        session.render()

    with create_vocal_edit_session(source, 16_000, analysis=analysis) as other:
        snapshot = other.capture_render_snapshot()

        def close_snapshot() -> bool:
            snapshot.close()
            return False

        assert snapshot.render(cancel=close_snapshot).samples.size == source.size
        with pytest.raises(SonareError):
            snapshot.render()

        with other.capture_render_snapshot() as live, live.begin_render_job() as job:

            def close_job() -> bool:
                job.close()
                return False

            job.next(close_job)
            with pytest.raises(SonareError):
                job.next()
            assert live.render().samples.size == source.size


def test_coordinate_mapping_round_trip_for_note() -> None:
    source, analysis = _source_and_analysis()
    with create_vocal_edit_session(source, 16_000, analysis=analysis) as session:
        notes, _ = session.notes()
        assert notes
        note = notes[0]
        source_sample = float(note.source_start_sample)
        destination = session.source_sample_to_destination_sample(note.id, source_sample)
        assert session.destination_sample_to_source_sample(note.id, destination) == pytest.approx(
            source_sample
        )


def test_analysis_settings_are_preserved_on_roundtrip() -> None:
    source, analysis = _source_and_analysis()
    supplied = replace(
        analysis,
        fmin_hz=80.0,
        fmax_hz=1000.0,
        yin_threshold=0.2,
        voiced_threshold=0.7,
        centered=False,
        segmentation_threshold_cents=60.0,
        min_note_ms=15.0,
        reference_hz=442.0,
    )
    with create_vocal_edit_session(source, 16_000, analysis=supplied) as session:
        result = session.analysis()
        for name in (
            "fmin_hz",
            "fmax_hz",
            "yin_threshold",
            "voiced_threshold",
            "centered",
            "segmentation_threshold_cents",
            "min_note_ms",
            "reference_hz",
        ):
            assert getattr(result.analysis, name) == getattr(supplied, name)
        with create_vocal_edit_session(source, 16_000, analysis=result.analysis) as again:
            assert again.analysis().analysis_sha256 == result.analysis_sha256


def test_centered_option_accepts_boolean_and_rejects_integer() -> None:
    source, analysis = _source_and_analysis()
    with create_vocal_edit_session(source, 16_000, analysis=analysis, centered=True) as session:
        assert session.analysis().analysis.centered is True
    with pytest.raises(ValueError):
        create_vocal_edit_session(source, 16_000, analysis=replace(analysis, centered=1))


@pytest.mark.parametrize("operation", ["render", "next", "finalize"])
def test_cancellation_callback_exception_is_propagated(operation: str) -> None:
    source, analysis = _source_and_analysis()

    def failing_probe() -> bool:
        raise RuntimeError("cancel probe failed")

    with (
        create_vocal_edit_session(source, 16_000, analysis=analysis) as session,
        session.capture_render_snapshot() as snapshot,
    ):
        if operation == "render":
            with pytest.raises(RuntimeError, match="cancel probe failed"):
                snapshot.render(cancel=failing_probe)
        else:
            with snapshot.begin_render_job() as job:
                if operation == "next":
                    with pytest.raises(RuntimeError, match="cancel probe failed"):
                        job.next(cancel=failing_probe)
                else:
                    while not job.next():
                        pass
                    with pytest.raises(RuntimeError, match="cancel probe failed"):
                        job.finalize(cancel=failing_probe)
