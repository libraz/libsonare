/// @file routing_capi_test.cpp
/// @brief Routed mixer C API tests.

#include <limits>

#include "mastering/api/insert_factory.h"
#include "routing_test_helpers.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

TEST_CASE("C-API mixer rejects null and duplicate strip ids at insertion", "[mixing][capi]") {
  SonareMixer* mixer = sonare_mixer_create(48000, 64);
  REQUIRE(mixer != nullptr);
  REQUIRE(sonare_mixer_add_strip(mixer, nullptr) == nullptr);
  REQUIRE(sonare_mixer_add_strip(mixer, "lead") != nullptr);
  REQUIRE(sonare_mixer_add_strip(mixer, "lead") == nullptr);
  REQUIRE(sonare_mixer_strip_count(mixer) == 1);
  sonare_mixer_destroy(mixer);
}

TEST_CASE("C-API scene mixer applies master bus trim polarity and width", "[mixing][capi][scene]") {
  constexpr int kBlock = 16;
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip source;
  source.id = "source";
  scene.strips.push_back(source);
  sonare::mixing::api::Bus master{"master", "master"};
  master.input_trim_db = 6.0206f;
  master.polarity_invert_left = true;
  master.width = 0.0f;
  scene.buses.push_back(master);

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), 48000, kBlock);
  REQUIRE(mixer != nullptr);

  std::vector<float> input_l(kBlock, 1.0f);
  std::vector<float> input_r(kBlock, 0.0f);
  const float* inputs_l[] = {input_l.data()};
  const float* inputs_r[] = {input_r.data()};
  std::vector<float> output_l(kBlock, 0.0f);
  std::vector<float> output_r(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, output_l.data(),
                                      output_r.data(), kBlock) == SONARE_OK);
  for (int i = 0; i < kBlock; ++i) {
    // +6.0206 dB doubles the left input, then left polarity inversion and
    // width=0 collapse the front pair to their common mid value (-1).
    REQUIRE_THAT(output_l[static_cast<size_t>(i)], WithinAbs(-1.0f, 0.0001f));
    REQUIRE_THAT(output_r[static_cast<size_t>(i)], WithinAbs(-1.0f, 0.0001f));
  }

  SonareMixMeterSnapshot meter{};
  REQUIRE(sonare_mixer_bus_meter(mixer, "master", &meter) == SONARE_OK);
  REQUIRE(meter.channel_count == 2);
  REQUIRE(std::isfinite(meter.peak_db[0]));
  REQUIRE(meter.peak_db[0] > -1.0f);
  REQUIRE(sonare_mixer_bus_meter(mixer, "missing", &meter) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_bus_meter(mixer, nullptr, &meter) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_bus_meter(mixer, "master", nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("C-API mixer recompilation preserves automation timeline and error kinds",
          "[mixing][capi][automation]") {
  constexpr int kBlock = 16;
  SonareMixer* mixer = sonare_mixer_create(48000, kBlock);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "source");
  REQUIRE(strip != nullptr);

  std::vector<float> input(kBlock, 1.0f);
  const float* inputs[] = {input.data()};
  std::vector<float> output_l(kBlock);
  std::vector<float> output_r(kBlock);
  REQUIRE(sonare_mixer_process_stereo(mixer, inputs, inputs, 1, output_l.data(), output_r.data(),
                                      kBlock) == SONARE_OK);

  REQUIRE(sonare_strip_schedule_fader_automation(strip, 2 * kBlock, -3.0f, 0) == SONARE_OK);
  REQUIRE(sonare_mixer_add_bus(mixer, "unused", "aux") == SONARE_OK);
  REQUIRE(sonare_mixer_compile(mixer) == SONARE_OK);
  // The rebuilt StripNode resumes at kBlock, so later absolute events remain
  // schedulable; the graph rebuild does not strand the lane at sample zero.
  REQUIRE(sonare_strip_schedule_fader_automation(strip, 3 * kBlock, -6.0f, 0) == SONARE_OK);
  // A decreasing timestamp is a caller error, not lane exhaustion.
  REQUIRE(sonare_strip_schedule_fader_automation(strip, 3 * kBlock - 1, -9.0f, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_mixer_process_stereo(mixer, inputs, inputs, 1, output_l.data(), output_r.data(),
                                      kBlock) == SONARE_OK);
  REQUIRE(sonare_mixer_process_stereo(mixer, inputs, inputs, 1, output_l.data(), output_r.data(),
                                      kBlock) == SONARE_OK);
  REQUIRE(output_l.back() < 0.99f);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("C-API strip runtime setters validate NULL and bad enums", "[mixing][capi]") {
  // NULL strip handle must be rejected by every setter.
  REQUIRE(sonare_strip_set_soloed(nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_solo_safe(nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_polarity_invert(nullptr, 1, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_pan_law(nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_channel_delay_samples(nullptr, 4) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_vca_offset_db(nullptr, -3.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_fader_automation(nullptr, 0, -6.0f, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_pan_automation(nullptr, 0, 0.5f, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_width_automation(nullptr, 0, 0.5f, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_send_automation(nullptr, 0, 0, -6.0f, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  SonareMixMeterSnapshot snapshot{};
  REQUIRE(sonare_strip_meter_tap(nullptr, SONARE_METER_TAP_PRE_FADER, &snapshot) ==
          SONARE_ERROR_INVALID_PARAMETER);

  SonareMixer* mixer = sonare_mixer_create(48000, 64);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "s");
  REQUIRE(strip != nullptr);

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  REQUIRE(sonare_strip_set_fader_db(strip, nan) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_pan(strip, inf, SONARE_PAN_MODE_BALANCE) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_width(strip, nan) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_fader_automation(strip, 0, inf, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_pan_automation(strip, 0, nan, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_fader_db(strip, -3.0f) == SONARE_OK);
  REQUIRE(sonare_strip_set_pan(strip, 0.25f, SONARE_PAN_MODE_BALANCE) == SONARE_OK);

  // Valid calls on a real strip succeed.
  REQUIRE(sonare_strip_set_soloed(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_solo_safe(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_polarity_invert(strip, 1, 0) == SONARE_OK);
  REQUIRE(sonare_strip_set_pan_law(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_channel_delay_samples(strip, 3) == SONARE_OK);
  REQUIRE(sonare_strip_set_channel_delay_samples(strip, sonare::mixing::kMaxAlignmentDelaySamples +
                                                            1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_vca_offset_db(strip, -2.0f) == SONARE_OK);

  // NULL out-pointer for meter tap is rejected even with a valid strip.
  REQUIRE(sonare_strip_meter_tap(strip, SONARE_METER_TAP_PRE_FADER, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Both valid taps succeed.
  REQUIRE(sonare_strip_meter_tap(strip, SONARE_METER_TAP_PRE_FADER, &snapshot) == SONARE_OK);
  REQUIRE(sonare_strip_meter_tap(strip, SONARE_METER_TAP_POST_FADER, &snapshot) == SONARE_OK);

  // Invalid enum values are rejected.
  REQUIRE(sonare_strip_set_pan_law(strip, 99) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_meter_tap(strip, 99, &snapshot) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_fader_automation(strip, 0, -6.0f, 99) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_pan_automation(strip, 0, 0.5f, 99) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_width_automation(strip, 0, 0.5f, 99) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_send_automation(strip, 0, 0, -6.0f, 99) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("C-API mixer reports latency and drains delayed output", "[mixing][capi]") {
  constexpr int kBlock = 8;
  SonareMixer* mixer = sonare_mixer_create(48000, kBlock);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "src");
  REQUIRE(strip != nullptr);
  REQUIRE(sonare_strip_set_channel_delay_samples(strip, 10) == SONARE_OK);

  int latency = -1;
  REQUIRE(sonare_mixer_latency_samples(mixer, &latency) == SONARE_OK);
  REQUIRE(latency == 10);
  int tail = -1;
  REQUIRE(sonare_mixer_tail_samples(mixer, &tail) == SONARE_OK);
  REQUIRE(tail >= 0);
  REQUIRE(sonare_mixer_latency_samples(nullptr, &latency) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_tail_samples(mixer, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  std::vector<float> in_l(kBlock, 0.0f);
  std::vector<float> in_r(kBlock, 0.0f);
  in_l[0] = 1.0f;
  in_r[0] = 1.0f;
  const float* inputs_l[] = {in_l.data()};
  const float* inputs_r[] = {in_r.data()};
  std::vector<float> out_l(kBlock, 0.0f);
  std::vector<float> out_r(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                      kBlock) == SONARE_OK);
  REQUIRE(block_energy(out_l, out_r) == 0.0);

  REQUIRE(sonare_mixer_drain_tail_stereo(mixer, out_l.data(), out_r.data(), kBlock) == SONARE_OK);
  REQUIRE(block_energy(out_l, out_r) > 0.0);
  REQUIRE(sonare_mixer_drain_tail_stereo(mixer, nullptr, out_r.data(), kBlock) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("C-API mixer tail follows the longest audible serial route", "[mixing][capi][tail]") {
  constexpr int kSampleRate = 48000;
  constexpr int kBlock = 64;
  const auto delay_tail = [=](const char* params) {
    auto processor = sonare::mastering::api::make_insert("effects.delay.stereo", params);
    REQUIRE(processor != nullptr);
    processor->prepare(kSampleRate, kBlock);
    return processor->tail_samples();
  };
  const char* strip_params = R"({"delayTimeLMs":10,"delayTimeRMs":10,"feedback":0,"dryWet":1})";
  const char* aux_params = R"({"delayTimeLMs":20,"delayTimeRMs":20,"feedback":0,"dryWet":1})";
  const char* master_params = R"({"delayTimeLMs":30,"delayTimeRMs":30,"feedback":0,"dryWet":1})";
  const char* orphan_params = R"({"delayTimeLMs":100,"delayTimeRMs":100,"feedback":0,"dryWet":1})";
  const int strip_tail = delay_tail(strip_params);
  const int aux_tail = delay_tail(aux_params);
  const int master_tail = delay_tail(master_params);
  const int expected_tail = strip_tail + aux_tail + master_tail;
  REQUIRE(expected_tail > std::max({strip_tail, aux_tail, master_tail}));

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip source;
  source.id = "source";
  source.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PostFader, "effects.delay.stereo", strip_params});
  source.sends.push_back({"to-aux", "aux", 0.0f, sonare::mixing::api::SendTiming::PostFader});
  scene.strips.push_back(std::move(source));

  sonare::mixing::api::Bus aux{"aux", "aux"};
  aux.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PostFader, "effects.delay.stereo", aux_params});
  scene.buses.push_back(std::move(aux));
  sonare::mixing::api::Bus master{"master", "master"};
  master.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PostFader, "effects.delay.stereo", master_params});
  scene.buses.push_back(std::move(master));
  sonare::mixing::api::Bus orphan{"orphan", "aux"};
  orphan.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PostFader, "effects.delay.stereo", orphan_params});
  scene.buses.push_back(std::move(orphan));
  scene.connections.push_back({"aux", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSampleRate, kBlock);
  REQUIRE(mixer != nullptr);
  int reported_tail = 0;
  REQUIRE(sonare_mixer_tail_samples(mixer, &reported_tail) == SONARE_OK);
  REQUIRE(reported_tail == expected_tail);

  std::vector<float> input_l(kBlock, 0.0f);
  std::vector<float> input_r(kBlock, 0.0f);
  input_l[0] = 1.0f;
  input_r[0] = 1.0f;
  const float* inputs_l[] = {input_l.data()};
  const float* inputs_r[] = {input_r.data()};
  std::vector<float> block_l(kBlock, 0.0f);
  std::vector<float> block_r(kBlock, 0.0f);
  std::vector<float> rendered_l(static_cast<size_t>(expected_tail + 2 * kBlock), 0.0f);
  std::vector<float> rendered_r(rendered_l.size(), 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, block_l.data(), block_r.data(),
                                      kBlock) == SONARE_OK);
  std::copy(block_l.begin(), block_l.end(), rendered_l.begin());
  std::copy(block_r.begin(), block_r.end(), rendered_r.begin());
  for (size_t cursor = kBlock; cursor < rendered_l.size(); cursor += kBlock) {
    REQUIRE(sonare_mixer_drain_tail_stereo(mixer, block_l.data(), block_r.data(), kBlock) ==
            SONARE_OK);
    const size_t count = std::min<size_t>(kBlock, rendered_l.size() - cursor);
    std::copy_n(block_l.begin(), count, rendered_l.begin() + static_cast<ptrdiff_t>(cursor));
    std::copy_n(block_r.begin(), count, rendered_r.begin() + static_cast<ptrdiff_t>(cursor));
  }
  float late_peak = 0.0f;
  const size_t late_begin = static_cast<size_t>(std::max(0, expected_tail - 8));
  const size_t late_end = std::min(rendered_l.size(), static_cast<size_t>(expected_tail + 9));
  for (size_t i = late_begin; i < late_end; ++i) {
    late_peak = std::max({late_peak, std::abs(rendered_l[i]), std::abs(rendered_r[i])});
  }
  REQUIRE(late_peak > 0.1f);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("C-API mixer drains serial Haas and fractional phase-align tails",
          "[mixing][capi][tail]") {
  constexpr int kSampleRate = 8000;
  constexpr int kBlock = 1;
  const char* haas_params = R"({"delayMs":0.25,"mix":1,"delayRight":true})";
  const char* phase_params = R"({"delaySamples":3,"fractionalDelaySamples":0.5,"delayRight":true})";

  auto haas = sonare::mastering::api::make_insert("stereo.haasEnhancer", haas_params);
  auto phase = sonare::mastering::api::make_insert("stereo.phaseAlign", phase_params);
  REQUIRE(haas != nullptr);
  REQUIRE(phase != nullptr);
  haas->prepare(kSampleRate, kBlock);
  phase->prepare(kSampleRate, kBlock);
  REQUIRE(haas->tail_samples() == 2);
  REQUIRE(phase->tail_samples() == 7);
  const int expected_tail = haas->tail_samples() + phase->tail_samples();

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip strip;
  strip.id = "source";
  strip.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PostFader, "stereo.haasEnhancer", haas_params});
  scene.strips.push_back(std::move(strip));
  sonare::mixing::api::Bus master{"master", "master"};
  master.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PostFader, "stereo.phaseAlign", phase_params});
  scene.buses.push_back(std::move(master));
  scene.connections.push_back({"source", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSampleRate, kBlock);
  REQUIRE(mixer != nullptr);
  int reported_tail = 0;
  REQUIRE(sonare_mixer_tail_samples(mixer, &reported_tail) == SONARE_OK);
  REQUIRE(reported_tail == expected_tail);

  float input_l = 1.0f;
  float input_r = 0.0f;
  const float* inputs_l[] = {&input_l};
  const float* inputs_r[] = {&input_r};
  float output_l = 0.0f;
  float output_r = 0.0f;
  REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, &output_l, &output_r, kBlock) ==
          SONARE_OK);

  for (int i = 0; i < expected_tail; ++i) {
    REQUIRE(sonare_mixer_drain_tail_stereo(mixer, &output_l, &output_r, kBlock) == SONARE_OK);
  }
  REQUIRE(std::abs(output_r) > 1.0e-6f);
  REQUIRE(sonare_mixer_drain_tail_stereo(mixer, &output_l, &output_r, kBlock) == SONARE_OK);
  REQUIRE_THAT(output_r, WithinAbs(0.0f, 1.0e-6f));

  sonare_mixer_destroy(mixer);
}

TEST_CASE("C-API VCA groups deduplicate duplicate member ids", "[mixing][capi]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 4096;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip strip;
  strip.id = "lead";
  strip.pan_law = 3;  // Linear0dB.
  scene.strips.push_back(strip);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"lead", "master"});
  scene.vca_groups.push_back({"lead-vca", -6.0f, {"lead", "lead"}});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);

  std::vector<float> input(kBlock, 1.0f);
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};
  std::vector<float> out_l(kBlock, 0.0f);
  std::vector<float> out_r(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE_THAT(out_l[kBlock - 1], WithinAbs(0.501187f, 0.01f));

  char* round_trip = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &round_trip) == SONARE_OK);
  REQUIRE(round_trip != nullptr);
  const std::string round_trip_json(round_trip);
  sonare_free_string(round_trip);
  const auto restored = sonare::mixing::api::scene_from_json(round_trip_json);
  REQUIRE(restored.vca_groups.size() == 1);
  REQUIRE(restored.vca_groups[0].members == std::vector<std::string>{"lead"});

  sonare_mixer_destroy(mixer);

  SonareMixer* live = sonare_mixer_create(kSr, kBlock);
  REQUIRE(live != nullptr);
  REQUIRE(sonare_mixer_add_strip(live, "lead") != nullptr);
  const char* members[] = {"lead", "lead"};
  REQUIRE(sonare_mixer_add_vca_group(live, "live-vca", -6.0f, members, 2) == SONARE_OK);
  input.assign(kBlock, 1.0f);
  out_l.assign(kBlock, 0.0f);
  out_r.assign(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(live, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE_THAT(out_l[kBlock - 1], WithinAbs(0.501187f, 0.01f));
  REQUIRE(sonare_mixer_remove_vca_group(live, "live-vca") == SONARE_OK);
  out_l.assign(kBlock, 0.0f);
  out_r.assign(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(live, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE(out_l[kBlock - 1] > 0.99f);
  sonare_mixer_destroy(live);
}

TEST_CASE("C-API VCA groups apply to strips added after group creation", "[mixing][capi]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 4096;

  SonareMixer* mixer = sonare_mixer_create(kSr, kBlock);
  REQUIRE(mixer != nullptr);
  const char* members[] = {"lead"};
  REQUIRE(sonare_mixer_add_vca_group(mixer, "live-vca", -6.0f, members, 1) == SONARE_OK);
  REQUIRE(sonare_mixer_add_strip(mixer, "lead") != nullptr);

  std::vector<float> input(kBlock, 1.0f);
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};
  std::vector<float> out_l(kBlock, 0.0f);
  std::vector<float> out_r(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE_THAT(out_l[kBlock - 1], WithinAbs(0.501187f, 0.01f));
  sonare_mixer_destroy(mixer);
}

TEST_CASE("C-API VCA group gain setter updates live gain and scene state", "[mixing][capi]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 4096;

  SonareMixer* mixer = sonare_mixer_create(kSr, kBlock);
  REQUIRE(mixer != nullptr);
  REQUIRE(sonare_mixer_add_strip(mixer, "lead") != nullptr);
  const char* members[] = {"lead"};
  REQUIRE(sonare_mixer_add_vca_group(mixer, "lead-vca", -6.0f, members, 1) == SONARE_OK);

  std::vector<float> input(kBlock, 1.0f);
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};
  std::vector<float> out_l(kBlock, 0.0f);
  std::vector<float> out_r(kBlock, 0.0f);

  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE_THAT(out_l[kBlock - 1], WithinAbs(0.501187f, 0.01f));

  REQUIRE(sonare_mixer_set_vca_group_gain_db(mixer, "lead-vca", -12.0f) == SONARE_OK);
  out_l.assign(kBlock, 0.0f);
  out_r.assign(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE_THAT(out_l[kBlock - 1], WithinAbs(0.251189f, 0.01f));

  REQUIRE(sonare_mixer_set_vca_group_members(mixer, "lead-vca", nullptr, 0) == SONARE_OK);
  out_l.assign(kBlock, 0.0f);
  out_r.assign(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE_THAT(out_l[kBlock - 1], WithinAbs(1.0f, 0.01f));
  REQUIRE(sonare_mixer_set_vca_group_members(mixer, "lead-vca", members, 1) == SONARE_OK);

  char* round_trip = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &round_trip) == SONARE_OK);
  REQUIRE(round_trip != nullptr);
  const std::string round_trip_json(round_trip);
  sonare_free_string(round_trip);
  const auto restored = sonare::mixing::api::scene_from_json(round_trip_json);
  REQUIRE(restored.vca_groups.size() == 1);
  REQUIRE_THAT(restored.vca_groups[0].gain_db, WithinAbs(-12.0f, 0.0001f));
  REQUIRE(restored.vca_groups[0].members == std::vector<std::string>{"lead"});

  REQUIRE(sonare_mixer_set_vca_group_gain_db(mixer, "missing-vca", -3.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_set_vca_group_gain_db(nullptr, "lead-vca", -3.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_set_vca_group_gain_db(mixer, nullptr, -3.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_set_vca_group_members(mixer, "missing-vca", members, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("C-API strip setters reflect into scene JSON for cached fields", "[mixing][capi]") {
  // Load a two-strip scene, mutate a strip through the runtime setters, then
  // serialize and re-parse. Fields that the C layer caches into scene_strip
  // (soloed, solo_safe, polarity, pan_law, channel_delay) must round-trip.
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip a;
  a.id = "a";
  sonare::mixing::api::Strip b;
  b.id = "b";
  scene.strips.push_back(a);
  scene.strips.push_back(b);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"a", "master"});
  scene.connections.push_back({"b", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), 48000, 64);
  REQUIRE(mixer != nullptr);

  SonareStrip* strip = sonare_mixer_strip_at(mixer, 0);
  REQUIRE(strip != nullptr);
  REQUIRE(sonare_strip_set_soloed(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_solo_safe(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_polarity_invert(strip, 1, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_pan_law(strip, 2) == SONARE_OK);  // Const6dB
  REQUIRE(sonare_strip_set_channel_delay_samples(strip, 11) == SONARE_OK);
  REQUIRE(sonare_strip_set_vca_offset_db(strip, -2.0f) == SONARE_OK);

  char* round_trip = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &round_trip) == SONARE_OK);
  REQUIRE(round_trip != nullptr);
  const std::string restored_json(round_trip);
  sonare_free_string(round_trip);
  sonare_mixer_destroy(mixer);

  const auto parsed = sonare::mixing::api::scene_from_json(restored_json);
  REQUIRE(parsed.strips.size() == 2);
  const auto& out = parsed.strips[0];
  REQUIRE(out.id == "a");
  REQUIRE(out.soloed);
  REQUIRE(out.solo_safe);
  REQUIRE(out.polarity_invert_left);
  REQUIRE(out.polarity_invert_right);
  REQUIRE(out.pan_law == 2);
  REQUIRE(out.channel_delay_samples == 11);
  REQUIRE_THAT(out.vca_offset_db, WithinAbs(-2.0f, 0.0001f));
}

TEST_CASE("C-API solo and solo-safe gate the audio output", "[mixing][capi]") {
  // Three strips with distinct constant levels feed the master. Soloing one
  // strip silences the others, except a solo-safe strip which must keep
  // contributing (the regression this guards).
  constexpr int kSr = 48000;
  // Large block so the per-strip pan smoothers settle to their steady gains
  // before the contributions are read at the block tail.
  constexpr int kBlock = 4096;

  sonare::mixing::api::Scene scene;
  for (const char* id : {"a", "b", "c"}) {
    sonare::mixing::api::Strip strip;
    strip.id = id;
    strip.pan_law = 3;  // Linear0dB so the steady L gain is exactly 1.0 (no -3 dB pan).
    scene.strips.push_back(strip);
    scene.connections.push_back({id, "master"});
  }
  scene.buses.push_back({"master", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);

  // Distinct constant levels so each strip's contribution to the master is
  // separable: a -> 1.0, b -> 0.1, c -> 0.01. Summed at the master, the steady
  // L output identifies exactly which strips contributed.
  std::vector<float> a_in(kBlock, 1.0f);
  std::vector<float> b_in(kBlock, 0.1f);
  std::vector<float> c_in(kBlock, 0.01f);

  // Returns the fully-settled master L level (last sample of the block).
  auto process_settled = [&](SonareMixer* mixer) {
    const float* in_l[] = {a_in.data(), b_in.data(), c_in.data()};
    const float* in_r[] = {a_in.data(), b_in.data(), c_in.data()};
    std::vector<float> out_l(kBlock, 0.0f);
    std::vector<float> out_r(kBlock, 0.0f);
    REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 3, out_l.data(), out_r.data(), kBlock) ==
            SONARE_OK);
    return out_l[kBlock - 1];
  };

  SECTION("no solo lets every strip through") {
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);
    REQUIRE_THAT(process_settled(mixer), WithinAbs(1.11f, 1e-3f));
    sonare_mixer_destroy(mixer);
  }

  SECTION("soloing one strip silences the others") {
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);
    SonareStrip* a_strip = sonare_mixer_strip_by_id(mixer, "a");
    REQUIRE(a_strip != nullptr);
    REQUIRE(sonare_strip_set_soloed(a_strip, 1) == SONARE_OK);
    // Only a (1.0) contributes; b and c are implied-muted.
    REQUIRE_THAT(process_settled(mixer), WithinAbs(1.0f, 1e-3f));
    sonare_mixer_destroy(mixer);
  }

  SECTION("a solo-safe strip survives another strip's solo") {
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);
    SonareStrip* a_strip = sonare_mixer_strip_by_id(mixer, "a");
    SonareStrip* c_strip = sonare_mixer_strip_by_id(mixer, "c");
    REQUIRE(a_strip != nullptr);
    REQUIRE(c_strip != nullptr);
    // c is solo-safe; soloing a must NOT mute c, but must mute b.
    REQUIRE(sonare_strip_set_solo_safe(c_strip, 1) == SONARE_OK);
    REQUIRE(sonare_strip_set_soloed(a_strip, 1) == SONARE_OK);
    // a (1.0) + c (0.01) contribute, b (0.1) is silenced.
    REQUIRE_THAT(process_settled(mixer), WithinAbs(1.01f, 1e-3f));
    sonare_mixer_destroy(mixer);
  }
}

TEST_CASE("C-API fader automation changes the strip's effective gain",
          "[mixing][capi][automation]") {
  // Scheduling fader automation and processing past the scheduled sample
  // position must lower the strip's output. -120 dB at sample 0 effectively
  // mutes a constant input.
  constexpr int kSr = 48000;
  // Large block so the pan/fader smoothers fully settle within one block.
  constexpr int kBlock = 4096;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip strip;
  strip.id = "lead";
  strip.pan_law = 3;  // Linear0dB so the settled unity fader yields unity L output.
  scene.strips.push_back(strip);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"lead", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);
  SonareStrip* lead = sonare_mixer_strip_by_id(mixer, "lead");
  REQUIRE(lead != nullptr);

  std::vector<float> input(kBlock, 1.0f);
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};
  std::vector<float> out_l(kBlock, 0.0f);
  std::vector<float> out_r(kBlock, 0.0f);

  // Baseline block at unity fader passes the signal through at full level once
  // the smoothers have settled by the block tail.
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE(out_l[kBlock - 1] > 0.99f);

  // Schedule a steep drop. The mixer advances its sample position across
  // process calls; place the event at the start of the next block's range.
  REQUIRE(sonare_strip_schedule_fader_automation(lead, kBlock, -120.0f, 0) == SONARE_OK);
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  // By the end of the block the smoothed fader has fallen far below unity.
  REQUIRE(out_l[kBlock - 1] < 0.1f);

  sonare_mixer_destroy(mixer);
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH
