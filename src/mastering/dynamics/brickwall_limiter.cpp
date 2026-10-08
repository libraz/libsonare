#include "mastering/dynamics/brickwall_limiter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "mastering/common/parameter_domain.h"
#include "mastering/dynamics/lookahead_validation.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/non_finite_sample.h"

namespace sonare::mastering::dynamics {

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
BrickwallLimiter::BrickwallLimiter(BrickwallLimiterConfig config) : ConfigBase(std::move(config)) {}

void BrickwallLimiter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

  (void)checked_lookahead_samples(sample_rate, config_.lookahead_ms);
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  // Inner limiter owns the lookahead buffer sizing. lookahead_ms changes
  // require buffer resize and are routed through prepare() (the non-RT-safe
  // control-thread path); the RT-safe snapshot path only forwards ceiling and
  // release updates via update_coefficients().
  limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, config_.release_ms});
  limiter_.prepare(sample_rate_, max_block_size_);
  // Seed the audio thread's live working config from the prepared baseline; the
  // hard-clip stage reads active_.ceiling_db, not config_.
  active_ = config_;
  prepared_ = true;
  non_finite_substitution_count_.reset();
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  republish_after_prepare();
}

void BrickwallLimiter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "BrickwallLimiter");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const BrickwallLimiterConfig& cfg = *adopt_snapshot_for_block();

  // Before the detector reads the block, not after: a non-finite sample left in
  // place drives the linked gain to zero, and every statistic below would then
  // describe a reduction this stage never performed.
  std::uint32_t substituted = 0;
  for (int ch = 0; ch < num_channels; ++ch) {
    substituted += static_cast<std::uint32_t>(
        resolve_non_finite_run(SampleDestination::kIrreversibleOutput, channels[ch],
                               static_cast<std::size_t>(num_samples)));
  }

  // The recursive cells belong to the inner limiter, so the discard this
  // processor publishes is its member's. Read as a difference across the block,
  // never summed: the base counts blocks, and a member may run more than once.
  const std::uint32_t inner_discards_before = limiter_.non_finite_discard_count();
  limiter_.run(detector_excluded_channel(num_channels), channels, num_channels, num_samples);

  const float ceiling = db_to_linear(cfg.ceiling_db);
  // Every plane is held under the ceiling, but the excluded one (an LFE) is not
  // part of the program the reported reduction describes.
  const int excluded = detector_excluded_channel(num_channels);
  float min_sample_gain = 1.0f;
  hard_clip_count_ = 0;
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      // The inner limiter can still emit a non-finite from a coefficient a
      // previous block poisoned, and this buffer is the caller's. Silence is
      // never over the ceiling, so it reaches the clip test as a no-op rather
      // than as a clip.
      if (resolve_non_finite(SampleDestination::kIrreversibleOutput, channels[ch][i])) {
        ++substituted;
      }
      const float abs_sample = std::abs(channels[ch][i]);
      if (abs_sample > ceiling && abs_sample > 0.0f) {
        const float gain = ceiling / abs_sample;
        channels[ch][i] *= gain;
        if (ch != excluded) min_sample_gain = std::min(min_sample_gain, gain);
        ++hard_clip_count_;
      }
    }
  }
  // Once per block, not per sample: nothing downstream reads the count mid-block.
  non_finite_substitution_count_.add(substituted);
  if (limiter_.non_finite_discard_count() != inner_discards_before) note_non_finite_discard();

  last_gain_reduction_db_ =
      std::min(limiter_.last_gain_reduction_db(), linear_to_db(min_sample_gain));
}

void BrickwallLimiter::reset() {
  limiter_.reset();
  last_gain_reduction_db_ = 0.0f;
  hard_clip_count_ = 0;
}

void BrickwallLimiter::set_config(const BrickwallLimiterConfig& config) {
  // Control-thread side: validate before mutating any state so any throw
  // leaves both the control-thread mirror (config_) and the audio-thread
  // snapshot unchanged.
  validate_config(config);
  if (prepared_) (void)checked_lookahead_samples(sample_rate_, config.lookahead_ms);
  rt::apply_config_diff(config_, config,
                        [this](const rt::ConfigDiff<BrickwallLimiterConfig>& diff) {
                          if (prepared_ && diff.changed(&BrickwallLimiterConfig::lookahead_ms)) {
                            // A lookahead change resizes the inner limiter's ring buffers, which is
                            // not RT-safe and MUST NOT race with process(). prepare() publishes the
                            // snapshot itself.
                            prepare(sample_rate_, max_block_size_);
                            return;
                          }
                          // Ceiling and release reach the audio thread through the snapshot, whose
                          // adoption sets both on the inner limiter together.
                          publish_current_config();
                        });
}

void BrickwallLimiter::set_release_ms(float release_ms) {
  if (!std::isfinite(release_ms) || release_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "brickwall limiter release must be non-negative");
  }
  // Through the outer snapshot, never the inner limiter's own: that one still
  // holds the ceiling from the last prepare and would put it back.
  BrickwallLimiterConfig next = config_;
  next.release_ms = release_ms;
  set_config(next);
}

void BrickwallLimiter::set_release_ms_in_place(float release_ms) noexcept {
  // RT-safe: forward to the inner limiter's in-place setter (scalar coefficient
  // recompute, no publish, no allocation). config_ and the published snapshot
  // are intentionally left unchanged.
  limiter_.set_release_ms_in_place(release_ms);
}

bool BrickwallLimiter::apply_parameter(BrickwallLimiterConfig& config, unsigned int param_id,
                                       float value) {
  switch (param_id) {
    case 0:
      config.ceiling_db = value;
      break;
    case 1:
      config.release_ms = std::max(0.0f, value);
      break;
    default:
      return false;
  }
  return true;
}

std::vector<rt::ParamDescriptor> BrickwallLimiter::parameter_descriptors() const {
  return {{"ceilingDb", 0}, {"releaseMs", 1}};
}

void BrickwallLimiter::validate_config(const BrickwallLimiterConfig& config) {
  if (!common::valid_ceiling_db(config.ceiling_db) || !std::isfinite(config.lookahead_ms) ||
      !std::isfinite(config.release_ms) || config.lookahead_ms < 0.0f || config.release_ms < 0.0f) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "brickwall limiter ceiling must be finite and <= 0, timing values finite and non-negative");
  }
}

void BrickwallLimiter::update_coefficients(const BrickwallLimiterConfig& config) {
  // RT-safe: forward ceiling and release to the inner limiter via its in-place
  // setters, which update scalar coefficients without publishing a snapshot
  // (no shared_ptr allocation) and without resizing the lookahead buffers.
  // Called from the audio thread on snapshot adoption, so it must not allocate.
  limiter_.set_threshold_in_place(config.ceiling_db);
  limiter_.set_release_ms_in_place(config.release_ms);
}

}  // namespace sonare::mastering::dynamics
