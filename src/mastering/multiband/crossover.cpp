#include "mastering/multiband/crossover.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/window.h"
#include "util/constants.h"

namespace sonare::mastering::multiband {

namespace {

using sonare::constants::kPiD;

struct SectionSpec {
  double lowpass_frequency_scale = 1.0;
  double highpass_frequency_scale = 1.0;
  double q = sonare::constants::kButterworthQD;
};

bool is_lr2_linkwitz_riley(CrossoverSlope slope, CrossoverMode mode) {
  return slope == CrossoverSlope::LR2 && mode == CrossoverMode::LinkwitzRiley;
}

bool uses_linkwitz_riley_compensation(CrossoverMode mode) {
  return mode == CrossoverMode::LinkwitzRiley;
}

double scaled_digital_frequency(double base_hz, double scale, double sample_rate) {
  const double base = std::clamp(base_hz, 1.0, sample_rate * 0.49);
  const double warped = std::tan(kPiD * base / sample_rate);
  const double scaled = std::max(scale, 1.0e-9) * warped;
  return std::clamp(sample_rate * std::atan(scaled) / kPiD, 1.0, sample_rate * 0.49);
}

std::vector<SectionSpec> butterworth_section_specs(int order) {
  std::vector<SectionSpec> sections;
  const int pairs = order / 2;
  for (int k = 0; k < pairs; ++k) {
    const double theta = kPiD * (2 * k + 1) / (2.0 * order);
    sections.push_back({1.0, 1.0, 1.0 / (2.0 * std::sin(theta))});
  }
  return sections;
}

std::vector<SectionSpec> bessel_section_specs(int order) {
  // Magnitude-normalized Bessel sections. Frequency scale is |pole| for low-pass
  // sections and its inverse for high-pass sections after the standard LP->HP transform.
  switch (order) {
    case 2:
      return {{1.2720196495140688, 1.0 / 1.2720196495140688, 0.5773502691896258}};
    case 4:
      return {{1.4301715599939920, 1.0 / 1.4301715599939920, 0.5219345816689801},
              {1.6033575162169744, 1.0 / 1.6033575162169744, 0.8055382818416656}};
    case 8:
      return {{1.7784659117747328, 1.0 / 1.7784659117747328, 0.5059910693974672},
              {1.8320926011986880, 1.0 / 1.8320926011986880, 0.5596091647957870},
              {1.9531957590221913, 1.0 / 1.9531957590221913, 0.7108520744416966},
              {2.1887262305275494, 1.0 / 2.1887262305275494, 1.2256694254081753}};
  }
  return butterworth_section_specs(order);
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
  if (config_.mode == CrossoverMode::FirLinearPhase) {
    rebuild_fir_kernels();
    rebuild_fir_state(fir_delay_history_.empty() ? 0 : static_cast<int>(fir_delay_history_.size()));
  } else {
    rebuild_state(states_.empty() ? 0 : static_cast<int>(states_[0].size()));
  }
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
  if (config_.mode == CrossoverMode::FirLinearPhase) {
    return split_fir(channels, num_channels, num_samples);
  }

  rebuild_state(num_channels);
  const int splits = static_cast<int>(config_.cutoffs_hz.size());

  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      float remainder = channels[ch][i];
      for (int split_index = 0; split_index < splits; ++split_index) {
        float low = lowpass(remainder, split_index, ch);
        const float high = highpass(remainder, split_index, ch);
        if (uses_linkwitz_riley_compensation(config_.mode)) {
          // Multi-way LR sums flat only when lower bands see the same downstream
          // all-pass phase rotations as the high-side remainder.
          for (int compensation_split = split_index + 1; compensation_split < splits;
               ++compensation_split) {
            low = allpass(low, split_index, compensation_split, ch);
          }
        }
        output.bands[static_cast<size_t>(split_index)][static_cast<size_t>(ch)]
                    [static_cast<size_t>(i)] = low;
        remainder = high;
      }
      output.bands.back()[static_cast<size_t>(ch)][static_cast<size_t>(i)] = remainder;
    }
  }

  return output;
}

void Crossover::reset() {
  for (auto& split_states : states_) {
    for (auto& channel_states : split_states) {
      for (auto& stage : channel_states.lowpass) {
        stage.reset_state();
      }
      for (auto& stage : channel_states.highpass) {
        stage.reset_state();
      }
    }
  }
  for (auto& band_states : compensation_states_) {
    for (auto& channel_states : band_states) {
      for (auto& split_states : channel_states.allpass_by_split) {
        for (auto& stage : split_states) {
          stage.reset_state();
        }
      }
    }
  }
  for (auto& split_history : fir_history_) {
    for (auto& channel_history : split_history) {
      std::fill(channel_history.begin(), channel_history.end(), 0.0f);
    }
  }
  for (auto& split_indices : fir_history_index_) {
    std::fill(split_indices.begin(), split_indices.end(), 0);
  }
  for (auto& channel_history : fir_delay_history_) {
    std::fill(channel_history.begin(), channel_history.end(), 0.0f);
  }
  std::fill(fir_delay_index_.begin(), fir_delay_index_.end(), 0);
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
  if (config.mode == CrossoverMode::FirLinearPhase &&
      (config.fir_kernel_size < 3 || config.fir_kernel_size % 2 == 0)) {
    throw std::invalid_argument("linear-phase crossover FIR kernel size must be odd and >= 3");
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

std::vector<Crossover::FilterSection> Crossover::filter_sections(CrossoverSlope slope,
                                                                 CrossoverMode mode) {
  const int order = filter_order(slope, mode);
  const auto to_filter_sections = [](const std::vector<SectionSpec>& specs) {
    std::vector<FilterSection> sections;
    sections.reserve(specs.size());
    for (const auto& section : specs) {
      sections.push_back(
          {section.lowpass_frequency_scale, section.highpass_frequency_scale, section.q});
    }
    return sections;
  };

  if (mode == CrossoverMode::LinkwitzRiley) {
    const int half = order / 2;
    if (half <= 1) {
      // LR2 is two cascaded first-order sections. The placeholder values keep the state shape
      // aligned with the number of stages; coefficients are installed by the LR2 special case.
      return {{1.0, 1.0, 0.0}, {1.0, 1.0, 0.0}};
    }
    const auto half_sections = butterworth_section_specs(half);
    std::vector<FilterSection> sections;
    sections.reserve(half_sections.size() * 2);
    for (const auto& section : half_sections) {
      sections.push_back(
          {section.lowpass_frequency_scale, section.highpass_frequency_scale, section.q});
    }
    for (const auto& section : half_sections) {
      sections.push_back(
          {section.lowpass_frequency_scale, section.highpass_frequency_scale, section.q});
    }
    return sections;
  }

  if (mode == CrossoverMode::Bessel) {
    return to_filter_sections(bessel_section_specs(order));
  }

  return to_filter_sections(butterworth_section_specs(order));
}

void Crossover::install_coefficients() {
  const auto sections = filter_sections(config_.slope, config_.mode);
  for (size_t split_index = 0; split_index < states_.size(); ++split_index) {
    const double f =
        std::clamp(static_cast<double>(config_.cutoffs_hz[split_index]), 1.0, sample_rate_ * 0.49);
    if (is_lr2_linkwitz_riley(config_.slope, config_.mode)) {
      const double k = std::tan(kPiD * f / sample_rate_);
      const double inv = 1.0 / (1.0 + k);
      const double lp_b = k * inv;
      const double hp_b = inv;
      const double a1 = (k - 1.0) * inv;

      for (auto& channel_states : states_[split_index]) {
        for (size_t stage_index = 0; stage_index < channel_states.lowpass.size() &&
                                     stage_index < channel_states.highpass.size();
             ++stage_index) {
          auto& lp = channel_states.lowpass[stage_index];
          lp.b0 = static_cast<float>(lp_b);
          lp.b1 = static_cast<float>(lp_b);
          lp.b2 = 0.0f;
          lp.a1 = static_cast<float>(a1);
          lp.a2 = 0.0f;

          auto& hp = channel_states.highpass[stage_index];
          hp.b0 = static_cast<float>(hp_b);
          hp.b1 = static_cast<float>(-hp_b);
          hp.b2 = 0.0f;
          hp.a1 = static_cast<float>(a1);
          hp.a2 = 0.0f;
        }
      }

      for (auto& band_states : compensation_states_) {
        for (auto& channel_states : band_states) {
          if (split_index >= channel_states.allpass_by_split.size()) {
            continue;
          }
          auto& allpass_stages = channel_states.allpass_by_split[split_index];
          for (auto& ap : allpass_stages) {
            ap.b0 = static_cast<float>(a1);
            ap.b1 = 1.0f;
            ap.b2 = 0.0f;
            ap.a1 = static_cast<float>(a1);
            ap.a2 = 0.0f;
          }
        }
      }
      continue;
    }

    for (auto& channel_states : states_[split_index]) {
      for (size_t k = 0; k < sections.size() && k < channel_states.lowpass.size() &&
                         k < channel_states.highpass.size();
           ++k) {
        const auto& section = sections[k];
        const double lp_w0 =
            2.0 * kPiD * scaled_digital_frequency(f, section.lowpass_frequency_scale, sample_rate_) /
            sample_rate_;
        const double lp_cos_w0 = std::cos(lp_w0);
        const double lp_sin_w0 = std::sin(lp_w0);
        const double lp_alpha = lp_sin_w0 / (2.0 * section.q);

        const double lp_b0 = (1.0 - lp_cos_w0) * 0.5;
        const double lp_b1 = 1.0 - lp_cos_w0;
        const double lp_b2 = (1.0 - lp_cos_w0) * 0.5;
        const double lp_a0 = 1.0 + lp_alpha;
        const double lp_a1 = -2.0 * lp_cos_w0;
        const double lp_a2 = 1.0 - lp_alpha;
        const double lp_inv = 1.0 / lp_a0;

        auto& lp = channel_states.lowpass[k];
        lp.b0 = static_cast<float>(lp_b0 * lp_inv);
        lp.b1 = static_cast<float>(lp_b1 * lp_inv);
        lp.b2 = static_cast<float>(lp_b2 * lp_inv);
        lp.a1 = static_cast<float>(lp_a1 * lp_inv);
        lp.a2 = static_cast<float>(lp_a2 * lp_inv);

        const double hp_w0 =
            2.0 * kPiD *
            scaled_digital_frequency(f, section.highpass_frequency_scale, sample_rate_) /
            sample_rate_;
        const double hp_cos_w0 = std::cos(hp_w0);
        const double hp_sin_w0 = std::sin(hp_w0);
        const double hp_alpha = hp_sin_w0 / (2.0 * section.q);
        const double hp_a0 = 1.0 + hp_alpha;
        const double hp_a1 = -2.0 * hp_cos_w0;
        const double hp_a2 = 1.0 - hp_alpha;
        const double hp_inv = 1.0 / hp_a0;

        auto& hp = channel_states.highpass[k];
        hp.b0 = static_cast<float>((1.0 + hp_cos_w0) * 0.5 * hp_inv);
        hp.b1 = static_cast<float>(-(1.0 + hp_cos_w0) * hp_inv);
        hp.b2 = static_cast<float>((1.0 + hp_cos_w0) * 0.5 * hp_inv);
        hp.a1 = static_cast<float>(hp_a1 * hp_inv);
        hp.a2 = static_cast<float>(hp_a2 * hp_inv);
      }
    }

    for (auto& band_states : compensation_states_) {
      for (auto& channel_states : band_states) {
        if (split_index >= channel_states.allpass_by_split.size()) {
          continue;
        }
        auto& allpass_stages = channel_states.allpass_by_split[split_index];
        for (size_t k = 0; k < sections.size() && k < allpass_stages.size(); ++k) {
          const double w0 =
              2.0 * kPiD *
              scaled_digital_frequency(f, sections[k].lowpass_frequency_scale, sample_rate_) /
              sample_rate_;
          const double cos_w0 = std::cos(w0);
          const double sin_w0 = std::sin(w0);
          const double alpha = sin_w0 / (2.0 * sections[k].q);

          const double a0 = 1.0 + alpha;
          const double a1 = -2.0 * cos_w0;
          const double a2 = 1.0 - alpha;
          const double inv = 1.0 / a0;

          auto& ap = allpass_stages[k];
          ap.b0 = static_cast<float>(a2 * inv);
          ap.b1 = static_cast<float>(a1 * inv);
          ap.b2 = 1.0f;
          ap.a1 = static_cast<float>(a1 * inv);
          ap.a2 = static_cast<float>(a2 * inv);
        }
      }
    }
  }
}

CrossoverOutput Crossover::split_fir(float* const* channels, int num_channels, int num_samples) {
  rebuild_fir_state(num_channels);

  CrossoverOutput output;
  output.bands.assign(static_cast<size_t>(num_bands()),
                      std::vector<std::vector<float>>(static_cast<size_t>(num_channels),
                                                      std::vector<float>(num_samples, 0.0f)));

  const int splits = static_cast<int>(config_.cutoffs_hz.size());
  std::vector<float> lowpasses(static_cast<size_t>(splits), 0.0f);
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      const float sample = channels[ch][i];
      for (int split_index = 0; split_index < splits; ++split_index) {
        lowpasses[static_cast<size_t>(split_index)] = process_fir_lowpass(sample, split_index, ch);
      }
      const float delayed = process_fir_delay(sample, ch);
      if (splits == 0) {
        output.bands[0][static_cast<size_t>(ch)][static_cast<size_t>(i)] = delayed;
        continue;
      }

      output.bands[0][static_cast<size_t>(ch)][static_cast<size_t>(i)] = lowpasses[0];
      for (int band = 1; band < splits; ++band) {
        output.bands[static_cast<size_t>(band)][static_cast<size_t>(ch)][static_cast<size_t>(i)] =
            lowpasses[static_cast<size_t>(band)] - lowpasses[static_cast<size_t>(band - 1)];
      }
      output.bands.back()[static_cast<size_t>(ch)][static_cast<size_t>(i)] =
          delayed - lowpasses.back();
    }
  }
  return output;
}

void Crossover::rebuild_fir_state(int num_channels) {
  rebuild_fir_kernels();
  const size_t splits = config_.cutoffs_hz.size();
  const size_t kernel_size = static_cast<size_t>(config_.fir_kernel_size);
  const size_t delay_size = static_cast<size_t>(config_.fir_kernel_size / 2 + 1);
  const bool shape_matches =
      fir_history_.size() == splits &&
      (splits == 0 || fir_history_[0].size() == static_cast<size_t>(num_channels)) &&
      (splits == 0 || num_channels == 0 || fir_history_[0][0].size() == kernel_size) &&
      fir_delay_history_.size() == static_cast<size_t>(num_channels) &&
      (num_channels == 0 || fir_delay_history_[0].size() == delay_size);
  if (shape_matches) {
    return;
  }

  fir_history_.assign(splits,
                      std::vector<std::vector<float>>(static_cast<size_t>(num_channels),
                                                      std::vector<float>(kernel_size, 0.0f)));
  fir_history_index_.assign(splits, std::vector<size_t>(static_cast<size_t>(num_channels), 0));
  fir_delay_history_.assign(static_cast<size_t>(num_channels),
                            std::vector<float>(delay_size, 0.0f));
  fir_delay_index_.assign(static_cast<size_t>(num_channels), 0);
}

void Crossover::rebuild_fir_kernels() {
  const size_t kernel_size = static_cast<size_t>(config_.fir_kernel_size);
  fir_kernels_.assign(config_.cutoffs_hz.size(), std::vector<float>(kernel_size, 0.0f));
  const int center = config_.fir_kernel_size / 2;
  for (size_t split_index = 0; split_index < config_.cutoffs_hz.size(); ++split_index) {
    const double cutoff =
        std::clamp(static_cast<double>(config_.cutoffs_hz[split_index]), 1.0, sample_rate_ * 0.49);
    const double normalized = cutoff / sample_rate_;
    double sum = 0.0;
    auto& kernel = fir_kernels_[split_index];
    for (int i = 0; i < config_.fir_kernel_size; ++i) {
      const int n = i - center;
      const double sinc =
          n == 0 ? 2.0 * normalized : std::sin(2.0 * kPiD * normalized * n) / (kPiD * n);
      const double window = hann_value(i, config_.fir_kernel_size);
      kernel[static_cast<size_t>(i)] = static_cast<float>(sinc * window);
      sum += kernel[static_cast<size_t>(i)];
    }
    if (std::abs(sum) > 1.0e-12) {
      for (auto& tap : kernel) {
        tap = static_cast<float>(tap / sum);
      }
    }
  }
}

float Crossover::process_fir_lowpass(float sample, int split_index, int channel) {
  auto& history = fir_history_[static_cast<size_t>(split_index)][static_cast<size_t>(channel)];
  auto& write_index =
      fir_history_index_[static_cast<size_t>(split_index)][static_cast<size_t>(channel)];
  history[write_index] = sample;
  double output = 0.0;
  const size_t size = history.size();
  const auto& kernel = fir_kernels_[static_cast<size_t>(split_index)];
  for (size_t tap = 0; tap < size; ++tap) {
    const size_t history_index = (write_index + size - tap) % size;
    output += static_cast<double>(kernel[tap]) * history[history_index];
  }
  write_index = (write_index + 1) % size;
  return static_cast<float>(output);
}

float Crossover::process_fir_delay(float sample, int channel) {
  auto& history = fir_delay_history_[static_cast<size_t>(channel)];
  auto& write_index = fir_delay_index_[static_cast<size_t>(channel)];
  history[write_index] = sample;
  const float output = history[(write_index + 1) % history.size()];
  write_index = (write_index + 1) % history.size();
  return output;
}

void Crossover::rebuild_state(int num_channels) {
  const size_t splits = config_.cutoffs_hz.size();
  const size_t bands = splits + 1;
  const size_t stages = filter_sections(config_.slope, config_.mode).size();
  const bool shape_matches =
      states_.size() == splits &&
      (splits == 0 || states_[0].size() == static_cast<size_t>(num_channels)) &&
      (splits == 0 || num_channels == 0 ||
       (states_[0][0].lowpass.size() == stages && states_[0][0].highpass.size() == stages)) &&
      compensation_states_.size() == bands &&
      (bands == 0 || compensation_states_[0].size() == static_cast<size_t>(num_channels)) &&
      (bands == 0 || num_channels == 0 ||
       compensation_states_[0][0].allpass_by_split.size() == splits);
  if (!shape_matches) {
    states_.assign(splits, std::vector<SplitChannelState>(static_cast<size_t>(num_channels)));
    for (auto& split_states : states_) {
      for (auto& channel_states : split_states) {
        channel_states.lowpass.assign(stages, Biquad{});
        channel_states.highpass.assign(stages, Biquad{});
      }
    }
    compensation_states_.assign(
        bands, std::vector<CompensationChannelState>(static_cast<size_t>(num_channels)));
    for (auto& band_states : compensation_states_) {
      for (auto& channel_states : band_states) {
        channel_states.allpass_by_split.assign(splits, std::vector<Biquad>(stages));
      }
    }
  }
  install_coefficients();
}

float Crossover::lowpass(float sample, int split_index, int channel) {
  float y = sample;
  auto& stages = states_[static_cast<size_t>(split_index)][static_cast<size_t>(channel)].lowpass;
  for (auto& stage : stages) {
    y = stage.process(y);
  }
  return y;
}

float Crossover::highpass(float sample, int split_index, int channel) {
  float y = sample;
  auto& stages = states_[static_cast<size_t>(split_index)][static_cast<size_t>(channel)].highpass;
  for (auto& stage : stages) {
    y = stage.process(y);
  }
  return is_lr2_linkwitz_riley(config_.slope, config_.mode) ? -y : y;
}

float Crossover::allpass(float sample, int band_index, int split_index, int channel) {
  float y = sample;
  auto& stages = compensation_states_[static_cast<size_t>(band_index)][static_cast<size_t>(channel)]
                     .allpass_by_split[static_cast<size_t>(split_index)];
  for (auto& stage : stages) {
    y = stage.process(y);
  }
  return y;
}

}  // namespace sonare::mastering::multiband
