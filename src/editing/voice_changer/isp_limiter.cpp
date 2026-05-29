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
  lookahead_samples_ = filter_.latency_samples();

  const std::size_t block = static_cast<std::size_t>(std::max(0, max_block_size_));
  const std::size_t oversampled_capacity = block * static_cast<std::size_t>(oversample_factor_);

  oversampled_.assign(oversampled_capacity, 0.0f);
  lookahead_.prepare(static_cast<std::size_t>(std::max(0, lookahead_samples_)));
  // Sliding window covers (lookahead_samples_ + 1) base-rate samples worth of
  // oversampled peaks, matching how mastering::maximizer::TruePeakLimiter
  // sizes its own peak window. The +1 ensures the current sample is included
  // alongside the lookahead horizon.
  const std::size_t window_size =
      static_cast<std::size_t>(std::max(1, lookahead_samples_ + 1) * oversample_factor_);
  oversampled_peak_window_.prepare(window_size);

  // Pre-size the per-channel history / scratch buffers used by the
  // upsample_with_history overload so the audio thread never allocates.
  const std::size_t history_size = static_cast<std::size_t>(std::max(0, oversample_factor_ * 12));
  history_holder_.assign(1, std::vector<float>(history_size, 0.0f));
  scratch_holder_.assign(1, std::vector<float>(history_size + block, 0.0f));
  input_ptr_.assign(1, nullptr);
  output_ptr_.assign(1, nullptr);

  update_time_constants();
  prepared_ = true;
  reset();
}

void IspLimiter::reset() noexcept {
  lookahead_.reset();
  oversampled_peak_window_.reset();
  std::fill(oversampled_.begin(), oversampled_.end(), 0.0f);
  for (auto& h : history_holder_) std::fill(h.begin(), h.end(), 0.0f);
  for (auto& s : scratch_holder_) std::fill(s.begin(), s.end(), 0.0f);
  gain_ = 1.0f;
}

void IspLimiter::set_config(const IspLimiterConfig& config) noexcept {
  config_ = config;
  if (prepared_) update_time_constants();
}

void IspLimiter::update_time_constants() {
  if (!(sample_rate_ > 0.0)) return;
  attack_alpha_ = rt::one_pole_alpha_from_time_ms(kIspAttackMs, sample_rate_);
  // Guard against pathologically short release times producing alpha == 1 which
  // would zip the gain back up in a single sample (audible thump).
  const float release_ms = std::max(1.0f, config_.release_ms);
  release_alpha_ = rt::one_pole_alpha_from_time_ms(release_ms, sample_rate_);
}

int IspLimiter::latency_samples() const noexcept { return prepared_ ? lookahead_samples_ : 0; }

void IspLimiter::process_block(float* buffer, int num_samples) noexcept {
  // RT-safe contract: silent no-op on any pre-condition violation, no
  // allocation, no throw. Caller-owned buffer is left untouched.
  if (!prepared_ || buffer == nullptr || num_samples <= 0) return;
  if (num_samples > max_block_size_) return;

  // Detect-only: upsample current block through the BS.1770-style FIR, walk
  // every oversampled sample through the sliding-max window to find the peak
  // over [now, now + lookahead], compute a target gain at base rate, then
  // smooth and apply that gain to the delayed signal. No downsampler — cheaper
  // and sufficient because the FIR reconstruction is a contractive operation
  // (the post-gain base-rate samples are bounded by the pre-gain oversampled
  // peak times the same gain).
  input_ptr_[0] = buffer;
  output_ptr_[0] = oversampled_.data();
  filter_.upsample_with_history(input_ptr_.data(), output_ptr_.data(), /*num_channels=*/1,
                                num_samples, history_holder_, scratch_holder_);

  // Apply a small headroom below the configured ceiling to absorb residual
  // reconstruction error from the truncated FIR.
  const float ceiling_db = config_.ceiling_dbtp - kCeilingHeadroomDb;
  const float ceiling = db_to_linear(ceiling_db);
  const int factor = oversample_factor_;

  for (int i = 0; i < num_samples; ++i) {
    // Push this base-rate sample's factor oversampled phases into the peak
    // window. The window always reflects [now, now + lookahead] in oversampled
    // units after warm-up; during warm-up it covers a smaller horizon, which
    // is safe (it just means earlier samples see no future peak).
    for (int phase = 0; phase < factor; ++phase) {
      const float os_sample = oversampled_[static_cast<std::size_t>(i * factor + phase)];
      oversampled_peak_window_.push(std::abs(os_sample));
    }
    const float peak = oversampled_peak_window_.max();
    const float target_gain = (peak > ceiling && peak > 0.0f)
                                  ? ceiling / std::max(peak, sonare::constants::kAmpEpsilon)
                                  : 1.0f;
    // Attack on gain reduction (target < gain), release on recovery — same
    // shape used by the sample-domain limiter in RealtimeVoiceChanger so the
    // two stages compose without one fighting the other.
    const float alpha = target_gain < gain_ ? attack_alpha_ : release_alpha_;
    gain_ += alpha * (target_gain - gain_);

    const float delayed = lookahead_.process(buffer[i]);
    float out = clamp_finite(delayed) * gain_;
    // Final hard ceiling at the *configured* dBTP (not the headroom-reduced
    // detection ceiling) — guarantees the published guarantee even in the
    // worst-case where the smoothed gain has not yet caught up with a single
    // overshooting sample.
    const float hard_ceiling = db_to_linear(config_.ceiling_dbtp);
    if (out > hard_ceiling) out = hard_ceiling;
    if (out < -hard_ceiling) out = -hard_ceiling;
    buffer[i] = out;
  }
}

}  // namespace sonare::editing::voice_changer
