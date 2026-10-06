/// @file mixing_wide_send_test.cpp
/// @brief Regression tests for wide and externally sourced channel-strip sends.

#include <array>
#include <vector>

#include "mixing_test_helpers.h"

namespace {

constexpr int kWideChannels = 12;
constexpr int kFrames = 8;

sonare::mixing::ChannelStripConfig test_strip_config() {
  sonare::mixing::ChannelStripConfig config;
  config.pan_law = sonare::mixing::PanLaw::Linear0dB;
  config.smoothing_ms = 0.0f;
  config.enable_metering = false;
  return config;
}

template <typename Pointer, size_t Channels>
void fill_planes(std::array<std::vector<float>, Channels>& planes,
                 std::array<Pointer, Channels>& pointers) {
  for (size_t channel = 0; channel < Channels; ++channel) {
    planes[channel].assign(kFrames, static_cast<float>(channel + 1));
    pointers[channel] = planes[channel].data();
  }
}

}  // namespace

TEST_CASE("ChannelStrip wide pre and post sends preserve every prepared plane",
          "[mixing][send][surround]") {
  auto strip = sonare::mixing::ChannelStrip(test_strip_config());
  // 7.1.4 has twelve planes and must be declared before prepare() sizes the
  // send scratch and tap banks.
  strip.set_prepared_channels(kWideChannels);
  const size_t pre_send = strip.add_send({0.0f, sonare::mixing::SendTiming::PreFader, 0.0f});
  const size_t post_send = strip.add_send({0.0f, sonare::mixing::SendTiming::PostFader, 0.0f});
  strip.prepare(48000.0, kFrames);
  strip.settle();

  std::array<std::vector<float>, kWideChannels> planes;
  std::array<float*, kWideChannels> channels{};
  fill_planes(planes, channels);
  strip.process(channels.data(), kWideChannels, kFrames);

  std::array<std::vector<float>, kWideChannels> pre_destination;
  std::array<std::vector<float>, kWideChannels> post_destination;
  std::array<float*, kWideChannels> pre_pointers{};
  std::array<float*, kWideChannels> post_pointers{};
  for (int channel = 0; channel < kWideChannels; ++channel) {
    pre_destination[static_cast<size_t>(channel)].assign(kFrames, 0.0f);
    post_destination[static_cast<size_t>(channel)].assign(kFrames, 0.0f);
    pre_pointers[static_cast<size_t>(channel)] =
        pre_destination[static_cast<size_t>(channel)].data();
    post_pointers[static_cast<size_t>(channel)] =
        post_destination[static_cast<size_t>(channel)].data();
  }

  strip.mix_send(pre_send, pre_pointers.data(), kWideChannels, kFrames);
  strip.mix_send(post_send, post_pointers.data(), kWideChannels, kFrames);

  for (int channel = 0; channel < kWideChannels; ++channel) {
    const float expected = static_cast<float>(channel + 1);
    for (int sample = 0; sample < kFrames; ++sample) {
      INFO("channel " << channel << ", sample " << sample);
      REQUIRE_THAT(pre_destination[static_cast<size_t>(channel)][static_cast<size_t>(sample)],
                   WithinAbs(expected, 0.0001f));
      REQUIRE_THAT(post_destination[static_cast<size_t>(channel)][static_cast<size_t>(sample)],
                   WithinAbs(expected, 0.0001f));
    }
  }
}

TEST_CASE("ChannelStrip widening after prepare keeps sparse wide sends processed",
          "[mixing][send][surround]") {
  auto config = test_strip_config();
  config.input_trim_db = -6.0206f;
  auto strip = sonare::mixing::ChannelStrip(config);
  // Prepare at the historical width, then widen the already prepared strip as
  // a graph host does when it discovers a surround layout. Keep both inserts
  // in the path so a sparse wide block must materialize its null stand-in
  // table instead of silently skipping the insert chain.
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(2.0f));
  strip.add_post_insert(std::make_unique<ScaleProcessor>(3.0f));
  const size_t pre_send = strip.add_send({0.0f, sonare::mixing::SendTiming::PreFader, 0.0f});
  const size_t post_send = strip.add_send({0.0f, sonare::mixing::SendTiming::PostFader, 0.0f});
  strip.prepare(48000.0, kFrames);
  strip.set_prepared_channels(kWideChannels);
  strip.settle();

  std::array<std::vector<float>, kWideChannels> planes;
  std::array<float*, kWideChannels> channels{};
  fill_planes(planes, channels);
  // Keep a hole in the middle while upper planes still carry signal. This
  // forces the null-plane staging path for the full twelve-channel block.
  channels[9] = nullptr;
  strip.process(channels.data(), kWideChannels, kFrames);

  std::array<std::vector<float>, kWideChannels> pre_destination;
  std::array<std::vector<float>, kWideChannels> post_destination;
  std::array<float*, kWideChannels> pre_pointers{};
  std::array<float*, kWideChannels> post_pointers{};
  for (int channel = 0; channel < kWideChannels; ++channel) {
    pre_destination[static_cast<size_t>(channel)].assign(kFrames, 0.0f);
    post_destination[static_cast<size_t>(channel)].assign(kFrames, 0.0f);
    pre_pointers[static_cast<size_t>(channel)] =
        pre_destination[static_cast<size_t>(channel)].data();
    post_pointers[static_cast<size_t>(channel)] =
        post_destination[static_cast<size_t>(channel)].data();
  }
  strip.mix_send(pre_send, pre_pointers.data(), kWideChannels, kFrames);
  strip.mix_send(post_send, post_pointers.data(), kWideChannels, kFrames);

  const float expected_pre_gain = sonare::db_to_linear(-6.0206f) * 2.0f;
  const float expected_post_gain = expected_pre_gain * 3.0f;
  for (int channel = 0; channel < kWideChannels; ++channel) {
    for (int sample = 0; sample < kFrames; ++sample) {
      INFO("channel " << channel << ", sample " << sample);
      const float expected_pre = channel == 9 ? 0.0f : expected_pre_gain * (channel + 1);
      const float expected_post = channel == 9 ? 0.0f : expected_post_gain * (channel + 1);
      REQUIRE_THAT(pre_destination[static_cast<size_t>(channel)][static_cast<size_t>(sample)],
                   WithinAbs(expected_pre, 0.0005f));
      REQUIRE_THAT(post_destination[static_cast<size_t>(channel)][static_cast<size_t>(sample)],
                   WithinAbs(expected_post, 0.0005f));
    }
  }
}

TEST_CASE("ChannelStrip externally sourced wide send preserves every prepared plane",
          "[mixing][send][surround]") {
  auto strip = sonare::mixing::ChannelStrip(test_strip_config());
  strip.set_prepared_channels(kWideChannels);
  const size_t send = strip.add_send({0.0f, sonare::mixing::SendTiming::PostFader, 0.0f});
  strip.prepare(48000.0, kFrames);

  std::array<std::vector<float>, kWideChannels> source_planes;
  std::array<const float*, kWideChannels> source{};
  fill_planes(source_planes, source);

  std::array<std::vector<float>, kWideChannels> destination_planes;
  std::array<float*, kWideChannels> destination{};
  for (int channel = 0; channel < kWideChannels; ++channel) {
    destination_planes[static_cast<size_t>(channel)].assign(kFrames, 0.0f);
    destination[static_cast<size_t>(channel)] =
        destination_planes[static_cast<size_t>(channel)].data();
  }

  strip.mix_send_from_at(send, source.data(), destination.data(), kWideChannels, kFrames, 0);

  for (int channel = 0; channel < kWideChannels; ++channel) {
    const float expected = static_cast<float>(channel + 1);
    for (int sample = 0; sample < kFrames; ++sample) {
      INFO("channel " << channel << ", sample " << sample);
      REQUIRE_THAT(destination_planes[static_cast<size_t>(channel)][static_cast<size_t>(sample)],
                   WithinAbs(expected, 0.0001f));
    }
  }
}

TEST_CASE("ChannelStrip wide send automation changes every plane at its sample offset",
          "[mixing][send][automation][surround]") {
  constexpr int kBlockStart = 100;
  constexpr int kOffset = 2;
  auto strip = sonare::mixing::ChannelStrip(test_strip_config());
  strip.set_prepared_channels(kWideChannels);
  const size_t send = strip.add_send({0.0f, sonare::mixing::SendTiming::PostFader, 0.0f});
  strip.prepare(48000.0, kFrames);
  REQUIRE(strip.schedule_send_automation(send, kBlockStart + kOffset, -6.0206f,
                                         sonare::AutomationCurve::Hold));

  std::array<std::vector<float>, kWideChannels> planes;
  std::array<float*, kWideChannels> channels{};
  fill_planes(planes, channels);
  strip.process_at(channels.data(), kWideChannels, kFrames, kBlockStart);

  std::array<std::vector<float>, kWideChannels> destination_planes;
  std::array<float*, kWideChannels> destination{};
  for (int channel = 0; channel < kWideChannels; ++channel) {
    destination_planes[static_cast<size_t>(channel)].assign(kFrames, 0.0f);
    destination[static_cast<size_t>(channel)] =
        destination_planes[static_cast<size_t>(channel)].data();
  }
  strip.mix_send_at(send, destination.data(), kWideChannels, kFrames, kBlockStart);

  for (int channel = 0; channel < kWideChannels; ++channel) {
    const float source = static_cast<float>(channel + 1);
    for (int sample = 0; sample < kFrames; ++sample) {
      const float expected = sample < kOffset ? source : 0.5f * source;
      INFO("channel " << channel << ", sample " << sample);
      REQUIRE_THAT(destination_planes[static_cast<size_t>(channel)][static_cast<size_t>(sample)],
                   WithinAbs(expected, 0.0001f));
    }
  }
}

TEST_CASE("ChannelStrip narrow external send keeps null source and destination planes safe",
          "[mixing][send]") {
  constexpr int kChannels = 4;
  auto strip = sonare::mixing::ChannelStrip(test_strip_config());
  REQUIRE(strip.prepared_channels() == 8);
  const size_t send = strip.add_send({0.0f, sonare::mixing::SendTiming::PostFader, 0.0f});
  strip.prepare(48000.0, kFrames);

  std::array<float, kFrames> source_left{};
  std::array<float, kFrames> source_right{};
  std::array<float, kFrames> source_surround{};
  source_left.fill(1.0f);
  source_right.fill(2.0f);
  source_surround.fill(4.0f);
  const float* source[] = {source_left.data(), nullptr, source_right.data(),
                           source_surround.data()};

  std::array<float, kFrames> destination_left{};
  std::array<float, kFrames> destination_surround{};
  std::array<float, kFrames> destination_last{};
  float* destination[] = {destination_left.data(), nullptr, destination_surround.data(),
                          destination_last.data()};

  strip.mix_send_from_at(send, source, destination, kChannels, kFrames, 0);

  for (int sample = 0; sample < kFrames; ++sample) {
    REQUIRE_THAT(destination_left[static_cast<size_t>(sample)], WithinAbs(1.0f, 0.0001f));
    REQUIRE_THAT(destination_surround[static_cast<size_t>(sample)], WithinAbs(2.0f, 0.0001f));
    REQUIRE_THAT(destination_last[static_cast<size_t>(sample)], WithinAbs(4.0f, 0.0001f));
  }
}
