#include "playback/upmix.h"

#include <algorithm>

namespace sonare::playback {

struct Upmixer::Impl {
  ChannelLayout output = ChannelLayout::FivePointOne;
};

int upmix_latency_frames(double sample_rate) noexcept {
  (void)sample_rate;
  return 0;
}

Upmixer::Upmixer() : impl_(std::make_unique<Impl>()) {}
Upmixer::~Upmixer() = default;

void Upmixer::prepare(double sample_rate, int max_block_size, ChannelLayout output) {
  (void)sample_rate;
  (void)max_block_size;
  impl_->output = output;
}

void Upmixer::set_params(const UpmixParams& params, bool immediate) noexcept {
  (void)params;
  (void)immediate;
}

void Upmixer::process(const float* left, const float* right, float* const* out,
                      int frames) noexcept {
  (void)left;
  (void)right;
  if (out == nullptr || frames <= 0) return;
  for (int ch = 0; ch < channel_count(impl_->output); ++ch) {
    if (out[ch] != nullptr) std::fill(out[ch], out[ch] + frames, 0.0f);
  }
}

void Upmixer::reset() noexcept {}

int Upmixer::latency_samples() const noexcept { return 0; }

int Upmixer::decay_frames() const noexcept { return 0; }

ChannelLayout Upmixer::output_layout() const noexcept { return impl_->output; }

}  // namespace sonare::playback
