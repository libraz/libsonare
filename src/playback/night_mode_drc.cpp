#include "playback/night_mode_drc.h"

#include <algorithm>

namespace sonare::playback {

struct NightModeDrc::Impl {
  ChannelLayout layout = ChannelLayout::Stereo;
};

float night_mode_curve_db(float x_db, float amount) noexcept {
  (void)x_db;
  (void)amount;
  return 0.0f;
}

NightModeDrc::NightModeDrc() : impl_(std::make_unique<Impl>()) {}
NightModeDrc::~NightModeDrc() = default;

void NightModeDrc::prepare(double sample_rate, int max_block_size, ChannelLayout layout) {
  (void)sample_rate;
  (void)max_block_size;
  impl_->layout = layout;
}

void NightModeDrc::set_amount(float amount) noexcept { (void)amount; }

void NightModeDrc::set_target_lufs(float target_lufs) noexcept { (void)target_lufs; }

void NightModeDrc::set_frozen(bool frozen) noexcept { (void)frozen; }

void NightModeDrc::process(float* const* planes, int frames) noexcept {
  if (planes == nullptr || frames <= 0) return;
  for (int ch = 0; ch < channel_count(impl_->layout); ++ch) {
    if (planes[ch] != nullptr) std::fill(planes[ch], planes[ch] + frames, 0.0f);
  }
}

void NightModeDrc::reset() noexcept {}

int NightModeDrc::latency_samples() const noexcept { return 0; }

NightModeDrcHandover NightModeDrc::handover() const noexcept { return {}; }

void NightModeDrc::accept_handover(const NightModeDrcHandover& state) noexcept { (void)state; }

float NightModeDrc::current_gain_db() const noexcept { return 0.0f; }

}  // namespace sonare::playback
