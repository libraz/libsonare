#include "editing/pitch_editor/f0_provider.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "analysis/analysis_rate.h"
#include "editing/pitch_editor/f0_refinement.h"

namespace sonare::editing::pitch_editor {

namespace {

constexpr int kEditingPitchHopReferenceSamples = 256;

}  // namespace

PitchConfig default_editing_pitch_config(int sample_rate) {
  PitchConfig config{};
  config.frame_length = window_length_at_rate(config.frame_length, sample_rate);
  config.hop_length = std::min(
      config.hop_length, window_length_at_rate(kEditingPitchHopReferenceSamples, sample_rate));
  return config;
}

F0Track pyin_f0_track(const Audio& audio, const PitchConfig& config) {
  PitchResult result = pyin(audio, config);
  F0Track track;
  track.f0_hz = std::move(result.f0);
  track.voiced_prob = std::move(result.voiced_prob);
  track.voiced = std::move(result.voiced_flag);
  track.hop_length = config.hop_length;
  track.sample_rate = audio.sample_rate();
  return track;
}

PyinF0Provider::PyinF0Provider(PitchConfig config) : config_(config) {}

F0Track PyinF0Provider::detect(const Audio& audio) {
  F0Track track = pyin_f0_track(audio, config_);

  // Keep the decoded pYIN candidate while restoring the continuous lag measured
  // from this frame. Scratch storage is shared across frames to avoid repeated
  // allocations in the editing path.
  std::vector<float> frame_samples;
  std::vector<double> difference;
  for (size_t frame = 0; frame < track.f0_hz.size(); ++frame) {
    const float f0_hz = track.f0_hz[frame];
    if (!track.voiced[frame] || !std::isfinite(f0_hz) || !(f0_hz > 0.0f)) continue;
    const int64_t frame_begin = static_cast<int64_t>(frame) * config_.hop_length -
                                (config_.center ? config_.frame_length / 2 : 0);
    const float refined = detail::refine_frame_f0(audio, frame_begin, config_.frame_length, f0_hz,
                                                  frame_samples, difference);
    if (refined >= config_.fmin && refined <= config_.fmax) track.f0_hz[frame] = refined;
  }
  return track;
}

}  // namespace sonare::editing::pitch_editor
