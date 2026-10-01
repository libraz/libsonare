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
#include "engine/realtime_engine.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/parametric.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"
#include "mixing/pan_law.h"
#include "mixing/panner.h"
#include "rt/command.h"
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
  bool set_parameter_impl(unsigned int param_id, float value) override {
    if (param_id != 0) return false;
    gain_ = value;
    return true;
  }
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override {
    return param_id == 0;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    return {{"gain", 0}};
  }
  float gain() const noexcept { return gain_; }

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

TEST_CASE("TrackMixerRuntime settles a pending lane insert target before an accepted snapshot",
          "[engine][track_mixer]") {
  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, 64);
  REQUIRE(mixer.set_track_lanes({{10}}));

  auto* processor = new AutomatableGainProcessor();
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::unique_ptr<sonare::rt::ProcessorBase>(processor));
  REQUIRE(mixer.bind_track_strip(10, &strip));
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 0.25f));

  // Publishing the same lane is accepted, but clears the temporary smoother
  // slot. The target must be applied to the live processor before that clear.
  REQUIRE(mixer.set_track_lanes({{10}}));
  REQUIRE(processor->gain() == Catch::Approx(0.25f).margin(1.0e-6f));
}

TEST_CASE("TrackMixerRuntime does not settle a lane insert target for a rejected snapshot",
          "[engine][track_mixer]") {
  constexpr int kBlock = 64;
  std::array<float, kBlock> source{};
  source.fill(1.0f);
  const float* source_channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kBlock);
  player.set_clips({clip_for_track(1, 10, source_channels, 1, kBlock)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE(mixer.set_track_lanes({{10}}));
  auto* processor = new AutomatableGainProcessor();
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::unique_ptr<sonare::rt::ProcessorBase>(processor));
  REQUIRE(mixer.bind_track_strip(10, &strip));

  std::array<float, kBlock> output{};
  float* output_channels[] = {output.data()};
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 1.0f));
  REQUIRE(mixer.render_clips(player, output_channels, 1, kBlock, 0));
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 0.25f));
  output.fill(0.0f);
  REQUIRE(mixer.render_clips(player, output_channels, 1, kBlock, 0));
  const float midramp = processor->gain();
  REQUIRE(midramp > 0.25f);
  REQUIRE(midramp < 1.0f);

  // An invalid output bus rejects before publication. It must not consume the
  // pending target or snap the processor while the current lane remains live.
  sonare::engine::TrackLaneConfig invalid{10};
  invalid.output_bus_id = 99;
  REQUIRE_FALSE(mixer.set_track_lanes({invalid}));
  CHECK(processor->gain() == Catch::Approx(midramp).margin(1.0e-6f));

  // Positive control: an explicit settle does apply the still-pending target.
  mixer.settle_smoothers();
  CHECK(processor->gain() == Catch::Approx(0.25f).margin(1.0e-6f));
}

TEST_CASE("TrackMixerRuntime retains a midramp lane insert target on same-lane resend",
          "[engine][track_mixer]") {
  constexpr int kBlock = 64;
  std::array<float, kBlock> source{};
  source.fill(1.0f);
  const float* source_channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kBlock);
  player.set_clips({clip_for_track(1, 10, source_channels, 1, kBlock)});

  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(48000.0, kBlock);
  REQUIRE(mixer.set_track_lanes({{10}}));
  auto* processor = new AutomatableGainProcessor();
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::unique_ptr<sonare::rt::ProcessorBase>(processor));
  REQUIRE(mixer.bind_track_strip(10, &strip));

  std::array<float, kBlock> output{};
  float* output_channels[] = {output.data()};
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 1.0f));
  REQUIRE(mixer.render_clips(player, output_channels, 1, kBlock, 0));
  REQUIRE(mixer.route_lane_insert_param_smoothed(0, 0, 0, 0.25f));
  output.fill(0.0f);
  REQUIRE(mixer.render_clips(player, output_channels, 1, kBlock, 0));
  REQUIRE(processor->gain() > 0.25f);
  REQUIRE(processor->gain() < 1.0f);

  // The accepted resend clears lane slots, so it must settle the target first
  // or the processor would remain forever at the midramp value.
  REQUIRE(mixer.set_track_lanes({{10}}));
  REQUIRE(processor->gain() == Catch::Approx(0.25f).margin(1.0e-6f));
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

  // A changed chain; an identical resend keeps the chain and its automation.
  sonare::mixing::api::Bus second_bus;
  second_bus.id = "1";
  second_bus.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "eq.parametric",
       R"({"band0.type":1,"band0.frequencyHz":2000,"band0.gainDb":0,"band0.enabled":1})"});
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

TEST_CASE("TrackMixerRuntime surround pan glide is independent of sub-block partitioning",
          "[engine][track_mixer][surround]") {
  // process() splits a block at automation / MIDI / clip boundaries, so
  // consecutive sub-blocks differ in length. A scatter gain ramped to its target
  // over "this sub-block" therefore glides for a different real duration
  // depending on where the split fell -- the same gesture rendered live and in
  // an offline bounce would not match. Driven from a sample-rate time constant
  // it is identical either way: one 64-sample block must equal four 16-sample
  // ones, sample for sample.
  constexpr int kTotal = 64;
  constexpr int kChunk = 16;
  constexpr double kSr = 48000.0;

  const auto run = [](int chunk) {
    std::array<float, kTotal> src_l{};
    std::array<float, kTotal> src_r{};
    src_l.fill(1.0f);
    src_r.fill(1.0f);

    sonare::engine::TrackMixerRuntime mixer;
    mixer.prepare(kSr, kTotal);
    REQUIRE(mixer.set_track_lanes({{10}}));
    sonare::mixing::api::Strip spec;
    spec.id = "vox";
    spec.surround_pan.azimuth = -110.0f;
    REQUIRE(mixer.set_track_strip(10, spec));
    mixer.settle_smoothers();

    // Prime: the first surround block snaps to placement, so the glide under
    // test starts from a settled position rather than from silence.
    std::array<std::array<float, kTotal>, 6> prime{};
    std::array<float*, 6> prime_out{};
    for (int c = 0; c < 6; ++c)
      prime_out[static_cast<size_t>(c)] = prime[static_cast<size_t>(c)].data();
    float* prime_src[] = {src_l.data(), src_r.data()};
    REQUIRE(mixer.mix_source(10, prime_src, prime_out.data(), 6, kChunk));

    // Move the pan. set_track_strip's in-place path retargets the smoothers
    // without rebuilding the strip, and the destination width is unchanged, so
    // what follows is a genuine glide rather than another snap.
    spec.surround_pan.azimuth = 110.0f;
    REQUIRE(mixer.set_track_strip(10, spec));

    std::array<std::array<float, kTotal>, 6> planes{};
    for (auto& plane : planes) plane.fill(0.0f);
    for (int offset = 0; offset < kTotal; offset += chunk) {
      std::array<float*, 6> out{};
      for (int c = 0; c < 6; ++c) {
        out[static_cast<size_t>(c)] = planes[static_cast<size_t>(c)].data() + offset;
      }
      float* chunk_src[] = {src_l.data() + offset, src_r.data() + offset};
      REQUIRE(mixer.mix_source(10, chunk_src, out.data(), 6, chunk));
    }
    return planes;
  };

  const auto single = run(kTotal);
  const auto chunked = run(kChunk);
  for (int c = 0; c < 6; ++c) {
    for (int i = 0; i < kTotal; ++i) {
      INFO("plane " << c << " sample " << i);
      REQUIRE(std::abs(chunked[static_cast<size_t>(c)][static_cast<size_t>(i)] -
                       single[static_cast<size_t>(c)][static_cast<size_t>(i)]) < 1.0e-5f);
    }
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

TEST_CASE("TrackMixerRuntime clears retained bus EQ state when its layout changes",
          "[engine][track_mixer][surround]") {
  constexpr int kBlock = 64;
  using TrackMixerRuntime = sonare::engine::TrackMixerRuntime;
  using TrackBusConfig = sonare::engine::TrackBusConfig;
  using TrackLaneConfig = sonare::engine::TrackLaneConfig;
  using EqBand = sonare::mastering::eq::EqBand;
  using EqBandType = sonare::mastering::eq::EqBandType;

  const auto peak_after = [](const std::array<float, kBlock>& plane, int first) {
    float peak = 0.0f;
    for (int i = first; i < kBlock; ++i) {
      peak = std::max(peak, std::abs(plane[static_cast<size_t>(i)]));
    }
    return peak;
  };
  const auto rear_peak = [&](const std::array<std::array<float, kBlock>, 6>& planes, int first) {
    return std::max(peak_after(planes[4], first), peak_after(planes[5], first));
  };
  const auto configure = [&](TrackMixerRuntime& mixer, std::vector<TrackBusConfig> buses) {
    mixer.prepare(48000.0, kBlock);
    REQUIRE(mixer.set_buses(std::move(buses)));
    TrackLaneConfig lane{10};
    lane.output_bus_id = 1;
    REQUIRE(mixer.set_track_lanes({lane}));

    sonare::mixing::api::Strip strip;
    strip.surround_pan.azimuth = -110.0f;  // Ls in 5.1.
    REQUIRE(mixer.set_track_strip(10, strip));

    sonare::mixing::api::Bus bus;
    bus.id = "1";
    // AllPass has unit magnitude, so any nonzero output after the impulse is
    // filter state rather than a gain-path artifact. A high Q keeps that state
    // measurable across the layout transition below.
    bus.eq.bands.push_back(EqBand{EqBandType::AllPass, 1000.0f, 0.0f, 50.0f, true});
    REQUIRE(mixer.set_bus_strip(1, bus));
    mixer.settle_smoothers();
  };
  const auto prime_with_rear_impulse = [&](TrackMixerRuntime& mixer) {
    std::array<float, kBlock> impulse{};
    impulse[0] = 1.0f;
    float* source[] = {impulse.data(), impulse.data()};
    std::array<std::array<float, kBlock>, 6> planes{};
    std::array<float*, 6> output{};
    for (int channel = 0; channel < 6; ++channel) {
      output[static_cast<size_t>(channel)] = planes[static_cast<size_t>(channel)].data();
    }
    REQUIRE(mixer.mix_source(10, source, output.data(), 6, kBlock));
    return std::pair{planes, rear_peak(planes, 1)};
  };
  const auto render_wide_silence = [&](TrackMixerRuntime& mixer) {
    std::array<float, kBlock> silence{};
    float* source[] = {silence.data(), silence.data()};
    std::array<std::array<float, kBlock>, 6> planes{};
    std::array<float*, 6> output{};
    for (int channel = 0; channel < 6; ++channel) {
      output[static_cast<size_t>(channel)] = planes[static_cast<size_t>(channel)].data();
    }
    REQUIRE(mixer.mix_source(10, source, output.data(), 6, kBlock));
    return rear_peak(planes, 0);
  };

  TrackMixerRuntime changed_layout;
  configure(changed_layout, {{1, 0.0f, sonare::ChannelLayout::FivePointOne}});
  const auto primed = prime_with_rear_impulse(changed_layout);
  REQUIRE(primed.second > 1.0e-4f);  // The rear all-pass tail is observable.

  // The same bus id is narrowed for one silent block. The EQ processes only
  // the active stereo pair, leaving its old rear-channel state untouched.
  REQUIRE(changed_layout.set_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));
  changed_layout.settle_smoothers();
  std::array<float, kBlock> silence{};
  float* silence_source[] = {silence.data(), silence.data()};
  std::array<std::array<float, kBlock>, 2> narrow{};
  std::array<float*, 2> narrow_output{{narrow[0].data(), narrow[1].data()}};
  REQUIRE(changed_layout.mix_source(10, silence_source, narrow_output.data(), 2, kBlock));
  REQUIRE(std::max(peak_after(narrow[0], 0), peak_after(narrow[1], 0)) < 1.0e-7f);

  // Widening the same bus id must start all six EQ planes from silence. The
  // unfixed path revives the rear state that was skipped while it was stereo.
  REQUIRE(changed_layout.set_buses({{1, 0.0f, sonare::ChannelLayout::FivePointOne}}));
  changed_layout.settle_smoothers();
  const float changed_layout_rear = render_wide_silence(changed_layout);
  CHECK(changed_layout_rear < 1.0e-7f);

  // Negative control: a same-layout re-send is a retained state update, so an
  // EQ tail must continue through it rather than being reset unconditionally.
  TrackMixerRuntime same_layout;
  configure(same_layout, {{1, 0.0f, sonare::ChannelLayout::FivePointOne}});
  const auto same_layout_primed = prime_with_rear_impulse(same_layout);
  REQUIRE(same_layout_primed.second > 1.0e-4f);
  REQUIRE(same_layout.set_buses({{1, 0.0f, sonare::ChannelLayout::FivePointOne}}));
  same_layout.settle_smoothers();
  CHECK(render_wide_silence(same_layout) > 1.0e-4f);

  // Negative control: the state follows the bus id when buses reorder, so its
  // dedicated EQ tail must survive the slot move as well.
  TrackMixerRuntime reordered;
  configure(reordered, {{1, 0.0f, sonare::ChannelLayout::FivePointOne},
                        {2, 0.0f, sonare::ChannelLayout::Stereo}});
  const auto reordered_primed = prime_with_rear_impulse(reordered);
  REQUIRE(reordered_primed.second > 1.0e-4f);
  REQUIRE(reordered.set_buses(
      {{2, 0.0f, sonare::ChannelLayout::Stereo}, {1, 0.0f, sonare::ChannelLayout::FivePointOne}}));
  reordered.settle_smoothers();
  CHECK(render_wide_silence(reordered) > 1.0e-4f);
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

TEST_CASE("TrackMixerRuntime re-snaps a lane's scatter gains when the master width changes",
          "[engine][track_mixer][surround]") {
  // Scatter gains are computed against a destination layout, so the values a 5.1
  // block carries out are a different quantity at 7.1. Gliding from them would
  // place the lane along a path neither layout describes, so a width change
  // starts from placement exactly as the lane's own first block does.
  constexpr int kBlock = 16;

  const auto render_wide = [](bool prime_at_five_one) {
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
    // -90 deg is a speaker position in 7.1 (Lss) and a crossfade between two
    // in 5.1, so the two layouts place this lane differently -- the condition
    // the non-vacuity check at the foot of the case verifies.
    spec.surround_pan.azimuth = -90.0f;
    REQUIRE(mixer.set_track_strip(10, spec));
    mixer.settle_smoothers();

    if (prime_at_five_one) {
      std::array<std::array<float, kBlock>, 6> narrow{};
      std::array<float*, 6> narrow_out{};
      for (int c = 0; c < 6; ++c) {
        narrow_out[static_cast<size_t>(c)] = narrow[static_cast<size_t>(c)].data();
      }
      REQUIRE(mixer.mix_source(10, source, narrow_out.data(), 6, kBlock));
      src_l.fill(1.0f);
      src_r.fill(1.0f);
    }

    std::array<std::array<float, kBlock>, 8> planes{};
    std::array<float*, 8> out{};
    for (int c = 0; c < 8; ++c) {
      out[static_cast<size_t>(c)] = planes[static_cast<size_t>(c)].data();
    }
    REQUIRE(mixer.mix_source(10, source, out.data(), 8, kBlock));
    return planes;
  };

  const auto first_block = render_wide(false);
  const auto after_width_change = render_wide(true);

  for (int c = 0; c < 8; ++c) {
    for (int i = 0; i < kBlock; ++i) {
      INFO("plane " << c << " sample " << i);
      REQUIRE(after_width_change[static_cast<size_t>(c)][static_cast<size_t>(i)] ==
              first_block[static_cast<size_t>(c)][static_cast<size_t>(i)]);
    }
  }

  // Non-vacuity, both halves. A snapped block holds its target from its first
  // sample, so a glide would be visible as a rising plane.
  bool constant_through_block = true;
  for (int c = 0; c < 8; ++c) {
    const auto& plane = after_width_change[static_cast<size_t>(c)];
    constant_through_block = constant_through_block && plane.front() == plane.back();
  }
  CHECK(constant_through_block);

  // And the two layouts must actually place this lane differently, or a glide
  // from the carried gains would be a no-op and the equality above would hold
  // for a lane the change cannot reach.
  std::array<float, kBlock> src_l{};
  std::array<float, kBlock> src_r{};
  src_l.fill(1.0f);
  src_r.fill(1.0f);
  float* source[] = {src_l.data(), src_r.data()};
  sonare::engine::TrackMixerRuntime narrow_mixer;
  narrow_mixer.prepare(48000.0, kBlock);
  REQUIRE(narrow_mixer.set_track_lanes({{10}}));
  sonare::mixing::api::Strip spec;
  spec.id = "vox";
  spec.surround_pan.azimuth = -90.0f;
  REQUIRE(narrow_mixer.set_track_strip(10, spec));
  narrow_mixer.settle_smoothers();
  std::array<std::array<float, kBlock>, 6> narrow{};
  std::array<float*, 6> narrow_out{};
  for (int c = 0; c < 6; ++c) {
    narrow_out[static_cast<size_t>(c)] = narrow[static_cast<size_t>(c)].data();
  }
  REQUIRE(narrow_mixer.mix_source(10, source, narrow_out.data(), 6, kBlock));

  bool layouts_differ = false;
  for (int c = 0; c < 6; ++c) {
    layouts_differ = layouts_differ || narrow[static_cast<size_t>(c)].back() !=
                                           first_block[static_cast<size_t>(c)].back();
  }
  CHECK(layouts_differ);
}

TEST_CASE("TrackMixerRuntime re-snaps a lane's scatter gains after a stereo interlude",
          "[engine][track_mixer][surround]") {
  // The destination width is a per-call argument on the engine's own render
  // entry point, so a host alternating a stereo monitor render with a surround
  // one is an ordinary sequence rather than a re-routing edge case. The stereo
  // blocks compute no scatter gains, so resuming from the ones the last surround
  // block left would place the lane where its pan was at an arbitrarily earlier
  // moment -- here, at an azimuth the caller has since moved away from.
  constexpr int kBlock = 16;
  constexpr float kBefore = -90.0f;
  constexpr float kAfter = 60.0f;

  const auto surround_block_at = [](float before, int interlude_blocks, float after) {
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
    spec.surround_pan.azimuth = before;
    REQUIRE(mixer.set_track_strip(10, spec));
    mixer.settle_smoothers();

    if (interlude_blocks > 0) {
      std::array<std::array<float, kBlock>, 6> primed{};
      std::array<float*, 6> primed_out{};
      for (int c = 0; c < 6; ++c) {
        primed_out[static_cast<size_t>(c)] = primed[static_cast<size_t>(c)].data();
      }
      REQUIRE(mixer.mix_source(10, source, primed_out.data(), 6, kBlock));

      spec.surround_pan.azimuth = after;
      REQUIRE(mixer.set_track_strip(10, spec));

      std::array<std::array<float, kBlock>, 2> stereo{};
      std::array<float*, 2> stereo_out{{stereo[0].data(), stereo[1].data()}};
      for (int block = 0; block < interlude_blocks; ++block) {
        src_l.fill(1.0f);
        src_r.fill(1.0f);
        REQUIRE(mixer.mix_source(10, source, stereo_out.data(), 2, kBlock));
      }
    }

    src_l.fill(1.0f);
    src_r.fill(1.0f);
    std::array<std::array<float, kBlock>, 6> planes{};
    std::array<float*, 6> out{};
    for (int c = 0; c < 6; ++c) {
      out[static_cast<size_t>(c)] = planes[static_cast<size_t>(c)].data();
    }
    REQUIRE(mixer.mix_source(10, source, out.data(), 6, kBlock));
    return planes;
  };

  // The lane's own first surround block at the post-interlude azimuth: the
  // placement the returning block has to match.
  const auto fresh = surround_block_at(kAfter, 0, kAfter);
  const auto returning = surround_block_at(kBefore, 4, kAfter);

  for (int c = 0; c < 6; ++c) {
    for (int i = 0; i < kBlock; ++i) {
      INFO("plane " << c << " sample " << i);
      REQUIRE(returning[static_cast<size_t>(c)][static_cast<size_t>(i)] ==
              fresh[static_cast<size_t>(c)][static_cast<size_t>(i)]);
    }
  }

  // Non-vacuity: the two azimuths must place the lane differently, or a glide
  // from the carried gains would be a no-op and the equality above would hold
  // for a lane the interlude cannot reach.
  const auto before_placement = surround_block_at(kBefore, 0, kBefore);
  bool azimuths_differ = false;
  for (int c = 0; c < 6; ++c) {
    azimuths_differ = azimuths_differ || before_placement[static_cast<size_t>(c)].back() !=
                                             fresh[static_cast<size_t>(c)].back();
  }
  CHECK(azimuths_differ);
}

namespace {

using sonare::engine::TrackLaneConfig;
using sonare::engine::TrackMixerRuntime;
using sonare::mastering::eq::EqBand;
using sonare::mastering::eq::EqBandType;
using sonare::mixing::api::Bus;
using sonare::mixing::api::InsertSlot;
using sonare::mixing::api::Strip;

constexpr int kBusBlock = 64;

EqBand boost_band() { return EqBand{EqBandType::Peak, 1000.0f, 12.0f, 1.0f, true}; }

// Deterministic per-track, per-channel tone.
float lane_tone(uint32_t track_id, int channel, int64_t frame) {
  const float step =
      0.011f * static_cast<float>(track_id % 7 + 1) + 0.017f * static_cast<float>(channel);
  return 0.4f * std::sin(step * static_cast<float>(frame));
}

struct StereoRender {
  std::vector<float> left;
  std::vector<float> right;
};

bool renders_equal(const StereoRender& a, const StereoRender& b) {
  return a.left == b.left && a.right == b.right;
}

float max_abs_difference(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  float worst = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
  return worst;
}

// Streams @p blocks blocks of every track's tone, starting at @p first_frame,
// and appends the stereo master to @p out.
void render_tones(TrackMixerRuntime& mixer, const std::vector<uint32_t>& tracks, int blocks,
                  int64_t first_frame, StereoRender& out) {
  std::array<float, kBusBlock> src_l{};
  std::array<float, kBusBlock> src_r{};
  std::array<float, kBusBlock> out_l{};
  std::array<float, kBusBlock> out_r{};
  float* source[] = {src_l.data(), src_r.data()};
  float* master[] = {out_l.data(), out_r.data()};
  for (int block = 0; block < blocks; ++block) {
    const int64_t base = first_frame + static_cast<int64_t>(block) * kBusBlock;
    out_l.fill(0.0f);
    out_r.fill(0.0f);
    REQUIRE(mixer.begin_source_mix(2, kBusBlock));
    for (uint32_t track : tracks) {
      for (int i = 0; i < kBusBlock; ++i) {
        src_l[static_cast<size_t>(i)] = lane_tone(track, 0, base + i);
        src_r[static_cast<size_t>(i)] = lane_tone(track, 1, base + i);
      }
      bool routed = false;
      REQUIRE(mixer.mix_source_into_lane(track, source, master, 2, kBusBlock, routed));
    }
    mixer.finish_source_mix(master, 2, kBusBlock);
    out.left.insert(out.left.end(), out_l.begin(), out_l.end());
    out.right.insert(out.right.end(), out_r.begin(), out_r.end());
  }
}

// The plain sum of lanes 10 and 20 over [first_frame, first_frame + frames).
StereoRender tone_sum(int64_t first_frame, int frames) {
  StereoRender sum;
  for (int64_t n = first_frame; n < first_frame + frames; ++n) {
    sum.left.push_back(lane_tone(10, 0, n) + lane_tone(20, 0, n));
    sum.right.push_back(lane_tone(10, 1, n) + lane_tone(20, 1, n));
  }
  return sum;
}

// One stereo bus (id 1) fed by lanes 10 and 20, configured from @p bus.
void configure_bus_rig(TrackMixerRuntime& mixer, const Bus& bus) {
  mixer.prepare(48000.0, kBusBlock);
  REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));
  TrackLaneConfig a{10};
  a.output_bus_id = 1;
  TrackLaneConfig b{20};
  b.output_bus_id = 1;
  REQUIRE(mixer.set_track_lanes({a, b}));
  REQUIRE(mixer.set_bus_strip(1, bus));
  mixer.settle_smoothers();
}

StereoRender render_bus_rig(const Bus& bus, int blocks) {
  TrackMixerRuntime mixer;
  configure_bus_rig(mixer, bus);
  StereoRender out;
  render_tones(mixer, {10, 20}, blocks, 0, out);
  return out;
}

Bus plain_bus(const char* id) {
  Bus bus;
  bus.id = id;
  return bus;
}

}  // namespace

TEST_CASE("TrackMixerRuntime default bus output equals the plain sum of its lanes",
          "[engine][track_mixer]") {
  constexpr int kBlocks = 8;
  const Bus bus = plain_bus("1");
  const StereoRender sum = tone_sum(0, kBlocks * kBusBlock);
  const StereoRender rendered = render_bus_rig(bus, kBlocks);
  REQUIRE(rendered.left == sum.left);
  REQUIRE(rendered.right == sum.right);

  // Sensitivity twins: every new bus stage moves the same comparison.
  Bus panned = bus;
  panned.pan = 0.3f;
  CHECK_FALSE(renders_equal(render_bus_rig(panned, kBlocks), sum));
  Bus equalized = bus;
  equalized.eq.bands.push_back(boost_band());
  CHECK_FALSE(renders_equal(render_bus_rig(equalized, kBlocks), sum));

  // The pan-stage skip is load-bearing: the default panner run at centre is not
  // an identity, so a bus that always ran it would fail the equality above.
  sonare::mixing::PannerProcessor centred;
  centred.prepare(48000.0, kBusBlock);
  StereoRender through_panner = sum;
  for (int block = 0; block < kBlocks; ++block) {
    const size_t offset = static_cast<size_t>(block) * kBusBlock;
    float* planes[] = {through_panner.left.data() + offset, through_panner.right.data() + offset};
    centred.process(planes, 2, kBusBlock);
  }
  CHECK_FALSE(renders_equal(through_panner, sum));
}

TEST_CASE("TrackMixerRuntime bus strip EQ follows each resent spec", "[engine][track_mixer]") {
  constexpr int kBlocks = 6;
  const Bus flat = plain_bus("1");
  Bus equalized = flat;
  equalized.eq.bands.push_back(boost_band());

  TrackMixerRuntime control;
  TrackMixerRuntime resent;
  configure_bus_rig(control, equalized);
  configure_bus_rig(resent, equalized);
  StereoRender control_out;
  StereoRender resent_out;
  render_tones(control, {10, 20}, kBlocks, 0, control_out);
  render_tones(resent, {10, 20}, kBlocks, 0, resent_out);
  // An identical resend keeps the EQ and its filter state.
  REQUIRE(resent.set_bus_strip(1, equalized));
  render_tones(control, {10, 20}, kBlocks, kBlocks * kBusBlock, control_out);
  render_tones(resent, {10, 20}, kBlocks, kBlocks * kBusBlock, resent_out);
  REQUIRE(renders_equal(resent_out, control_out));
  CHECK_FALSE(renders_equal(control_out, tone_sum(0, 2 * kBlocks * kBusBlock)));

  // A resend without "eq" is flat.
  REQUIRE(resent.set_bus_strip(1, flat));
  StereoRender after_flat;
  render_tones(resent, {10, 20}, kBlocks, 2 * kBlocks * kBusBlock, after_flat);
  REQUIRE(renders_equal(after_flat, tone_sum(2 * kBlocks * kBusBlock, kBlocks * kBusBlock)));

  // A band set through the setter lands in the retained spec, so the next
  // resend without it is a change that clears it.
  TrackMixerRuntime setter;
  configure_bus_rig(setter, flat);
  REQUIRE(setter.set_bus_eq_band(1, 0, boost_band()));
  StereoRender with_band;
  render_tones(setter, {10, 20}, kBlocks, 0, with_band);
  CHECK_FALSE(renders_equal(with_band, tone_sum(0, kBlocks * kBusBlock)));
  REQUIRE(setter.set_bus_strip(1, flat));
  StereoRender cleared;
  render_tones(setter, {10, 20}, kBlocks, kBlocks * kBusBlock, cleared);
  REQUIRE(renders_equal(cleared, tone_sum(kBlocks * kBusBlock, kBlocks * kBusBlock)));

  REQUIRE_FALSE(setter.set_bus_eq_band(99, 0, boost_band()));
  REQUIRE_FALSE(
      setter.set_bus_eq_band(1, sonare::mastering::eq::ParametricEq::kMaxBands, boost_band()));
  REQUIRE_FALSE(setter.set_bus_eq_band(
      1, 0, EqBand{EqBandType::Peak, 30000.0f, 6.0f, 1.0f, true}));  // above Nyquist
}

TEST_CASE("TrackMixerRuntime track strip EQ follows its spec across resends and rebuilds",
          "[engine][track_mixer]") {
  constexpr int kBlocks = 6;
  Strip flat;
  flat.id = "track-10";
  Strip equalized = flat;
  equalized.eq.bands.push_back(boost_band());

  const auto rig = [](TrackMixerRuntime& mixer, const Strip& spec) {
    mixer.prepare(48000.0, kBusBlock);
    REQUIRE(mixer.set_track_lanes({{10}}));
    REQUIRE(mixer.set_track_strip(10, spec));
    mixer.settle_smoothers();
  };

  TrackMixerRuntime control;
  TrackMixerRuntime resent;
  TrackMixerRuntime plain;
  rig(control, equalized);
  rig(resent, equalized);
  rig(plain, flat);
  StereoRender control_out;
  StereoRender resent_out;
  StereoRender plain_out;
  render_tones(control, {10}, kBlocks, 0, control_out);
  render_tones(resent, {10}, kBlocks, 0, resent_out);
  render_tones(plain, {10}, kBlocks, 0, plain_out);
  REQUIRE(resent.set_track_strip(10, equalized));
  render_tones(control, {10}, kBlocks, kBlocks * kBusBlock, control_out);
  render_tones(resent, {10}, kBlocks, kBlocks * kBusBlock, resent_out);
  render_tones(plain, {10}, kBlocks, kBlocks * kBusBlock, plain_out);
  REQUIRE(renders_equal(resent_out, control_out));
  CHECK_FALSE(renders_equal(control_out, plain_out));

  // A band set through the setter is cleared by a resend that lacks it.
  REQUIRE(plain.set_track_eq_band(10, 0, boost_band()));
  REQUIRE(plain.set_track_strip(10, flat));
  TrackMixerRuntime reference;
  rig(reference, flat);
  StereoRender plain_after;
  StereoRender reference_out;
  render_tones(plain, {10}, kBlocks, 0, plain_after);
  render_tones(reference, {10}, kBlocks, 0, reference_out);
  CHECK(max_abs_difference(plain_after.left, reference_out.left) < 1.0e-6f);

  // An insert-topology change rebuilds the strip, and the rebuild carries the
  // spec's EQ rather than dropping it.
  Strip equalized_with_insert = equalized;
  equalized_with_insert.inserts.push_back({InsertSlot::PreFader, "utility.gain", "{}"});
  Strip flat_with_insert = flat;
  flat_with_insert.inserts.push_back({InsertSlot::PreFader, "utility.gain", "{}"});
  REQUIRE(control.set_track_strip(10, equalized_with_insert));
  TrackMixerRuntime fresh;
  TrackMixerRuntime fresh_flat;
  rig(fresh, equalized_with_insert);
  rig(fresh_flat, flat_with_insert);
  StereoRender rebuilt;
  StereoRender fresh_out;
  StereoRender fresh_flat_out;
  render_tones(control, {10}, kBlocks, 0, rebuilt);
  render_tones(fresh, {10}, kBlocks, 0, fresh_out);
  render_tones(fresh_flat, {10}, kBlocks, 0, fresh_flat_out);
  CHECK(max_abs_difference(rebuilt.left, fresh_out.left) < 1.0e-6f);
  CHECK(max_abs_difference(rebuilt.left, fresh_flat_out.left) > 1.0e-2f);
}

TEST_CASE("TrackMixerRuntime keeps a bus insert chain across an identical resend",
          "[engine][track_mixer]") {
  constexpr int kBlocks = 8;
  Bus verb = plain_bus("1");
  verb.inserts.push_back(
      {InsertSlot::PreFader, "effects.reverb.fdn", R"({"decaySec":2,"dryWet":0.5})"});

  TrackMixerRuntime control;
  configure_bus_rig(control, verb);
  StereoRender control_out;
  render_tones(control, {10, 20}, kBlocks, 0, control_out);
  render_tones(control, {10, 20}, kBlocks, kBlocks * kBusBlock, control_out);

  const auto resent_render = [&](const Bus& resend) {
    TrackMixerRuntime mixer;
    configure_bus_rig(mixer, verb);
    StereoRender out;
    render_tones(mixer, {10, 20}, kBlocks, 0, out);
    REQUIRE(mixer.set_bus_strip(1, resend));
    render_tones(mixer, {10, 20}, kBlocks, kBlocks * kBusBlock, out);
    return out;
  };

  // Identical resend: no rebuild, so the reverb tail is untouched.
  REQUIRE(renders_equal(resent_render(verb), control_out));

  // A pan-only resend keeps the chain too: the unattenuated side is unchanged.
  Bus verb_panned = verb;
  verb_panned.pan = 0.5f;
  verb_panned.pan_law = 3;  // Linear0dB: the near side stays at exactly unity.
  const StereoRender panned = resent_render(verb_panned);
  CHECK(max_abs_difference(panned.right, control_out.right) < 1.0e-6f);
  CHECK(max_abs_difference(panned.left, control_out.left) > 1.0e-3f);

  // Changed insert params rebuild the chain.
  Bus verb_changed = verb;
  verb_changed.inserts[0].params_json = R"({"decaySec":2,"dryWet":0.6})";
  CHECK_FALSE(renders_equal(resent_render(verb_changed), control_out));
}

TEST_CASE("TrackMixerRuntime refuses non-default pan on a surround bus", "[engine][track_mixer]") {
  constexpr int kBlocks = 4;
  Bus panned = plain_bus("1");
  panned.pan = 0.5f;

  TrackMixerRuntime control;
  TrackMixerRuntime mixer;
  configure_bus_rig(control, panned);
  configure_bus_rig(mixer, panned);
  StereoRender control_out;
  StereoRender mixer_out;
  render_tones(control, {10, 20}, kBlocks, 0, control_out);
  render_tones(mixer, {10, 20}, kBlocks, 0, mixer_out);
  // Widening a bus that carries non-default pan fails and changes nothing.
  REQUIRE_FALSE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::FivePointOne}}));
  render_tones(control, {10, 20}, kBlocks, kBlocks * kBusBlock, control_out);
  render_tones(mixer, {10, 20}, kBlocks, kBlocks * kBusBlock, mixer_out);
  REQUIRE(renders_equal(mixer_out, control_out));

  // Pan set through the setter is retained too; returning it to default
  // unblocks the widening.
  TrackMixerRuntime setter;
  configure_bus_rig(setter, plain_bus("1"));
  REQUIRE(setter.set_bus_pan(1, 0.3f));
  REQUIRE_FALSE(setter.set_buses({{1, 0.0f, sonare::ChannelLayout::FivePointOne}}));
  REQUIRE(setter.set_bus_pan(1, 0.0f));
  REQUIRE(setter.set_buses({{1, 0.0f, sonare::ChannelLayout::FivePointOne}}));

  // On a surround bus every pan entry point refuses; EQ is still allowed.
  REQUIRE_FALSE(setter.set_bus_pan(1, 0.2f));
  REQUIRE_FALSE(setter.set_bus_pan_law(1, sonare::mixing::PanLaw::Linear0dB));
  REQUIRE_FALSE(setter.set_bus_pan_mode(1, sonare::mixing::PanMode::StereoPan));
  REQUIRE_FALSE(setter.set_bus_dual_pan(1, -0.5f, 0.5f));
  REQUIRE_FALSE(setter.set_bus_strip(1, panned));
  REQUIRE(setter.set_bus_strip(1, plain_bus("1")));
  REQUIRE(setter.set_bus_eq_band(1, 0, boost_band()));

  // Unknown buses and non-finite values are refused.
  REQUIRE_FALSE(control.set_bus_pan(99, 0.1f));
  REQUIRE_FALSE(control.set_bus_pan(0, 0.1f));
  REQUIRE_FALSE(control.set_bus_pan(1, std::nanf("")));
  REQUIRE_FALSE(control.set_bus_dual_pan(1, -1.0f, std::nanf("")));
}

TEST_CASE("TrackMixerRuntime bus pan setters move the bus output", "[engine][track_mixer]") {
  constexpr int kBlocks = 8;
  TrackMixerRuntime mixer;
  configure_bus_rig(mixer, plain_bus("1"));
  REQUIRE(mixer.set_bus_pan_law(1, sonare::mixing::PanLaw::Linear0dB));
  REQUIRE(mixer.set_bus_pan(1, -1.0f));
  mixer.settle_smoothers();
  StereoRender hard_left;
  render_tones(mixer, {10, 20}, kBlocks, 0, hard_left);
  const StereoRender sum = tone_sum(0, kBlocks * kBusBlock);
  CHECK(max_abs_difference(hard_left.left, sum.left) < 1.0e-6f);
  for (float sample : hard_left.right) REQUIRE(sample == 0.0f);

  // Dual pan swaps the channels outright.
  REQUIRE(mixer.set_bus_pan_mode(1, sonare::mixing::PanMode::DualPan));
  REQUIRE(mixer.set_bus_dual_pan(1, 1.0f, -1.0f));
  mixer.settle_smoothers();
  StereoRender swapped;
  render_tones(mixer, {10, 20}, kBlocks, 0, swapped);
  CHECK(max_abs_difference(swapped.left, sum.right) < 1.0e-6f);
  CHECK(max_abs_difference(swapped.right, sum.left) < 1.0e-6f);
}

TEST_CASE("TrackMixerRuntime keys bus state by id across set_buses", "[engine][track_mixer]") {
  constexpr int kBlocks = 8;
  // Bus 1: hard left, boosted, and latent (a limiter far above the signal is a
  // pure lookahead delay), so its PDC share differs from bus 2's.
  Bus a = plain_bus("1");
  a.pan = -1.0f;
  a.pan_law = 3;
  a.eq.bands.push_back(boost_band());
  a.inserts.push_back({InsertSlot::PreFader, "dynamics.limiter",
                       R"({"thresholdDb":24,"lookaheadMs":1,"releaseMs":50})"});
  Bus b = plain_bus("2");
  b.pan = 1.0f;
  b.pan_law = 3;

  TrackLaneConfig lane10{10};
  lane10.output_bus_id = 1;
  TrackLaneConfig lane20{20};
  lane20.output_bus_id = 2;

  const auto rig = [&](TrackMixerRuntime& mixer, std::vector<sonare::engine::TrackBusConfig> buses,
                       const std::vector<TrackLaneConfig>& lanes) {
    mixer.prepare(48000.0, kBusBlock);
    REQUIRE(mixer.set_buses(std::move(buses)));
    REQUIRE(mixer.set_track_lanes(lanes));
    for (const Bus* bus : {&a, &b}) {
      const uint32_t id = bus == &a ? 1u : 2u;
      bool declared = false;
      for (const TrackLaneConfig& lane : lanes) declared = declared || lane.output_bus_id == id;
      if (declared) REQUIRE(mixer.set_bus_strip(id, *bus));
    }
    mixer.settle_smoothers();
  };

  TrackMixerRuntime ordered;
  rig(ordered, {{1, 0.0f}, {2, 0.0f}}, {lane10, lane20});
  TrackMixerRuntime reordered;
  rig(reordered, {{1, 0.0f}, {2, 0.0f}}, {lane10, lane20});
  REQUIRE(reordered.set_buses({{2, 0.0f}, {1, 0.0f}}));
  CHECK(reordered.latency_samples() == ordered.latency_samples());

  StereoRender ordered_out;
  StereoRender reordered_out;
  render_tones(ordered, {10, 20}, kBlocks, 0, ordered_out);
  render_tones(reordered, {10, 20}, kBlocks, 0, reordered_out);
  CHECK(max_abs_difference(reordered_out.left, ordered_out.left) < 1.0e-6f);
  CHECK(max_abs_difference(reordered_out.right, ordered_out.right) < 1.0e-6f);
  // Non-vacuity: bus 1 is hard left and bus 2 hard right.
  CHECK(max_abs_difference(ordered_out.left, ordered_out.right) > 1.0e-2f);

  // Removing bus 1 leaves bus 2 with its own state and nothing of bus 1's.
  TrackMixerRuntime removed;
  rig(removed, {{1, 0.0f}, {2, 0.0f}}, {lane10, lane20});
  REQUIRE(removed.set_track_lanes({lane20}));
  REQUIRE(removed.set_buses({{2, 0.0f}}));
  TrackMixerRuntime only_b;
  rig(only_b, {{2, 0.0f}}, {lane20});
  StereoRender removed_out;
  StereoRender only_b_out;
  render_tones(removed, {20}, kBlocks, 0, removed_out);
  render_tones(only_b, {20}, kBlocks, 0, only_b_out);
  CHECK(max_abs_difference(removed_out.left, only_b_out.left) < 1.0e-6f);
  CHECK(max_abs_difference(removed_out.right, only_b_out.right) < 1.0e-6f);

  // Re-declaring bus 1 gives it a fresh default state.
  REQUIRE(removed.set_buses({{2, 0.0f}, {1, 0.0f}}));
  REQUIRE(removed.set_track_lanes({lane10, lane20}));
  TrackMixerRuntime fresh;
  fresh.prepare(48000.0, kBusBlock);
  REQUIRE(fresh.set_buses({{2, 0.0f}, {1, 0.0f}}));
  REQUIRE(fresh.set_track_lanes({lane10, lane20}));
  REQUIRE(fresh.set_bus_strip(2, b));
  fresh.settle_smoothers();
  removed.settle_smoothers();
  StereoRender readded_out;
  StereoRender fresh_out;
  render_tones(removed, {10, 20}, kBlocks, 0, readded_out);
  render_tones(fresh, {10, 20}, kBlocks, 0, fresh_out);
  CHECK(max_abs_difference(readded_out.left, fresh_out.left) < 1.0e-6f);
  CHECK(max_abs_difference(readded_out.right, fresh_out.right) < 1.0e-6f);
}

namespace {

constexpr int kEngineBlock = 256;
constexpr int kEngineFrames = kEngineBlock * 32;

// The clip keeps a pointer to `planes`, so a tone outlives every engine using it.
struct EngineTone {
  std::vector<float> left = std::vector<float>(kEngineFrames);
  std::vector<float> right = std::vector<float>(kEngineFrames);
  std::array<const float*, 2> planes{};
  EngineTone() {
    for (int n = 0; n < kEngineFrames; ++n) {
      left[static_cast<size_t>(n)] = lane_tone(10, 0, n);
      right[static_cast<size_t>(n)] = lane_tone(10, 1, n);
    }
    planes = {left.data(), right.data()};
  }
};

void start_master_engine(sonare::engine::RealtimeEngine& engine, const EngineTone& tone,
                         const Strip& master) {
  engine.prepare(48000.0, kEngineBlock);
  sonare::engine::ClipSchedule clip{
      1, {tone.planes.data(), 2, kEngineFrames}, 0.0, 0, 0, kEngineFrames, false, 1.0f, 0, 0};
  engine.set_clips({clip});
  REQUIRE(engine.set_master_strip(master));
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
}

void render_engine(sonare::engine::RealtimeEngine& engine, int blocks, StereoRender& out) {
  std::array<float, kEngineBlock> left{};
  std::array<float, kEngineBlock> right{};
  float* io[] = {left.data(), right.data()};
  for (int block = 0; block < blocks; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    engine.process(io, 2, kEngineBlock);
    out.left.insert(out.left.end(), left.begin(), left.end());
    out.right.insert(out.right.end(), right.begin(), right.end());
  }
}

}  // namespace

TEST_CASE("RealtimeEngine master strip resend keeps its inserts and follows its EQ",
          "[engine][track_mixer]") {
  constexpr int kBlocks = 6;
  const EngineTone tone;
  Strip verb;
  verb.id = "master";
  verb.inserts.push_back(
      {InsertSlot::PreFader, "effects.reverb.fdn", R"({"decaySec":2,"dryWet":0.5})"});
  Strip verb_eq = verb;
  verb_eq.eq.bands.push_back(boost_band());

  sonare::engine::RealtimeEngine control;
  start_master_engine(control, tone, verb_eq);
  StereoRender control_out;
  render_engine(control, 2 * kBlocks, control_out);

  // An identical resend neither rebuilds the reverb nor disturbs the EQ.
  sonare::engine::RealtimeEngine resent;
  start_master_engine(resent, tone, verb_eq);
  StereoRender resent_out;
  render_engine(resent, kBlocks, resent_out);
  REQUIRE(resent.set_master_strip(verb_eq));
  render_engine(resent, kBlocks, resent_out);
  REQUIRE(renders_equal(resent_out, control_out));

  // Non-vacuity: the EQ is audible on the master.
  sonare::engine::RealtimeEngine plain;
  start_master_engine(plain, tone, verb);
  StereoRender plain_out;
  render_engine(plain, 2 * kBlocks, plain_out);
  CHECK_FALSE(renders_equal(plain_out, control_out));

  // A resend without "eq" is flat, including a band the setter added.
  sonare::engine::RealtimeEngine cleared;
  start_master_engine(cleared, tone, verb);
  StereoRender cleared_out;
  render_engine(cleared, kBlocks, cleared_out);
  REQUIRE(cleared.set_master_eq_band(0, boost_band()));
  REQUIRE(cleared.set_master_strip(verb));
  render_engine(cleared, kBlocks, cleared_out);
  REQUIRE(renders_equal(cleared_out, plain_out));

  // Changed insert params still rebuild the chain.
  sonare::engine::RealtimeEngine rebuilt;
  start_master_engine(rebuilt, tone, verb);
  StereoRender rebuilt_out;
  render_engine(rebuilt, kBlocks, rebuilt_out);
  Strip verb_changed = verb;
  verb_changed.inserts[0].params_json = R"({"decaySec":2,"dryWet":0.6})";
  REQUIRE(rebuilt.set_master_strip(verb_changed));
  render_engine(rebuilt, kBlocks, rebuilt_out);
  CHECK_FALSE(renders_equal(rebuilt_out, plain_out));
}
