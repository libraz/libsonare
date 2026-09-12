#include "editing/note_model/note_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

#include "editing/note_model/pitch_decomposition.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "effects/formant_warp.h"
#include "effects/pitch_shift.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::editing::note_model {
namespace {

using sonare::constants::kCentsPerSemitone;
using sonare::constants::kHalfPi;
using sonare::constants::kSemitonesPerOctave;

int64_t saturating_add(int64_t a, int64_t b) noexcept {
  if (b > 0 && a > std::numeric_limits<int64_t>::max() - b) {
    return std::numeric_limits<int64_t>::max();
  }
  if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) {
    return std::numeric_limits<int64_t>::min();
  }
  return a + b;
}

/// Same rule as NoteEditor::fade_samples: half the placed region at most, so
/// the head and tail zones never consume the same samples.
int64_t fade_samples(float fade_ms, int sample_rate, int64_t region_length) noexcept {
  const double requested =
      std::round(static_cast<double>(fade_ms) * 0.001 * static_cast<double>(sample_rate));
  const int64_t limit = std::max<int64_t>(0, region_length / 2);
  if (!(requested > 0.0)) return 0;
  if (requested >= static_cast<double>(limit)) return limit;
  return static_cast<int64_t>(requested);
}

void check_disjoint_spans(const std::vector<NoteObject>& notes) {
  std::vector<size_t> order(notes.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::sort(order.begin(), order.end(),
            [&notes](size_t a, size_t b) { return notes[a].onset_sample < notes[b].onset_sample; });
  for (size_t i = 1; i < order.size(); ++i) {
    SONARE_CHECK(notes[order[i]].onset_sample >= notes[order[i - 1]].offset_sample,
                 ErrorCode::InvalidParameter);
  }
}

/// Cross-fade phase at offset @p k of a span of @p length: 0 at either edge,
/// 1 once past the fade zone.
float fade_phase(int64_t k, int64_t length, int64_t fade) noexcept {
  if (fade <= 0) return 1.0f;
  if (k < fade) return (static_cast<float>(k) + 0.5f) / static_cast<float>(fade);
  if (k >= length - fade) return (static_cast<float>(length - k) - 0.5f) / static_cast<float>(fade);
  return 1.0f;
}

/// Writes @p segment at @p dest, cross-fading its edges against the untouched
/// source over @p fade samples with the equal-power (cos/sin) weights
/// NoteEditor splices with: the source ramps down while the segment ramps up,
/// so the seam keeps its level instead of dipping through silence.
void overlay(std::vector<float>& output, const Audio& source, const std::vector<float>& segment,
             int64_t dest, int64_t fade) {
  const int64_t n = static_cast<int64_t>(output.size());
  const int64_t seg_len = static_cast<int64_t>(segment.size());
  if (seg_len <= 0 || dest >= n || dest <= -seg_len) return;

  const int64_t begin = std::max<int64_t>(dest, 0);
  const int64_t end = std::min<int64_t>(dest + seg_len, n);
  for (int64_t j = begin; j < end; ++j) {
    const int64_t k = j - dest;
    const float phase = fade_phase(k, seg_len, fade);
    const float segment_gain = phase >= 1.0f ? 1.0f : std::sin(kHalfPi * phase);
    const float source_gain = phase >= 1.0f ? 0.0f : std::cos(kHalfPi * phase);
    output[static_cast<size_t>(j)] = segment_gain * segment[static_cast<size_t>(k)] +
                                     source_gain * source[static_cast<size_t>(j)];
  }
}

/// Scales @p segment by @p envelope stretched over its whole length. The
/// envelope is a set of gain points rather than a signal, so it is resampled by
/// linear interpolation between its endpoints; one entry is a constant gain.
void apply_envelope(std::vector<float>& segment, const std::vector<float>& envelope) {
  const size_t n = segment.size();
  const size_t points = envelope.size();
  if (points == 0 || n == 0) return;
  if (points == 1) {
    for (float& sample : segment) {
      sample *= envelope[0];
    }
    return;
  }

  const double step =
      static_cast<double>(points - 1) / static_cast<double>(n > 1 ? n - 1 : size_t{1});
  for (size_t i = 0; i < n; ++i) {
    const double position = static_cast<double>(i) * step;
    const size_t lo = std::min(static_cast<size_t>(position), points - 1);
    const size_t hi = std::min(lo + 1, points - 1);
    const float frac = static_cast<float>(position - static_cast<double>(lo));
    segment[i] *= envelope[lo] * (1.0f - frac) + envelope[hi] * frac;
  }
}

/// True when at least one frame carries a pitch decompose_pitch can measure.
bool has_usable_pitch(const std::vector<float>& f0_hz) noexcept {
  return std::any_of(f0_hz.begin(), f0_hz.end(),
                     [](float hz) { return hz > 0.0f && std::isfinite(hz); });
}

/// Rescales the note's own drift and vibrato by the edit's two changes and
/// repitches @p segment through them. The deltas are a change from the measured
/// curve, so -1 cancels a component and 0 leaves it alone. Duration-preserving,
/// so the segment keeps its length for the rest of the chain.
void apply_pitch_curve(std::vector<float>& segment, const NoteObject& note, int sample_rate,
                       const NoteRenderConfig& config) {
  const PitchDecomposition split = decompose_pitch(note, config.decomposition);
  const size_t n = note.f0_hz.values.size();
  // Unreachable while validation and decompose_pitch agree on what a usable
  // curve is; an error rather than a skip so that they cannot part silently.
  SONARE_CHECK(split.drift.size() == n, ErrorCode::InvalidParameter);

  std::vector<float> deltas(n);
  for (size_t i = 0; i < n; ++i) {
    deltas[i] = (split.drift[i] * note.edit.drift_change +
                 split.vibrato[i] * note.edit.vibrato_depth_change) /
                kCentsPerSemitone;
  }

  // The note's own frames, which is the cadence the deltas are stated over.
  pitch_editor::F0Track track;
  track.f0_hz = note.f0_hz.values;
  track.voiced.resize(n);
  for (size_t i = 0; i < n; ++i) {
    track.voiced[i] = track.f0_hz[i] > 0.0f && std::isfinite(track.f0_hz[i]);
  }
  track.sample_rate = sample_rate;
  track.frame_rate_hz = note.f0_hz.frame_rate_hz;

  pitch_editor::PitchCorrectionConfig correction;
  correction.backend = config.stretch_backend;
  const Audio repitched =
      pitch_editor::PitchCorrector(correction)
          .resynthesize(Audio::from_vector(segment, sample_rate), track, deltas);
  segment.assign(repitched.begin(), repitched.end());
}

}  // namespace

void erase_span(std::vector<float>& output, const Audio& source, int64_t begin, int64_t end,
                int64_t fade) {
  const int64_t length = end - begin;
  for (int64_t j = begin; j < end; ++j) {
    const float phase = fade_phase(j - begin, length, fade);
    const float source_gain = phase >= 1.0f ? 0.0f : std::cos(kHalfPi * phase);
    output[static_cast<size_t>(j)] = source_gain * source[static_cast<size_t>(j)];
  }
}

void validate_note_for_render(const NoteObject& note) {
  SONARE_CHECK(note.onset_sample >= 0 && note.length_samples() > 0, ErrorCode::InvalidParameter);
  const NoteEdit& edit = note.edit;
  SONARE_CHECK(std::isfinite(edit.pitch_shift_semitones) && std::isfinite(edit.gain_db) &&
                   std::isfinite(edit.time_stretch_ratio) && edit.time_stretch_ratio > 0.0f &&
                   std::isfinite(edit.formant_shift_semitones) &&
                   std::isfinite(edit.vibrato_depth_change) && std::isfinite(edit.drift_change),
               ErrorCode::InvalidParameter);
  // A pitch-curve edit with no curve to read is a wiring bug, not a no-op.
  // The frames are scanned too: a median can outlive every frame that
  // produced it, and decompose_pitch reports such a note as unmeasured.
  if (edit.vibrato_depth_change != 0.0f || edit.drift_change != 0.0f) {
    SONARE_CHECK(note.median_hz > 0.0f && note.f0_hz.frame_rate_hz > 0.0f &&
                     has_usable_pitch(note.f0_hz.values),
                 ErrorCode::InvalidParameter);
  }
  for (const float value : edit.amplitude_envelope) {
    SONARE_CHECK(std::isfinite(value) && value >= 0.0f, ErrorCode::InvalidParameter);
  }
}

void validate_render_config(const NoteRenderConfig& config) {
  SONARE_CHECK(std::isfinite(config.fade_ms) && config.fade_ms >= 0.0f,
               ErrorCode::InvalidParameter);
}

Audio render_notes(const Audio& audio, const std::vector<NoteObject>& notes,
                   const NoteRenderConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  validate_render_config(config);

  bool all_identity = true;
  for (const NoteObject& note : notes) {
    validate_note_for_render(note);
    all_identity = all_identity && note.edit.is_identity();
  }
  check_disjoint_spans(notes);

  // Nothing to resynthesize: the source passes through unchanged, bit for bit.
  if (all_identity) {
    return Audio::from_buffer(audio.data(), audio.size(), audio.sample_rate());
  }

  const int sample_rate = audio.sample_rate();
  const int64_t n_samples = static_cast<int64_t>(audio.size());
  std::vector<float> output(audio.begin(), audio.end());

  for (const NoteObject& note : notes) {
    if (note.edit.is_identity()) continue;

    const int64_t onset = std::min(note.onset_sample, n_samples);
    const int64_t offset = std::clamp(note.offset_sample, onset, n_samples);
    if (offset <= onset) continue;

    erase_span(output, audio, onset, offset,
               fade_samples(config.fade_ms, sample_rate, offset - onset));
    if (note.edit.muted) continue;

    std::vector<float> segment(audio.begin() + onset, audio.begin() + offset);
    if (note.edit.vibrato_depth_change != 0.0f || note.edit.drift_change != 0.0f) {
      apply_pitch_curve(segment, note, sample_rate, config);
    }
    if (note.edit.time_stretch_ratio != 1.0f) {
      TimeStretchConfig stretch_config;
      stretch_config.backend = config.stretch_backend;
      // time_stretch takes a rate, which is the reciprocal of a stretch ratio.
      const Audio stretched = time_stretch(Audio::from_vector(segment, sample_rate),
                                           1.0f / note.edit.time_stretch_ratio, stretch_config);
      segment.assign(stretched.begin(), stretched.end());
    }
    if (note.edit.pitch_shift_semitones != 0.0f) {
      PitchShiftConfig shift_config;
      shift_config.backend = config.stretch_backend;
      const Audio shifted = pitch_shift(Audio::from_vector(segment, sample_rate),
                                        note.edit.pitch_shift_semitones, shift_config);
      segment.assign(shifted.begin(), shifted.end());
    }
    if (segment.empty()) continue;
    if (note.edit.formant_shift_semitones != 0.0f) {
      FormantWarpConfig warp_config;
      // The warp takes a frequency ratio for what the edit states in semitones.
      warp_config.factor = std::pow(2.0f, note.edit.formant_shift_semitones / kSemitonesPerOctave);
      const Audio warped =
          FormantWarp(warp_config).process(Audio::from_vector(segment, sample_rate));
      segment.assign(warped.begin(), warped.end());
    }
    apply_envelope(segment, note.edit.amplitude_envelope);
    if (note.edit.gain_db != 0.0f) {
      const float gain = db_to_linear(note.edit.gain_db);
      for (float& sample : segment) {
        sample *= gain;
      }
    }

    const int64_t fade =
        fade_samples(config.fade_ms, sample_rate, static_cast<int64_t>(segment.size()));
    overlay(output, audio, segment, saturating_add(onset, note.edit.time_offset_samples), fade);
  }

  return Audio::from_vector(std::move(output), sample_rate);
}

}  // namespace sonare::editing::note_model
