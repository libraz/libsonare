#pragma once

#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include "analysis/acoustic_analyzer.h"
#include "analysis/beat_analyzer.h"
#include "analysis/boundary_detector.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/section_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "core/audio.h"
#include "core/convert.h"
#include "core/spectrum.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/mel_spectrogram.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "feature/vqt.h"
#include "quick.h"
#include "sonare.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

void fill_acoustic_result(const AcousticParameters& params, SonareAcousticResult* out);
/// Converts a caller's pitch class; false (and @p out untouched) outside C..B.
[[nodiscard]] bool from_c_pitch_class(SonarePitchClass pitch, PitchClass* out);
/// Converts a caller's mode; false (and @p out untouched) outside Major..Locrian.
[[nodiscard]] bool from_c_mode(SonareMode mode, Mode* out);
bool fill_key_profile(SonareKeyProfileType profile_type, KeyConfig* config);
bool fill_key_modes(const SonareMode* modes, size_t mode_count, KeyConfig* config);
void fill_chord_result(const std::vector<Chord>& chords, float tuning,
                       SonareChordAnalysisResult* out);

/// Core semitone fraction for a caller's tuning in cents.
/// @throws SonareException(InvalidParameter) unless @p cents is finite and in [-50, 50).
inline float tuning_from_cents(float cents) {
  constexpr float kMaxTuningCents = 50.0f;
  util::check::in_interval(cents, -kMaxTuningCents, true, kMaxTuningCents, false,
                           "options.tuning_cents");
  return cents / constants::kCentsPerSemitone;
}

/// Caller-facing cents for a core tuning given as a fraction of a semitone.
inline float tuning_to_cents(float semitones) { return semitones * constants::kCentsPerSemitone; }
SonareError fill_cqt_result(const CqtResult& result, SonareCqtResult* out);
