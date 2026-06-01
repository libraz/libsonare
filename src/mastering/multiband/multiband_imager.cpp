#include "mastering/multiband/multiband_imager.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "mastering/common/scoped_no_denormals.h"
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
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  crossover_.prepare(sample_rate_, max_block_size_);
  // Pre-size split scratch for stereo so the steady-state audio path is
  // allocation-free; it is grown on demand only if a wider block arrives.
  crossover_.prepare_scratch(scratch_, 2, max_block_size_);
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
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandImager");
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    }
  }

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
      auto& stages = allpass_[static_cast<size_t>(band)];
      for (int i = 0; i < num_samples; ++i) {
        const size_t index = static_cast<size_t>(i);
        const float mid = 0.5f * (left[index] + right[index]);
        const float input_side = 0.5f * (left[index] - right[index]);
        const float original_energy = mid * mid + input_side * input_side;
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
        float out_mid = mid;
        // Preserve total M/S energy for both widening and narrowing so the
        // perceived loudness stays consistent across width settings.
        if (band_config.preserve_energy && band_config.width != 1.0f) {
          const float adjusted_energy = out_mid * out_mid + side * side;
          if (adjusted_energy > 0.0f && original_energy > 0.0f) {
            const float scale = std::sqrt(original_energy / adjusted_energy);
            out_mid *= scale;
            side *= scale;
          }
        }
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
  config_ = config;
  crossover_.set_config(config_.crossover);
  if (prepared_) {
    prepare(sample_rate_, max_block_size_);
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

void MultibandImager::validate_config(const MultibandImagerConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "multiband imager band count must match crossover");
  }
  for (const auto& band : config.bands) {
    if (band.width < 0.0f || band.decorrelation_amount < 0.0f || band.decorrelation_amount > 1.0f) {
      throw SonareException(ErrorCode::InvalidParameter, "imager width must be non-negative");
    }
  }
}

}  // namespace sonare::mastering::multiband
