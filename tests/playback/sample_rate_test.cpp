#include <catch2/catch_test_macros.hpp>

#include "playback/renderer.h"
#include "playback/upmix.h"

using sonare::ChannelLayout;
using namespace sonare::playback;

TEST_CASE("upmix frame length follows 21.3 ms at each rate", "[playback][sample-rate]") {
  CHECK(upmix_latency_frames(44100.0) == 1024);
  CHECK(upmix_latency_frames(48000.0) == 1024);
  CHECK(upmix_latency_frames(96000.0) == 2048);
}

TEST_CASE("headphone latency at 44.1 and 96 kHz", "[playback][sample-rate]") {
  RendererConfig config;
  config.prepare.input_layout = InputLayout::Stereo;
  PlaybackRenderer at_44k(config, nullptr, 44100, 512);
  PlaybackRenderer at_96k(config, nullptr, 96000, 512);
  CHECK(at_44k.latency_samples() == 1024 + 221 + 44);
  CHECK(at_96k.latency_samples() == 2048 + 480 + 96);
}
