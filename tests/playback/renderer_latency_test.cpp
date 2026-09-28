#include <catch2/catch_test_macros.hpp>

#include "playback/renderer.h"

using sonare::ChannelLayout;
using namespace sonare::playback;

namespace {

RendererConfig config_for(InputLayout input, TargetKind kind, ChannelLayout layout) {
  RendererConfig config;
  config.prepare.input_layout = input;
  config.prepare.target_kind = kind;
  config.prepare.target_layout = layout;
  return config;
}

}  // namespace

TEST_CASE("latency depends on the target only", "[playback][latency]") {
  struct Target {
    TargetKind kind;
    ChannelLayout layout;
    int expected;
  };
  const Target targets[] = {
      {TargetKind::Speakers, ChannelLayout::Stereo, 288},
      {TargetKind::Speakers, ChannelLayout::FivePointOne, 1312},
      {TargetKind::Speakers, ChannelLayout::SevenPointOne, 1312},
      {TargetKind::Headphones, ChannelLayout::Stereo, 1312},
  };
  const InputLayout inputs[] = {InputLayout::Mono, InputLayout::Stereo, InputLayout::FivePointOne,
                                InputLayout::SevenPointOne};
  for (const Target& target : targets) {
    for (InputLayout input : inputs) {
      PlaybackRenderer renderer(config_for(input, target.kind, target.layout), nullptr, 48000, 512);
      CHECK(renderer.latency_samples() == target.expected);
    }
  }
}
