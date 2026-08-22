#include "engine/track_mixer.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "engine/meter_telemetry.h"
#include "mastering/eq/eq_band.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"
#include "mixing/pan_law.h"
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

class ProcessCountingGain final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    ++process_calls;
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] *= 0.5f;
      }
    }
  }
  void reset() override { process_calls = 0; }

  int process_calls = 0;
};

class AutomatableGainProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] *= gain_;
      }
    }
  }
  void reset() override {}
  bool set_parameter(unsigned int param_id, float value) override {
    if (param_id != 0 || !std::isfinite(value)) return false;
    gain_ = value;
    return true;
  }
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override {
    return param_id == 0;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    return {{"gain", 0}};
  }

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

TEST_CASE("TrackMixerRuntime engine-owned strips omit embedded metering", "[engine][track_mixer]") {
  constexpr int kFrames = 64;
  constexpr float kSourceLevel = 0.5f;

  sonare::mixing::api::Strip spec;
  auto factory_strip = sonare::engine::make_channel_strip_from_spec(spec);
  REQUIRE(factory_strip);
  REQUIRE_FALSE(factory_strip->metering_enabled());

  // Externally bound strips keep the standalone ChannelStrip default so their
  // snapshots remain available to callers that own and inspect the strip.
  sonare::mixing::ChannelStrip external_strip;
  REQUIRE(external_strip.metering_enabled());

  std::array<float, kFrames> source{};
  source.fill(kSourceLevel);
  float* source_channels[] = {source.data()};

  sonare::engine::TrackMixerRuntime owned_mixer;
  owned_mixer.prepare(48000.0, kFrames);
  REQUIRE(owned_mixer.set_track_lanes({{10}}));
  REQUIRE(owned_mixer.set_track_strip(10, spec));

  sonare::engine::TrackMixerRuntime external_mixer;
  external_mixer.prepare(48000.0, kFrames);
  REQUIRE(external_mixer.set_track_lanes({{10}}));
  REQUIRE(external_mixer.bind_track_strip(10, &external_strip));

  std::array<float, kFrames> owned_output{};
  std::array<float, kFrames> external_output{};
  float* owned_channels[] = {owned_output.data()};
  float* external_channels[] = {external_output.data()};

  sonare::engine::MeterTelemetryTap telemetry;
  telemetry.prepare(48000.0, kFrames, 0, 8, sonare::mixing::MeterConfig{true, false, 4});
  telemetry.begin_block();
  REQUIRE(
      owned_mixer.mix_source(10, source_channels, owned_channels, 1, kFrames, &telemetry, 1234));
  telemetry.end_block();
  REQUIRE(external_mixer.mix_source(10, source_channels, external_channels, 1, kFrames));

  REQUIRE(owned_output == external_output);

  sonare::engine::MeterTelemetryRecord record;
  REQUIRE(telemetry.pop(record));
  REQUIRE(record.target_id == 1);
  REQUIRE(record.render_frame == 1234);
  REQUIRE(record.channel_count == 1);
  REQUIRE(record.peak_db[0] == Catch::Approx(-6.0206f).margin(0.01f));
  REQUIRE_FALSE(telemetry.pop(record));
}

TEST_CASE("TrackMixerRuntime lane PFL/AFL taps preserve main and sum staged sources",
          "[engine][track_mixer][monitor]") {
  constexpr int kFrames = 8;
  std::array<float, kFrames> source_a{};
  std::array<float, kFrames> source_b{};
  source_a.fill(1.0f);
  source_b.fill(0.5f);
  float* source_a_channels[] = {source_a.data()};
  float* source_b_channels[] = {source_b.data()};

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kFrames);
  REQUIRE(mixer.set_track_lanes({{10}, {20}}));

  std::array<float, kFrames> monitor_storage{};
  float* monitor_channels[] = {monitor_storage.data()};
  mixer.set_monitor_bus(monitor_channels, 1);

  const auto render_staged = [&](bool include_second_source) {
    std::array<float, kFrames> output{};
    float* output_channels[] = {output.data()};
    monitor_storage.fill(0.0f);
    REQUIRE(mixer.begin_source_mix(1, kFrames));
    bool routed = false;
    REQUIRE(mixer.mix_source_into_lane(10, source_a_channels, output_channels, 1, kFrames, routed));
    REQUIRE(routed);
    if (include_second_source) {
      routed = false;
      REQUIRE(
          mixer.mix_source_into_lane(20, source_b_channels, output_channels, 1, kFrames, routed));
      REQUIRE(routed);
    }
    mixer.finish_source_mix(output_channels, 1, kFrames);
    return std::pair{output, monitor_storage};
  };

  mixer.set_lane_monitor_mode(0, sonare::engine::TrackMonitorMode::kOff);
  auto off = render_staged(false);
  REQUIRE(off.first[0] == Catch::Approx(1.0f));
  REQUIRE(off.second[0] == Catch::Approx(0.0f));

  mixer.set_lane_monitor_mode(0, sonare::engine::TrackMonitorMode::kPfl);
  auto pfl = render_staged(false);
  REQUIRE(pfl.first[0] == Catch::Approx(off.first[0]));
  REQUIRE(pfl.second[0] == Catch::Approx(1.0f));

  // AFL includes the lane fader/gate stage, while PFL above was taken before it.
  REQUIRE(mixer.set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kFaderDb, -6.0f));
  mixer.set_lane_monitor_mode(0, sonare::engine::TrackMonitorMode::kAfl);
  mixer.settle_smoothers();
  auto afl = render_staged(false);
  REQUIRE(afl.first[0] == Catch::Approx(0.501187f).margin(0.0001f));
  REQUIRE(afl.second[0] == Catch::Approx(afl.first[0]));

  // A staged rack source visits the lane mixer once, so two PFL lanes sum once
  // into the shared monitor bus. The main output remains the normal lane sum.
  REQUIRE(mixer.set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kFaderDb, 0.0f));
  mixer.settle_smoothers();
  mixer.set_lane_monitor_mode(0, sonare::engine::TrackMonitorMode::kPfl);
  mixer.set_lane_monitor_mode(1, sonare::engine::TrackMonitorMode::kPfl);
  auto summed = render_staged(true);
  REQUIRE(summed.first[0] == Catch::Approx(1.5f));
  REQUIRE(summed.second[0] == Catch::Approx(1.5f));

  // Republished/reordered lanes retain the mode by track id, not by stale array
  // position: track 10 moves to lane 1 and continues to feed PFL.
  REQUIRE(mixer.set_track_lanes({{20}, {10}}));
  auto reordered = render_staged(false);
  REQUIRE(reordered.first[0] == Catch::Approx(1.0f));
  REQUIRE(reordered.second[0] == Catch::Approx(1.0f));
}

TEST_CASE("TrackMixerRuntime stages multiple sources before processing a lane once",
          "[engine][track_mixer]") {
  constexpr int kFrames = 16;
  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kFrames);
  REQUIRE(mixer.set_track_lanes({{10}}));

  sonare::mixing::ChannelStrip strip;
  auto counter = std::make_unique<ProcessCountingGain>();
  ProcessCountingGain* raw_counter = counter.get();
  strip.add_pre_insert(std::move(counter));
  REQUIRE(mixer.bind_track_strip(10, &strip));

  std::array<float, kFrames> first{};
  std::array<float, kFrames> second{};
  first.fill(0.25f);
  second.fill(0.75f);
  float* first_channels[] = {first.data()};
  float* second_channels[] = {second.data()};
  std::array<float, kFrames> out{};
  float* out_channels[] = {out.data()};

  REQUIRE(mixer.begin_source_mix(1, kFrames));
  bool first_routed = false;
  bool second_routed = false;
  REQUIRE(mixer.mix_source_into_lane(10, first_channels, out_channels, 1, kFrames, first_routed));
  REQUIRE(mixer.mix_source_into_lane(10, second_channels, out_channels, 1, kFrames, second_routed));
  REQUIRE(first_routed);
  REQUIRE(second_routed);
  mixer.finish_source_mix(out_channels, 1, kFrames);

  REQUIRE(raw_counter->process_calls == 1);
  for (float sample : out) {
    REQUIRE(sample == Catch::Approx(0.5f));
  }
}

TEST_CASE("TrackMixerRuntime clears lane insert automation slots when lanes change",
          "[engine][track_mixer]") {
  std::array<float, 4> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 4);
  player.set_clips({clip_for_track(1, 20, channels, 1, 4)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 4);
  REQUIRE(mixer.set_track_lanes({{10}}));

  sonare::mixing::ChannelStrip first_strip;
  first_strip.add_pre_insert(std::make_unique<AutomatableGainProcessor>());
  REQUIRE(mixer.bind_track_strip(10, &first_strip));
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 0.25f));

  REQUIRE(mixer.set_track_lanes({{20}}));
  sonare::mixing::ChannelStrip second_strip;
  second_strip.add_pre_insert(std::make_unique<AutomatableGainProcessor>());
  REQUIRE(mixer.bind_track_strip(20, &second_strip));

  std::array<float, 4> out{};
  float* out_channels[] = {out.data()};
  REQUIRE(mixer.render_clips(player, out_channels, 1, 4, 0));

  REQUIRE(out[0] == Catch::Approx(1.0f).margin(1.0e-6f));
  REQUIRE(out[3] == Catch::Approx(1.0f).margin(1.0e-6f));
}

TEST_CASE("TrackMixerRuntime ramps fader on an in-place strip update instead of jumping",
          "[engine][track_mixer]") {
  // A live track-gain edit republishes the track strip via set_track_strip. When
  // only smoothable scalars change (here the fader) and the insert topology is
  // unchanged, the existing strip must be updated in place so its fader smoother
  // RAMPS from the old value -- rebuilding a fresh strip would settle straight to
  // the new gain and jump (an audible click).
  constexpr int kFrames = 256;
  std::array<float, kFrames> source{};
  source.fill(1.0f);  // DC so the output equals the applied gain.
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kFrames);
  player.set_clips({clip_for_track(1, 10, channels, 1, kFrames)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kFrames);
  REQUIRE(mixer.set_track_lanes({{10}}));

  // Establish the strip at a low fader (-40 dB ~= 0.01 linear), settled.
  sonare::mixing::api::Strip low;
  low.fader_db = -40.0f;
  REQUIRE(mixer.set_track_strip(10, low));

  std::array<float, kFrames> out{};
  float* out_channels[] = {out.data()};
  REQUIRE(mixer.render_clips(player, out_channels, 1, kFrames, 0));
  REQUIRE(out[0] == Catch::Approx(0.01f).margin(0.005f));  // sanity: sits at -40 dB

  // Raise the fader to unity with the SAME (empty) insert chain.
  sonare::mixing::api::Strip high;
  high.fader_db = 0.0f;
  REQUIRE(mixer.set_track_strip(10, high));

  std::fill(out.begin(), out.end(), 0.0f);
  REQUIRE(mixer.render_clips(player, out_channels, 1, kFrames, 0));

  // First sample still near the old ~0.01 (ramping up), NOT jumped to unity.
  REQUIRE(out[0] < 0.5f);
  // And it is genuinely rising toward unity across the block.
  REQUIRE(out[kFrames - 1] > out[0]);
}

TEST_CASE("TrackMixerRuntime clears bus insert automation slots when bus strip changes",
          "[engine][track_mixer]") {
  constexpr int kFrames = 256;
  std::array<float, kFrames> source{};
  for (int i = 0; i < kFrames; ++i) {
    source[static_cast<size_t>(i)] = 0.25f * std::sin(2.0f * 3.14159265358979323846f * 1000.0f *
                                                      static_cast<float>(i) / 48000.0f);
  }
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kFrames);
  player.set_clips({clip_for_track(1, 10, channels, 1, kFrames)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kFrames);
  REQUIRE(mixer.set_buses({{1, 0.0f}}));
  sonare::engine::TrackLaneConfig lane{10};
  lane.output_bus_id = 1;
  REQUIRE(mixer.set_track_lanes({lane}));

  sonare::mixing::api::Bus first_bus;
  first_bus.id = "1";
  first_bus.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "eq.parametric",
       R"({"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":0,"band0.enabled":1})"});
  REQUIRE(mixer.set_bus_strip(1, first_bus));
  size_t bus_index = 0;
  unsigned int param_id = 0;
  REQUIRE(mixer.resolve_bus_insert_param(1, 0, "band0.gainDb", &bus_index, &param_id));
  REQUIRE(bus_index == 0);
  REQUIRE(mixer.route_bus_insert_param_smoothed(bus_index, 0, param_id, 12.0f));

  sonare::mixing::api::Bus second_bus;
  second_bus.id = "1";
  second_bus.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "eq.parametric",
       R"({"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":0,"band0.enabled":1})"});
  REQUIRE(mixer.set_bus_strip(1, second_bus));

  sonare::engine::TrackMixerRuntime flat;
  flat.prepare(48000.0, kFrames);
  REQUIRE(flat.set_buses({{1, 0.0f}}));
  REQUIRE(flat.set_track_lanes({lane}));
  REQUIRE(flat.set_bus_strip(1, second_bus));

  std::array<float, kFrames> out{};
  std::array<float, kFrames> flat_out{};
  float* out_channels[] = {out.data()};
  float* flat_channels[] = {flat_out.data()};
  for (int block = 0; block < 4; ++block) {
    out.fill(0.0f);
    flat_out.fill(0.0f);
    REQUIRE(mixer.render_clips(player, out_channels, 1, kFrames, 0));
    REQUIRE(flat.render_clips(player, flat_channels, 1, kFrames, 0));
  }

  auto rms = [](const std::array<float, kFrames>& samples) {
    double sum = 0.0;
    for (float sample : samples) {
      sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
  };
  REQUIRE(rms(out) == Catch::Approx(rms(flat_out)).margin(1.0e-5));
}

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

TEST_CASE("TrackMixerRuntime rejects width and unknown typed lane parameters",
          "[engine][track_mixer]") {
  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 64);
  REQUIRE(mixer.set_track_lanes({{10}}));

  // Width remains a standalone strip control. Arrangement typed lanes own only
  // fader and pan, so width=3 must not look like a successful lane route.
  REQUIRE_FALSE(mixer.set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kWidth, 1.0f));
  REQUIRE_FALSE(mixer.set_lane_parameter(0, 99u, 0.0f));
  REQUIRE_FALSE(mixer.set_lane_parameter(1, sonare::engine::TrackMixerRuntime::kFaderDb, 0.0f));
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

TEST_CASE("TrackMixerRuntime lane pan honors the strip's configured pan law",
          "[engine][track_mixer]") {
  // Regression for the live-vs-offline pan divergence: lane pan automation must
  // use the strip's configured pan law (here constant-power), not a hardcoded
  // linear balance. At pan 0.5 the constant-power away/near ratio (~0.414)
  // differs from the linear ratio (0.5), so the law is observable.
  std::array<float, 256> src_l{};
  std::array<float, 256> src_r{};
  src_l.fill(1.0f);
  src_r.fill(1.0f);
  const float* a[] = {src_l.data(), src_r.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 256);
  player.set_clips({clip_for_track(1, 10, a, 2, 256)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 256);
  REQUIRE(mixer.set_track_lanes({{10}}));

  // Bind a stereo strip configured for the constant-power (-3 dB) pan law.
  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Const3dB, 5.0f});
  REQUIRE(mixer.bind_track_strip(10, &strip));
  REQUIRE(mixer.set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kPan, 0.5f));

  std::array<float, 256> out_l{};
  std::array<float, 256> out_r{};
  float* out[] = {out_l.data(), out_r.data()};
  // Render enough blocks for the 5 ms pan smoother to fully settle.
  for (int block = 0; block < 12; ++block) {
    out_l.fill(0.0f);
    out_r.fill(0.0f);
    REQUIRE(mixer.render_clips(player, out, 2, 256, 0));
  }

  const float ratio = out_l.back() / out_r.back();
  // Constant-power balance at pan 0.5: cos(0.75*pi/2)/sin(0.75*pi/2) ~= 0.4142.
  REQUIRE(ratio == Catch::Approx(0.41421356f).margin(0.01f));
  // Clearly distinct from the old hardcoded linear balance (ratio 0.5).
  REQUIRE(ratio < 0.47f);
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

TEST_CASE("TrackMixerRuntime toggles a bus insert bypass", "[engine][track_mixer]") {
  constexpr int kBlock = 64;
  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));

  sonare::mixing::api::Bus bus;
  bus.id = "1";
  bus.inserts.push_back({sonare::mixing::api::InsertSlot::PreFader, "eq.parametric", "{}"});
  REQUIRE(mixer.set_bus_strip(1, bus));

  // Unknown bus, out-of-range insert, and unset bus id all fail; the resolved
  // bus insert toggles successfully with and without reset-on-bypass.
  REQUIRE_FALSE(mixer.set_bus_insert_bypassed(2, 0, true));
  REQUIRE_FALSE(mixer.set_bus_insert_bypassed(1, 7, true));
  REQUIRE_FALSE(mixer.set_bus_insert_bypassed(0, 0, true));
  REQUIRE(mixer.set_bus_insert_bypassed(1, 0, true, true));
  REQUIRE(mixer.set_bus_insert_bypassed(1, 0, false));
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

// Requires the FX suite: the shared bus insert is an FDN reverb
// (effects.reverb.fdn), which make_insert cannot build without it.
#if defined(SONARE_WITH_FX)
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
#endif  // SONARE_WITH_FX

TEST_CASE("TrackMixerRuntime applies a bus input trim to its output", "[engine][track_mixer]") {
  constexpr int kBlock = 64;
  constexpr int kBlocks = 16;  // > the 5 ms trim smoother time constant.
  std::array<float, kBlock> src_l{};
  std::array<float, kBlock> src_r{};
  src_l.fill(1.0f);
  src_r.fill(1.0f);
  float* source[] = {src_l.data(), src_r.data()};

  // Routes the DC stereo source through a stereo group bus and returns the
  // master front-left sample once the bus trim smoother has settled.
  auto front_left_at_trim = [&](float trim_db) {
    sonare::engine::TrackMixerRuntime mixer;
    mixer.prepare(48000.0, kBlock);
    REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));
    sonare::engine::TrackLaneConfig lane{10};
    lane.output_bus_id = 1;
    REQUIRE(mixer.set_track_lanes({lane}));
    sonare::mixing::api::Bus bus;
    bus.id = "1";
    bus.input_trim_db = trim_db;
    REQUIRE(mixer.set_bus_strip(1, bus));
    mixer.settle_smoothers();
    std::array<float, kBlock> out_l{};
    std::array<float, kBlock> out_r{};
    float* out[] = {out_l.data(), out_r.data()};
    for (int block = 0; block < kBlocks; ++block) {
      out_l.fill(0.0f);
      out_r.fill(0.0f);
      REQUIRE(mixer.mix_source(10, source, out, 2, kBlock));
    }
    return out_l.back();
  };

  const float unity = front_left_at_trim(0.0f);
  const float halved = front_left_at_trim(-6.0205999f);  // -6 dB -> x0.5
  REQUIRE(std::abs(unity) > 0.1f);
  REQUIRE(std::abs(halved - 0.5f * unity) < 0.01f * std::abs(unity));
}

TEST_CASE("TrackMixerRuntime inverts a bus front-pair polarity", "[engine][track_mixer]") {
  constexpr int kBlock = 64;
  std::array<float, kBlock> src_l{};
  std::array<float, kBlock> src_r{};
  src_l.fill(1.0f);
  src_r.fill(1.0f);
  float* source[] = {src_l.data(), src_r.data()};

  auto fronts = [&](bool invert_left, bool invert_right) {
    sonare::engine::TrackMixerRuntime mixer;
    mixer.prepare(48000.0, kBlock);
    REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));
    sonare::engine::TrackLaneConfig lane{10};
    lane.output_bus_id = 1;
    REQUIRE(mixer.set_track_lanes({lane}));
    sonare::mixing::api::Bus bus;
    bus.id = "1";
    bus.polarity_invert_left = invert_left;
    bus.polarity_invert_right = invert_right;
    REQUIRE(mixer.set_bus_strip(1, bus));
    mixer.settle_smoothers();
    std::array<float, kBlock> out_l{};
    std::array<float, kBlock> out_r{};
    float* out[] = {out_l.data(), out_r.data()};
    REQUIRE(mixer.mix_source(10, source, out, 2, kBlock));
    return std::pair<float, float>{out_l.back(), out_r.back()};
  };

  const auto [unity_l, unity_r] = fronts(false, false);
  const auto [flipped_l, flipped_r] = fronts(true, false);
  REQUIRE(std::abs(unity_l) > 0.1f);
  // Inverting only the left channel negates it and leaves the right untouched.
  REQUIRE(std::abs(flipped_l - (-unity_l)) < 1e-5f);
  REQUIRE(std::abs(flipped_r - unity_r) < 1e-5f);
}

TEST_CASE("TrackMixerRuntime applies a bus stereo width to its output", "[engine][track_mixer]") {
  constexpr int kBlock = 64;
  constexpr int kBlocks = 16;  // > the 5 ms width smoother time constant.
  // A pure-side stereo source (L = +1, R = -1): width 0 collapses it to the
  // (silent) mid, width 1 preserves it.
  std::array<float, kBlock> src_l{};
  std::array<float, kBlock> src_r{};
  src_l.fill(1.0f);
  src_r.fill(-1.0f);
  float* source[] = {src_l.data(), src_r.data()};

  auto front_energy_at_width = [&](float width) {
    sonare::engine::TrackMixerRuntime mixer;
    mixer.prepare(48000.0, kBlock);
    REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));
    sonare::engine::TrackLaneConfig lane{10};
    lane.output_bus_id = 1;
    REQUIRE(mixer.set_track_lanes({lane}));
    sonare::mixing::api::Bus bus;
    bus.id = "1";
    bus.width = width;
    REQUIRE(mixer.set_bus_strip(1, bus));
    mixer.settle_smoothers();
    std::array<float, kBlock> out_l{};
    std::array<float, kBlock> out_r{};
    float* out[] = {out_l.data(), out_r.data()};
    for (int block = 0; block < kBlocks; ++block) {
      out_l.fill(0.0f);
      out_r.fill(0.0f);
      REQUIRE(mixer.mix_source(10, source, out, 2, kBlock));
    }
    return out_l.back() * out_l.back() + out_r.back() * out_r.back();
  };

  const float wide = front_energy_at_width(1.0f);
  const float narrow = front_energy_at_width(0.0f);
  REQUIRE(wide > 0.1f);
  REQUIRE(narrow < wide * 0.05f);
}

TEST_CASE("TrackMixerRuntime settles a bus width so the first block opens settled",
          "[engine][track_mixer]") {
  constexpr int kBlock = 64;
  // Pure-side stereo source. Width 0 collapses it to the (silent) mid. If the
  // width smoother is settled at the configured target the very first rendered
  // sample is silent; without the settle it would glide down from width 1.0 and
  // the block would open at near-full side.
  std::array<float, kBlock> src_l{};
  std::array<float, kBlock> src_r{};
  src_l.fill(1.0f);
  src_r.fill(-1.0f);
  float* source[] = {src_l.data(), src_r.data()};

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));
  sonare::engine::TrackLaneConfig lane{10};
  lane.output_bus_id = 1;
  REQUIRE(mixer.set_track_lanes({lane}));
  sonare::mixing::api::Bus bus;
  bus.id = "1";
  bus.width = 0.0f;
  REQUIRE(mixer.set_bus_strip(1, bus));
  mixer.settle_smoothers();

  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  float* out[] = {out_l.data(), out_r.data()};
  REQUIRE(mixer.mix_source(10, source, out, 2, kBlock));

  float peak = 0.0f;
  for (int i = 0; i < kBlock; ++i) {
    peak = std::max(peak, std::abs(out_l[i]));
    peak = std::max(peak, std::abs(out_r[i]));
  }
  REQUIRE(peak < 1.0e-4f);
}

TEST_CASE("Strip specs decode their pan law through the shared wire mapping",
          "[engine][track_mixer]") {
  // The strip spec carries the law as the wire integer, so the engine path has
  // to agree with the shared decoder — including its fallback, which is what a
  // spec built from unvalidated input relies on.
  for (int index = 0; index < sonare::mixing::kPanLawCount; ++index) {
    sonare::mixing::api::Strip spec;
    spec.pan_law = index;
    auto strip = sonare::engine::make_channel_strip_from_spec(spec);
    REQUIRE(strip);
    CAPTURE(index);
    REQUIRE(strip->pan_law() == sonare::mixing::pan_law_from_index(index));
  }

  sonare::mixing::api::Strip out_of_range;
  out_of_range.pan_law = 7;
  auto strip = sonare::engine::make_channel_strip_from_spec(out_of_range);
  REQUIRE(strip);
  REQUIRE(strip->pan_law() == sonare::mixing::PanLaw::Const3dB);
}

TEST_CASE("TrackMixerRuntime aligns a latent bus path against the dry mix",
          "[engine][track_mixer][pdc]") {
  // Lane 10 reaches the master directly; lane 20 reaches it through a bus whose
  // insert chain carries lookahead. Both must land on the same sample: a
  // parallel-compression bus that arrives late combs against the dry signal it
  // is summed with.
  //
  // The insert is a limiter with a ceiling far above the signal, so it is a
  // pure `lookaheadMs` delay (1 ms = 48 samples at 48 kHz) and the assertion is
  // about timing alone, not about gain reduction.
  constexpr int kFrames = 256;
  constexpr int kBusLatency = 48;

  std::array<float, kFrames> impulse{};
  impulse[0] = 0.5f;
  const float* channels[] = {impulse.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kFrames);
  player.set_clips(
      {clip_for_track(1, 10, channels, 1, kFrames), clip_for_track(2, 20, channels, 1, kFrames)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kFrames);
  REQUIRE(mixer.set_buses({{1, 0.0f}}));

  sonare::engine::TrackLaneConfig dry_lane{10};
  sonare::engine::TrackLaneConfig bus_lane{20};
  bus_lane.output_bus_id = 1;
  REQUIRE(mixer.set_track_lanes({dry_lane, bus_lane}));

  sonare::mixing::api::Bus latent_bus;
  latent_bus.id = "1";
  latent_bus.inserts.push_back({sonare::mixing::api::InsertSlot::PreFader, "dynamics.limiter",
                                R"({"thresholdDb":24,"lookaheadMs":1,"releaseMs":50})"});
  REQUIRE(mixer.set_bus_strip(1, latent_bus));

  // The runtime now advertises the real end-to-end maximum: no lane strip
  // carries latency, so the whole figure is the bus insert chain.
  // CHECK rather than REQUIRE so the rendered-alignment assertions below still
  // run and report independently when the advertised figure is wrong.
  CHECK(mixer.latency_samples_q8() == (kBusLatency << 8));
  CHECK(mixer.latency_samples() == kBusLatency);

  std::array<float, kFrames> out{};
  float* out_channels[] = {out.data()};
  REQUIRE(mixer.render_clips(player, out_channels, 1, kFrames, 0));

  // Both contributions arrive coincidentally at the compensated position and
  // sum there, rather than appearing as an early dry peak and a late bus peak.
  REQUIRE(out[kBusLatency] == Catch::Approx(1.0f).margin(1.0e-3f));
  double early_energy = 0.0;
  for (int i = 0; i < kBusLatency; ++i) {
    early_energy += static_cast<double>(out[static_cast<size_t>(i)]) * out[static_cast<size_t>(i)];
  }
  REQUIRE(early_energy < 1.0e-8);
}

TEST_CASE("TrackMixerRuntime keeps PDC delay history across an unrelated strip edit",
          "[engine][track_mixer][pdc]") {
  // Lane 20 carries a latent insert, so lane 10 is given a compensation delay.
  // Editing lane 20 re-derives every alignment; the banks whose alignment did
  // not change must keep the audio they are holding. Rebuilding one zero-fills
  // it and punches a hole the length of the compensation delay into the mix.
  constexpr int kFrames = 64;
  constexpr int kLatency = 8;

  std::array<float, kFrames> dc{};
  dc.fill(0.5f);
  const float* channels[] = {dc.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kFrames);
  player.set_clips(
      {clip_for_track(1, 10, channels, 1, kFrames), clip_for_track(2, 20, channels, 1, kFrames)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kFrames);
  REQUIRE(mixer.set_track_lanes({{10}, {20}}));

  // 0.16666667 ms at 48 kHz rounds to exactly kLatency lookahead samples, and
  // the ceiling sits far above the signal so the insert is a pure delay.
  sonare::mixing::api::Strip latent;
  latent.inserts.push_back({sonare::mixing::api::InsertSlot::PreFader, "dynamics.limiter",
                            R"({"thresholdDb":24,"lookaheadMs":0.16666667,"releaseMs":50})"});
  REQUIRE(mixer.set_track_strip(20, latent));
  REQUIRE(mixer.set_track_strip(10, sonare::mixing::api::Strip{}));
  REQUIRE(mixer.latency_samples() == kLatency);

  std::array<float, kFrames> out{};
  float* out_channels[] = {out.data()};

  // Render past the compensation delay so lane 10's bank is full of audio.
  for (int block = 0; block < 4; ++block) {
    out.fill(0.0f);
    REQUIRE(mixer.render_clips(player, out_channels, 1, kFrames, 0));
  }
  const float steady = out[kFrames - 1];
  REQUIRE(steady > 0.1f);

  // A scalar-only edit on the other lane: the insert topology is unchanged, so
  // it takes the in-place fast path whose whole purpose is to preserve state.
  const uint64_t generation_before = mixer.pdc_storage_generation();
  sonare::mixing::api::Strip quieter = latent;
  quieter.fader_db = -3.0f;
  REQUIRE(mixer.set_track_strip(20, quieter));
  REQUIRE(mixer.pdc_storage_generation() == generation_before);

  // ... and the same through the EQ-band and channel-delay setters, the other
  // two routes into recompute_lane_pdc.
  REQUIRE(mixer.set_track_eq_band(10, 0, sonare::mastering::eq::EqBand{}));
  REQUIRE(mixer.set_track_channel_delay_samples(10, 0));
  REQUIRE(mixer.pdc_storage_generation() == generation_before);

  // The next block opens where the previous one left off. A rebuilt bank would
  // have opened with kLatency samples of silence instead.
  out.fill(0.0f);
  REQUIRE(mixer.render_clips(player, out_channels, 1, kFrames, 0));
  for (int i = 0; i < kFrames; ++i) {
    INFO("sample " << i);
    REQUIRE(out[static_cast<size_t>(i)] > 0.1f);
  }
}

TEST_CASE("TrackMixerRuntime delivers a lane's send on the lane's own timebase",
          "[engine][track_mixer][pdc]") {
  // Lane 20 carries a look-ahead insert, so lane 10 -- which has none -- is
  // delayed by kLatency to meet it. Lane 10 also feeds a bus through a
  // post-fader send, and that copy has to arrive at the same instant as its
  // direct contribution. A send tapped upstream of the alignment arrives early
  // instead, which combs the bus return against the dry path -- and the comb
  // moves whenever an unrelated lane's insert latency changes.
  constexpr int kFrames = 64;
  constexpr int kLatency = 8;

  std::array<float, kFrames> impulse{};
  impulse[0] = 1.0f;
  std::array<float, kFrames> silence{};
  const float* impulse_channels[] = {impulse.data()};
  const float* silent_channels[] = {silence.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kFrames);
  player.set_clips({clip_for_track(1, 10, impulse_channels, 1, kFrames),
                    clip_for_track(2, 20, silent_channels, 1, kFrames)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kFrames);
  REQUIRE(mixer.set_buses({{1, 0.0f}}));

  sonare::engine::TrackLaneConfig sending{10};
  sending.sends.push_back({1, 0.0f, true, sonare::mixing::SendTiming::PostFader});
  REQUIRE(mixer.set_track_lanes({sending, {20}}));

  // 0.16666667 ms at 48 kHz rounds to exactly kLatency lookahead samples, and
  // the ceiling sits far above the signal so the insert is a pure delay.
  sonare::mixing::api::Strip latent;
  latent.inserts.push_back({sonare::mixing::api::InsertSlot::PreFader, "dynamics.limiter",
                            R"({"thresholdDb":24,"lookaheadMs":0.16666667,"releaseMs":50})"});
  REQUIRE(mixer.set_track_strip(20, latent));
  REQUIRE(mixer.latency_samples() == kLatency);

  std::array<float, kFrames> out{};
  float* out_channels[] = {out.data()};
  REQUIRE(mixer.render_clips(player, out_channels, 1, kFrames, 0));

  // One impulse in, one impulse out, at the compensated position. An unaligned
  // send would show as a second nonzero sample at frame 0 -- the pre-echo.
  for (int i = 0; i < kFrames; ++i) {
    if (i == kLatency) continue;
    INFO("sample " << i);
    REQUIRE(out[static_cast<size_t>(i)] == 0.0f);
  }
  // Direct and send coincide there, so the sample carries both contributions
  // (the same figure a lane with a unity send produces with no PDC in play).
  REQUIRE(out[kLatency] > 2.82f);
  REQUIRE(out[kLatency] < 2.84f);
}

TEST_CASE("TrackMixerRuntime gates a post-fader send with the lane's fader and mute",
          "[engine][track_mixer]") {
  // A post-fader send is declared as a tap on the lane's audible signal, so
  // whatever silences the lane silences the send. Leaving the send at full level
  // through a mute (or another lane's solo) keeps the track audible through the
  // bus return, which is not what any of the three controls means.
  constexpr int kFrames = 64;

  std::array<float, kFrames> source{};
  source.fill(0.5f);
  std::array<float, kFrames> silence{};
  const float* source_channels[] = {source.data()};
  const float* silent_channels[] = {silence.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kFrames);
  player.set_clips({clip_for_track(1, 10, source_channels, 1, kFrames),
                    clip_for_track(2, 20, silent_channels, 1, kFrames)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kFrames);
  REQUIRE(mixer.set_buses({{1, 0.0f}}));
  sonare::engine::TrackLaneConfig sending{10};
  sending.sends.push_back({1, 0.0f, true, sonare::mixing::SendTiming::PostFader});
  REQUIRE(mixer.set_track_lanes({sending, {20}}));

  std::array<float, kFrames> out{};
  float* out_channels[] = {out.data()};
  const auto render = [&]() {
    out.fill(0.0f);
    REQUIRE(mixer.render_clips(player, out_channels, 1, kFrames, 0));
  };
  // The bus return is the only thing that can keep the lane audible once the
  // direct path is gated, so the whole master sum is the assertion.
  const auto require_silent = [&](const char* what) {
    // One block to publish the gate target, then settle it so the assertion is
    // an exact zero rather than a point on the anti-click ramp.
    render();
    mixer.settle_smoothers();
    render();
    for (int i = 0; i < kFrames; ++i) {
      INFO(what << ", sample " << i);
      REQUIRE(out[static_cast<size_t>(i)] == 0.0f);
    }
  };

  render();
  const float audible = out[kFrames - 1];
  REQUIRE(audible > 1.4f);

  REQUIRE(mixer.set_lane_solo_mute(0, false, true));
  require_silent("muted");
  REQUIRE(mixer.set_lane_solo_mute(0, false, false));

  REQUIRE(mixer.set_lane_solo_mute(1, true, false));
  require_silent("another lane soloed");
  REQUIRE(mixer.set_lane_solo_mute(1, false, false));

  REQUIRE(mixer.set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kFaderDb, -120.0f));
  render();
  mixer.settle_smoothers();
  render();
  for (int i = 0; i < kFrames; ++i) {
    INFO("fader floored, sample " << i);
    REQUIRE(std::abs(out[static_cast<size_t>(i)]) < 1.0e-4f);
  }

  // Restoring the controls brings both paths back, together.
  REQUIRE(mixer.set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kFaderDb, 0.0f));
  mixer.settle_smoothers();
  render();
  REQUIRE(out[kFrames - 1] == Catch::Approx(audible));
}

TEST_CASE("TrackMixerRuntime rejects an out-of-range channel delay in the core",
          "[engine][track_mixer]") {
  // The bound lives here, not only at the C ABI, because the WASM facade calls
  // this method directly. A samples/milliseconds mix-up must fail on every
  // surface rather than succeeding at the four-second ceiling on one of them.
  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 64);
  REQUIRE(mixer.set_track_lanes({{10}}));
  REQUIRE(mixer.set_track_strip(10, sonare::mixing::api::Strip{}));

  constexpr int kMax = sonare::mixing::kMaxAlignmentDelaySamples;

  // The largest usable value is applied exactly, so the bound rejects only what
  // is genuinely out of range.
  REQUIRE(mixer.set_track_channel_delay_samples(10, kMax));
  REQUIRE(mixer.latency_samples() == kMax);

  // Both ends of the acceptance condition.
  CHECK_FALSE(mixer.set_track_channel_delay_samples(10, -1));
  CHECK_FALSE(mixer.set_track_channel_delay_samples(10, 500000));
  CHECK_FALSE(mixer.set_track_channel_delay_samples(10, kMax + 1));

  // A rejected request leaves the previously applied delay alone: the failure
  // is a rejection, not a silent substitution.
  REQUIRE(mixer.latency_samples() == kMax);

  REQUIRE(mixer.set_track_channel_delay_samples(10, 0));
  REQUIRE(mixer.latency_samples() == 0);
}
