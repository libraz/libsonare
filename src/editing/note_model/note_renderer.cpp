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
#include "util/insertion_sort.h"

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
  insertion_sort(order.begin(), order.end(), [&notes](size_t a, size_t b) {
    return notes[a].onset_sample < notes[b].onset_sample;
  });
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

/// Writes @p segment at @p dest, cross-fading its edges against @p output's
/// OWN current content over @p fade samples with the equal-power (cos/sin)
/// weights NoteEditor splices with: the existing content ramps down while the
/// segment ramps up, so the seam keeps its level instead of dipping through
/// silence.
/// @details Blends against @p output rather than the pristine source
///          deliberately: all edited source spans are vacated before any
///          destination is written, and a neighbouring note earlier in the
///          render can have already overwritten part of this range too.
///          output starts as a copy of the source and only diverges where an
///          erase_span/overlay call touched it. Blending against output[j]
///          preserves the source in untouched spans and uses the current
///          taper or destination in edited spans.
void overlay(std::vector<float>& output, const std::vector<float>& segment, int64_t dest,
             int64_t fade) {
  const int64_t n = static_cast<int64_t>(output.size());
  const int64_t seg_len = static_cast<int64_t>(segment.size());
  if (seg_len <= 0 || dest >= n || dest <= -seg_len) return;

  const int64_t begin = std::max<int64_t>(dest, 0);
  const int64_t end = std::min<int64_t>(dest + seg_len, n);
  for (int64_t j = begin; j < end; ++j) {
    const int64_t k = j - dest;
    const float phase = fade_phase(k, seg_len, fade);
    const float segment_gain = phase >= 1.0f ? 1.0f : std::sin(kHalfPi * phase);
    const float existing_gain = phase >= 1.0f ? 0.0f : std::cos(kHalfPi * phase);
    output[static_cast<size_t>(j)] = segment_gain * segment[static_cast<size_t>(k)] +
                                     existing_gain * output[static_cast<size_t>(j)];
  }
}

/// Scales @p segment by @p envelope and the note's linear gain in one double
/// precision operation. The envelope is a set of gain points rather than a
/// signal, so it is resampled by linear interpolation between its endpoints;
/// one entry is a constant gain. Combining the two factors before narrowing
/// avoids an intermediate float overflow that a later finite gain could undo.
void apply_envelope(std::vector<float>& segment, const std::vector<float>& envelope,
                    float linear_gain) {
  const size_t n = segment.size();
  const size_t points = envelope.size();
  if (n == 0) return;
  const double max_float = static_cast<double>(std::numeric_limits<float>::max());
  const auto scale_sample = [linear_gain, max_float](float sample, double envelope_gain) {
    const double scaled =
        static_cast<double>(sample) * envelope_gain * static_cast<double>(linear_gain);
    SONARE_CHECK(std::isfinite(scaled) && std::abs(scaled) <= max_float,
                 ErrorCode::InvalidParameter);
    return static_cast<float>(scaled);
  };
  if (points == 1) {
    for (float& sample : segment) {
      sample = scale_sample(sample, static_cast<double>(envelope[0]));
    }
    return;
  }

  if (points == 0) {
    for (float& sample : segment) {
      sample = scale_sample(sample, 1.0);
    }
    return;
  }

  const double step =
      static_cast<double>(points - 1) / static_cast<double>(n > 1 ? n - 1 : size_t{1});
  for (size_t i = 0; i < n; ++i) {
    const double position = static_cast<double>(i) * step;
    const size_t lo = std::min(static_cast<size_t>(position), points - 1);
    const size_t hi = std::min(lo + 1, points - 1);
    const double frac = position - static_cast<double>(lo);
    const double envelope_gain =
        static_cast<double>(envelope[lo]) * (1.0 - frac) + static_cast<double>(envelope[hi]) * frac;
    segment[i] = scale_sample(segment[i], envelope_gain);
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
    // Decomposition accepts any unusable value as a missing measurement;
    // the pitch corrector consumes a normalized, validated F0 track.
    if (!track.voiced[i]) track.f0_hz[i] = 0.0f;
  }
  track.sample_rate = sample_rate;
  track.frame_rate_hz = note.f0_hz.frame_rate_hz;

  const Audio repitched = pitch_editor::PitchCorrector().resynthesize(
      Audio::from_vector(segment, sample_rate), track, deltas);
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

bool is_valid_note_edit(const NoteEdit& edit) noexcept {
  if (!std::isfinite(edit.pitch_shift_semitones) || !std::isfinite(edit.gain_db) ||
      !std::isfinite(edit.time_stretch_ratio) || edit.time_stretch_ratio <= 0.0f ||
      !std::isfinite(edit.formant_shift_semitones) || !std::isfinite(edit.vibrato_depth_change) ||
      !std::isfinite(edit.drift_change)) {
    return false;
  }

  const float linear_gain = db_to_linear(edit.gain_db);
  if (!std::isfinite(linear_gain)) return false;
  const double gain = static_cast<double>(linear_gain);
  const double max_float = static_cast<double>(std::numeric_limits<float>::max());
  for (const float value : edit.amplitude_envelope) {
    if (!std::isfinite(value) || value < 0.0f) return false;
    const double combined_gain = gain * static_cast<double>(value);
    if (!std::isfinite(combined_gain) || combined_gain > max_float) return false;
  }
  return true;
}

bool is_valid_note_span(const NoteObject& note, int64_t audio_samples) noexcept {
  return audio_samples >= 0 && note.onset_sample >= 0 && note.offset_sample > note.onset_sample &&
         note.offset_sample <= audio_samples;
}

void validate_note_for_render(const NoteObject& note, int64_t audio_samples) {
  // Check the ordering and the optional audio bound before deriving a length:
  // malformed INT64_MIN/MAX pairs must never reach a signed subtraction.
  SONARE_CHECK(note.onset_sample >= 0 && note.offset_sample > note.onset_sample,
               ErrorCode::InvalidParameter);
  if (audio_samples >= 0) {
    SONARE_CHECK(is_valid_note_span(note, audio_samples), ErrorCode::InvalidParameter);
  }
  const NoteEdit& edit = note.edit;
  SONARE_CHECK(is_valid_note_edit(edit), ErrorCode::InvalidParameter);
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

void validate_note_for_render(const NoteObject& note) { validate_note_for_render(note, -1); }

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
    validate_note_for_render(note, static_cast<int64_t>(audio.size()));
    all_identity = all_identity && note.edit.is_identity();
  }
  check_disjoint_spans(notes);

  // Nothing to resynthesize: the source passes through unchanged, bit for bit.
  if (all_identity) {
    SONARE_CHECK(
        std::all_of(audio.begin(), audio.end(), [](float sample) { return std::isfinite(sample); }),
        ErrorCode::InvalidParameter);
    return Audio::from_buffer(audio.data(), audio.size(), audio.sample_rate());
  }

  const int sample_rate = audio.sample_rate();
  std::vector<float> output(audio.begin(), audio.end());

  // Vacate every edited source span before rendering any destination. A moved
  // note may land on another edited note's source span, so erasing one note
  // while rendering the next would otherwise remove an earlier destination.
  for (const NoteObject& note : notes) {
    if (note.edit.is_identity()) continue;

    const int64_t onset = note.onset_sample;
    const int64_t offset = note.offset_sample;

    erase_span(output, audio, onset, offset,
               fade_samples(config.fade_ms, sample_rate, offset - onset));
  }

  // Render in vector order after clearing all edited source spans. Later notes
  // overwrite earlier destinations, with an edge blend against existing audio.
  for (const NoteObject& note : notes) {
    if (note.edit.is_identity() || note.edit.muted) continue;

    const int64_t onset = note.onset_sample;
    const int64_t offset = note.offset_sample;

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
    const float gain = db_to_linear(note.edit.gain_db);
    apply_envelope(segment, note.edit.amplitude_envelope, gain);

    const int64_t fade =
        fade_samples(config.fade_ms, sample_rate, static_cast<int64_t>(segment.size()));
    overlay(output, segment, saturating_add(onset, note.edit.time_offset_samples), fade);
  }

  SONARE_CHECK(
      std::all_of(output.begin(), output.end(), [](float sample) { return std::isfinite(sample); }),
      ErrorCode::InvalidParameter);
  return Audio::from_vector(std::move(output), sample_rate);
}

}  // namespace sonare::editing::note_model
