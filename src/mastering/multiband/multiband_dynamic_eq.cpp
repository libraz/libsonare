#include "mastering/multiband/multiband_dynamic_eq.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mastering::multiband {

MultibandDynamicEq::MultibandDynamicEq(MultibandDynamicEqConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

void MultibandDynamicEq::prepare(double sample_rate, int max_block_size) {
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
  for (size_t band = 0; band < processors_.size(); ++band) {
    processors_[band].prepare(sample_rate_, max_block_size_);
    configure_processor(band);
  }
  reset();
}

void MultibandDynamicEq::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandDynamicEq");
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
  // Only reconfigure/re-prepare the crossover when its parameters actually
  // change; rebuilding it zeroes the crossover filter state and would click on
  // band-parameter-only updates. Sub-processors are always rebuilt, prepared and
  // reconfigured.
  const bool crossover_changed = config.crossover != config_.crossover;
  config_ = config;
  rebuild_processors();
  if (prepared_) {
    if (crossover_changed) {
      crossover_.set_config(config_.crossover);
      crossover_.prepare_scratch(scratch_, 2, max_block_size_);
    }
    for (size_t band = 0; band < processors_.size(); ++band) {
      processors_[band].prepare(sample_rate_, max_block_size_);
      configure_processor(band);
    }
  }
}

bool MultibandDynamicEq::set_parameter(unsigned int param_id, float value) {
  const unsigned int crossover_band = param_id / kParamsPerCrossoverBand;
  if (crossover_band >= processors_.size()) {
    return false;
  }
  const unsigned int local_id = param_id % kParamsPerCrossoverBand;
  if (!processors_[crossover_band].set_parameter(local_id, value)) {
    return false;
  }
  // Keep config_ in sync so config() and a subsequent set_config() observe the
  // automated value (mirrors MultibandCompressor::set_parameter). Only the band
  // the local id addresses changed; copy it back when it is within the stored
  // config (the inner DynamicEq keeps up to kMaxBands slots, config may hold
  // fewer).
  const size_t dyn_band = local_id / eq::DynamicEq::kParamsPerBand;
  auto& bands = config_.bands[crossover_band];
  if (dyn_band < bands.size()) {
    bands[dyn_band] = processors_[crossover_band].band(dyn_band);
  }
  return true;
}

std::vector<rt::ParamDescriptor> MultibandDynamicEq::parameter_descriptors() const {
  // Mirror set_parameter's id layout: crossover band `cb` occupies a block of
  // kParamsPerCrossoverBand ids, within which the DynamicEq per-band layout
  // applies (kParamsPerBand fields per dynamic band; see DynamicEq::set_parameter
  // for the field order). The keys match the construction-time convention
  // band{cb}.dyn{db}.<field> read by populate_dynamic_eq_bands.
  static constexpr std::array<const char*, eq::DynamicEq::kParamsPerBand> kFieldKeys{
      "frequencyHz", "staticGainDb",    "q",        "thresholdDb", "ratio",      "rangeDb",
      "sidechainQ",  "sidechainFreqHz", "attackMs", "releaseMs",   "lookaheadMs"};
  std::vector<rt::ParamDescriptor> descriptors;
  descriptors.reserve(processors_.size() * kParamsPerCrossoverBand);
  for (unsigned int cb = 0; cb < processors_.size(); ++cb) {
    const unsigned int block = cb * kParamsPerCrossoverBand;
    for (unsigned int db = 0; db < eq::DynamicEq::kMaxBands; ++db) {
      const std::string prefix = "band" + std::to_string(cb) + ".dyn" + std::to_string(db) + ".";
      const unsigned int band_offset = db * eq::DynamicEq::kParamsPerBand;
      for (unsigned int field = 0; field < eq::DynamicEq::kParamsPerBand; ++field) {
        descriptors.push_back({prefix + kFieldKeys[field], block + band_offset + field});
      }
    }
  }
  return descriptors;
}

void MultibandDynamicEq::validate_config(const MultibandDynamicEqConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "multiband dynamic EQ band count must match crossover");
  }
  for (const auto& band : config.bands) {
    if (band.size() > eq::DynamicEq::kMaxBands) {
      throw SonareException(ErrorCode::InvalidParameter, "too many dynamic EQ bands");
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
