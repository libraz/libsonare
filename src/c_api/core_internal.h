#pragma once

#include <sonare/sonare_c.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include "analysis/acoustic_analyzer.h"
#include "analysis/beat_analyzer.h"
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
PitchClass from_c_pitch_class(SonarePitchClass pitch);
Mode from_c_mode(SonareMode mode);
bool fill_key_profile(SonareKeyProfileType profile_type, KeyConfig* config);
bool fill_key_modes(const SonareMode* modes, size_t mode_count, KeyConfig* config);
void fill_chord_result(const std::vector<Chord>& chords, SonareChordAnalysisResult* out);
SonareError fill_cqt_result(const CqtResult& result, SonareCqtResult* out);

// Marshal a quick-analysis value through temporary owners before publishing it
// to the C struct. This keeps a later allocation failure from leaking an array
// already allocated for an earlier field.
inline SonareError fill_analysis_result(const AnalysisResult& result, SonareAnalysisResult* out) {
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SonareAnalysisResult staged{};
  staged.bpm = result.bpm;
  staged.bpm_confidence = result.bpm_confidence;
  staged.key.root = static_cast<SonarePitchClass>(result.key.root);
  staged.key.mode = static_cast<SonareMode>(result.key.mode);
  staged.key.confidence = result.key.confidence;
  staged.time_signature = {result.time_signature.numerator, result.time_signature.denominator,
                           result.time_signature.confidence};

  std::unique_ptr<float[]> beat_times;
  if (!result.beats.empty()) {
    beat_times = std::make_unique<float[]>(result.beats.size());
    for (size_t i = 0; i < result.beats.size(); ++i) beat_times[i] = result.beats[i].time;
  }
  std::unique_ptr<SonareAnalysisBpmCandidate[]> bpm_candidates;
  if (!result.bpm_candidates.empty()) {
    bpm_candidates = std::make_unique<SonareAnalysisBpmCandidate[]>(result.bpm_candidates.size());
    for (size_t i = 0; i < result.bpm_candidates.size(); ++i) {
      const auto& candidate = result.bpm_candidates[i];
      bpm_candidates[i] = {candidate.value, candidate.confidence,
                           static_cast<SonareBpmCandidateRelation>(candidate.relation)};
    }
  }
  std::unique_ptr<SonareTimeSignature[]> time_signatures;
  if (!result.time_signature_candidates.empty()) {
    time_signatures =
        std::make_unique<SonareTimeSignature[]>(result.time_signature_candidates.size());
    for (size_t i = 0; i < result.time_signature_candidates.size(); ++i) {
      const auto& candidate = result.time_signature_candidates[i];
      time_signatures[i] = {candidate.numerator, candidate.denominator, candidate.confidence};
    }
  }
  staged.beat_count = result.beats.size();
  staged.beat_times = beat_times.release();
  staged.bpm_candidate_count = result.bpm_candidates.size();
  staged.bpm_candidates = bpm_candidates.release();
  staged.time_signature_candidate_count = result.time_signature_candidates.size();
  staged.time_signature_candidates = time_signatures.release();
  *out = staged;
  return SONARE_OK;
}
