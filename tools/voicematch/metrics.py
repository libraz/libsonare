"""Per-note timbre metrics and model-vs-oracle deltas.

All metrics are computed on a mono mix, per note event, from windows derived
from the score (the harness knows every onset/offset exactly). Level-dependent
metrics are taken after both renders are normalized to equal overall RMS, so
what remains measures timbre and balance, not master gain.

Metric set (per note):
  f0_hz / f0_cents_err   measured fundamental and deviation from equal temperament
  harmonics_db           first 12 harmonic magnitudes in dB relative to h1
  centroid_hz            amplitude-weighted spectral centroid of the sustain
  odd_even_db            mean(h3,h5,h7,h9) - mean(h2,h4,h6,h8)
  tnr_db                 tonal-to-noise ratio in the sustain window
  attack_ms              10% -> 90% rise time of the amplitude envelope
  sustain_slope_db_s     linear dB/s fit over the sustain window
  release_ms             time after note-off for the envelope to fall 40 dB
  sustain_rms_db         sustain-window RMS (post global normalization)

Percussion metric set (per hit, `analyze_hit`):
  bands_db               1/3-octave levels, dB relative to the loudest band
  band_decay_db_s        per-octave-band decay slope after the peak
  onset_ms               note-on to the strike the rest of the set is measured from
  attack_ms              strike to first arrival within 3 dB of the peak
  decay_ms               end of the attack to the last moment within 20 dB of the peak
  crest_db               peak-to-RMS ratio over the hit
  centroid_hz            broadband spectral centroid of the hit
  level_db               hit RMS (post global normalization)
  flatness_db            how much of the hit stands in peaks rather than a continuum
  stereo_width           1 - |channel correlation| over the hit's own window
A drum note has no fundamental, so every metric above that is anchored on one —
the harmonic ladder, the intonation error, the tonal-to-noise ratio — measures a
frequency the sound does not contain. What is left of a percussion hit is its
level *profile*, how fast each part of that profile dies, how peaky it is and
how wide, which is what these measure instead. `flatness_db` is what stands in
for `tnr_db` here and it needs no target frequencies, which is why it can.
"""

# Every importer reads this module by name, and two of them read attributes off
# it — private ones included — so the whole surface is re-exported here.
# ruff: noqa: F401

from __future__ import annotations

import math
from dataclasses import asdict, dataclass

import numpy as np
from metrics_attack import (
    ATTACK_ANCHOR_HZ,
    ATTACK_BANDS_HZ,
    ATTACK_LF_BANDS_HZ,
    ATTACK_LF_WINDOW_MS,
    ATTACK_PEAK_BASELINE_HZ,
    ATTACK_PEAK_FLOOR_DB,
    ATTACK_PEAK_FLOOR_HZ,
    ATTACK_PEAK_PROMINENCE_DB,
    ATTACK_WINDOW_MS,
    ATTACK_WINDOWS,
    _anchor_power,
    _bands_against_anchor,
    attack_bands,
    attack_low_bands,
    attack_peaks,
)
from metrics_bands import (
    BAND_AGREEMENT_MAX_RATIO,
    BAND_EDGE_MIN_ROWS,
    BAND_EDGE_MIN_SPREAD_FRACTION,
    BAND_FLOOR_DB,
    OCTAVE_CENTERS,
    OCTAVE_RATIO,
    THIRD_OCTAVE_CENTERS,
    THIRD_OCTAVE_RATIO,
    TILT_HIGH_HZ,
    TILT_LOW_HZ,
    band_edge_index,
    band_edges_by_timbre,
    band_tilt_db,
    measure_agreement_edge,
    measure_band_edge,
    shared_band_edge,
)
from metrics_decay import (
    EDC_FIT_RANGE_DB,
    EDC_MIN_DROP_DB,
    EDC_MIN_FRAMES,
    EDC_MIN_R2,
    _band_decay,
    _band_power,
    _edc_slope,
)
from metrics_hit import (
    ATTACK_FLOOR_MS,
    FLATNESS_FLOOR_DB,
    FLATNESS_LOW_HZ,
    HIT_ATTACK_TOLERANCE_DB,
    HIT_ENVELOPE_HOP_MS,
    HIT_ENVELOPE_WIN_MS,
    HIT_LONG_MAX_SEC,
    HIT_MAX_SEC,
    HIT_ONSET_FLOOR_DB,
    HIT_ONSET_SEARCH_SEC,
    HIT_TONE_WINDOW_S,
    LONG_DECAY_DRUM_NOTES,
    PITCH_DROP_FLOOR_DB,
    PITCH_DROP_FRAME_S,
    PITCH_DROP_HOP_S,
    PITCH_DROP_POINTS,
    PITCH_DROP_SPAN,
    PITCH_DROP_WINDOW_S,
    HitMetrics,
    _hit_onset,
    analyze_hit,
    compare_hit,
    hit_tone,
    pitch_drop,
    spectral_flatness_db,
)
from metrics_modal import (
    MODAL_BASELINE_HZ,
    MODAL_FLOOR_DB,
    MODAL_MAX_HZ,
    MODAL_MAX_MODES,
    MODAL_MERGE_CENTS,
    MODAL_PROMINENCE_DB,
    measure_modes,
    modal_profile,
)
from metrics_modulation import (
    BEAT_BAND_HZ,
    MOD_FRAME_HOP_S,
    MOD_FRAME_WIN_S,
    MOD_MIN_FRAMES,
    MOD_TRACK_CENTS,
    MOD_TRACK_POINTS,
    MOD_WINDOW_S,
    VIBRATO_BAND_HZ,
    _band_peak,
    f0_width_cents,
    modulation_note,
)
from metrics_note import (
    HELD_FLOOR_DB,
    HELD_WINDOW_S,
    MIN_SUSTAIN_SEC,
    SILENT_WINDOW_DB,
    SUSTAIN_WINDOW_S,
    NoteMetrics,
    _under_peak_db,
    analyze_note,
    compare_note,
    level_of,
    note_onset,
)
from metrics_partials import (
    HARMONIC_SHARE_TOLERANCE_CENTS,
    HARMONIC_SHARE_WINDOW_S,
    INHARMONICITY_FLOOR_DB,
    INHARMONICITY_MIN_CENTS,
    INHARMONICITY_SEED_PARTIALS,
    INHARMONICITY_TOLERANCES,
    LADDER_FLOOR_MARGIN_DB,
    MAX_EXTRAPOLATED_PARTIAL,
    MAX_FIT_PARTIALS,
    MAX_INHARMONICITY_B,
    MIN_PARTIALS_FOR_B,
    estimate_inharmonicity_b,
    fit_partial_series,
    harmonic_share,
    ladder_present,
    partial_hz,
    partial_offset,
    stretch_cents,
)
from metrics_signal import (
    AUDIBILITY_A_FLOOR_DB,
    AUDIBILITY_MASK_SPAN_DB,
    AUDIBILITY_MIN_WEIGHT,
    MONO_MODES,
    N_HARMONICS,
    _db,
    _peak_near,
    _rms_envelope,
    _spectrum,
    a_weight_db,
    audibility_weights,
    channel_correlation,
    channel_width,
    midi_to_hz,
    normalize_rms,
    to_mono,
)
from smf import Note
