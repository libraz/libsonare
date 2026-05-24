#include "analysis/pitch_editor/note_segmenter.h"

#include <algorithm>
#include <cmath>

namespace sonare::analysis::pitch_editor {

NoteSegmenter::NoteSegmenter(NoteSegmenterConfig config) : config_(config) {}

std::vector<NoteRegion> NoteSegmenter::segment(const F0Track& track) const {
  std::vector<NoteRegion> regions;
  if (track.f0_hz.empty() || track.voiced.size() != track.f0_hz.size() || track.hop_length <= 0 ||
      track.sample_rate <= 0) {
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
  return 1200.0f * std::log2(hz / reference_hz);
}

NoteRegion NoteSegmenter::make_region(const F0Track& track, int start, int end) const {
  std::vector<float> cents;
  for (int frame = start; frame < end; ++frame) {
    if (track.voiced[static_cast<size_t>(frame)] &&
        track.f0_hz[static_cast<size_t>(frame)] > 0.0f) {
      cents.push_back(hz_to_cents(track.f0_hz[static_cast<size_t>(frame)], config_.reference_hz));
    }
  }
  std::sort(cents.begin(), cents.end());
  float median = 0.0f;
  if (!cents.empty()) {
    const size_t mid = cents.size() / 2;
    median = cents.size() % 2 == 0 ? 0.5f * (cents[mid - 1] + cents[mid]) : cents[mid];
  }

  return {start * track.hop_length, end * track.hop_length, median, start, end};
}

}  // namespace sonare::analysis::pitch_editor
