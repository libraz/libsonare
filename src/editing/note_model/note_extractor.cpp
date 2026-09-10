#include "editing/note_model/note_extractor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::editing::note_model {
namespace {

using sonare::constants::kCentsPerOctave;

/// Same guards as NoteSegmenter's private converter, which cannot be called
/// from here: a non-positive ratio would make log2 return NaN/-inf.
float hz_to_cents(float hz, float reference_hz) {
  if (!(hz > 0.0f) || !std::isfinite(hz) || !(reference_hz > 0.0f)) return 0.0f;
  return kCentsPerOctave * std::log2(hz / reference_hz);
}

bool usable_pitch(const pitch_editor::F0Track& track, int frame) {
  const float hz = track.f0_hz[static_cast<size_t>(frame)];
  return hz > 0.0f && std::isfinite(hz);
}

int64_t frame_to_sample(int frame, double samples_per_frame, int64_t n_samples) noexcept {
  if (frame <= 0 || !(samples_per_frame > 0.0)) return 0;
  const double samples = static_cast<double>(frame) * samples_per_frame;
  if (samples >= static_cast<double>(n_samples)) return n_samples;
  return static_cast<int64_t>(samples);
}

float rms_over(const Audio& audio, int64_t begin, int64_t end) {
  if (end <= begin) return 0.0f;
  double sum = 0.0;
  for (int64_t i = begin; i < end; ++i) {
    const double value = static_cast<double>(audio[static_cast<size_t>(i)]);
    sum += value * value;
  }
  return static_cast<float>(std::sqrt(sum / static_cast<double>(end - begin)));
}

/// Median absolute deviation: median(|x - median(x)|).
float median_absolute_deviation(const std::vector<float>& values) {
  if (values.empty()) return 0.0f;
  const float center = sonare::median(values.data(), values.size());
  std::vector<float> deviations(values.size());
  for (size_t i = 0; i < values.size(); ++i) {
    deviations[i] = std::abs(values[i] - center);
  }
  return sonare::median(deviations.data(), deviations.size());
}

}  // namespace

std::vector<NoteObject> extract_notes(const Audio& audio, const pitch_editor::F0Track& track,
                                      const NoteExtractorConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(track.n_frames() > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(track.frame_rate() > 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.voiced_threshold) &&
                   std::isfinite(config.segmenter.segmentation_threshold_cents) &&
                   std::isfinite(config.segmenter.min_note_ms) &&
                   std::isfinite(config.segmenter.reference_hz),
               ErrorCode::InvalidParameter);

  // A short array would silently mark the tail unvoiced, which reads as a
  // shorter note rather than as a malformed track.
  const size_t n = static_cast<size_t>(track.n_frames());
  SONARE_CHECK(track.voiced.empty() ? track.voiced_prob.size() == n : track.voiced.size() == n,
               ErrorCode::InvalidParameter);

  pitch_editor::F0Track resolved = track;
  // NoteSegmenter guards on hop_length / sample_rate even for a track carrying
  // an explicit cadence, so fill both from the cadence: leaving them unset
  // segments to nothing instead of failing.
  if (resolved.sample_rate <= 0) resolved.sample_rate = audio.sample_rate();
  if (resolved.hop_length <= 0) {
    const double per_frame =
        static_cast<double>(resolved.sample_rate) / static_cast<double>(resolved.frame_rate());
    SONARE_CHECK(
        per_frame >= 1.0 && per_frame <= static_cast<double>(std::numeric_limits<int>::max()),
        ErrorCode::InvalidParameter);
    resolved.hop_length = static_cast<int>(per_frame);
  }
  if (resolved.voiced.empty()) {
    resolved.voiced.assign(n, false);
    for (size_t i = 0; i < n; ++i) {
      resolved.voiced[i] = resolved.voiced_prob[i] >= config.voiced_threshold;
    }
  }

  const std::vector<pitch_editor::NoteRegion> regions =
      pitch_editor::NoteSegmenter(config.segmenter).segment(resolved);

  const int n_frames = resolved.n_frames();
  const float frame_rate = resolved.frame_rate();
  const double samples_per_frame = resolved.samples_per_frame();
  const int64_t n_samples = static_cast<int64_t>(audio.size());
  const float threshold_cents = config.segmenter.segmentation_threshold_cents;

  std::vector<NoteObject> notes;
  notes.reserve(regions.size());
  for (const pitch_editor::NoteRegion& region : regions) {
    const int frame_start = std::clamp(region.frame_start, 0, n_frames);
    const int frame_end = std::clamp(region.frame_end, frame_start, n_frames);

    NoteObject note;
    // The clamped span is what both curves are cut from, so the note must carry
    // that one rather than the region's own bounds.
    note.frame_start = frame_start;
    note.frame_end = frame_end;
    note.onset_sample = std::clamp<int64_t>(region.onset_sample, 0, n_samples);
    note.offset_sample = std::clamp<int64_t>(region.offset_sample, note.onset_sample, n_samples);
    note.median_cents = region.median_cents;
    note.median_hz =
        config.segmenter.reference_hz * std::pow(2.0f, region.median_cents / kCentsPerOctave);

    note.f0_hz.values.assign(resolved.f0_hz.begin() + frame_start,
                             resolved.f0_hz.begin() + frame_end);
    note.f0_hz.frame_rate_hz = frame_rate;
    note.f0_hz.frame_offset = frame_start;

    // One RMS per F0 frame over that frame's samples, so both curves index alike.
    note.amplitude.values.resize(static_cast<size_t>(frame_end - frame_start));
    note.amplitude.frame_rate_hz = frame_rate;
    note.amplitude.frame_offset = frame_start;
    for (int frame = frame_start; frame < frame_end; ++frame) {
      const int64_t begin = frame_to_sample(frame, samples_per_frame, n_samples);
      const int64_t end = std::max(begin, frame_to_sample(frame + 1, samples_per_frame, n_samples));
      note.amplitude.values[static_cast<size_t>(frame - frame_start)] = rms_over(audio, begin, end);
    }

    int voiced_frames = 0;
    std::vector<float> voiced_cents;
    voiced_cents.reserve(static_cast<size_t>(frame_end - frame_start));
    for (int frame = frame_start; frame < frame_end; ++frame) {
      if (!resolved.voiced[static_cast<size_t>(frame)]) continue;
      ++voiced_frames;
      if (usable_pitch(resolved, frame)) {
        voiced_cents.push_back(
            hz_to_cents(resolved.f0_hz[static_cast<size_t>(frame)], config.segmenter.reference_hz));
      }
    }

    const int span_frames = frame_end - frame_start;
    note.voiced_ratio = span_frames > 0
                            ? static_cast<float>(voiced_frames) / static_cast<float>(span_frames)
                            : 0.0f;
    if (voiced_cents.empty() || !(threshold_cents > 0.0f)) {
      note.f0_stability = 0.0f;
    } else {
      const float mad = median_absolute_deviation(voiced_cents);
      note.f0_stability = 1.0f - std::min(1.0f, mad / threshold_cents);
    }

    notes.push_back(std::move(note));
  }

  return notes;
}

}  // namespace sonare::editing::note_model
