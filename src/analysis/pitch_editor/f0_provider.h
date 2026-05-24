#pragma once

/// @file f0_provider.h
/// @brief F0 provider abstraction for monophonic pitch editing.

#include <vector>

#include "core/audio.h"
#include "feature/pitch.h"

namespace sonare::analysis::pitch_editor {

struct F0Track {
  std::vector<float> f0_hz;
  std::vector<float> voiced_prob;
  std::vector<bool> voiced;
  int hop_length = 512;
  int sample_rate = 48000;

  int n_frames() const noexcept { return static_cast<int>(f0_hz.size()); }
};

class F0Provider {
 public:
  virtual ~F0Provider() = default;
  virtual F0Track detect(const Audio& audio) = 0;
};

class PyinF0Provider final : public F0Provider {
 public:
  explicit PyinF0Provider(PitchConfig config = {});
  F0Track detect(const Audio& audio) override;
  const PitchConfig& config() const noexcept { return config_; }

 private:
  PitchConfig config_{};
};

}  // namespace sonare::analysis::pitch_editor
