#include "mastering/dynamics/limiter.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::mastering::dynamics {

Limiter::Limiter(LimiterConfig config)
    : config_(config), config_publisher_(std::make_unique<rt::RtPublisher<LimiterConfig>>()) {
  validate_config(config_);
  // Seed the publisher so a downstream audio thread that starts before
  // prepare() sees a defined snapshot. prepare() will publish again with
  // post-prepare derived state already applied so the first audio block does
  // not redundantly recompute coefficients.
  config_publisher_->publish(std::make_shared<const LimiterConfig>(config_));
}

void Limiter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  lookahead_samples_ = static_cast<int>(std::round(sample_rate_ * config_.lookahead_ms * 0.001));
  update_coefficients(config_);
  prepared_ = true;
  // Preallocate per-channel lookahead and scratch up front so the audio-thread
  // process() path never resizes (which would malloc). A block requesting more
  // than kRealtimePreparedChannels channels throws in prepare_buffers().
  lookahead_.assign(kRealtimePreparedChannels, {});
  for (auto& buffer : lookahead_) {
    buffer.prepare(static_cast<size_t>(std::max(lookahead_samples_, 0)));
  }
  delayed_.assign(kRealtimePreparedChannels, 0.0f);
  gain_smoother_.prepare(sample_rate_, 0.0f, config_.release_ms);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  auto fresh = std::make_shared<const LimiterConfig>(config_);
  applied_snapshot_ = fresh.get();
  config_publisher_->publish(std::move(fresh));
  config_publisher_->acquire();
}

const LimiterConfig* Limiter::adopt_snapshot_for_block() noexcept {
  // Audio-thread entry. acquire() drains the publish ring to the newest
  // snapshot and retires superseded ones via the wait-free retire ring (no
  // alloc, no free, no lock on this thread). If a new snapshot was adopted,
  // re-derive the scalar coefficients — those writes target members the per-
  // sample loop reads, but the loop has not started yet for this block, so no
  // race.
  config_publisher_->acquire();
  const LimiterConfig* current = config_publisher_->current();
  if (current && current != applied_snapshot_) {
    update_coefficients(*current);
    applied_snapshot_ = current;
  }
  // Fallback path: only reachable if the constructor's initial publish was
  // dropped (ring full, which cannot happen on a fresh publisher) AND prepare
  // was never called. In that case use the control-thread mirror; the per-
  // sample loop is itself guarded by prepared_ so this path stays defined.
  return current ? current : &config_;
}

void Limiter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Limiter");
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

  prepare_buffers(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    }
  }

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const LimiterConfig& cfg = *adopt_snapshot_for_block();

  const float ceiling = db_to_linear(cfg.threshold_db);
  float min_gain = 1.0f;
  // Reuse the preallocated scratch (sized in prepare()) instead of allocating a
  // fresh vector each block; only the first num_channels entries are read.
  std::fill(delayed_.begin(), delayed_.begin() + num_channels, 0.0f);
  for (int i = 0; i < num_samples; ++i) {
    float peak = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      delayed_[static_cast<size_t>(ch)] =
          lookahead_[static_cast<size_t>(ch)].process(channels[ch][i]);
      peak = std::max(peak, lookahead_[static_cast<size_t>(ch)].peak());
    }

    const float target_gain = peak > ceiling && peak > 0.0f ? ceiling / peak : 1.0f;
    // Smooth the linked target once, then apply the same gain to every channel
    // so the stereo image is preserved.
    const float gain = gain_smoother_.smooth_bidirectional(target_gain, release_coeff_, true);
    for (int ch = 0; ch < num_channels; ++ch) {
      channels[ch][i] = delayed_[static_cast<size_t>(ch)] * gain;
    }
    min_gain = std::min(min_gain, gain);
  }

  last_gain_reduction_db_ = std::min(0.0f, linear_to_db(min_gain));
}

void Limiter::reset() {
  for (auto& buffer : lookahead_) {
    buffer.reset();
  }
  gain_smoother_.reset(1.0f);
  last_gain_reduction_db_ = 0.0f;
}

void Limiter::set_config(const LimiterConfig& config) {
  // Control-thread side: validate before publishing so any throw leaves both
  // the control-thread mirror (config_) and the audio-thread snapshot
  // unchanged. The audio thread sees the new snapshot only after publish()
  // succeeds; validation never runs partway through a config_ write that the
  // audio thread could observe. Note: changing lookahead_ms does NOT resize
  // the lookahead buffers from this call (that would require allocation); the
  // buffer size is fixed at prepare() time.
  validate_config(config);
  config_ = config;
  config_publisher_->publish(std::make_shared<const LimiterConfig>(config_));
}

void Limiter::set_release_ms(float release_ms) {
  if (release_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "limiter release must be non-negative");
  }
  config_.release_ms = release_ms;
  // Re-publish so the audio thread picks up the new release time via the
  // standard snapshot path (keeps a single source of truth for derived state).
  config_publisher_->publish(std::make_shared<const LimiterConfig>(config_));
}

bool Limiter::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.threshold_db = value;
      break;
    case 1:
      config_.release_ms = std::max(0.0f, value);
      break;
    default:
      return false;
  }
  // Publish the mutated config_ as a new snapshot so the audio thread adopts
  // it (and re-derives release_coeff_ via update_coefficients) on the next
  // block. set_parameter is therefore NOT realtime-safe under the snapshot
  // model — the shared_ptr allocation matches set_config().
  config_publisher_->publish(std::make_shared<const LimiterConfig>(config_));
  return true;
}

void Limiter::validate_config(const LimiterConfig& config) {
  if (config.lookahead_ms < 0.0f || config.release_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "limiter timing values must be non-negative");
  }
}

void Limiter::prepare_buffers(int num_channels) {
  // The lookahead buffers and delayed_ scratch are preallocated to
  // kRealtimePreparedChannels in prepare(). Never resize on the audio thread;
  // reject blocks wider than the prepared state (matches the established
  // dynamics-processor pattern).
  if (lookahead_.size() >= static_cast<size_t>(num_channels)) {
    return;
  }

  throw SonareException(ErrorCode::InvalidParameter, "num_channels exceeds prepared Limiter state");
}

void Limiter::update_coefficients(const LimiterConfig& config) {
  release_coeff_ = time_to_coefficient(sample_rate_, config.release_ms);
}

}  // namespace sonare::mastering::dynamics
