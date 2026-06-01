/// @file chain_streaming.cpp
/// @brief Streaming implementation of the high-level mastering chain.

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
#include "util/exception.h"

namespace sonare::mastering::api {

// ---------------------------------------------------------------------------
// StreamingMasteringChain
// ---------------------------------------------------------------------------

struct StreamingMasteringChain::Impl {
  std::vector<std::unique_ptr<rt::ProcessorBase>> processors;
};

StreamingMasteringChain::StreamingMasteringChain(MasteringChainConfig config)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)) {
  if (config_.repair.declick.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain does not support repair.declick");
  }
  if (config_.repair.declip.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain does not support repair.declip");
  }
  if (config_.repair.decrackle.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain does not support repair.decrackle");
  }
  if (config_.repair.dehum.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain does not support repair.dehum");
  }
  if (config_.repair.dereverb.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain does not support repair.dereverb");
  }
  if (config_.repair.denoise.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain does not support repair.denoise");
  }
  if (config_.loudness.enabled) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "StreamingMasteringChain does not support loudness (whole-signal LUFS required)");
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
}

void StreamingMasteringChain::reset() {
  for (auto& proc : impl_->processors) {
    proc->reset();
  }
}

int StreamingMasteringChain::latency_samples() const noexcept {
  int total = 0;
  for (const auto& proc : impl_->processors) {
    total += proc->latency_samples();
  }
  return total;
}

}  // namespace sonare::mastering::api
