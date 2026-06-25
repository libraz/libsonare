#include "engine/track_mixer.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>

#include "rt/processor_base.h"

namespace {

class GainProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit GainProcessor(float gain) : gain_(gain) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] *= gain_;
      }
    }
  }
  void reset() override {}

 private:
  float gain_ = 1.0f;
};

sonare::engine::ClipSchedule clip_for_track(uint32_t clip_id, uint32_t track_id,
                                            const float* const* samples, int channels, int frames,
                                            float gain = 1.0f) {
  sonare::engine::ClipSchedule clip{
      clip_id, {samples, channels, frames}, 0.0, 0, 0, frames, false, gain, 0, 0};
  clip.track_id = track_id;
  return clip;
}

}  // namespace

TEST_CASE("TrackMixerRuntime routes clip tracks into independent lanes", "[engine][track_mixer]") {
  std::array<float, 4> source_a_l{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> source_a_r{0.5f, 0.5f, 0.5f, 0.5f};
  std::array<float, 4> source_b_l{0.25f, 0.25f, 0.25f, 0.25f};
  std::array<float, 4> source_b_r{0.75f, 0.75f, 0.75f, 0.75f};
  const float* a[] = {source_a_l.data(), source_a_r.data()};
  const float* b[] = {source_b_l.data(), source_b_r.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 4);
  player.set_clips({clip_for_track(1, 10, a, 2, 4), clip_for_track(2, 20, b, 2, 4)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 4);
  REQUIRE(mixer.set_track_lanes({{10}, {20}}));

  std::array<float, 4> out_l{};
  std::array<float, 4> out_r{};
  float* out[] = {out_l.data(), out_r.data()};
  REQUIRE(mixer.render_clips(player, out, 2, 4, 0));

  REQUIRE(out_l[0] == 1.25f);
  REQUIRE(out_r[3] == 1.25f);
}

TEST_CASE("TrackMixerRuntime keeps unknown clip tracks on the main bus", "[engine][track_mixer]") {
  std::array<float, 4> source_a{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> source_unknown{0.5f, 0.5f, 0.5f, 0.5f};
  const float* a[] = {source_a.data()};
  const float* unknown[] = {source_unknown.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 4);
  player.set_clips({clip_for_track(1, 10, a, 1, 4), clip_for_track(2, 99, unknown, 1, 4)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 4);
  REQUIRE(mixer.set_track_lanes({{10}}));

  std::array<float, 4> out_l{};
  float* out[] = {out_l.data()};
  REQUIRE(mixer.render_clips(player, out, 1, 4, 0));

  REQUIRE(out_l[0] > 1.49f);
  REQUIRE(out_l[0] < 1.51f);
}

TEST_CASE("TrackMixerRuntime validates lane snapshots", "[engine][track_mixer]") {
  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 4);

  REQUIRE_FALSE(mixer.set_track_lanes({{0}}));
  REQUIRE_FALSE(mixer.set_track_lanes({{1}, {1}}));

  std::vector<sonare::engine::TrackLaneConfig> too_many;
  too_many.resize(sonare::engine::TrackMixerRuntime::kMaxTrackLanes + 1);
  for (size_t i = 0; i < too_many.size(); ++i) {
    too_many[i].track_id = static_cast<uint32_t>(i + 1);
  }
  REQUIRE_FALSE(mixer.set_track_lanes(std::move(too_many)));
}

TEST_CASE("TrackMixerRuntime aligns strip latency across active lanes", "[engine][track_mixer]") {
  std::array<float, 16> latent_source{};
  std::array<float, 16> dry_source{};
  latent_source[0] = 1.0f;
  dry_source[0] = 1.0f;
  const float* latent[] = {latent_source.data()};
  const float* dry[] = {dry_source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 16);
  player.set_clips({clip_for_track(1, 10, latent, 1, 16), clip_for_track(2, 20, dry, 1, 16)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 16);
  REQUIRE(mixer.set_track_lanes({{10}, {20}}));

  sonare::mixing::ChannelStrip latent_strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  latent_strip.set_channel_delay_samples(4);
  REQUIRE(mixer.bind_track_strip(10, &latent_strip));
  REQUIRE(mixer.latency_samples_q8() == (4 << 8));

  std::array<float, 16> out_l{};
  float* out[] = {out_l.data()};
  REQUIRE(mixer.render_clips(player, out, 1, 16, 0));

  REQUIRE(out_l[0] == 0.0f);
  REQUIRE(out_l[1] == 0.0f);
  REQUIRE(out_l[2] == 0.0f);
  REQUIRE(out_l[3] == 0.0f);
  REQUIRE(out_l[4] > 2.3f);
  REQUIRE(out_l[4] < 2.5f);
}

TEST_CASE("TrackMixerRuntime mixes post-fader sends into buses", "[engine][track_mixer]") {
  std::array<float, 16> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 16);
  player.set_clips({clip_for_track(1, 10, channels, 1, 16)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 16);
  REQUIRE(mixer.set_buses({{1, 0.0f}}));

  sonare::engine::TrackLaneConfig lane{10};
  lane.sends.push_back({1, 0.0f, true});
  REQUIRE(mixer.set_track_lanes({lane}));

  std::array<float, 16> out_l{};
  float* out[] = {out_l.data()};
  REQUIRE(mixer.render_clips(player, out, 1, 16, 0));
  REQUIRE(out_l.back() > 2.82f);
  REQUIRE(out_l.back() < 2.84f);

  lane.sends[0].level_db = -6.0206f;
  REQUIRE(mixer.set_track_lanes({lane}));
  out_l.fill(0.0f);
  REQUIRE(mixer.render_clips(player, out, 1, 16, 0));
  REQUIRE(out_l.back() > 2.11f);
  REQUIRE(out_l.back() < 2.13f);

  lane.sends[0].enabled = false;
  REQUIRE(mixer.set_track_lanes({lane}));
  out_l.fill(0.0f);
  REQUIRE(mixer.render_clips(player, out, 1, 16, 0));
  REQUIRE(out_l.back() > 1.41f);
  REQUIRE(out_l.back() < 1.42f);
}

TEST_CASE("TrackMixerRuntime validates buses and routes sends through bus strip",
          "[engine][track_mixer]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 4;
  std::array<float, kFrames> source{};
  for (int i = 0; i < kFrames; ++i) {
    source[static_cast<size_t>(i)] = 0.25f * std::sin(2.0f * 3.14159265358979323846f * 1000.0f *
                                                      static_cast<float>(i) / 48000.0f);
  }
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kBlock);
  player.set_clips({clip_for_track(1, 10, channels, 1, kFrames)});

  sonare::engine::TrackMixerRuntime flat;
  flat.prepare(48000.0, kBlock);
  REQUIRE(flat.set_buses({{1, -120.0f}}));
  sonare::engine::TrackLaneConfig flat_lane{10};
  flat_lane.sends.push_back({1, 0.0f, true});
  REQUIRE(flat.set_track_lanes({flat_lane}));
  std::array<float, kBlock> flat_out{};
  float* flat_io[] = {flat_out.data()};
  REQUIRE(flat.render_clips(player, flat_io, 1, kBlock, 0));

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE_FALSE(mixer.set_buses({{1, 0.0f}, {1, 0.0f}}));
  REQUIRE(mixer.set_buses({{1, -120.0f}, {2, 0.0f}}));
  sonare::engine::TrackLaneConfig bad_lane{10};
  bad_lane.sends.push_back({99, 0.0f, true});
  REQUIRE_FALSE(mixer.set_track_lanes({bad_lane}));
  sonare::engine::TrackLaneConfig dup_lane{10};
  dup_lane.sends.push_back({1, 0.0f, true});
  dup_lane.sends.push_back({1, -6.0f, true});
  REQUIRE_FALSE(mixer.set_track_lanes({dup_lane}));

  sonare::engine::TrackLaneConfig lane{10};
  lane.sends.push_back({2, 0.0f, true});
  REQUIRE(mixer.set_track_lanes({lane}));
  sonare::mixing::api::Bus bus;
  bus.id = "2";
  bus.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "eq.parametric",
       R"({"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":12,"band0.enabled":1})"});
  REQUIRE(mixer.set_bus_strip(2, bus));

  std::array<float, kBlock> eq_out{};
  float* eq_io[] = {eq_out.data()};
  for (int block = 0; block < 6; ++block) {
    eq_out.fill(0.0f);
    REQUIRE(mixer.render_clips(player, eq_io, 1, kBlock, 0));
  }

  auto rms = [](const std::array<float, kBlock>& samples) {
    double sum = 0.0;
    for (float sample : samples) {
      sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
  };
  REQUIRE(rms(eq_out) > rms(flat_out) * 1.5);
}

TEST_CASE("TrackMixerRuntime applies lane fader pan and solo mute", "[engine][track_mixer]") {
  std::array<float, 256> source_a_l{};
  std::array<float, 256> source_a_r{};
  std::array<float, 256> source_b_l{};
  std::array<float, 256> source_b_r{};
  source_a_l.fill(1.0f);
  source_a_r.fill(1.0f);
  source_b_l.fill(1.0f);
  source_b_r.fill(1.0f);
  const float* a[] = {source_a_l.data(), source_a_r.data()};
  const float* b[] = {source_b_l.data(), source_b_r.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 256);
  player.set_clips({clip_for_track(1, 10, a, 2, 256), clip_for_track(2, 20, b, 2, 256)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 256);
  REQUIRE(mixer.set_track_lanes({{10}, {20}}));

  std::array<float, 256> out_l{};
  std::array<float, 256> out_r{};
  float* out[] = {out_l.data(), out_r.data()};
  REQUIRE(mixer.render_clips(player, out, 2, 256, 0));
  REQUIRE(mixer.set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kFaderDb, -12.0f));
  REQUIRE(mixer.set_lane_parameter(1, sonare::engine::TrackMixerRuntime::kPan, 1.0f));
  out_l.fill(0.0f);
  out_r.fill(0.0f);
  REQUIRE(mixer.render_clips(player, out, 2, 256, 0));
  REQUIRE(out_l.back() < out_r.back());
  REQUIRE(out_l.back() > 0.2f);

  REQUIRE(mixer.set_lane_solo_mute(0, true, false));
  for (int block = 0; block < 4; ++block) {
    out_l.fill(0.0f);
    out_r.fill(0.0f);
    REQUIRE(mixer.render_clips(player, out, 2, 256, 0));
  }
  REQUIRE(out_l.back() < 0.45f);
  REQUIRE(out_r.back() < 0.45f);

  REQUIRE(mixer.set_lane_solo_mute(0, true, true));
  for (int block = 0; block < 4; ++block) {
    out_l.fill(0.0f);
    out_r.fill(0.0f);
    REQUIRE(mixer.render_clips(player, out, 2, 256, 0));
  }
  REQUIRE(out_l.back() < 0.1f);
  REQUIRE(out_r.back() < 0.1f);
}

TEST_CASE("TrackMixerRuntime applies repeated lane commands without a republish",
          "[engine][track_mixer]") {
  // Regression for the lane-remap skip: when the published config is unchanged,
  // the hot fader/solo/mute path skips the LaneState remap. Repeated commands
  // on different lanes must still each land on the correct lane (the skip must
  // not stomp or misroute already-applied state).
  std::array<float, 256> source_a{};
  std::array<float, 256> source_b{};
  source_a.fill(1.0f);
  source_b.fill(1.0f);
  const float* a[] = {source_a.data()};
  const float* b[] = {source_b.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 256);
  player.set_clips({clip_for_track(1, 10, a, 1, 256), clip_for_track(2, 20, b, 1, 256)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 256);
  REQUIRE(mixer.set_track_lanes({{10}, {20}}));

  std::array<float, 256> out_l{};
  float* out[] = {out_l.data()};
  REQUIRE(mixer.render_clips(player, out, 1, 256, 0));

  // Two commands in a row with no intervening republish: the second hits the
  // remap-skip path. Pull lane 0 down hard and leave lane 1 alone.
  REQUIRE(mixer.set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kFaderDb, -60.0f));
  REQUIRE(mixer.set_lane_parameter(1, sonare::engine::TrackMixerRuntime::kFaderDb, 0.0f));
  for (int block = 0; block < 8; ++block) {
    out_l.fill(0.0f);
    REQUIRE(mixer.render_clips(player, out, 1, 256, 0));
  }
  // Only lane 1 (unity) survives -> ~1.0, not ~2.0 (both) or near-silence.
  REQUIRE(out_l.back() > 0.85f);
  REQUIRE(out_l.back() < 1.15f);
}

TEST_CASE("TrackMixerRuntime carries lane smoother state by track id across reorders",
          "[engine][track_mixer]") {
  std::array<float, 256> source_a{};
  std::array<float, 256> source_b{};
  source_a.fill(1.0f);
  source_b.fill(1.0f);
  const float* a[] = {source_a.data()};
  const float* b[] = {source_b.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 256);
  player.set_clips({clip_for_track(1, 10, a, 1, 256), clip_for_track(2, 20, b, 1, 256)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 256);
  REQUIRE(mixer.set_track_lanes({{10}, {20}}));

  std::array<float, 256> out_l{};
  float* out[] = {out_l.data()};
  REQUIRE(mixer.render_clips(player, out, 1, 256, 0));
  REQUIRE(mixer.set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kFaderDb, -12.0f));
  for (int block = 0; block < 8; ++block) {
    out_l.fill(0.0f);
    REQUIRE(mixer.render_clips(player, out, 1, 256, 0));
  }
  REQUIRE(out_l.back() > 1.20f);
  REQUIRE(out_l.back() < 1.35f);

  REQUIRE(mixer.set_track_lanes({{20}, {10}}));
  out_l.fill(0.0f);
  REQUIRE(mixer.render_clips(player, out, 1, 256, 0));
  REQUIRE(out_l.back() > 1.20f);
  REQUIRE(out_l.back() < 1.35f);
}

TEST_CASE("TrackMixerRuntime carries solo mute state by track id across remove and re-add",
          "[engine][track_mixer]") {
  std::array<float, 256> source_a{};
  std::array<float, 256> source_b{};
  source_a.fill(1.0f);
  source_b.fill(1.0f);
  const float* a[] = {source_a.data()};
  const float* b[] = {source_b.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 256);
  player.set_clips({clip_for_track(1, 10, a, 1, 256), clip_for_track(2, 20, b, 1, 256)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 256);
  REQUIRE(mixer.set_track_lanes({{10}, {20}}));
  REQUIRE(mixer.set_lane_solo_mute(0, true, false));

  std::array<float, 256> out_l{};
  float* out[] = {out_l.data()};
  for (int block = 0; block < 8; ++block) {
    out_l.fill(0.0f);
    REQUIRE(mixer.render_clips(player, out, 1, 256, 0));
  }
  REQUIRE(out_l.back() > 0.95f);
  REQUIRE(out_l.back() < 1.05f);

  REQUIRE(mixer.set_track_lanes({{20}}));
  out_l.fill(0.0f);
  REQUIRE(mixer.render_clips(player, out, 1, 256, 0));
  REQUIRE(out_l.back() > 1.3f);
  REQUIRE(out_l.back() < 2.1f);

  REQUIRE(mixer.set_track_lanes({{20}, {10}}));
  for (int block = 0; block < 4; ++block) {
    out_l.fill(0.0f);
    REQUIRE(mixer.render_clips(player, out, 1, 256, 0));
  }
  REQUIRE(out_l.back() > 0.95f);
  REQUIRE(out_l.back() < 1.05f);
}

TEST_CASE("TrackMixerRuntime processes bound ChannelStrip for a track lane",
          "[engine][track_mixer]") {
  std::array<float, 256> source_a{};
  std::array<float, 256> source_b{};
  source_a.fill(1.0f);
  source_b.fill(1.0f);
  const float* a[] = {source_a.data()};
  const float* b[] = {source_b.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 256);
  player.set_clips({clip_for_track(1, 10, a, 1, 256), clip_for_track(2, 20, b, 1, 256)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 256);
  REQUIRE(mixer.set_track_lanes({{10}, {20}}));

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<GainProcessor>(0.25f));
  REQUIRE(mixer.bind_track_strip(10, &strip));

  std::array<float, 256> out_l{};
  float* out[] = {out_l.data()};
  REQUIRE(mixer.render_clips(player, out, 1, 256, 0));
  REQUIRE(out_l.back() > 1.20f);
  REQUIRE(out_l.back() < 1.40f);

  REQUIRE(mixer.set_track_lanes({{20}, {10}}));
  out_l.fill(0.0f);
  REQUIRE(mixer.render_clips(player, out, 1, 256, 0));
  REQUIRE(out_l.back() > 1.20f);
  REQUIRE(out_l.back() < 1.40f);
}

TEST_CASE("TrackMixerRuntime applies scene EQ insert for a track lane", "[engine][track_mixer]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 4;
  std::array<float, kFrames> source{};
  for (int i = 0; i < kFrames; ++i) {
    source[static_cast<size_t>(i)] = 0.25f * std::sin(2.0f * 3.14159265358979323846f * 100.0f *
                                                      static_cast<float>(i) / 48000.0f);
  }
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kBlock);
  player.set_clips({clip_for_track(1, 10, channels, 1, kFrames)});

  sonare::engine::TrackMixerRuntime flat;
  flat.prepare(48000.0, kBlock);
  REQUIRE(flat.set_track_lanes({{10}}));
  std::array<float, kBlock> flat_out{};
  float* flat_io[] = {flat_out.data()};
  REQUIRE(flat.render_clips(player, flat_io, 1, kBlock, 0));

  sonare::engine::TrackMixerRuntime eq;
  eq.prepare(48000.0, kBlock);
  REQUIRE(eq.set_track_lanes({{10}}));
  sonare::mixing::api::Strip strip_spec;
  strip_spec.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "eq.parametric",
       R"({"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":12,"band0.enabled":1})"});
  REQUIRE(eq.set_track_strip(10, strip_spec));

  std::array<float, kBlock> eq_out{};
  float* eq_io[] = {eq_out.data()};
  for (int block = 0; block < 6; ++block) {
    eq_out.fill(0.0f);
    REQUIRE(eq.render_clips(player, eq_io, 1, kBlock, 0));
  }

  auto rms = [](const std::array<float, kBlock>& samples) {
    double sum = 0.0;
    for (float sample : samples) {
      sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
  };
  REQUIRE(rms(eq_out) > rms(flat_out) * 1.5);

  REQUIRE_FALSE(eq.set_track_insert_bypassed(10, 7, true));
  REQUIRE(eq.set_track_insert_bypassed(10, 0, true, true));
  std::array<float, kBlock> bypassed_out{};
  float* bypassed_io[] = {bypassed_out.data()};
  REQUIRE(eq.render_clips(player, bypassed_io, 1, kBlock, 0));
  REQUIRE(std::abs(rms(bypassed_out) - rms(flat_out)) < 0.001);
}

TEST_CASE("TrackMixerRuntime applies embedded EQ band changes", "[engine][track_mixer]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 4;
  std::array<float, kFrames> source{};
  for (int i = 0; i < kFrames; ++i) {
    source[static_cast<size_t>(i)] =
        std::sin(2.0f * 3.14159265358979323846f * 1000.0f * static_cast<float>(i) / 48000.0f);
  }
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kBlock);
  player.set_clips({clip_for_track(1, 10, channels, 1, kFrames)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE(mixer.set_track_lanes({{10}}));
  sonare::mixing::api::Strip strip_spec;
  REQUIRE(mixer.set_track_strip(10, strip_spec));

  std::array<float, kBlock> flat_out{};
  float* flat_io[] = {flat_out.data()};
  REQUIRE(mixer.render_clips(player, flat_io, 1, kBlock, 0));

  sonare::mastering::eq::EqBand band{sonare::mastering::eq::EqBandType::Peak, 1000.0f, 12.0f, 1.0f,
                                     true};
  REQUIRE_FALSE(mixer.set_track_eq_band(99, 0, band));
  REQUIRE(mixer.set_track_eq_band(10, 0, band));
  std::array<float, kBlock> eq_out{};
  float* eq_io[] = {eq_out.data()};
  for (int block = 0; block < 4; ++block) {
    eq_out.fill(0.0f);
    REQUIRE(mixer.render_clips(player, eq_io, 1, kBlock, 0));
  }

  auto rms = [](const std::array<float, kBlock>& samples) {
    double sum = 0.0;
    for (float sample : samples) {
      sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
  };
  REQUIRE(rms(eq_out) > rms(flat_out) * 1.5);
}

TEST_CASE("TrackMixerRuntime scatters a lane across a surround master",
          "[engine][track_mixer][surround]") {
  constexpr int kBlock = 16;
  std::array<float, kBlock> src_l{};
  std::array<float, kBlock> src_r{};
  src_l.fill(1.0f);
  src_r.fill(1.0f);
  float* source[] = {src_l.data(), src_r.data()};

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE(mixer.set_track_lanes({{10}}));

  // Strip panned hard to the surround-left speaker (Ls @ -110 deg in 5.1).
  sonare::mixing::api::Strip spec;
  spec.id = "vox";
  spec.surround_pan.azimuth = -110.0f;
  REQUIRE(mixer.set_track_strip(10, spec));
  mixer.settle_smoothers();

  // 6-channel master mix: L R C LFE Ls Rs.
  std::array<std::array<float, kBlock>, 6> planes{};
  std::array<float*, 6> out{};
  for (int c = 0; c < 6; ++c) {
    out[static_cast<size_t>(c)] = planes[static_cast<size_t>(c)].data();
  }

  REQUIRE(mixer.mix_source(10, source, out.data(), 6, kBlock));

  // The block's final sample is at the fully-ramped target gain: all energy in
  // Ls (plane 4), the other planes (incl. LFE) silent.
  REQUIRE(planes[4].back() > 0.9f);
  for (int c : {0, 1, 2, 3, 5}) {
    REQUIRE(std::abs(planes[static_cast<size_t>(c)].back()) < 1e-4f);
  }
}

TEST_CASE("TrackMixerRuntime stereo render ignores surround pan",
          "[engine][track_mixer][surround]") {
  constexpr int kBlock = 16;
  std::array<float, kBlock> src_l{};
  std::array<float, kBlock> src_r{};
  src_l.fill(1.0f);
  src_r.fill(1.0f);
  float* source[] = {src_l.data(), src_r.data()};

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE(mixer.set_track_lanes({{10}}));
  sonare::mixing::api::Strip spec;
  spec.id = "vox";
  spec.surround_pan.azimuth = -110.0f;  // must not affect the stereo path
  REQUIRE(mixer.set_track_strip(10, spec));
  mixer.settle_smoothers();

  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  float* out[] = {out_l.data(), out_r.data()};
  REQUIRE(mixer.mix_source(10, source, out, 2, kBlock));

  // A centered stereo source stays centered: both channels carry equal energy.
  REQUIRE(out_l.back() > 0.9f);
  REQUIRE(out_r.back() > 0.9f);
  REQUIRE(std::abs(out_l.back() - out_r.back()) < 1e-4f);
}

TEST_CASE("TrackMixerRuntime scatters a lane through a surround group bus into the master",
          "[engine][track_mixer][surround]") {
  constexpr int kBlock = 16;
  std::array<float, kBlock> src_l{};
  std::array<float, kBlock> src_r{};
  src_l.fill(1.0f);
  src_r.fill(1.0f);
  float* source[] = {src_l.data(), src_r.data()};

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  // A 5.1 group bus; the lane sums its post-fader output into it instead of the
  // master, and the bus then carries it through to the 5.1 master.
  REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::FivePointOne}}));
  sonare::engine::TrackLaneConfig lane{10};
  lane.output_bus_id = 1;
  REQUIRE(mixer.set_track_lanes({lane}));

  sonare::mixing::api::Strip spec;
  spec.id = "vox";
  spec.surround_pan.azimuth = -110.0f;  // Ls
  REQUIRE(mixer.set_track_strip(10, spec));
  mixer.settle_smoothers();

  std::array<std::array<float, kBlock>, 6> planes{};
  std::array<float*, 6> out{};
  for (int c = 0; c < 6; ++c) {
    out[static_cast<size_t>(c)] = planes[static_cast<size_t>(c)].data();
  }
  REQUIRE(mixer.mix_source(10, source, out.data(), 6, kBlock));

  // The lane is panned to Ls (plane 4) and reaches the master through the bus;
  // the other planes (incl. LFE) stay silent.
  REQUIRE(planes[4].back() > 0.9f);
  for (int c : {0, 1, 2, 3, 5}) {
    REQUIRE(std::abs(planes[static_cast<size_t>(c)].back()) < 1e-4f);
  }
}

TEST_CASE("TrackMixerRuntime surround group bus applies its gain to every plane",
          "[engine][track_mixer][surround]") {
  constexpr int kBlock = 16;
  std::array<float, kBlock> src_l{};
  std::array<float, kBlock> src_r{};
  src_l.fill(1.0f);
  src_r.fill(1.0f);
  float* source[] = {src_l.data(), src_r.data()};

  auto ls_at_bus_gain = [&](float bus_gain_db) {
    sonare::engine::TrackMixerRuntime mixer;
    mixer.prepare(48000.0, kBlock);
    REQUIRE(mixer.set_buses({{1, bus_gain_db, sonare::ChannelLayout::FivePointOne}}));
    sonare::engine::TrackLaneConfig lane{10};
    lane.output_bus_id = 1;
    REQUIRE(mixer.set_track_lanes({lane}));
    sonare::mixing::api::Strip spec;
    spec.id = "vox";
    spec.surround_pan.azimuth = -110.0f;  // Ls
    REQUIRE(mixer.set_track_strip(10, spec));
    mixer.settle_smoothers();
    std::array<std::array<float, kBlock>, 6> planes{};
    std::array<float*, 6> out{};
    for (int c = 0; c < 6; ++c) {
      out[static_cast<size_t>(c)] = planes[static_cast<size_t>(c)].data();
    }
    REQUIRE(mixer.mix_source(10, source, out.data(), 6, kBlock));
    return planes[4].back();
  };

  // A -6 dB bus halves the surround plane the lane was scattered into.
  const float unity = ls_at_bus_gain(0.0f);
  const float halved = ls_at_bus_gain(-6.0205999f);
  REQUIRE(unity > 0.9f);
  REQUIRE(std::abs(halved - 0.5f * unity) < 0.01f * unity);
}

TEST_CASE("TrackMixerRuntime surround group bus feeds eq.midSide a 2-plane view",
          "[engine][track_mixer][surround]") {
  // eq.midSide aborts on a non-stereo width. Routed onto a 5.1 group bus its
  // catalog StereoPairOnly policy must clamp it to the front pair so the
  // surround render does not terminate (the throw would escape the noexcept
  // mix path otherwise).
  constexpr int kBlock = 16;
  std::array<float, kBlock> src_l{};
  std::array<float, kBlock> src_r{};
  src_l.fill(1.0f);
  src_r.fill(1.0f);
  float* source[] = {src_l.data(), src_r.data()};

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::FivePointOne}}));
  sonare::engine::TrackLaneConfig lane{10};
  lane.output_bus_id = 1;
  REQUIRE(mixer.set_track_lanes({lane}));

  sonare::mixing::api::Strip spec;
  spec.id = "vox";
  spec.surround_pan.azimuth = -110.0f;  // Ls
  REQUIRE(mixer.set_track_strip(10, spec));

  sonare::mixing::api::Bus bus;
  bus.id = "1";
  bus.inserts.push_back({sonare::mixing::api::InsertSlot::PreFader, "eq.midSide", "{}"});
  REQUIRE(mixer.set_bus_strip(1, bus));
  mixer.settle_smoothers();

  std::array<std::array<float, kBlock>, 6> planes{};
  std::array<float*, 6> out{};
  for (int c = 0; c < 6; ++c) {
    out[static_cast<size_t>(c)] = planes[static_cast<size_t>(c)].data();
  }
  // No abort: the SPO clamp keeps eq.midSide on the front pair while the lane's
  // Ls energy passes through the bus untouched on plane 4.
  REQUIRE(mixer.mix_source(10, source, out.data(), 6, kBlock));
  REQUIRE(planes[4].back() > 0.9f);
}

TEST_CASE("TrackMixerRuntime stages a multi-source rack through a shared bus once per block",
          "[engine][track_mixer]") {
  using Catch::Approx;
  // Two sources routed to one stateful bus (an FDN reverb) must drive that bus
  // with the SUM of their sends and advance its tail exactly once per block. The
  // staged begin/into-lane/finish path is therefore bit-identical to a single
  // lane carrying the combined source through the same bus -- whereas calling
  // mix_source() per source would clear and re-process the reverb once per
  // source, advancing the tail twice per block. Running several blocks lets that
  // time dilation accumulate into the reverb tail so the equivalence is sensitive
  // to it, not just to the first (near-dry) block.
  constexpr int kBlock = 64;
  constexpr int kBlocks = 8;

  auto make_reverb_bus = [](sonare::engine::TrackMixerRuntime& mixer) {
    REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));
    sonare::mixing::api::Bus bus;
    bus.id = "1";
    bus.inserts.push_back({sonare::mixing::api::InsertSlot::PreFader, "effects.reverb.fdn", "{}"});
    REQUIRE(mixer.set_bus_strip(1, bus));
  };
  auto lane_to_bus = [](uint32_t track_id) {
    sonare::engine::TrackLaneConfig lane{track_id};
    lane.sends.push_back({1, 0.0f});  // 0 dB post-fader send into bus 1
    return lane;
  };

  // Reference: one lane carrying (a+b) through the bus, processed once per block.
  sonare::engine::TrackMixerRuntime ref;
  ref.prepare(48000.0, kBlock);
  make_reverb_bus(ref);
  REQUIRE(ref.set_track_lanes({lane_to_bus(10)}));
  ref.settle_smoothers();

  // Staged: two lanes (a and b) accumulated into the shared bus, finished once.
  sonare::engine::TrackMixerRuntime staged;
  staged.prepare(48000.0, kBlock);
  make_reverb_bus(staged);
  REQUIRE(staged.set_track_lanes({lane_to_bus(10), lane_to_bus(20)}));
  staged.settle_smoothers();

  float total_energy = 0.0f;
  for (int block = 0; block < kBlocks; ++block) {
    std::array<float, kBlock> a{};
    std::array<float, kBlock> b{};
    std::array<float, kBlock> sum{};
    for (int i = 0; i < kBlock; ++i) {
      const float t = static_cast<float>(block * kBlock + i);
      a[static_cast<size_t>(i)] = std::sin(0.07f * t);
      b[static_cast<size_t>(i)] = 0.5f * std::cos(0.11f * t);
      sum[static_cast<size_t>(i)] = a[static_cast<size_t>(i)] + b[static_cast<size_t>(i)];
    }
    float* a_src[] = {a.data(), a.data()};
    float* b_src[] = {b.data(), b.data()};
    float* sum_src[] = {sum.data(), sum.data()};

    std::array<float, kBlock> ref_l{};
    std::array<float, kBlock> ref_r{};
    float* ref_out[] = {ref_l.data(), ref_r.data()};
    REQUIRE(ref.mix_source(10, sum_src, ref_out, 2, kBlock));

    std::array<float, kBlock> st_l{};
    std::array<float, kBlock> st_r{};
    float* st_out[] = {st_l.data(), st_r.data()};
    REQUIRE(staged.begin_source_mix(2, kBlock));
    bool routed_a = false;
    bool routed_b = false;
    REQUIRE(staged.mix_source_into_lane(10, a_src, st_out, 2, kBlock, routed_a));
    REQUIRE(staged.mix_source_into_lane(20, b_src, st_out, 2, kBlock, routed_b));
    REQUIRE(routed_a);
    REQUIRE(routed_b);
    staged.finish_source_mix(st_out, 2, kBlock);

    for (int i = 0; i < kBlock; ++i) {
      REQUIRE(st_l[static_cast<size_t>(i)] == Approx(ref_l[static_cast<size_t>(i)]).margin(1e-5f));
      REQUIRE(st_r[static_cast<size_t>(i)] == Approx(ref_r[static_cast<size_t>(i)]).margin(1e-5f));
      total_energy += std::abs(ref_l[static_cast<size_t>(i)]);
    }
  }
  // Sanity: the bus + dry path actually produced signal (not an all-silent match).
  REQUIRE(total_energy > 0.0f);
}
