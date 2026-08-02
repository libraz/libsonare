#include "editing/voice_changer/isp_limiter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::editing::voice_changer {
namespace {

/// Sub-millisecond attack. Mirrors RealtimeVoiceChanger::kLimiterAttackMs so
/// the ISP stage tapers transient gain reductions across ~5 samples @48k
/// instead of a single-sample step (which produces an audible click and a
/// brick-shaped artifact in the spectrum).
constexpr float kIspAttackMs = 0.1f;

/// Numerical tolerance below the ceiling to account for FIR reconstruction
/// error. Without it the detect-only gain envelope can leave residual peaks
/// 0.02-0.05 dB above the ceiling at heavily oversampled material.
constexpr float kCeilingHeadroomDb = 0.05f;

inline float clamp_finite(float value) noexcept {
  if (std::isnan(value)) return 0.0f;
  if (std::isinf(value)) return value > 0.0f ? 1.0f : -1.0f;
  return value;
}

}  // namespace

IspLimiter::IspLimiter() : filter_(0, 4) {}

void IspLimiter::prepare(double sample_rate, int max_block_size) {
  // Silently no-op on bad arguments rather than throwing — this object lives
  // inside the realtime voice changer which never throws out of prepare paths
  // either (RealtimeVoiceChanger::prepare validates and throws upstream, so
  // by the time we get here arguments are already sane).
  if (!(sample_rate > 0.0) || max_block_size < 0) return;
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  oversample_factor_ = filter_.factor();
  // The delayed polyphase detector emits a fully reconstructed sample one FIR
  // latency behind the input. Keep the audio behind that detector by enough
  // additional samples for the finite attack to settle (five time constants),
  // so the true-peak bound is reached by gain shaping rather than clipping a
  // base-rate sample.
  const int attack_settle_samples =
      static_cast<int>(std::ceil(5.0 * static_cast<double>(kIspAttackMs) * 0.001 * sample_rate_));
  lookahead_samples_ = filter_.latency_samples() + std::max(1, attack_settle_samples);

  const std::size_t block = static_cast<std::size_t>(std::max(0, max_block_size_));
  const std::size_t oversampled_capacity = block * static_cast<std::size_t>(oversample_factor_);

  oversampled_.assign(oversampled_capacity, 0.0f);
  lookahead_.prepare(static_cast<std::size_t>(std::max(0, lookahead_samples_)));
  // Keep the same two-latency horizon in the peak detector. The +1 includes
  // the current sample alongside the completed forward stencil.
  const std::size_t window_size =
      static_cast<std::size_t>(std::max(1, lookahead_samples_ + 1) * oversample_factor_);
  oversampled_peak_window_.prepare(window_size);

  // Pre-size the per-channel history / scratch buffers used by the
  // upsample_with_history overload so the audio thread never allocates.
  const std::size_t history_size = static_cast<std::size_t>(filter_.history_samples());
  history_holder_.assign(1, std::vector<float>(history_size, 0.0f));
  scratch_holder_.assign(1, std::vector<float>(history_size + block, 0.0f));
  input_ptr_.assign(1, nullptr);
  output_ptr_.assign(1, nullptr);

  ceiling_dbtp_.prepare(sample_rate_, 12.0f);
  release_ms_.prepare(sample_rate_, 12.0f);
  update_time_constants();
  prepared_ = true;
  // Reapply the stored config now that the smoothers have a real sample rate.
  set_config(config_);
  reset();
}

void IspLimiter::reset() noexcept {
  lookahead_.reset();
  oversampled_peak_window_.reset();
  std::fill(oversampled_.begin(), oversampled_.end(), 0.0f);
  for (auto& h : history_holder_) std::fill(h.begin(), h.end(), 0.0f);
  for (auto& s : scratch_holder_) std::fill(s.begin(), s.end(), 0.0f);
  gain_ = 1.0f;
  has_processed_ = false;
  ceiling_dbtp_.reset(ceiling_dbtp_.target());
  release_ms_.reset(release_ms_.target());
}

void IspLimiter::set_config(const IspLimiterConfig& config) noexcept {
  config_ = config;
  ceiling_dbtp_.set_target(config_.ceiling_dbtp);
  release_ms_.set_target(config_.release_ms);
  // A config applied before the first sample is initial state, not a live
  // transition. Snap it so a freshly prepared standalone limiter honours its
  // requested ceiling from sample zero; live updates still ramp below.
  if (!has_processed_) {
    ceiling_dbtp_.reset(config_.ceiling_dbtp);
    release_ms_.reset(config_.release_ms);
  }
}

void IspLimiter::update_time_constants() {
  if (!(sample_rate_ > 0.0)) return;
  attack_alpha_ = rt::one_pole_alpha_from_time_ms(kIspAttackMs, sample_rate_);
}

int IspLimiter::latency_samples() const noexcept { return prepared_ ? lookahead_samples_ : 0; }

void IspLimiter::process_block(float* buffer, int num_samples) noexcept {
  // RT-safe contract: silent no-op on any pre-condition violation, no
  // allocation, no throw. Caller-owned buffer is left untouched.
  if (!prepared_ || buffer == nullptr || num_samples <= 0) return;
  if (num_samples > max_block_size_) return;
  has_processed_ = true;

  // Detect-only: upsample current block through the BS.1770-style FIR, walk
  // every oversampled sample through the sliding-max window to find the peak
  // over [now, now + lookahead], compute a target gain at base rate, then
  // smooth and apply that gain to the delayed signal. No downsampler — cheaper
  // and sufficient because the FIR reconstruction is a contractive operation
  // (the post-gain base-rate samples are bounded by the pre-gain oversampled
  // peak times the same gain).
  input_ptr_[0] = buffer;
  output_ptr_[0] = oversampled_.data();
  filter_.upsample_with_history_delayed(input_ptr_.data(), output_ptr_.data(),
                                        /*num_channels=*/1, num_samples, history_holder_,
                                        scratch_holder_);

  // Apply a small headroom below the configured ceiling to absorb residual
  // reconstruction error from the truncated FIR.
  const int factor = oversample_factor_;

  for (int i = 0; i < num_samples; ++i) {
    // Push this base-rate sample's factor oversampled phases into the peak
    // window. The delayed reconstruction and two-latency audio delay keep the
    // window aligned across block boundaries; the final samples of a block no
    // longer use a zero-padded forward FIR stencil.
    for (int phase = 0; phase < factor; ++phase) {
      const float os_sample = oversampled_[static_cast<std::size_t>(i * factor + phase)];
      oversampled_peak_window_.push(std::abs(os_sample));
    }
    const float ceiling_dbtp = ceiling_dbtp_.process();
    const float ceiling = db_to_linear(ceiling_dbtp - kCeilingHeadroomDb);
    const float peak = oversampled_peak_window_.max();
    const float target_gain = (peak > ceiling && peak > 0.0f)
                                  ? ceiling / std::max(peak, sonare::constants::kAmpEpsilon)
                                  : 1.0f;
    // Attack on gain reduction (target < gain), release on recovery — same
    // shape used by the sample-domain limiter in RealtimeVoiceChanger so the
    // two stages compose without one fighting the other.
    // Guard against pathologically short release times producing alpha == 1
    // which would zip the gain back up in a single sample (audible thump).
    const float release_alpha =
        rt::one_pole_alpha_from_time_ms(std::max(1.0f, release_ms_.process()), sample_rate_);
    if (target_gain < gain_) {
      // Compensate the finite attack over the audio lookahead horizon. A
      // one-pole ramp toward `target_gain` would still be above that target
      // when the completed oversampled peak reaches the delayed signal, which
      // is why the old code fell back to a base-rate sample clip. Solve the
      // one-pole recurrence for a slightly lower target so the gain reaches
      // the true-peak bound exactly at the delayed sample instead.
      // `upsample_with_history_delayed` itself consumes one FIR latency before
      // its first completed peak is available. The audio delay is two FIR
      // latencies, leaving one FIR latency of actual gain-ramp lead time.
      const int attack_lead_samples = std::max(1, lookahead_samples_ - filter_.latency_samples());
      const float remaining =
          std::pow(1.0f - attack_alpha_, static_cast<float>(attack_lead_samples));
      const float denominator = 1.0f - remaining;
      const float compensated_target =
          denominator > 0.0f ? (target_gain - gain_ * remaining) / denominator : target_gain;
      if (compensated_target > 0.0f) {
        gain_ += attack_alpha_ * (compensated_target - gain_);
      } else {
        // An exceptionally large transient cannot reach the safe gain within
        // the fixed lookahead even when ramping toward silence. Limit its gain
        // directly rather than clipping one base-rate sample (which would
        // recreate the inter-sample peak we are preventing).
        gain_ = target_gain;
      }
    } else {
      gain_ += release_alpha * (target_gain - gain_);
    }

    const float delayed = clamp_finite(lookahead_.process(buffer[i]));
    // Never clip the base-rate waveform here. A sample hard-clamp introduces a
    // discontinuity that the same interpolation FIR can reconstruct above the
    // dBTP ceiling. The gain safety bound above is the final protection.
    buffer[i] = delayed * gain_;
  }
}

}  // namespace sonare::editing::voice_changer
