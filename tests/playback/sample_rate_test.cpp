#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "playback/hrtf_set.h"
#include "playback/night_mode_drc.h"
#include "playback/renderer.h"
#include "playback/shrf_fixture.h"
#include "playback/upmix.h"
#include "util/constants.h"

using sonare::ChannelLayout;
using sonare::SpeakerRole;
using namespace sonare::playback;

namespace {

constexpr int kRates[] = {44100, 48000};
constexpr int kBlock = 256;

using Planes = std::vector<std::vector<float>>;

/// Renders impulses on the given input planes (all at frame @p at) and returns
/// @p frames of output.
Planes impulse_render(PlaybackRenderer& renderer, int in_channels,
                      const std::vector<int>& impulse_planes, int at, int frames) {
  const int out_count = renderer.output_channel_count();
  Planes in(static_cast<size_t>(in_channels), std::vector<float>(kBlock, 0.0f));
  Planes out(static_cast<size_t>(out_count), std::vector<float>(static_cast<size_t>(frames), 0.0f));
  std::vector<const float*> in_ptrs(static_cast<size_t>(in_channels));
  std::vector<float*> out_ptrs(static_cast<size_t>(out_count));
  for (int ch = 0; ch < in_channels; ++ch) in_ptrs[static_cast<size_t>(ch)] = in[ch].data();
  for (int start = 0; start < frames; start += kBlock) {
    for (auto& plane : in) std::fill(plane.begin(), plane.end(), 0.0f);
    if (at >= start && at < start + kBlock) {
      for (int p : impulse_planes)
        in[static_cast<size_t>(p)][static_cast<size_t>(at - start)] = 0.1f;
    }
    for (int ch = 0; ch < out_count; ++ch) {
      out_ptrs[static_cast<size_t>(ch)] = out[static_cast<size_t>(ch)].data() + start;
    }
    REQUIRE(
        renderer.process_planar(in_ptrs.data(), in_channels, out_ptrs.data(), out_count, kBlock));
  }
  return out;
}

/// Centroid (first moment) of a plane's response, in frames.
double centroid(const std::vector<float>& plane) {
  double weighted = 0.0;
  double sum = 0.0;
  for (size_t i = 0; i < plane.size(); ++i) {
    weighted += static_cast<double>(i) * plane[i];
    sum += plane[i];
  }
  return weighted / sum;
}

/// Lag (left relative to right, positive when the left ear lags) of the
/// cross-correlation peak, refined by parabolic interpolation.
double interaural_lag(const std::vector<float>& left, const std::vector<float>& right,
                      int max_lag) {
  const auto corr = [&](int lag) {
    double acc = 0.0;
    for (size_t i = 0; i < left.size(); ++i) {
      const long j = static_cast<long>(i) - lag;
      if (j >= 0 && j < static_cast<long>(right.size())) {
        acc += static_cast<double>(left[i]) * right[static_cast<size_t>(j)];
      }
    }
    return acc;
  };
  int best = -max_lag;
  double best_value = corr(best);
  for (int lag = -max_lag + 1; lag <= max_lag; ++lag) {
    const double value = corr(lag);
    if (value > best_value) {
      best_value = value;
      best = lag;
    }
  }
  const double a = corr(best - 1);
  const double c = corr(best + 1);
  const double denom = a - 2.0 * best_value + c;
  return best + (denom != 0.0 ? 0.5 * (a - c) / denom : 0.0);
}

RendererConfig speakers_51() {
  RendererConfig config;
  config.prepare.input_layout = InputLayout::FivePointOne;
  config.prepare.target_kind = TargetKind::Speakers;
  config.prepare.target_layout = ChannelLayout::FivePointOne;
  config.realtime.limiter_enabled = false;
  return config;
}

}  // namespace

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

TEST_CASE("the look-ahead stages keep their milliseconds at both rates",
          "[playback][sample-rate]") {
  double drc_ms[2] = {};
  double limiter_ms[2] = {};
  for (int r = 0; r < 2; ++r) {
    const int rate = kRates[r];
    PlaybackRenderer renderer(speakers_51(), nullptr, rate, 512);
    const RendererDiagnostics d = renderer.diagnostics();
    const double drc = d.stage_latency_q8[static_cast<size_t>(Stage::NightMode)] / 256.0;
    const double limiter = d.stage_latency_q8[static_cast<size_t>(Stage::OutputLimiter)] / 256.0;
    const double upmix = d.stage_latency_q8[static_cast<size_t>(Stage::Upmix)] / 256.0;
    // Each rate against the requested value, within half a frame of rounding.
    CHECK(std::abs(drc - kNightModeLookaheadMs * 1e-3 * rate) <= 0.5);
    CHECK(std::abs(limiter - kOutputLimiterLookaheadMs * 1e-3 * rate) <= 0.5);
    CHECK(upmix == static_cast<double>(upmix_latency_frames(rate)));
    CHECK(renderer.latency_samples() == static_cast<int>(drc + limiter + upmix));
    drc_ms[r] = drc * 1e3 / rate;
    limiter_ms[r] = limiter * 1e3 / rate;
  }
  // And the two rates against each other, within one frame of the lower rate.
  const double frame_ms = 1e3 / kRates[0];
  CHECK(std::abs(drc_ms[0] - drc_ms[1]) <= frame_ms);
  CHECK(std::abs(limiter_ms[0] - limiter_ms[1]) <= frame_ms);
}

TEST_CASE("distance compensation keeps its seconds end to end", "[playback][sample-rate]") {
  constexpr float kNear = 2.0f;
  constexpr float kFar = 3.0f;
  double delay_seconds[2] = {};
  for (int r = 0; r < 2; ++r) {
    const int rate = kRates[r];
    RendererConfig config = speakers_51();
    for (SpeakerRole role : {SpeakerRole::L, SpeakerRole::R}) {
      SpeakerPrepare& speaker = config.prepare.speakers[static_cast<size_t>(role)];
      speaker.has_distance = true;
      speaker.distance_m = role == SpeakerRole::L ? kNear : kFar;
    }
    PlaybackRenderer renderer(config, nullptr, rate, kBlock);
    const Planes out = impulse_render(renderer, 6, {0, 1}, 100, 2048);
    // L (near) waits for R (far); the whole path is linear, so the centroid
    // difference is the Lagrange fractional delay exactly.
    const double delay = centroid(out[0]) - centroid(out[1]);
    const double expected = (kFar - kNear) / sonare::constants::kSoundSpeedMps * rate;
    INFO("rate " << rate);
    CHECK(std::abs(delay - expected) <= 0.01);
    delay_seconds[r] = delay / rate;
  }
  CHECK(std::abs(delay_seconds[0] - delay_seconds[1]) <= 0.01 / kRates[0]);
}

TEST_CASE("the interaural delay keeps its seconds end to end", "[playback][sample-rate]") {
  // A 5-degree grid puts the +30 degree R speaker on a grid point: ITD = 24 sin 30 = 12
  // frames at the set's 48 kHz.
  test::ShrfFixtureSpec spec;
  spec.n_az = 72;
  spec.az_step_deg = 5.0f;
  spec.taps = 32;
  const std::vector<uint8_t> bytes = test::make_shrf_fixture(spec);
  const HrtfSet set = HrtfSet::from_memory(bytes.data(), bytes.size());
  const double itd_seconds = test::fixture_itd_samples(spec, 0, 6) / 48000.0;

  double measured_seconds[2] = {};
  for (int r = 0; r < 2; ++r) {
    const int rate = kRates[r];
    RendererConfig config;
    config.prepare.input_layout = InputLayout::FivePointOne;
    config.prepare.room_preset = RoomPreset::None;
    config.realtime.limiter_enabled = false;
    PlaybackRenderer renderer(config, &set, rate, kBlock);
    const Planes out = impulse_render(renderer, 6, {static_cast<int>(SpeakerRole::R)}, 100, 2048);
    const double lag = interaural_lag(out[0], out[1], 40);
    INFO("rate " << rate);
    CHECK(std::abs(lag - itd_seconds * rate) <= 0.1);
    measured_seconds[r] = lag / rate;
  }
  CHECK(std::abs(measured_seconds[0] - measured_seconds[1]) <= 0.1 / kRates[0]);
}
