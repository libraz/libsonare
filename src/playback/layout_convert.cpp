#include "playback/layout_convert.h"

namespace sonare::playback {

struct LayoutConverter::Impl {};

OutputBus make_output_bus(const PrepareConfig& config) noexcept {
  (void)config;
  return {};
}

void validate_channel_map(ChannelLayout layout, const SpeakerRole* map, int length) {
  (void)layout;
  (void)map;
  (void)length;
}

ChannelLayout converted_source_layout(ChannelLayout input, const OutputBus& bus) noexcept {
  (void)bus;
  return input;
}

LayoutConverter::LayoutConverter() : impl_(std::make_unique<Impl>()) {}
LayoutConverter::~LayoutConverter() = default;

void LayoutConverter::prepare(double sample_rate, int max_block_size, ChannelLayout source,
                              const OutputBus& bus) {
  (void)sample_rate;
  (void)max_block_size;
  (void)source;
  (void)bus;
}

void LayoutConverter::set_lfe_mix_db(float db) noexcept { (void)db; }

void LayoutConverter::process(const float* const* source, float* const* bus, int frames) noexcept {
  (void)source;
  (void)bus;
  (void)frames;
}

void LayoutConverter::reset() noexcept {}

int LayoutConverter::decay_frames() const noexcept { return 0; }

}  // namespace sonare::playback
