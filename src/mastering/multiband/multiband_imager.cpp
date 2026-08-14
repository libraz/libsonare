#include "mastering/multiband/multiband_imager.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "mastering/dynamics/channel_limits.h"
#include "mastering/stereo/constant_power_width.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mastering::multiband {

constexpr float MultibandImager::kDecorrelationFrequenciesHz[];

float MultibandImager::allpass_coefficient(float frequency_hz, double sample_rate) noexcept {
  // First-order all-pass break-frequency coefficient via the bilinear transform:
  //   c = (tan(pi*fc/fs) - 1) / (tan(pi*fc/fs) + 1)
  // matched to the difference equation y = -c*x + x1 + c*y1. The cutoff is
  // clamped below Nyquist so high target frequencies stay well-defined at low
  // sample rates.
  const double fc = std::clamp(static_cast<double>(frequency_hz), 1.0, sample_rate * 0.49);
  const double t = std::tan(sonare::constants::kPiD * fc / sample_rate);
  return static_cast<float>((t - 1.0) / (t + 1.0));
}

float MultibandImager::Allpass::process(float input) noexcept {
  const float output = -coefficient * input + x1 + coefficient * y1;
  x1 = input;
  y1 = output;
  return output;
}

void MultibandImager::Allpass::reset() noexcept {
  x1 = 0.0f;
  y1 = 0.0f;
}

MultibandImager::MultibandImager(MultibandImagerConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
}

void MultibandImager::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void MultibandImager::prepare(double sample_rate, int max_block_size, int max_channels) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  if (max_channels < 1 || max_channels > static_cast<int>(dynamics::kRealtimePreparedChannels)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "max_channels exceeds MultibandImager capacity");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  max_working_channels_ = max_channels;
  prepared_ = true;
  crossover_.prepare(sample_rate_, max_block_size_, max_working_channels_);
  crossover_.prepare_scratch(scratch_, max_working_channels_, max_block_size_);
  allpass_.resize(config_.bands.size());
  for (auto& band_stages : allpass_) {
    for (int stage = 0; stage < kNumAllpassStages; ++stage) {
      band_stages[static_cast<size_t>(stage)].coefficient =
          allpass_coefficient(kDecorrelationFrequenciesHz[stage], sample_rate_);
    }
  }
  reset();
}

void MultibandImager::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandImager");
  if (!validate_block_size(num_channels, num_samples)) {
    return;
  }
  if (num_channels > max_working_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared MultibandImager capacity");
  }
  if (num_samples > max_block_size_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared MultibandImager block size");
  }
  validate_channel_buffers(channels, num_channels);

  crossover_.ensure_scratch(scratch_, num_channels, num_samples);
  crossover_.split_into(channels, num_channels, num_samples, scratch_);
  const int num_bands = scratch_.num_bands();
  if (num_channels >= 2) {
    for (int band = 0; band < num_bands; ++band) {
      const auto& band_config = config_.bands[static_cast<size_t>(band)];
      if (!band_config.enabled || band_config.width == 1.0f) {
        continue;
      }

      auto& left = scratch_.bands[static_cast<size_t>(band)][0];
      auto& right = scratch_.bands[static_cast<size_t>(band)][1];
      // The decorrelation allpass only contributes when widening (width > 1)
      // with a non-zero amount; otherwise running it would waste CPU and could
      // subtly alter the signal, so skip it entirely.
      const bool use_decorrelation =
          band_config.decorrelation_amount > 0.0f && band_config.width > 1.0f;
      const float energy_scale =
          band_config.preserve_energy
              ? sonare::mastering::stereo::constant_power_width_gain(band_config.width)
              : 1.0f;
      auto& stages = allpass_[static_cast<size_t>(band)];
      for (int i = 0; i < num_samples; ++i) {
        const size_t index = static_cast<size_t>(i);
        const float mid = 0.5f * (left[index] + right[index]);
        const float input_side = 0.5f * (left[index] - right[index]);
        float side = input_side * band_config.width;
        if (use_decorrelation) {
          float decorated_side = input_side;
          for (auto& stage : stages) {
            decorated_side = stage.process(decorated_side);
          }
          const float extra_width = std::min(band_config.width - 1.0f, 1.0f);
          const float mix = band_config.decorrelation_amount * extra_width;
          side = (1.0f - mix) * side + mix * decorated_side * band_config.width;
        }
        // Match the full-band imager's signal-independent constant-power
        // compensation. A per-sample energy ratio is a nonlinear modulator.
        const float out_mid = mid * energy_scale;
        side *= energy_scale;
        left[index] = out_mid + side;
        right[index] = out_mid - side;
      }
    }
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
    for (int band = 0; band < num_bands; ++band) {
      const auto& band_samples = scratch_.bands[static_cast<size_t>(band)][static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] += band_samples[static_cast<size_t>(i)];
      }
    }
  }
}

void MultibandImager::reset() {
  crossover_.reset();
  for (auto& band_stages : allpass_) {
    for (auto& stage : band_stages) {
      stage.reset();
    }
  }
}

void MultibandImager::set_config(const MultibandImagerConfig& config) {
  validate_config(config);
  // Only reconfigure/re-prepare the crossover when its parameters actually
  // change; rebuilding it zeroes the crossover filter state and would click on
  // band-parameter-only updates. The allpass decorrelation state is rebuilt and
  // reset on every set_config (it was before this change too).
  const bool crossover_changed = config.crossover != config_.crossover;
  config_ = config;
  if (prepared_ && crossover_changed) {
    // Re-prepare (which rebuilds crossover state and the allpass stages) only
    // when the crossover layout changed, e.g. the band count.
    prepare(sample_rate_, max_block_size_, max_working_channels_);
  }
}

bool MultibandImager::set_parameter(unsigned int param_id, float value) {
  const size_t band = param_id / kBandStride;
  if (band >= config_.bands.size()) {
    return false;
  }
  auto& band_config = config_.bands[band];
  switch (param_id % kBandStride) {
    case 0:
      band_config.width = std::max(0.0f, value);
      return true;
    case 1:
      band_config.decorrelation_amount = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> MultibandImager::parameter_descriptors() const {
  // Mirrors set_parameter exactly: id = band * kBandStride + band_param, valid
  // for every band that exists (band < config_.bands.size()) and band_param in
  // [0, kBandStride). Keys use the construction-time band{i}.<field> convention.
  static constexpr const char* kBandParamKeys[kBandStride] = {"width", "decorrelationAmount"};
  std::vector<rt::ParamDescriptor> descriptors;
  descriptors.reserve(config_.bands.size() * kBandStride);
  for (unsigned int band = 0; band < config_.bands.size(); ++band) {
    const std::string prefix = "band" + std::to_string(band) + ".";
    for (unsigned int band_param = 0; band_param < kBandStride; ++band_param) {
      descriptors.push_back({prefix + kBandParamKeys[band_param], band * kBandStride + band_param});
    }
  }
  return descriptors;
}

void MultibandImager::validate_config(const MultibandImagerConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "multiband imager band count must match crossover");
  }
  for (const auto& band : config.bands) {
    if (!std::isfinite(band.width) || band.width < 0.0f || band.decorrelation_amount < 0.0f ||
        band.decorrelation_amount > 1.0f) {
      throw SonareException(ErrorCode::InvalidParameter, "imager width must be non-negative");
    }
  }
}

}  // namespace sonare::mastering::multiband
