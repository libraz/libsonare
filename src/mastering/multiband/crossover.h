#pragma once

/// @file crossover.h
/// @brief Stateful multiband crossover built from matched low/high-pass filter pairs.

#include <vector>

namespace sonare::mastering::multiband {

enum class CrossoverSlope {
  LR2,
  LR4,
  LR8,
};

enum class CrossoverMode {
  LinkwitzRiley,
  Butterworth,
  Bessel,
};

struct CrossoverConfig {
  std::vector<float> cutoffs_hz{120.0f, 2000.0f};
  CrossoverSlope slope = CrossoverSlope::LR4;
  CrossoverMode mode = CrossoverMode::LinkwitzRiley;
};

struct CrossoverOutput {
  std::vector<std::vector<std::vector<float>>> bands;

  int num_bands() const { return static_cast<int>(bands.size()); }
  int num_channels() const { return bands.empty() ? 0 : static_cast<int>(bands[0].size()); }
  int num_samples() const {
    return bands.empty() || bands[0].empty() ? 0 : static_cast<int>(bands[0][0].size());
  }
};

class Crossover {
 public:
  explicit Crossover(CrossoverConfig config = {});

  void prepare(double sample_rate, int max_block_size);
  CrossoverOutput split(float* const* channels, int num_channels, int num_samples);
  void reset();

  void set_config(const CrossoverConfig& config);
  const CrossoverConfig& config() const { return config_; }
  int num_bands() const { return static_cast<int>(config_.cutoffs_hz.size()) + 1; }

 private:
  struct Biquad {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float z1 = 0.0f;
    float z2 = 0.0f;

    float process(float x) {
      const float y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }

    void reset_state() {
      z1 = 0.0f;
      z2 = 0.0f;
    }
  };

  static void validate_config(const CrossoverConfig& config, double sample_rate = 0.0);
  static int filter_order(CrossoverSlope slope, CrossoverMode mode);
  struct FilterSection {
    double lowpass_frequency_scale = 1.0;
    double highpass_frequency_scale = 1.0;
    double q = 0.7071067811865475;
  };
  static std::vector<FilterSection> filter_sections(CrossoverSlope slope, CrossoverMode mode);
  void rebuild_state(int num_channels);
  void install_coefficients();
  float lowpass(float sample, int split_index, int channel);
  float highpass(float sample, int split_index, int channel);
  float allpass(float sample, int band_index, int split_index, int channel);

  struct SplitChannelState {
    std::vector<Biquad> lowpass;
    std::vector<Biquad> highpass;
  };

  struct CompensationChannelState {
    std::vector<std::vector<Biquad>> allpass_by_split;
  };

  CrossoverConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  std::vector<std::vector<SplitChannelState>> states_;
  std::vector<std::vector<CompensationChannelState>> compensation_states_;
};

}  // namespace sonare::mastering::multiband
