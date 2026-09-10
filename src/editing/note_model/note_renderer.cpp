#include "editing/note_model/note_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

#include "effects/pitch_shift.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::editing::note_model {
namespace {

using sonare::constants::kHalfPi;

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

/// Ramps the source out over [@p begin, @p end) rather than cutting it. A note
/// that is muted, shortened or moved away leaves this range behind, and a hard
/// cut would put a step there; whatever @ref overlay writes back over the range
/// replaces it outright.
void erase_span(std::vector<float>& output, const Audio& source, int64_t begin, int64_t end,
                int64_t fade) {
  const int64_t length = end - begin;
  for (int64_t j = begin; j < end; ++j) {
    const float phase = fade_phase(j - begin, length, fade);
    const float source_gain = phase >= 1.0f ? 0.0f : std::cos(kHalfPi * phase);
    output[static_cast<size_t>(j)] = source_gain * source[static_cast<size_t>(j)];
  }
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

}  // namespace

Audio render_notes(const Audio& audio, const std::vector<NoteObject>& notes,
                   const NoteRenderConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.fade_ms) && config.fade_ms >= 0.0f,
               ErrorCode::InvalidParameter);

  bool all_identity = true;
  for (const NoteObject& note : notes) {
    SONARE_CHECK(note.onset_sample >= 0 && note.length_samples() > 0, ErrorCode::InvalidParameter);
    const NoteEdit& edit = note.edit;
    SONARE_CHECK(std::isfinite(edit.pitch_shift_semitones) && std::isfinite(edit.gain_db) &&
                     std::isfinite(edit.time_stretch_ratio) && edit.time_stretch_ratio > 0.0f,
                 ErrorCode::InvalidParameter);
    all_identity = all_identity && edit.is_identity();
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
