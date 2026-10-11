#include "editing/pitch_editor/note_segmenter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "util/math_utils.h"

namespace sonare::editing::pitch_editor {
namespace {

// One cycle of the slowest common vibrato (4 Hz): a span held for half of it has seen a full
// excursion whatever phase the note started on.
constexpr double kReferenceSettleMs = 250.0;

int saturated_sample_offset(int frame, double samples_per_frame) noexcept {
  if (frame <= 0 || !(samples_per_frame > 0.0)) return 0;
  const double samples = static_cast<double>(frame) * samples_per_frame;
  if (samples > static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(samples);
}

// A frame carries a usable pitch only when it is both voiced and a finite,
// positive Hz value. +inf satisfies "> 0.0f", so a plain positivity check
// alone reads a non-finite frame as strongly voiced instead of as carrying no
// measurement -- the same contract note_extractor.cpp's own usable_pitch()
// enforces for the shared non-finite-F0 convention.
bool usable_pitch(const F0Track& track, int frame) noexcept {
  const float hz = track.f0_hz[static_cast<size_t>(frame)];
  return track.voiced[static_cast<size_t>(frame)] && std::isfinite(hz) && hz > 0.0f;
}

// Cents in double so a span compared against 2 x threshold does not round at the boundary.
double frame_cents(float hz, float reference_hz) noexcept {
  return static_cast<double>(constants::kCentsPerOctave) *
         std::log2(static_cast<double>(hz) / reference_hz);
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

  // One cadence rule per track (F0Track::frame_rate): the sample offsets below
  // are derived from the same one, so a host-supplied frame_rate_hz cannot be
  // honoured for the duration threshold and ignored for the region bounds.
  const float frame_rate = track.frame_rate();
  const double threshold = config_.segmentation_threshold_cents;
  const int min_frames =
      std::max(1, static_cast<int>(std::ceil(config_.min_note_ms * 0.001f * frame_rate)));
  const int settle_frames =
      std::max(min_frames, static_cast<int>(std::ceil(kReferenceSettleMs * 0.001 * frame_rate)));

  int start = -1;
  int sustained = 0;
  int member_count = 0;
  int last_growth = 0;
  double low = 0.0;
  double high = 0.0;
  bool frozen = false;
  bool gliding = false;
  double glide_sign = 0.0;
  double last_cents = 0.0;
  const auto open_region = [&](int first_frame, double seed_cents, bool follows_glide) {
    start = first_frame;
    sustained = 0;
    member_count = 1;
    last_growth = first_frame;
    low = high = seed_cents;
    frozen = false;
    gliding = follows_glide;
  };
  const auto close_region = [&](int end_frame) {
    if (start >= 0 && end_frame - start >= min_frames) {
      regions.push_back(make_region(track, start, end_frame));
    }
  };

  for (int frame = 0; frame < track.n_frames(); ++frame) {
    if (!usable_pitch(track, frame)) {
      close_region(frame);
      start = -1;
      continue;
    }
    const double cents = frame_cents(track.f0_hz[static_cast<size_t>(frame)], config_.reference_hz);
    if (start < 0) {
      open_region(frame, cents, false);
      last_cents = cents;
      continue;
    }
    if (gliding) {
      // The note opened mid-glide: carry its pitch along until the glide stops.
      if ((cents - last_cents) * glide_sign > 0.0 && frame - start < settle_frames) {
        low = high = cents;
        last_growth = frame;
        last_cents = cents;
        continue;
      }
      gliding = false;
    }
    last_cents = cents;
    const double reference = 0.5 * (low + high);
    // While settling a frame also has to keep the span within 2 x threshold, so the midpoint
    // stays within the threshold of every frame it was built from.
    const bool deviates =
        std::abs(cents - reference) > threshold ||
        (!frozen && std::max(high, cents) - std::min(low, cents) > 2.0 * threshold);
    if (deviates) {
      ++sustained;
      if (sustained >= min_frames) {
        const int split = frame - sustained + 1;
        glide_sign = cents > reference ? 1.0 : -1.0;
        close_region(split);
        open_region(split, cents, true);
      }
      continue;
    }
    sustained = 0;
    if (!frozen) {
      if (cents < low || cents > high) {
        low = std::min(low, cents);
        high = std::max(high, cents);
        last_growth = frame;
      }
      ++member_count;
      frozen = (member_count >= settle_frames && frame - last_growth >= settle_frames / 2) ||
               frame - start >= 2 * settle_frames;
    }
  }
  close_region(track.n_frames());

  return regions;
}

float NoteSegmenter::hz_to_cents(float hz, float reference_hz) {
  // log2 of a non-positive ratio is NaN/-inf, and a non-finite hz (e.g. +inf
  // from an unclamped detector) is not a measurement either; guard all three
  // so a bad reference, an unvoiced 0 Hz, or a non-finite hz cannot poison the
  // cents statistics.
  if (!std::isfinite(hz) || !(hz > 0.0f) || !(reference_hz > 0.0f)) {
    return 0.0f;
  }
  return constants::kCentsPerOctave * std::log2(hz / reference_hz);
}

NoteRegion NoteSegmenter::make_region(const F0Track& track, int start, int end) const {
  std::vector<float> cents;
  for (int frame = start; frame < end; ++frame) {
    if (usable_pitch(track, frame)) {
      cents.push_back(hz_to_cents(track.f0_hz[static_cast<size_t>(frame)], config_.reference_hz));
    }
  }
  const float median = sonare::median(cents.data(), cents.size());

  const double samples_per_frame = track.samples_per_frame();
  return {saturated_sample_offset(start, samples_per_frame),
          saturated_sample_offset(end, samples_per_frame), median, start, end};
}

}  // namespace sonare::editing::pitch_editor
