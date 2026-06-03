/// @file chain_streaming.cpp
/// @brief Streaming implementation of the high-level mastering chain.

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/eq/tilt.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/tape.h"
#include "mastering/spectral/air_band.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
#include "rt/processor_base.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::api {
namespace {

// Reject the chain stages that fundamentally require whole-signal buffering and
// therefore cannot run block-by-block. Loudness is handled separately because
// it can be approximated with a precomputed static gain (see the
// options-taking constructor below).
void reject_non_streaming_repair(const MasteringChainConfig& config) {
  if (config.repair.declick.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain does not support repair.declick");
  }
  if (config.repair.declip.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain does not support repair.declip");
  }
  if (config.repair.decrackle.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain does not support repair.decrackle");
  }
  if (config.repair.dehum.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain does not support repair.dehum");
  }
  if (config.repair.dereverb.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain does not support repair.dereverb");
  }
  if (config.repair.denoise.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain does not support repair.denoise");
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// StreamingMasteringChain
// ---------------------------------------------------------------------------

struct StreamingMasteringChain::Impl {
  std::vector<std::unique_ptr<rt::ProcessorBase>> processors;
  // Optional final loudness limiter (gain + true-peak limit), present only when
  // the loudness stage is enabled and a precomputed static gain was supplied.
  // The static gain is applied in process_block() immediately before this
  // limiter, mirroring the offline chain's loudness stage (gain -> limiter).
  std::unique_ptr<rt::ProcessorBase> loudness_limiter;
};

StreamingMasteringChain::StreamingMasteringChain(MasteringChainConfig config)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)) {
  reject_non_streaming_repair(config_);
  if (config_.loudness.enabled) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "StreamingMasteringChain does not support loudness without a precomputed static gain "
        "(whole-signal LUFS required); construct with StreamingMasteringChainOptions and supply "
        "loudness_static_gain_db, e.g. target_lufs - measured_integrated_lufs");
  }
}

StreamingMasteringChain::StreamingMasteringChain(MasteringChainConfig config,
                                                 StreamingMasteringChainOptions options)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)) {
  reject_non_streaming_repair(config_);
  if (config_.loudness.enabled) {
    if (!std::isfinite(options.loudness_static_gain_db)) {
      throw SonareException(
          ErrorCode::InvalidParameter,
          "StreamingMasteringChain: loudness is enabled but loudness_static_gain_db is not finite; "
          "supply a precomputed static gain (e.g. target_lufs - measured_integrated_lufs)");
    }
    loudness_static_gain_linear_ = ::sonare::db_to_linear(options.loudness_static_gain_db);
  }
}

StreamingMasteringChain::~StreamingMasteringChain() = default;
StreamingMasteringChain::StreamingMasteringChain(StreamingMasteringChain&&) noexcept = default;
StreamingMasteringChain& StreamingMasteringChain::operator=(StreamingMasteringChain&&) noexcept =
    default;

void StreamingMasteringChain::prepare(double sample_rate, int max_block_size, int num_channels) {
  if (num_channels != 1 && num_channels != 2) {
    throw SonareException(ErrorCode::InvalidParameter, "num_channels must be 1 or 2");
  }
  if (max_block_size <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be > 0");
  }
  if (sample_rate <= 0.0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be > 0");
  }

  impl_->processors.clear();
  stage_names_.clear();

  auto add_stage = [&](std::unique_ptr<rt::ProcessorBase> proc, const char* name) {
    proc->prepare(sample_rate, max_block_size);
    impl_->processors.push_back(std::move(proc));
    stage_names_.emplace_back(name);
  };

  // 1. eq.tilt
  if (config_.eq.tilt.enabled) {
    auto tilt = std::make_unique<mastering::eq::TiltEq>();
    tilt->set_tilt_db(config_.eq.tilt.tilt_db);
    tilt->set_pivot_hz(config_.eq.tilt.pivot_hz);
    add_stage(std::move(tilt), "eq.tilt");
  }

  // 2. dynamics.deesser
  if (config_.dynamics.deesser.enabled) {
    add_stage(std::make_unique<mastering::dynamics::DeEsser>(config_.dynamics.deesser.config),
              "dynamics.deesser");
  }

  // 3. dynamics.transientShaper
  if (config_.dynamics.transient_shaper.enabled) {
    add_stage(std::make_unique<mastering::dynamics::TransientShaper>(
                  config_.dynamics.transient_shaper.config),
              "dynamics.transientShaper");
  }

  // 4. dynamics.compressor
  if (config_.dynamics.compressor.enabled) {
    add_stage(std::make_unique<mastering::dynamics::Compressor>(config_.dynamics.compressor.config),
              "dynamics.compressor");
  }

  // 5. dynamics.multibandComp
  if (config_.dynamics.multiband_comp.enabled) {
    add_stage(std::make_unique<mastering::multiband::MultibandCompressor>(
                  config_.dynamics.multiband_comp.config),
              "dynamics.multibandComp");
  }

  // 6. saturation.tape
  if (config_.saturation.tape.enabled) {
    add_stage(std::make_unique<mastering::saturation::Tape>(config_.saturation.tape.config),
              "saturation.tape");
  }

  // 7. saturation.exciter
  if (config_.saturation.exciter.enabled) {
    add_stage(std::make_unique<mastering::saturation::Exciter>(config_.saturation.exciter.config),
              "saturation.exciter");
  }

  // 8. spectral.airBand
  if (config_.spectral.air_band.enabled) {
    add_stage(std::make_unique<mastering::spectral::AirBand>(config_.spectral.air_band.config),
              "spectral.airBand");
  }

  // 9. stereo.imager (stereo only)
  if (num_channels == 2 && config_.stereo.imager.enabled) {
    add_stage(std::make_unique<mastering::stereo::Imager>(config_.stereo.imager.config),
              "stereo.imager");
  }

  // 10. stereo.monoMaker (stereo only)
  if (num_channels == 2 && config_.stereo.mono_maker.enabled) {
    add_stage(std::make_unique<mastering::stereo::MonoMaker>(config_.stereo.mono_maker.config),
              "stereo.monoMaker");
  }

  // 11. maximizer.truePeakLimiter
  if (config_.maximizer.true_peak_limiter.enabled) {
    add_stage(std::make_unique<mastering::maximizer::TruePeakLimiter>(
                  config_.maximizer.true_peak_limiter.config),
              "maximizer.truePeakLimiter");
  }

  // 12. loudness (precomputed static gain + dedicated true-peak limiter).
  // Built only when loudness is enabled (which requires a finite static gain,
  // enforced in the options constructor). The static gain itself is applied in
  // process_block(); here we prepare the matching final limiter that mirrors the
  // offline chain's loudness stage so the streaming preview's ceiling behaviour
  // matches the offline render.
  impl_->loudness_limiter.reset();
  if (config_.loudness.enabled) {
    mastering::maximizer::TruePeakLimiterConfig limiter_config;
    limiter_config.ceiling_db = config_.loudness.ceiling_db;
    limiter_config.oversample_factor = config_.loudness.true_peak_oversample;
    limiter_config.release_ms = config_.loudness.release_ms;
    limiter_config.apply_gain_at_input_rate = config_.loudness.apply_gain_at_input_rate;
    auto limiter = std::make_unique<mastering::maximizer::TruePeakLimiter>(limiter_config);
    limiter->prepare(sample_rate, max_block_size);
    impl_->loudness_limiter = std::move(limiter);
    stage_names_.emplace_back("loudness.optimize");
  }

  prepared_channels_ = num_channels;
  max_block_size_ = max_block_size;
}

void StreamingMasteringChain::process_block(float* const* channels, int num_channels,
                                            int num_samples) {
  if (prepared_channels_ == 0) {
    throw SonareException(ErrorCode::InvalidState,
                          "StreamingMasteringChain::process_block called before prepare()");
  }
  if (num_channels != prepared_channels_) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "StreamingMasteringChain::process_block num_channels mismatch with prepare()");
  }
  if (num_samples < 0 || num_samples > max_block_size_) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "StreamingMasteringChain::process_block num_samples exceeds max_block_size from prepare()");
  }
  if (num_samples == 0) {
    return;
  }
  for (auto& proc : impl_->processors) {
    proc->process(channels, num_channels, num_samples);
  }
  // Loudness stage: apply the precomputed static gain, then the dedicated
  // true-peak limiter (mirrors the offline chain's loudness stage order). The
  // gain is 1.0 (no-op) unless the chain was constructed with a finite
  // loudness_static_gain_db and the config enables loudness.
  if (impl_->loudness_limiter) {
    if (loudness_static_gain_linear_ != 1.0f) {
      for (int ch = 0; ch < num_channels; ++ch) {
        float* buffer = channels[ch];
        for (int i = 0; i < num_samples; ++i) buffer[i] *= loudness_static_gain_linear_;
      }
    }
    impl_->loudness_limiter->process(channels, num_channels, num_samples);
  }
}

void StreamingMasteringChain::reset() {
  for (auto& proc : impl_->processors) {
    proc->reset();
  }
  if (impl_->loudness_limiter) {
    impl_->loudness_limiter->reset();
  }
}

int StreamingMasteringChain::latency_samples() const noexcept {
  int total = 0;
  for (const auto& proc : impl_->processors) {
    total += proc->latency_samples();
  }
  if (impl_->loudness_limiter) {
    total += impl_->loudness_limiter->latency_samples();
  }
  return total;
}

}  // namespace sonare::mastering::api
