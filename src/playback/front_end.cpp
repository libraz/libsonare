#include "playback/front_end.h"

namespace sonare::playback {

struct FrontEnd::Impl {
  ChannelLayout input = ChannelLayout::Stereo;
};

LoudnessGain compute_loudness_gain(const RealtimeConfig& config) noexcept {
  (void)config;
  return {};
}

FrontEnd::FrontEnd() : impl_(std::make_unique<Impl>()) {}
FrontEnd::~FrontEnd() = default;

void FrontEnd::prepare(double sample_rate, int max_block_size, ChannelLayout input,
                       const PrepareConfig& config, const OutputBus& bus) {
  (void)sample_rate;
  (void)max_block_size;
  (void)config;
  (void)bus;
  impl_->input = input;
}

void FrontEnd::apply_realtime(const RealtimeConfig& config, bool immediate) noexcept {
  (void)config;
  (void)immediate;
}

void FrontEnd::process(const float* const* in, float* const* bus, int frames) noexcept {
  (void)in;
  (void)bus;
  (void)frames;
}

void FrontEnd::reset() noexcept {}

int FrontEnd::latency() const noexcept { return 0; }

int FrontEnd::drain_frames() const noexcept { return 0; }

ChannelLayout FrontEnd::input_layout() const noexcept { return impl_->input; }

int FrontEnd::input_channel_count() const noexcept { return channel_count(impl_->input); }

void FrontEnd::set_draining(bool draining) noexcept { (void)draining; }

void FrontEnd::start_fade_out(int frames) noexcept { (void)frames; }

bool FrontEnd::fade_out_done() const noexcept { return false; }

NightModeDrcHandover FrontEnd::handover() const noexcept { return {}; }

void FrontEnd::accept_handover(const NightModeDrcHandover& state) noexcept { (void)state; }

uint32_t FrontEnd::inactive_stages(const RealtimeConfig& config) const noexcept {
  (void)config;
  return 0;
}

std::array<int, kStageCount> FrontEnd::stage_latency_q8() const noexcept { return {}; }

}  // namespace sonare::playback
