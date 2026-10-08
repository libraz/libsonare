"""Seconds beside samples on edit requests, and absence in place of ``-1``."""

from __future__ import annotations

import numpy as np
import pytest

import libsonare
from libsonare import (
    NoteEdit,
    NoteObject,
    PercussiveEvent,
    PercussiveEventEdit,
    RealtimeEngine,
    SonareValueError,
    SpectralRegionOp,
)

SR = 22050


def _tone(length: int, freq: float = 440.0) -> np.ndarray:
    return (0.4 * np.sin(2.0 * np.pi * freq * np.arange(length) / SR)).astype(np.float32)


def _noise(length: int, seed: int = 1) -> np.ndarray:
    return (0.5 * np.random.default_rng(seed).uniform(-1.0, 1.0, length)).astype(np.float32)


class TestSpectralEdit:
    samples = _tone(SR // 2)
    base = {"low_hz": 300.0, "high_hz": 600.0, "gain_db": -30.0, "mode": "gain"}

    def _edit(self, **op: object) -> np.ndarray:
        return np.asarray(
            libsonare.spectral_edit(self.samples, SR, [SpectralRegionOp(**{**self.base, **op})])
        )

    def test_omitted_end_is_the_end_of_the_signal(self) -> None:
        omitted = self._edit()
        assert np.array_equal(omitted, self._edit(end_sample=self.samples.size))
        assert not np.array_equal(omitted, self.samples)

    def test_negative_bounds_are_refused(self) -> None:
        for field in ("start_sample", "end_sample"):
            with pytest.raises(SonareValueError, match=field):
                self._edit(**{field: -1})
        with pytest.raises(SonareValueError, match="end_sec"):
            self._edit(end_sec=-0.1)

    def test_seconds_equal_samples(self) -> None:
        by_samples = self._edit(start_sample=2205, end_sample=8820)
        assert np.array_equal(by_samples, self._edit(start_sec=0.1, end_sec=0.4))

    def test_both_spellings_are_refused_naming_both(self) -> None:
        with pytest.raises(SonareValueError, match=r"start_sample.*start_sec"):
            self._edit(start_sample=0, start_sec=0.0)
        with pytest.raises(SonareValueError, match=r"end_sample.*end_sec"):
            self._edit(end_sample=10, end_sec=1.0)


class TestNoteRegions:
    samples = _tone(SR // 2)

    def test_note_stretch_seconds_equal_samples(self) -> None:
        by_samples = libsonare.note_stretch(
            self.samples, SR, onset_sample=2205, offset_sample=6615, stretch_ratio=1.5
        )
        by_seconds = libsonare.note_stretch(
            self.samples, SR, onset_sec=0.1, offset_sec=0.3, stretch_ratio=1.5
        )
        assert by_samples == by_seconds

    def test_note_move_seconds_equal_samples(self) -> None:
        by_samples = libsonare.note_move(
            self.samples, SR, onset_sample=2205, offset_sample=4410, target_onset_sample=6615
        )
        by_seconds = libsonare.note_move(
            self.samples, SR, onset_sec=0.1, offset_sec=0.2, target_onset_sec=0.3
        )
        assert by_samples == by_seconds

    def test_both_spellings_and_negative_bounds_are_refused(self) -> None:
        with pytest.raises(SonareValueError, match=r"onset_sample.*onset_sec"):
            libsonare.note_stretch(self.samples, SR, onset_sample=0, onset_sec=0.0)
        with pytest.raises(SonareValueError, match="offset_sample"):
            libsonare.note_stretch(self.samples, SR, offset_sample=-1)
        with pytest.raises(SonareValueError, match=r"target_onset_sample.*target_onset_sec"):
            libsonare.note_move(self.samples, SR, target_onset_sample=5, target_onset_sec=1.0)
        with pytest.raises(SonareValueError, match="target_onset_sec"):
            libsonare.note_move(self.samples, SR, target_onset_sec=-0.5)


class TestNoteAndEventObjects:
    samples = _noise(SR)

    def test_render_notes_seconds_equal_samples(self) -> None:
        by_samples = libsonare.render_notes(
            self.samples,
            SR,
            [
                NoteObject(
                    onset_sample=2205,
                    offset_sample=4410,
                    edit=NoteEdit(time_offset_samples=4410, gain_db=-6.0),
                )
            ],
        )
        by_seconds = libsonare.render_notes(
            self.samples,
            SR,
            [
                NoteObject(
                    onset_sec=0.1,
                    offset_sec=0.2,
                    edit=NoteEdit(time_offset_sec=0.2, gain_db=-6.0),
                )
            ],
        )
        assert np.array_equal(np.asarray(by_samples), np.asarray(by_seconds))
        assert not np.array_equal(np.asarray(by_samples), self.samples)

    def test_render_notes_refuses_both_spellings_and_a_missing_span(self) -> None:
        with pytest.raises(SonareValueError, match=r"onset_sample.*onset_sec"):
            libsonare.render_notes(
                self.samples, SR, [NoteObject(onset_sample=0, onset_sec=0.0, offset_sample=10)]
            )
        with pytest.raises(SonareValueError, match=r"time_offset_samples.*time_offset_sec"):
            libsonare.render_notes(
                self.samples,
                SR,
                [
                    NoteObject(
                        onset_sample=0,
                        offset_sample=10,
                        edit=NoteEdit(time_offset_samples=1, time_offset_sec=1.0),
                    )
                ],
            )
        with pytest.raises(SonareValueError, match="onset"):
            libsonare.render_notes(self.samples, SR, [NoteObject(offset_sample=10)])
        with pytest.raises(SonareValueError, match="onset_sample"):
            libsonare.render_notes(
                self.samples, SR, [NoteObject(onset_sample=-1, offset_sample=10)]
            )

    def test_render_percussive_events_seconds_equal_samples(self) -> None:
        by_samples = libsonare.render_percussive_events(
            self.samples,
            SR,
            [
                PercussiveEvent(
                    onset_sample=2205,
                    offset_sample=4410,
                    edit=PercussiveEventEdit(time_offset_samples=-1103, gain_db=-6.0),
                )
            ],
        )
        by_seconds = libsonare.render_percussive_events(
            self.samples,
            SR,
            [
                PercussiveEvent(
                    onset_sec=0.1,
                    offset_sec=0.2,
                    edit=PercussiveEventEdit(time_offset_sec=-1103 / SR, gain_db=-6.0),
                )
            ],
        )
        assert np.array_equal(np.asarray(by_samples), np.asarray(by_seconds))
        assert not np.array_equal(np.asarray(by_samples), self.samples)

    def test_render_percussive_events_refuses_both_spellings(self) -> None:
        with pytest.raises(SonareValueError, match=r"offset_sample.*offset_sec"):
            libsonare.render_percussive_events(
                self.samples,
                SR,
                [PercussiveEvent(onset_sample=0, offset_sample=100, offset_sec=1.0)],
            )


class TestTrimPadding:
    samples = np.concatenate([np.zeros(SR // 4), _tone(SR // 4), np.zeros(SR // 2)]).astype(
        np.float32
    )

    def test_padding_sec_equals_padding_samples(self) -> None:
        by_samples = libsonare.mastering_repair_trim_silence(self.samples, SR, padding_samples=2205)
        by_seconds = libsonare.mastering_repair_trim_silence(self.samples, SR, padding_sec=0.1)
        assert np.array_equal(np.asarray(by_samples), np.asarray(by_seconds))
        detect = libsonare.mastering_repair_detect_trim_range
        assert detect(self.samples, SR, padding_samples=2205) == detect(
            self.samples, SR, padding_sec=0.1
        )

    def test_both_spellings_and_negative_padding_are_refused(self) -> None:
        with pytest.raises(SonareValueError, match=r"padding_samples.*padding_sec"):
            libsonare.mastering_repair_trim_silence(
                self.samples, SR, padding_samples=1, padding_sec=0.1
            )
        with pytest.raises(SonareValueError, match="padding_sec"):
            libsonare.mastering_repair_trim_silence(self.samples, SR, padding_sec=-0.1)
        with pytest.raises(SonareValueError, match="padding_samples"):
            libsonare.mastering_repair_trim_silence(self.samples, SR, padding_samples=-1)


class TestEngine:
    def test_punch_in_window_seconds_and_required_bounds(self) -> None:
        with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
            engine.set_capture_punch(start_sec=1.0, end_sample=48128)
            assert engine.capture_status().punch_enabled
            engine.set_capture_punch(48000, 48128, enabled=False)
            assert not engine.capture_status().punch_enabled
            with pytest.raises(SonareValueError, match=r"start_sample.*start_sec"):
                engine.set_capture_punch(1, 2, start_sec=1.0)
            with pytest.raises(SonareValueError, match="start_sample"):
                engine.set_capture_punch(-1, 2)
            with pytest.raises(SonareValueError, match="both bounds"):
                engine.set_capture_punch(1)

    def test_render_frame_none_is_immediate_and_negative_is_refused(self) -> None:
        with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
            engine.play()
            engine.play(render_frame=0)
            for call in (
                lambda: engine.play(-1),
                lambda: engine.seek_sample(0, -1),
                lambda: engine.set_parameter(1, 0.5, -1),
                lambda: engine.push_midi_panic(-1),
            ):
                with pytest.raises(SonareValueError, match="render_frame"):
                    call()


class TestSentinelsAreAbsence:
    def test_fix_frames_x_max(self) -> None:
        assert libsonare.fix_frames([0, 5, 20]) == [0, 5, 20]
        assert libsonare.fix_frames([0, 5, 20], x_max=10) == [0, 5, 10]
        with pytest.raises(SonareValueError, match="x_max"):
            libsonare.fix_frames([0, 5, 20], x_max=-1)

    def test_vqt_gamma(self) -> None:
        samples = _tone(SR // 2)
        automatic = libsonare.vqt(samples, SR, n_bins=24)
        constant_q = libsonare.vqt(samples, SR, n_bins=24, gamma=0.0)
        assert not np.array_equal(np.asarray(automatic.magnitude), np.asarray(constant_q.magnitude))
        for gamma in (-1.0, float("nan")):
            with pytest.raises(SonareValueError, match="gamma"):
                libsonare.vqt(samples, SR, n_bins=24, gamma=gamma)

    def test_metering_percentiles(self) -> None:
        samples = _noise(SR * 4)
        defaults = libsonare.metering_dynamic_range(samples, SR)
        explicit = libsonare.metering_dynamic_range(
            samples, SR, low_percentile=0.1, high_percentile=0.95
        )
        assert defaults == explicit
        for field in ("low_percentile", "high_percentile"):
            with pytest.raises(SonareValueError, match=field):
                libsonare.metering_dynamic_range(samples, SR, **{field: -1.0})

    def test_set_program_bank(self) -> None:
        project = libsonare.Project()
        try:
            project.set_sample_rate(48000)
            _, clip_id = project.add_midi_clip(0.0, 4.0)
            project.set_program(clip_id, 24)
            project.set_program(clip_id, 24, 0)
            with pytest.raises(SonareValueError, match="bank"):
                project.set_program(clip_id, 24, -1)
            with pytest.raises(SonareValueError, match="bank"):
                project.set_program_on_channel(clip_id, 0, 0, 24, -1)
        finally:
            project.close()


class TestVocalRenderRange:
    def _session(self):  # type: ignore[no-untyped-def]
        from libsonare.vocal_edit import VocalAnalysis, create_vocal_edit_session

        rate = 16_000
        source = (0.25 * np.sin(2.0 * np.pi * 440.0 * np.arange(4096) / rate)).astype(np.float32)
        frames = 33
        analysis = VocalAnalysis(
            frame_origin_sample=0.0,
            samples_per_frame=128.0,
            frame_length_samples=256,
            f0_hz=np.full(frames, 440.0, dtype=np.float32),
            voiced=np.ones(frames, dtype=np.uint8),
            algorithm_id="host",
            algorithm_version=1,
        )
        return create_vocal_edit_session(source, rate, analysis=analysis)

    def test_seconds_equal_samples_and_open_bounds(self) -> None:
        try:
            session = self._session()
        except Exception as exc:  # native build without vocal edit
            pytest.skip(str(exc))
        with session, session.capture_render_snapshot() as snapshot:
            by_samples = snapshot.render((1600, 3200))
            by_seconds = snapshot.render(start_sec=0.1, end_sec=0.2)
            assert np.array_equal(by_samples.samples, by_seconds.samples)
            assert snapshot.render((1600, None)).samples.size == 4096 - 1600
            assert snapshot.render(end_sec=0.1).samples.size == 1600
            with pytest.raises(SonareValueError):
                snapshot.render((-1, 10))
            with pytest.raises(SonareValueError, match="not both"):
                snapshot.render((0, 10), start_sec=0.0)
