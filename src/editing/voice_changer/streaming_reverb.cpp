#include "editing/voice_changer/streaming_reverb.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "rt/biquad_design.h"
#include "rt/tail_budget.h"
#include "util/db.h"
#include "util/dsp_primitives.h"

namespace sonare::editing::voice_changer {

void StreamingReverb::prepare(double sample_rate, int max_block_size) {
  (void)max_block_size;  // Reverb is per-sample; the block size only constrains
                         // the higher-level chain, not the reverb's footprint.
  sample_rate_ = sample_rate;

  // Comb buffers sized for kMaxTimeMs so set_config() never needs to resize.
  const std::size_t max_comb = comb_capacity(sample_rate_);
  for (auto& buf : comb_buf_) {
    buf.assign(max_comb, 0.0f);
  }
  // Fixed 5 ms diffusion allpass. Plenty long for speech material and short
  // enough to keep the impulse response tight on percussive sounds.
  const std::size_t allpass_samples = allpass_capacity(sample_rate_);
  allpass_buf_.assign(allpass_samples, 0.0f);
  allpass_delay_ = allpass_samples - 1;

  std::fill(comb_pos_.begin(), comb_pos_.end(), 0u);
  allpass_pos_ = 0;
  for (auto& smoother : comb_delay_) smoother.prepare(sample_rate_, 20.0f);
  for (auto& smoother : comb_fb_) smoother.prepare(sample_rate_, 20.0f);
  damping_alpha_.prepare(sample_rate_, 20.0f);
  mix_.prepare(sample_rate_, kMixGlideMs);
  allpass_g_.prepare(sample_rate_, 20.0f);
  set_config(config_);
  reset();
}

void StreamingReverb::reset() noexcept {
  for (auto& buf : comb_buf_) std::fill(buf.begin(), buf.end(), 0.0f);
  std::fill(comb_pos_.begin(), comb_pos_.end(), 0u);
  std::fill(comb_lp_.begin(), comb_lp_.end(), 0.0f);
  std::fill(allpass_buf_.begin(), allpass_buf_.end(), 0.0f);
  allpass_pos_ = 0;
  for (auto& smoother : comb_delay_) smoother.reset(smoother.target());
  for (auto& smoother : comb_fb_) smoother.reset(smoother.target());
  damping_alpha_.reset(damping_alpha_.target());
  mix_.reset(mix_.target());
  allpass_g_.reset(allpass_g_.target());
}

std::size_t StreamingReverb::comb_capacity(double sample_rate) {
  return static_cast<std::size_t>(sample_rate * (kMaxTimeMs * 0.001) + 8);
}

std::size_t StreamingReverb::allpass_capacity(double sample_rate) {
  return std::max<std::size_t>(8, static_cast<std::size_t>(sample_rate * 0.005));
}

std::array<StreamingReverb::CombTap, StreamingReverb::kNumCombs> StreamingReverb::comb_taps(
    const StreamingReverbConfig& config, double sample_rate, std::size_t max_delay,
    int channel_index) {
  const double time_sec = std::max(0.04, static_cast<double>(config.time_ms) / 1000.0);

  // Derive a per-channel seed by XORing with a 32-bit golden-ratio constant
  // scaled by the channel index. Without this, stereo channels would share
  // the exact same comb delays / feedbacks / allpass coefficient, producing a
  // fully mono (L=R) reverb tail from any non-pan-spread source.
  static constexpr std::uint32_t kChannelSeedSalt = 0x9E3779B9u;
  const std::uint32_t useed = static_cast<std::uint32_t>(config.seed) ^
                              (kChannelSeedSalt * static_cast<std::uint32_t>(channel_index));

  // Map seed to a small jitter in [-0.05, +0.05] of the base ratios.
  const float jitter_a = static_cast<float>(((useed * 2654435761u) >> 24) & 0xFFu) / 255.0f - 0.5f;
  const float jitter_b = static_cast<float>(((useed * 40503u) >> 24) & 0xFFu) / 255.0f - 0.5f;
  const std::array<double, kNumCombs> ratios = {
      0.42 + 0.1 * jitter_a,
      0.61 + 0.1 * jitter_b,
  };

  std::array<CombTap, kNumCombs> taps{};
  for (std::size_t i = 0; i < kNumCombs; ++i) {
    const double tap_seconds = time_sec * ratios[i];
    std::size_t delay_samples = static_cast<std::size_t>(tap_seconds * sample_rate);
    delay_samples = std::clamp<std::size_t>(delay_samples, 16u, max_delay);
    // Feedback gain chosen so each comb decays by ~60 dB in roughly time_ms.
    const double db_per_sec = -60.0 / time_sec;
    const double db_per_loop = db_per_sec * (static_cast<double>(delay_samples) / sample_rate);
    taps[i].delay_samples = static_cast<float>(delay_samples);
    taps[i].feedback = std::clamp(static_cast<float>(db_to_linear(db_per_loop)), 0.0f, 0.97f);
  }
  return taps;
}

float StreamingReverb::damping_cutoff_hz(const StreamingReverbConfig& config) {
  // Damping: damping=0 -> ~12 kHz LP (bright), damping=1 -> ~1 kHz LP (dark).
  return 1000.0f + (1.0f - std::clamp(config.damping, 0.0f, 1.0f)) * 11000.0f;
}

float StreamingReverb::allpass_gain(const StreamingReverbConfig& config, int channel_index) {
  static constexpr std::uint32_t kChannelSeedSalt = 0x9E3779B9u;
  const std::uint32_t useed = static_cast<std::uint32_t>(config.seed) ^
                              (kChannelSeedSalt * static_cast<std::uint32_t>(channel_index));
  // Allpass gain sign is seed-bit-dependent so different seeds yield distinct
  // phase responses on otherwise identical settings.
  return (useed & 0x1u) ? 0.7f : -0.7f;
}

void StreamingReverb::set_config(const StreamingReverbConfig& config, int channel_index) {
  config_ = config;

  const std::size_t cap = comb_buf_[0].size();
  if (cap < 4) return;  // prepare() not run yet; nothing to apply.
  const auto taps = comb_taps(config_, sample_rate_, cap - 2, channel_index);
  for (std::size_t i = 0; i < kNumCombs; ++i) {
    comb_delay_[i].set_target(taps[i].delay_samples);
    comb_fb_[i].set_target(taps[i].feedback);
  }
  damping_alpha_.set_target(
      rt::one_pole_lowpass_alpha_matched(damping_cutoff_hz(config_), sample_rate_));
  allpass_g_.set_target(allpass_gain(config_, channel_index));
  mix_.set_target(std::clamp(config_.mix, 0.0f, 1.0f));
}

rt::TailBudget StreamingReverb::tail(const StreamingReverbConfig& config, double sample_rate,
                                     int num_channels, double output_scale) {
  rt::TailBudget budget;
  if (!(sample_rate > 0.0)) return budget;
  // A mix gliding to zero still lets the loops through, at most at full level, until it lands.
  const double target_mix = std::clamp(config.mix, 0.0f, 1.0f);
  const double mix = target_mix > 0.0 ? target_mix : 1.0;

  const std::size_t max_delay = comb_capacity(sample_rate) - 2;
  const double damping_alpha =
      rt::one_pole_lowpass_alpha_matched(damping_cutoff_hz(config), sample_rate);
  // The damping cell delays every pass by its group delay and rings on after the last echo.
  const double damping_lag = (1.0 - damping_alpha) / std::max(damping_alpha, 1.0e-6);
  double heard = 0.0;
  for (int ch = 0; ch < std::max(1, num_channels); ++ch) {
    // The two comb outputs are averaged, so either alone is at most the full level; the
    // diffusion allpass then spreads an echo over a kernel of at most (1+|g|)/(1-|g|).
    const double g = std::abs(static_cast<double>(allpass_gain(config, ch)));
    const double scale = mix * output_scale * (1.0 + g) / (1.0 - g);
    heard = std::max(heard, scale);
    rt::TailBudget combs;
    for (const CombTap& tap : comb_taps(config, sample_rate, max_delay, ch)) {
      const double loop = static_cast<double>(tap.delay_samples) + damping_lag + 1.0;
      rt::TailBudget comb;
      comb.delay(loop).recirculation(loop, tap.feedback, scale).decay(1.0 - damping_alpha, scale);
      combs.alongside(comb);
    }
    // The allpass recirculates whatever the combs leave in it once they have died away.
    rt::TailBudget diffusion;
    diffusion.recirculation(static_cast<double>(allpass_capacity(sample_rate)), g, scale);
    budget.alongside(combs.then(diffusion));
  }
  if (target_mix > 0.0) return budget;
  rt::TailBudget glide;
  glide.decay(1.0 - time_to_attack_release_rate(sample_rate, kMixGlideMs), heard);
  return rt::TailBudget::reported(std::min(budget.samples(), glide.samples()));
}

float StreamingReverb::process_sample(float input) noexcept {
  if (allpass_buf_.empty()) return input;

  const std::size_t cap = comb_buf_[0].size();
  if (cap < 4) return input;

  // When muted (mix<=0) we still advance the comb/allpass delay lines and let
  // the damping/feedback states decay. Freezing them while muted would resume
  // from stale pre-mute contents on the next mix>0 block, producing an audible
  // burst of old material. We only skip the wet contribution to the output.
  const float mix = mix_.process();
  const bool muted = mix <= 1.0e-6f;

  // One tick per sample, read by every comb. damping_alpha_ is shared, so
  // advancing it inside the comb loop stepped it kNumCombs times per sample:
  // the configured smoothing time came out kNumCombs times shorter than asked
  // for, and -- worse -- comb 0 and comb 1 applied DIFFERENT damping
  // coefficients within one sample, which is not a filter the model describes.
  // The per-comb smoothers below are per-comb state and correctly tick once.
  const float damping_alpha = damping_alpha_.process();

  float comb_sum = 0.0f;
  for (std::size_t i = 0; i < kNumCombs; ++i) {
    const float delay = std::clamp(comb_delay_[i].process(), 16.0f, static_cast<float>(cap - 2));
    const std::size_t delay_floor = static_cast<std::size_t>(std::floor(delay));
    const float fraction = delay - static_cast<float>(delay_floor);
    const std::size_t read_a = (comb_pos_[i] + cap - delay_floor) % cap;
    const std::size_t read_b = (read_a + cap - 1) % cap;
    const float y = comb_buf_[i][read_a] + fraction * (comb_buf_[i][read_b] - comb_buf_[i][read_a]);
    comb_lp_[i] += damping_alpha * (y - comb_lp_[i]);
    comb_buf_[i][comb_pos_[i]] = input + comb_fb_[i].process() * comb_lp_[i];
    comb_pos_[i] = (comb_pos_[i] + 1) % cap;
    comb_sum += y;
  }
  const float comb_out = comb_sum / static_cast<float>(kNumCombs);

  const std::size_t ap_cap = allpass_buf_.size();
  const std::size_t ap_read = (allpass_pos_ + ap_cap - allpass_delay_) % ap_cap;
  const float ap_delayed = allpass_buf_[ap_read];
  const float allpass_g = allpass_g_.process();
  const float ap_in = comb_out + allpass_g * ap_delayed;
  const float ap_out = ap_delayed - allpass_g * ap_in;
  allpass_buf_[allpass_pos_] = ap_in;
  allpass_pos_ = (allpass_pos_ + 1) % ap_cap;

  if (muted) return input;
  return input * (1.0f - mix) + ap_out * mix;
}

bool StreamingReverb::discard_non_finite() noexcept {
  const std::size_t ap_cap = allpass_buf_.size();
  const float newest_diffusion =
      ap_cap == 0 ? 0.0f : allpass_buf_[(allpass_pos_ + ap_cap - 1) % ap_cap];
  bool resting = std::isfinite(newest_diffusion);
  for (const float lp : comb_lp_) resting = resting && std::isfinite(lp);
  if (resting) return false;
  // The delay lines are the feedback, so a usable damping cell over a poisoned
  // line is not half a tank. The walk is O(line) and only on the recovery path.
  for (auto& buf : comb_buf_) std::fill(buf.begin(), buf.end(), 0.0f);
  std::fill(comb_lp_.begin(), comb_lp_.end(), 0.0f);
  std::fill(allpass_buf_.begin(), allpass_buf_.end(), 0.0f);
  return true;
}

void StreamingReverb::process_block(const float* input, float* output, int num_samples) noexcept {
  if (input == nullptr || output == nullptr || num_samples <= 0) return;
  for (int i = 0; i < num_samples; ++i) {
    output[i] = process_sample(input[i]);
  }
}

}  // namespace sonare::editing::voice_changer
