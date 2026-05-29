#include "mastering/multiband/multiband_dynamic_eq.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "mastering/common/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::mastering::multiband {

MultibandDynamicEq::MultibandDynamicEq(MultibandDynamicEqConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

void MultibandDynamicEq::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  crossover_.prepare(sample_rate_, max_block_size_);
  // Pre-size split scratch for stereo so the steady-state audio path is
  // allocation-free; it is grown on demand only if a wider block arrives.
  crossover_.prepare_scratch(scratch_, 2, max_block_size_);
  for (size_t band = 0; band < processors_.size(); ++band) {
    processors_[band].prepare(sample_rate_, max_block_size_);
    configure_processor(band);
  }
  reset();
}

void MultibandDynamicEq::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandDynamicEq");
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }

  crossover_.ensure_scratch(scratch_, num_channels, num_samples);
  crossover_.split_into(channels, num_channels, num_samples, scratch_);
  const int num_bands = scratch_.num_bands();
  for (int band = 0; band < num_bands; ++band) {
    auto& processor = processors_[static_cast<size_t>(band)];
    processor.process(scratch_.band_channels[static_cast<size_t>(band)].data(), num_channels,
                      num_samples);
    last_detector_db_[static_cast<size_t>(band)] = processor.last_detector_db();
    auto& gains = last_applied_gain_db_[static_cast<size_t>(band)];
    std::fill(gains.begin(), gains.end(), 0.0f);
    for (size_t i = 0; i < config_.bands[static_cast<size_t>(band)].size(); ++i) {
      gains[i] = processor.last_applied_gain_db(i);
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

void MultibandDynamicEq::reset() {
  crossover_.reset();
  for (auto& processor : processors_) {
    processor.reset();
  }
  std::fill(last_detector_db_.begin(), last_detector_db_.end(), sonare::constants::kFloorDb);
  for (auto& gains : last_applied_gain_db_) {
    std::fill(gains.begin(), gains.end(), 0.0f);
  }
}

void MultibandDynamicEq::set_config(const MultibandDynamicEqConfig& config) {
  validate_config(config);
  config_ = config;
  crossover_.set_config(config_.crossover);
  rebuild_processors();
  if (prepared_) {
    prepare(sample_rate_, max_block_size_);
  }
}

bool MultibandDynamicEq::set_parameter(unsigned int param_id, float value) {
  const unsigned int crossover_band = param_id / kParamsPerCrossoverBand;
  if (crossover_band >= processors_.size()) {
    return false;
  }
  const unsigned int local_id = param_id % kParamsPerCrossoverBand;
  return processors_[crossover_band].set_parameter(local_id, value);
}

void MultibandDynamicEq::validate_config(const MultibandDynamicEqConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw std::invalid_argument("multiband dynamic EQ band count must match crossover");
  }
  for (const auto& band : config.bands) {
    if (band.size() > eq::DynamicEq::kMaxBands) {
      throw std::invalid_argument("too many dynamic EQ bands");
    }
  }
}

void MultibandDynamicEq::rebuild_processors() {
  processors_.assign(config_.bands.size(), {});
  last_detector_db_.assign(config_.bands.size(), sonare::constants::kFloorDb);
  last_applied_gain_db_.assign(config_.bands.size(),
                               std::vector<float>(eq::DynamicEq::kMaxBands, 0.0f));
}

void MultibandDynamicEq::configure_processor(size_t band_index) {
  auto& processor = processors_[band_index];
  processor.clear();
  const auto& bands = config_.bands[band_index];
  for (size_t i = 0; i < bands.size(); ++i) {
    processor.set_band(i, bands[i]);
  }
}

}  // namespace sonare::mastering::multiband
