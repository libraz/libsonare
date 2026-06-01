from __future__ import annotations

from typing import assert_type

import libsonare

samples = [0.0, 0.1, -0.1, 0.0]
ir_samples = [1.0, 0.5, 0.25, 0.125, 0.0]

bpm: float = libsonare.detect_bpm(samples, sample_rate=22050)
downbeats: list[float] = libsonare.detect_downbeats(samples, sample_rate=22050)
key: libsonare.Key = libsonare.detect_key(
    samples, high_pass_hz=80.0, profile=libsonare.KeyProfile.FARALDO_EDMA, genre_hint="edm"
)
key_candidates: list[libsonare.KeyCandidate] = libsonare.detect_key_candidates(
    samples, high_pass_hz=80.0, modes=[libsonare.Mode.MAJOR, "dorian"], profile="edma"
)
analysis: libsonare.AnalysisResult = libsonare.analyze(samples)
acoustic: libsonare.AcousticResult = libsonare.analyze_impulse_response(ir_samples)
blind_acoustic: libsonare.AcousticResult = libsonare.detect_acoustic(ir_samples)
chords: libsonare.ChordAnalysisResult = libsonare.detect_chords(
    samples,
    use_hmm=True,
    hmm_beam_width=8,
    use_key_context=True,
    key_root=libsonare.PitchClass.C,
    key_mode=libsonare.Mode.MAJOR,
    detect_inversions=True,
    chroma_method="nnls",
)
cyclic_frames, cyclic_data = libsonare.cyclic_tempogram(samples)
mastered: libsonare.MasteringChainResult = libsonare.master_audio(samples, preset_name="aiMusic")

assert_type(bpm, float)
assert_type(downbeats, list[float])
assert_type(key.shortName, str)
assert_type(key_candidates[0].correlation, float)
assert_type(analysis.beatTimes, list[float])
assert_type(acoustic.rt60Bands, list[float])
assert_type(blind_acoustic.isBlind, bool)
assert_type(chords.chords, list[libsonare.Chord])
assert_type(cyclic_frames, int)
assert_type(cyclic_data, list[float])
assert_type(mastered.stages, list[str])
