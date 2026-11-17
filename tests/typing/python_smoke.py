from __future__ import annotations

from typing import Any, Literal, assert_type

import libsonare
import numpy as np

samples = [0.0, 0.1, -0.1, 0.0]
ir_samples = [1.0, 0.5, 0.25, 0.125, 0.0]

bpm: float = libsonare.detect_bpm(samples, sample_rate=22050)
downbeats: np.ndarray[Any, Any] = libsonare.detect_downbeats(samples, sample_rate=22050)
key: libsonare.Key = libsonare.detect_key(
    samples,
    22050,
    high_pass_hz=80.0,
    profile=libsonare.KeyProfile.FARALDO_EDMA,
    genre_hint="edm",
)
key_candidates: list[libsonare.KeyCandidate] = libsonare.detect_key_candidates(
    samples, 22050, high_pass_hz=80.0, modes=[libsonare.Mode.MAJOR, "dorian"], profile="edma"
)
analysis: libsonare.AnalysisResult = libsonare.analyze(samples, 22050)
acoustic: libsonare.AcousticResult = libsonare.analyze_impulse_response(ir_samples, 48000)
blind_acoustic: libsonare.AcousticResult = libsonare.detect_acoustic(ir_samples, 48000)
rir: libsonare.RirResult = libsonare.synthesize_rir(
    6.0, 5.0, 3.0, absorption=0.2, sample_rate=48000
)
room_estimate: libsonare.RoomEstimate = libsonare.estimate_room(
    ir_samples, 48000, prefer_eyring=True
)
morphed: libsonare.RoomMorphResult = libsonare.room_morph(samples, 22050, 6.0, 5.0, 3.0, wet=0.4)
morphed_stereo: libsonare.RoomMorphStereoResult = libsonare.room_morph_stereo(
    samples, samples, 22050, 6.0, 5.0, 3.0, receiver_spacing_m=0.5, wet=0.4
)
chords: libsonare.ChordAnalysisResult = libsonare.detect_chords(
    samples,
    22050,
    use_hmm=True,
    hmm_beam_width=8,
    use_key_context=True,
    key_root=libsonare.PitchClass.C,
    key_mode=libsonare.Mode.MAJOR,
    detect_inversions=True,
    chroma_method="nnls",
)
cyclic_data = libsonare.cyclic_tempogram(samples, 22050)
mastered: libsonare.MasteringChainResult = libsonare.master_audio(samples, 48000, preset="aiMusic")
catalog = libsonare.mastering_processor_catalog()
param_info = libsonare.mastering_insert_param_info("saturation.softClipper")
insert_timing = libsonare.mastering_insert_timing(
    "saturation.softClipper", {"aliasing": 3.0}, 48000
)
project = libsonare.Project()
project.set_assist_sidecar("python-smoke", b"legacy")
project.set_assist_sidecar({"moduleId": "python-smoke-descriptor"})
assist_sidecars: list[libsonare.AssistSidecar] = project.assist_sidecars()

assert_type(bpm, float)
assert_type(downbeats, np.ndarray[Any, Any])
assert_type(key.shortName, str)
assert_type(key_candidates[0].correlation, float)
assert_type(analysis.beatTimes, list[float])
assert_type(acoustic.rt60Bands, list[float])
assert_type(blind_acoustic.isBlind, bool)
assert_type(rir.rir, list[float])
assert_type(rir.sample_rate, int)
assert_type(room_estimate.rt60_bands, list[float])
assert_type(room_estimate.confidence, float)
assert_type(morphed.audio, list[float])
assert_type(morphed.diagnostics, list[libsonare.RirDiagnostic])
assert_type(rir.diagnostics, list[libsonare.RirDiagnostic])
assert_type(chords.chords, list[libsonare.Chord])
assert_type(cyclic_data, np.ndarray[Any, Any])
assert_type(mastered.stages, list[str])
assert_type(catalog, list[libsonare.MasteringProcessorCatalogEntry])
assert_type(catalog[0].kind, libsonare.MasteringProcessorKind)
assert_type(catalog[0].channel_policy, libsonare.MasteringChannelPolicy)
assert_type(param_info, list[libsonare.MasteringInsertParamInfo])
assert_type(param_info[0].rt_safe, bool)
assert_type(param_info[0].id, int | None)
assert_type(param_info[0].type, Literal["boolean", "number", "enum", "string", "array"])
assert_type(param_info[0].choices, tuple[libsonare.MasteringInsertParamChoice, ...] | None)
assert_type(param_info[0].slot, str | None)
assert_type(param_info[0].unit, libsonare.MasteringInsertParamUnit | None)
assert_type(param_info[0].scale, libsonare.MasteringInsertParamScale)
assert_type(param_info[0].ui_min, float | None)
assert_type(param_info[0].ui_max, float | None)
assert_type(param_info[0].depends_on, tuple[libsonare.MasteringInsertParamDependency, ...])
assert_type(param_info[0].depends_on[0].relation, libsonare.MasteringInsertParamRelation)
assert_type(param_info[0].depends_on[0].factor, float)
assert_type(param_info[0].coupled_with, tuple[str, ...])
assert_type(catalog[0].slots, tuple[libsonare.MasteringInsertSlot, ...])
assert_type(catalog[0].slots[0].activation, Literal["anyKey", "always"])
assert_type(insert_timing, libsonare.MasteringInsertTiming)
assert_type(insert_timing.latency_samples, int)
assert_type(project.get_assist_sidecar(0), libsonare.AssistSidecar)
assert_type(assist_sidecars, list[libsonare.AssistSidecar])
