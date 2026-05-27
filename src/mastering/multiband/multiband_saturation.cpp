#include "mastering/multiband/multiband_saturation.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "mastering/common/scoped_no_denormals.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/soft_clipper.h"
#include "mastering/saturation/tape.h"
#include "mastering/saturation/tube.h"
#include "util/db.h"

namespace sonare::mastering::multiband {

namespace {

std::unique_ptr<common::ProcessorBase> make_processor(const SaturationBandConfig& band) {
  // Map the shared band config onto each algorithm's native parameters. drive_db
  // and mix carry over where the algorithm supports them; output_gain_db is
  // applied uniformly by the multiband loop so it works for every type.
  switch (band.type) {
    case SaturationType::Tape: {
      saturation::TapeConfig cfg;
      cfg.drive_db = band.drive_db;
      cfg.output_gain_db = 0.0f;
      return std::make_unique<saturation::Tape>(cfg);
    }
    case SaturationType::Tube: {
      saturation::TubeConfig cfg;
      cfg.drive_db = band.drive_db;
      cfg.mix = std::clamp(band.mix, 0.0f, 1.0f);
      return std::make_unique<saturation::Tube>(cfg);
    }
    case SaturationType::Exciter: {
      saturation::ExciterConfig cfg;
      cfg.drive_db = band.drive_db;
      cfg.amount = std::clamp(band.mix, 0.0f, 1.0f);
      return std::make_unique<saturation::Exciter>(cfg);
    }
    case SaturationType::SoftClip:
      break;
  }
  saturation::SoftClipperConfig cfg;
  cfg.drive_db = band.drive_db;
  cfg.mix = std::clamp(band.mix, 0.0f, 1.0f);
  return std::make_unique<saturation::SoftClipper>(cfg);
}

}  // namespace

MultibandSaturation::MultibandSaturation(MultibandSaturationConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

MultibandSaturation::~MultibandSaturation() = default;

void MultibandSaturation::prepare(double sample_rate, int max_block_size) {
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
  for (auto& processor : processors_) {
    processor->prepare(sample_rate_, max_block_size_);
  }
  reset();
}

void MultibandSaturation::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  if (!prepared_) {
    throw std::logic_error("MultibandSaturation must be prepared before processing");
  }
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
    const auto& band_config = config_.bands[static_cast<size_t>(band)];
    if (!band_config.enabled) {
      continue;
    }
    processors_[static_cast<size_t>(band)]->process(
        scratch_.band_channels[static_cast<size_t>(band)].data(), num_channels, num_samples);
    const float output_gain = db_to_linear(band_config.output_gain_db);
    if (output_gain != 1.0f) {
      for (int ch = 0; ch < num_channels; ++ch) {
        auto& band_samples = scratch_.bands[static_cast<size_t>(band)][static_cast<size_t>(ch)];
        for (int i = 0; i < num_samples; ++i) {
          band_samples[static_cast<size_t>(i)] *= output_gain;
        }
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

void MultibandSaturation::reset() {
  crossover_.reset();
  for (auto& processor : processors_) {
    processor->reset();
  }
}

void MultibandSaturation::set_config(const MultibandSaturationConfig& config) {
  validate_config(config);
  config_ = config;
  crossover_.set_config(config_.crossover);
  rebuild_processors();
  if (prepared_) {
    prepare(sample_rate_, max_block_size_);
  }
}

bool MultibandSaturation::set_parameter(unsigned int param_id, float value) {
  const size_t band = param_id / kBandStride;
  if (band >= config_.bands.size()) {
    return false;
  }
  auto& band_config = config_.bands[band];
  auto& processor = *processors_[band];
  switch (param_id % kBandStride) {
    case 0:
      band_config.drive_db = value;
      // drive_db is parameter 0 on every algorithm except the exciter, whose
      // drive is parameter 1 (parameter 0 is its band-pass frequency).
      return processor.set_parameter(band_config.type == SaturationType::Exciter ? 1u : 0u, value);
    case 1: {
      band_config.mix = std::clamp(value, 0.0f, 1.0f);
      switch (band_config.type) {
        case SaturationType::SoftClip:
          return processor.set_parameter(1u, band_config.mix);
        case SaturationType::Tube:
          return processor.set_parameter(2u, band_config.mix);
        case SaturationType::Exciter:
          return processor.set_parameter(2u, band_config.mix);
        case SaturationType::Tape:
          // The tape model has no wet/dry mix parameter; only config_ is kept
          // in sync so config() reflects the requested value.
          return true;
      }
      return true;
    }
    case 2:
      // output_gain_db is applied by the multiband loop, not the sub-processor.
      band_config.output_gain_db = value;
      return true;
    default:
      return false;
  }
}

void MultibandSaturation::validate_config(const MultibandSaturationConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw std::invalid_argument("multiband saturation band count must match crossover");
  }
  for (const auto& band : config.bands) {
    if (band.mix < 0.0f || band.mix > 1.0f) {
      throw std::invalid_argument("saturation mix must be in [0, 1]");
    }
  }
}

void MultibandSaturation::rebuild_processors() {
  processors_.clear();
  processors_.reserve(config_.bands.size());
  for (const auto& band_config : config_.bands) {
    processors_.push_back(make_processor(band_config));
  }
}

}  // namespace sonare::mastering::multiband
