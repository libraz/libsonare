#include "editing/voice_changer/streaming_reverb.h"

#include <algorithm>
#include <cmath>

#include "rt/biquad_design.h"

namespace sonare::editing::voice_changer {

void StreamingReverb::prepare(double sample_rate, int max_block_size) {
  (void)max_block_size;  // Reverb is per-sample; the block size only constrains
                         // the higher-level chain, not the reverb's footprint.
  sample_rate_ = sample_rate;

  // Comb buffers sized for kMaxTimeMs so set_config() never needs to resize.
  const std::size_t max_comb = static_cast<std::size_t>(sample_rate_ * (kMaxTimeMs * 0.001) + 8);
  for (auto& buf : comb_buf_) {
    buf.assign(max_comb, 0.0f);
  }
  // Fixed 5 ms diffusion allpass. Plenty long for speech material and short
  // enough to keep the impulse response tight on percussive sounds.
  const std::size_t allpass_samples =
      std::max<std::size_t>(8, static_cast<std::size_t>(sample_rate_ * 0.005));
  allpass_buf_.assign(allpass_samples, 0.0f);
  allpass_delay_ = allpass_samples - 1;

  std::fill(comb_pos_.begin(), comb_pos_.end(), 0u);
  allpass_pos_ = 0;
}

void StreamingReverb::reset() noexcept {
  for (auto& buf : comb_buf_) std::fill(buf.begin(), buf.end(), 0.0f);
  std::fill(comb_pos_.begin(), comb_pos_.end(), 0u);
  std::fill(comb_lp_.begin(), comb_lp_.end(), 0.0f);
  std::fill(allpass_buf_.begin(), allpass_buf_.end(), 0.0f);
  allpass_pos_ = 0;
}

void StreamingReverb::set_config(const StreamingReverbConfig& config, int channel_index) {
  config_ = config;

  const double time_sec = std::max(0.04, static_cast<double>(config_.time_ms) / 1000.0);

  // Derive a per-channel seed by XORing with a 32-bit golden-ratio constant
  // scaled by the channel index. Without this, stereo channels would share
  // the exact same comb delays / feedbacks / allpass coefficient, producing a
  // fully mono (L=R) reverb tail from any non-pan-spread source.
  static constexpr std::uint32_t kChannelSeedSalt = 0x9E3779B9u;
  const std::uint32_t useed = static_cast<std::uint32_t>(config_.seed) ^
                              (kChannelSeedSalt * static_cast<std::uint32_t>(channel_index));

  // Map seed to a small jitter in [-0.05, +0.05] of the base ratios.
  const float jitter_a = static_cast<float>(((useed * 2654435761u) >> 24) & 0xFFu) / 255.0f - 0.5f;
  const float jitter_b = static_cast<float>(((useed * 40503u) >> 24) & 0xFFu) / 255.0f - 0.5f;
  const std::array<double, kNumCombs> ratios = {
      0.42 + 0.1 * jitter_a,
      0.61 + 0.1 * jitter_b,
  };

  const std::size_t cap = comb_buf_[0].size();
  if (cap < 4) return;  // prepare() not run yet; nothing to apply.
  const std::size_t max_delay = cap - 2;
  for (std::size_t i = 0; i < kNumCombs; ++i) {
    const double tap_seconds = time_sec * ratios[i];
    std::size_t delay_samples = static_cast<std::size_t>(tap_seconds * sample_rate_);
    delay_samples = std::clamp<std::size_t>(delay_samples, 16u, max_delay);
    comb_delay_[i] = delay_samples;
    // Feedback gain chosen so each comb decays by ~60 dB in roughly time_ms.
    const double db_per_sec = -60.0 / time_sec;
    const double db_per_loop = db_per_sec * (static_cast<double>(delay_samples) / sample_rate_);
    comb_fb_[i] = static_cast<float>(std::pow(10.0, db_per_loop / 20.0));
    comb_fb_[i] = std::clamp(comb_fb_[i], 0.0f, 0.97f);
  }
  // Damping: damping=0 -> ~12 kHz LP (bright), damping=1 -> ~1 kHz LP (dark).
  const float damping_hz = 1000.0f + (1.0f - std::clamp(config_.damping, 0.0f, 1.0f)) * 11000.0f;
  damping_alpha_ = rt::one_pole_lowpass_alpha_matched(damping_hz, sample_rate_);
  // Allpass gain sign is seed-bit-dependent so different seeds yield distinct
  // phase responses on otherwise identical settings.
  allpass_g_ = (useed & 0x1u) ? 0.7f : -0.7f;
}

float StreamingReverb::process_sample(float input) noexcept {
  if (config_.mix <= 0.0f || allpass_buf_.empty()) return input;

  const std::size_t cap = comb_buf_[0].size();
  if (cap < 4) return input;

  float comb_sum = 0.0f;
  for (std::size_t i = 0; i < kNumCombs; ++i) {
    const std::size_t delay = comb_delay_[i];
    const std::size_t read_pos = (comb_pos_[i] + cap - delay) % cap;
    const float y = comb_buf_[i][read_pos];
    comb_lp_[i] += damping_alpha_ * (y - comb_lp_[i]);
    comb_buf_[i][comb_pos_[i]] = input + comb_fb_[i] * comb_lp_[i];
    comb_pos_[i] = (comb_pos_[i] + 1) % cap;
    comb_sum += y;
  }
  const float comb_out = comb_sum / static_cast<float>(kNumCombs);

  const std::size_t ap_cap = allpass_buf_.size();
  const std::size_t ap_read = (allpass_pos_ + ap_cap - allpass_delay_) % ap_cap;
  const float ap_delayed = allpass_buf_[ap_read];
  const float ap_in = comb_out + allpass_g_ * ap_delayed;
  const float ap_out = ap_delayed - allpass_g_ * ap_in;
  allpass_buf_[allpass_pos_] = ap_in;
  allpass_pos_ = (allpass_pos_ + 1) % ap_cap;

  return input * (1.0f - config_.mix) + ap_out * config_.mix;
}

void StreamingReverb::process_block(const float* input, float* output, int num_samples) noexcept {
  if (input == nullptr || output == nullptr || num_samples <= 0) return;
  for (int i = 0; i < num_samples; ++i) {
    output[i] = process_sample(input[i]);
  }
}

}  // namespace sonare::editing::voice_changer
