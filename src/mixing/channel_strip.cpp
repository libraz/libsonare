#include "mixing/channel_strip.h"

#include <algorithm>

namespace sonare::mixing {

namespace {

void zero_taps(std::vector<std::vector<float>>& taps, int num_channels, int num_samples) {
  const int rows = std::min<int>(num_channels, static_cast<int>(taps.size()));
  for (int ch = 0; ch < rows; ++ch) {
    const int n = std::min<int>(num_samples, static_cast<int>(taps[ch].size()));
    std::fill(taps[ch].begin(), taps[ch].begin() + n, 0.0f);
  }
}

void copy_to_taps(float* const* channels, std::vector<std::vector<float>>& taps, int num_channels,
                  int num_samples) {
  const int rows = std::min<int>(num_channels, static_cast<int>(taps.size()));
  for (int ch = 0; ch < rows; ++ch) {
    if (channels[ch] == nullptr) {
      continue;
    }
    const int n = std::min<int>(num_samples, static_cast<int>(taps[ch].size()));
    std::copy(channels[ch], channels[ch] + n, taps[ch].begin());
  }
}

}  // namespace

ChannelStrip::ChannelStrip(ChannelStripConfig config)
    : fader_({config.fader_db, config.smoothing_ms}),
      panner_({config.pan, config.pan_law, config.smoothing_ms}),
      eq_position_(config.eq_position) {}

void ChannelStrip::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate;
  max_block_size_ = std::max(0, max_block_size);

  fader_.prepare(sample_rate, max_block_size);
  panner_.prepare(sample_rate, max_block_size);
  eq_.prepare(sample_rate, max_block_size);
  meter_.prepare(sample_rate, max_block_size);
  for (auto& send : sends_) {
    send->prepare(sample_rate, max_block_size);
  }

  const auto rows = static_cast<size_t>(kPreparedChannels);
  const auto cols = static_cast<size_t>(max_block_size_);
  pre_tap_.assign(rows, std::vector<float>(cols, 0.0f));
  post_tap_.assign(rows, std::vector<float>(cols, 0.0f));
  send_temp_.assign(rows, std::vector<float>(cols, 0.0f));

  // ParametricEq allocates its per-channel filter state lazily on the first process() with a
  // given channel count. Warm it up here (off the audio thread) so process() stays RT-safe.
  if (max_block_size_ > 0) {
    float* warm[kPreparedChannels];
    for (int ch = 0; ch < kPreparedChannels; ++ch) {
      warm[ch] = post_tap_[static_cast<size_t>(ch)].data();
    }
    eq_.process(warm, kPreparedChannels, max_block_size_);
    eq_.reset();
    zero_taps(post_tap_, kPreparedChannels, max_block_size_);
  }
}

void ChannelStrip::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  const int clamped_samples = std::min(num_samples, max_block_size_);

  if (effectively_muted()) {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) {
        std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
      }
    }
    zero_taps(pre_tap_, num_channels, clamped_samples);
    zero_taps(post_tap_, num_channels, clamped_samples);
    meter_.process(channels, num_channels, num_samples);
    return;
  }

  if (eq_position_.load(std::memory_order_relaxed) == EqPosition::PreFader) {
    eq_.process(channels, num_channels, num_samples);
  }

  // Pre-fader tap (after pre-fader EQ if enabled) feeds pre-fader aux sends.
  copy_to_taps(channels, pre_tap_, num_channels, clamped_samples);

  fader_.process(channels, num_channels, num_samples);
  panner_.process(channels, num_channels, num_samples);

  if (eq_position_.load(std::memory_order_relaxed) == EqPosition::PostFader) {
    eq_.process(channels, num_channels, num_samples);
  }

  // Post-fader tap is the final output, used by post-fader sends and the meter.
  copy_to_taps(channels, post_tap_, num_channels, clamped_samples);

  meter_.process(channels, num_channels, num_samples);
}

void ChannelStrip::reset() {
  fader_.reset();
  panner_.reset();
  eq_.reset();
  meter_.reset();
  for (auto& send : sends_) {
    send->reset();
  }
  zero_taps(pre_tap_, kPreparedChannels, max_block_size_);
  zero_taps(post_tap_, kPreparedChannels, max_block_size_);
  zero_taps(send_temp_, kPreparedChannels, max_block_size_);
}

size_t ChannelStrip::add_send(const SendConfig& cfg) {
  sends_.push_back(std::make_unique<SendProcessor>(cfg));
  if (max_block_size_ > 0) {
    sends_.back()->prepare(sample_rate_, max_block_size_);
  }
  return sends_.size() - 1;
}

void ChannelStrip::set_send_db(size_t index, float db) {
  if (index >= sends_.size()) {
    return;
  }
  sends_[index]->set_send_db(db);
}

SendTiming ChannelStrip::send_timing(size_t index) const {
  if (index >= sends_.size()) {
    return SendTiming::PostFader;
  }
  return sends_[index]->timing();
}

void ChannelStrip::mix_send(size_t index, float* const* dest, int num_channels, int num_samples) {
  if (index >= sends_.size() || dest == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  SendProcessor& send = *sends_[index];
  auto& tap = (send.timing() == SendTiming::PreFader) ? pre_tap_ : post_tap_;

  const int rows =
      std::min<int>(std::min(num_channels, kPreparedChannels), static_cast<int>(tap.size()));
  const int n = std::min(num_samples, max_block_size_);

  float* temp[kPreparedChannels];
  for (int ch = 0; ch < rows; ++ch) {
    std::copy(tap[ch].begin(), tap[ch].begin() + n, send_temp_[ch].begin());
    temp[ch] = send_temp_[ch].data();
  }

  // Applies the smoothed send gain in place on the copied tap, leaving dest untouched.
  send.process(temp, rows, n);

  for (int ch = 0; ch < rows; ++ch) {
    if (dest[ch] == nullptr) {
      continue;
    }
    for (int i = 0; i < n; ++i) {
      dest[ch][i] += send_temp_[ch][i];
    }
  }
}

void ChannelStrip::set_muted(bool muted) noexcept {
  muted_.store(muted, std::memory_order_relaxed);
}

bool ChannelStrip::muted() const noexcept { return muted_.load(std::memory_order_relaxed); }

bool ChannelStrip::effectively_muted() const noexcept {
  return muted() || (implied_mute() && !solo_safe());
}

void ChannelStrip::set_soloed(bool soloed) noexcept {
  soloed_.store(soloed, std::memory_order_relaxed);
}

bool ChannelStrip::soloed() const noexcept { return soloed_.load(std::memory_order_relaxed); }

void ChannelStrip::set_solo_safe(bool solo_safe) noexcept {
  solo_safe_.store(solo_safe, std::memory_order_relaxed);
}

bool ChannelStrip::solo_safe() const noexcept { return solo_safe_.load(std::memory_order_relaxed); }

void ChannelStrip::set_implied_mute(bool implied_mute) noexcept {
  implied_mute_.store(implied_mute, std::memory_order_relaxed);
}

bool ChannelStrip::implied_mute() const noexcept {
  return implied_mute_.load(std::memory_order_relaxed);
}

}  // namespace sonare::mixing
