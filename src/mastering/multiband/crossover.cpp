#include "mastering/multiband/crossover.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::multiband {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<double> butterworth_q_values(int order) {
  std::vector<double> qs;
  const int pairs = order / 2;
  for (int k = 0; k < pairs; ++k) {
    const double theta = kPi * (2 * k + 1) / (2.0 * order);
    qs.push_back(1.0 / (2.0 * std::sin(theta)));
  }
  return qs;
}

}  // namespace

Crossover::Crossover(CrossoverConfig config) : config_(std::move(config)) {
  validate_config(config_);
}

void Crossover::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  validate_config(config_, sample_rate);
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  rebuild_state(states_.empty() ? 0 : static_cast<int>(states_[0].size()));
  reset();
}

CrossoverOutput Crossover::split(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("Crossover must be prepared before processing");
  }
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }

  CrossoverOutput output;
  output.bands.assign(static_cast<size_t>(num_bands()),
                      std::vector<std::vector<float>>(static_cast<size_t>(num_channels),
                                                      std::vector<float>(num_samples, 0.0f)));
  if (num_channels == 0 || num_samples == 0) {
    return output;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }

  rebuild_state(num_channels);
  const int splits = static_cast<int>(config_.cutoffs_hz.size());

  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      float remainder = channels[ch][i];
      for (int split_index = 0; split_index < splits; ++split_index) {
        const float low = lowpass(remainder, split_index, ch);
        output.bands[static_cast<size_t>(split_index)][static_cast<size_t>(ch)]
                    [static_cast<size_t>(i)] = low;
        remainder -= low;
      }
      output.bands.back()[static_cast<size_t>(ch)][static_cast<size_t>(i)] = remainder;
    }
  }

  return output;
}

void Crossover::reset() {
  for (auto& split_states : states_) {
    for (auto& channel_states : split_states) {
      for (auto& stage : channel_states) {
        stage.reset_state();
      }
    }
  }
}

void Crossover::set_config(const CrossoverConfig& config) {
  validate_config(config, prepared_ ? sample_rate_ : 0.0);
  config_ = config;
  if (prepared_) {
    prepare(sample_rate_, max_block_size_);
  }
}

void Crossover::validate_config(const CrossoverConfig& config, double sample_rate) {
  for (size_t i = 0; i < config.cutoffs_hz.size(); ++i) {
    if (!(config.cutoffs_hz[i] > 0.0f)) {
      throw std::invalid_argument("crossover cutoffs must be positive");
    }
    if (i > 0 && !(config.cutoffs_hz[i] > config.cutoffs_hz[i - 1])) {
      throw std::invalid_argument("crossover cutoffs must be strictly ascending");
    }
    if (sample_rate > 0.0 && !(config.cutoffs_hz[i] < static_cast<float>(sample_rate * 0.5))) {
      throw std::invalid_argument("crossover cutoffs must be below Nyquist");
    }
  }
}

int Crossover::filter_order(CrossoverSlope slope, CrossoverMode /*mode*/) {
  switch (slope) {
    case CrossoverSlope::LR2:
      return 2;
    case CrossoverSlope::LR4:
      return 4;
    case CrossoverSlope::LR8:
      return 8;
  }
  return 4;
}

std::vector<double> Crossover::filter_q_values(CrossoverSlope slope, CrossoverMode mode) {
  const int order = filter_order(slope, mode);

  if (mode == CrossoverMode::LinkwitzRiley) {
    const int half = order / 2;
    std::vector<double> half_qs;
    if (half <= 1) {
      // LR2: two cascaded 1st-order one-pole sections -> biquad with Q = 0.5.
      return {0.5, 0.5};
    }
    half_qs = butterworth_q_values(half);
    std::vector<double> qs;
    qs.reserve(half_qs.size() * 2);
    for (double q : half_qs) qs.push_back(q);
    for (double q : half_qs) qs.push_back(q);
    return qs;
  }

  if (mode == CrossoverMode::Bessel) {
    std::vector<double> qs = butterworth_q_values(order);
    for (double& q : qs) q *= 0.85;
    return qs;
  }

  return butterworth_q_values(order);
}

void Crossover::install_coefficients() {
  const auto qs = filter_q_values(config_.slope, config_.mode);
  for (size_t split_index = 0; split_index < states_.size(); ++split_index) {
    const double f =
        std::clamp(static_cast<double>(config_.cutoffs_hz[split_index]), 1.0, sample_rate_ * 0.49);
    for (auto& channel_states : states_[split_index]) {
      for (size_t k = 0; k < qs.size() && k < channel_states.size(); ++k) {
        make_lowpass_biquad(f, sample_rate_, qs[k], channel_states[k]);
      }
    }
  }
}

void Crossover::rebuild_state(int num_channels) {
  const size_t splits = config_.cutoffs_hz.size();
  const size_t stages = filter_q_values(config_.slope, config_.mode).size();
  const bool shape_matches =
      states_.size() == splits &&
      (splits == 0 || states_[0].size() == static_cast<size_t>(num_channels)) &&
      (splits == 0 || num_channels == 0 || states_[0][0].size() == stages);
  if (!shape_matches) {
    states_.assign(splits, std::vector<std::vector<Biquad>>(static_cast<size_t>(num_channels),
                                                            std::vector<Biquad>(stages)));
  }
  install_coefficients();
}

float Crossover::lowpass(float sample, int split_index, int channel) {
  float y = sample;
  auto& stages = states_[static_cast<size_t>(split_index)][static_cast<size_t>(channel)];
  for (auto& stage : stages) {
    y = stage.process(y);
  }
  return y;
}

}  // namespace sonare::mastering::multiband
