#include "editing/pitch_editor/note_segmenter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "util/math_utils.h"

namespace sonare::editing::pitch_editor {
namespace {

int saturated_sample_offset(int frame, int hop_length) noexcept {
  if (frame <= 0 || hop_length <= 0) return 0;
  const int64_t samples = static_cast<int64_t>(frame) * static_cast<int64_t>(hop_length);
  if (samples > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
  return static_cast<int>(samples);
}

}  // namespace

NoteSegmenter::NoteSegmenter(NoteSegmenterConfig config) : config_(config) {}

std::vector<NoteRegion> NoteSegmenter::segment(const F0Track& track) const {
  std::vector<NoteRegion> regions;
  if (track.f0_hz.empty() || track.voiced.size() != track.f0_hz.size() || track.hop_length <= 0 ||
      track.sample_rate <= 0) {
    return regions;
  }
  // A non-positive or non-finite reference frequency makes every hz_to_cents
  // return 0 (its own guard), collapsing all pitch statistics to a single value
  // and silently producing meaningless segmentation. Bail out explicitly.
  if (!(config_.reference_hz > 0.0f) || !std::isfinite(config_.reference_hz)) {
    return regions;
  }

  const int min_frames =
      std::max(1, static_cast<int>(std::ceil(config_.min_note_ms * 0.001f *
                                             static_cast<float>(track.sample_rate) /
                                             static_cast<float>(track.hop_length))));

  int start = -1;
  float reference_cents = 0.0f;
  int sustained_deviation = 0;

  for (int frame = 0; frame < track.n_frames(); ++frame) {
    const bool voiced =
        track.voiced[static_cast<size_t>(frame)] && track.f0_hz[static_cast<size_t>(frame)] > 0.0f;
    if (!voiced) {
      if (start >= 0 && frame - start >= min_frames) {
        regions.push_back(make_region(track, start, frame));
      }
      start = -1;
      sustained_deviation = 0;
      continue;
    }

    const float cents = hz_to_cents(track.f0_hz[static_cast<size_t>(frame)], config_.reference_hz);
    if (start < 0) {
      start = frame;
      reference_cents = cents;
      sustained_deviation = 0;
      continue;
    }

    if (std::abs(cents - reference_cents) > config_.segmentation_threshold_cents) {
      ++sustained_deviation;
      if (sustained_deviation >= min_frames) {
        const int split = frame - sustained_deviation + 1;
        if (split - start >= min_frames) {
          regions.push_back(make_region(track, start, split));
        }
        start = split;
        reference_cents = cents;
        sustained_deviation = 0;
      }
    } else {
      sustained_deviation = 0;
    }
  }

  if (start >= 0 && track.n_frames() - start >= min_frames) {
    regions.push_back(make_region(track, start, track.n_frames()));
  }

  return regions;
}

float NoteSegmenter::hz_to_cents(float hz, float reference_hz) {
  // log2 of a non-positive ratio is NaN/-inf; guard both inputs so a bad
  // reference (or an unvoiced 0 Hz) cannot poison the cents statistics.
  if (!(hz > 0.0f) || !(reference_hz > 0.0f)) {
    return 0.0f;
  }
  return constants::kCentsPerOctave * std::log2(hz / reference_hz);
}

NoteRegion NoteSegmenter::make_region(const F0Track& track, int start, int end) const {
  std::vector<float> cents;
  for (int frame = start; frame < end; ++frame) {
    if (track.voiced[static_cast<size_t>(frame)] &&
        track.f0_hz[static_cast<size_t>(frame)] > 0.0f) {
      cents.push_back(hz_to_cents(track.f0_hz[static_cast<size_t>(frame)], config_.reference_hz));
    }
  }
  const float median = sonare::median(cents.data(), cents.size());

  return {saturated_sample_offset(start, track.hop_length),
          saturated_sample_offset(end, track.hop_length), median, start, end};
}

}  // namespace sonare::editing::pitch_editor
