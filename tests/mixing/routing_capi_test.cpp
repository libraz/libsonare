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

  // A channel delay is not latency, but its delayed output is still owed as tail.
  int latency = -1;
  REQUIRE(sonare_mixer_latency_samples(mixer, &latency) == SONARE_OK);
  REQUIRE(latency == 0);
  int tail = -1;
  REQUIRE(sonare_mixer_tail_samples(mixer, &tail) == SONARE_OK);
  REQUIRE(tail >= 10);
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

TEST_CASE("C-API mixer channel delay moves its own strip relative to the others",
          "[mixing][capi][pdc]") {
  constexpr int kBlock = 256;
  constexpr int kBlocks = 4;
  constexpr int kImpulse = 10;
  constexpr int kDelay = 100;
  // 0.16666667 ms of lookahead is exactly 8 samples at 48 kHz.
  constexpr int kInsertLatency = 8;

  struct Result {
    int onset = -1;
    int latency = -1;
  };
  // Strip "a" is plain, strip "b" carries a latent insert; only @p sounding gets the impulse.
  const auto render = [&](int delay, bool via_setter, size_t sounding) {
    sonare::mixing::api::Scene scene;
    sonare::mixing::api::Strip a;
    a.id = "a";
    if (!via_setter) a.channel_delay_samples = delay;
    sonare::mixing::api::Strip b;
    b.id = "b";
    b.inserts.push_back({sonare::mixing::api::InsertSlot::PreFader, "dynamics.limiter",
                         R"({"thresholdDb":24,"lookaheadMs":0.16666667,"releaseMs":50})"});
    scene.strips = {a, b};
    const std::string json = sonare::mixing::api::scene_to_json(scene);
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), 48000, kBlock);
    REQUIRE(mixer != nullptr);
    if (via_setter) {
      SonareStrip* strip = sonare_mixer_strip_by_id(mixer, "a");
      REQUIRE(strip != nullptr);
      REQUIRE(sonare_strip_set_channel_delay_samples(strip, delay) == SONARE_OK);
    }

    std::array<std::vector<float>, 2> inputs{std::vector<float>(kBlock, 0.0f),
                                             std::vector<float>(kBlock, 0.0f)};
    const float* inputs_l[] = {inputs[0].data(), inputs[1].data()};
    const float* inputs_r[] = {inputs[0].data(), inputs[1].data()};
    std::vector<float> out_l(kBlock, 0.0f);
    std::vector<float> out_r(kBlock, 0.0f);
    Result result;
    for (int block = 0; block < kBlocks; ++block) {
      inputs[sounding][kImpulse] = block == 0 ? 1.0f : 0.0f;
      REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 2, out_l.data(), out_r.data(),
                                          kBlock) == SONARE_OK);
      for (int i = 0; i < kBlock && result.onset < 0; ++i) {
        if (std::abs(out_l[static_cast<size_t>(i)]) > 0.25f) result.onset = block * kBlock + i;
      }
    }
    REQUIRE(sonare_mixer_latency_samples(mixer, &result.latency) == SONARE_OK);
    sonare_mixer_destroy(mixer);
    return result;
  };

  const int baseline = kImpulse + kInsertLatency;
  for (const bool via_setter : {false, true}) {
    INFO("via setter " << via_setter);
    REQUIRE(render(0, via_setter, 0).onset == baseline);
    REQUIRE(render(0, via_setter, 1).onset == baseline);
    const Result moved = render(kDelay, via_setter, 0);
    const Result unmoved = render(kDelay, via_setter, 1);
    CHECK(moved.onset == baseline + kDelay);
    CHECK(unmoved.onset == baseline);
    CHECK(moved.latency == kInsertLatency);
  }
}

// Routes through effects.delay.stereo at every stage, so this case
// additionally needs the FX suite.
#if defined(SONARE_WITH_FX)
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
#endif  // SONARE_WITH_FX

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

TEST_CASE("C-API VCA group gain rejects non-finite and applies a valid value", "[mixing][capi]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 4096;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  SonareMixer* mixer = sonare_mixer_create(kSr, kBlock);
  REQUIRE(mixer != nullptr);
  REQUIRE(sonare_mixer_add_strip(mixer, "lead") != nullptr);
  const char* members[] = {"lead"};

  // A non-finite gain must not create the group, and the strip's gain must be
  // left untouched (checked below via readback, not merely a rejected call).
  REQUIRE(sonare_mixer_add_vca_group(mixer, "lead-vca", nan, members, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_add_vca_group(mixer, "lead-vca", inf, members, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  size_t group_count = 0;
  REQUIRE(sonare_mixer_vca_group_count(mixer, &group_count) == SONARE_OK);
  REQUIRE(group_count == 0);

  std::vector<float> input(kBlock, 1.0f);
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};
  std::vector<float> out_l(kBlock, 0.0f);
  std::vector<float> out_r(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE_THAT(out_l[kBlock - 1], WithinAbs(1.0f, 0.01f));

  // A valid gain is applied on the live strip.
  REQUIRE(sonare_mixer_add_vca_group(mixer, "lead-vca", -6.0f, members, 1) == SONARE_OK);
  out_l.assign(kBlock, 0.0f);
  out_r.assign(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE_THAT(out_l[kBlock - 1], WithinAbs(0.501187f, 0.01f));

  // A non-finite gain update must be rejected and leave the applied gain unchanged.
  REQUIRE(sonare_mixer_set_vca_group_gain_db(mixer, "lead-vca", nan) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_set_vca_group_gain_db(mixer, "lead-vca", inf) ==
          SONARE_ERROR_INVALID_PARAMETER);
  out_l.assign(kBlock, 0.0f);
  out_r.assign(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE_THAT(out_l[kBlock - 1], WithinAbs(0.501187f, 0.01f));

  // A valid gain update is applied.
  REQUIRE(sonare_mixer_set_vca_group_gain_db(mixer, "lead-vca", -12.0f) == SONARE_OK);
  out_l.assign(kBlock, 0.0f);
  out_r.assign(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE_THAT(out_l[kBlock - 1], WithinAbs(0.251189f, 0.01f));

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

// A recompile is a topology refresh, not an audio reset. Strip DSP state
// already survives one -- StripNode is built around a pointer into the
// persistently-owned ChannelStrip -- but every bus got a brand-new FxBus and a
// fresh make_insert() on each compile, so the in-flight contents of a bus
// insert were discarded. The trigger is what makes it reachable: a strip-only
// edit marks the graph dirty, and the next processed block silently rebuilds
// every bus chain.
//
// A single compile cannot see this: the state has to be put in, a second
// compile forced, and the state observed afterwards.
TEST_CASE("recompiling for a strip edit keeps bus insert state", "[mixing][routing]") {
  constexpr int kSampleRate = 48000;
  constexpr int kBlock = 64;
  // Longer than one block, so the impulse is still inside the aux delay line
  // when the recompile happens rather than already emitted.
  const char* aux_params = R"({"delayTimeLMs":10,"delayTimeRMs":10,"feedback":0,"dryWet":1})";

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip source;
  source.id = "source";
  source.sends.push_back({"to-aux", "aux", 0.0f, sonare::mixing::api::SendTiming::PostFader});
  scene.strips.push_back(std::move(source));
  sonare::mixing::api::Bus aux{"aux", "aux"};
  aux.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PostFader, "effects.delay.stereo", aux_params});
  scene.buses.push_back(std::move(aux));
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"aux", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);

  // Renders an impulse through the mixer, optionally forcing a recompile with a
  // strip-only edit after the first block, and returns the summed output energy
  // after that point. The two runs differ only in whether the recompile happens.
  auto render = [&](bool recompile_midway) {
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSampleRate, kBlock);
    REQUIRE(mixer != nullptr);
    std::vector<float> input_l(kBlock, 0.0f);
    std::vector<float> input_r(kBlock, 0.0f);
    input_l[0] = 1.0f;
    input_r[0] = 1.0f;
    const float* inputs_l[] = {input_l.data()};
    const float* inputs_r[] = {input_r.data()};
    std::vector<float> out_l(kBlock, 0.0f);
    std::vector<float> out_r(kBlock, 0.0f);

    // Block 1 carries the impulse in; the 10 ms delay holds it.
    REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                        kBlock) == SONARE_OK);

    if (recompile_midway) {
      SonareStrip* strip = sonare_mixer_strip_by_id(mixer, "source");
      REQUIRE(strip != nullptr);
      // Touches nothing about any bus, but marks the graph dirty so the next
      // processed block recompiles.
      REQUIRE(sonare_strip_set_channel_delay_samples(strip, 3) == SONARE_OK);
    }

    // Silence from here: everything that comes out is the delay line emptying.
    std::fill(input_l.begin(), input_l.end(), 0.0f);
    std::fill(input_r.begin(), input_r.end(), 0.0f);
    double energy = 0.0;
    const int blocks = (kSampleRate / 1000 * 10) / kBlock + 4;
    for (int block = 0; block < blocks; ++block) {
      REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                          kBlock) == SONARE_OK);
      for (int i = 0; i < kBlock; ++i) {
        energy += static_cast<double>(out_l[i]) * out_l[i];
      }
    }
    sonare_mixer_destroy(mixer);
    return energy;
  };

  const double undisturbed = render(false);
  // Non-vacuity: the impulse really does come back out of the aux delay, so an
  // empty result below would mean lost state rather than a silent fixture.
  REQUIRE(undisturbed > 1e-6);

  const double after_recompile = render(true);
  REQUIRE(after_recompile > 1e-6);
  REQUIRE_THAT(after_recompile, WithinAbs(undisturbed, undisturbed * 0.01));
}

TEST_CASE("C-API scene mixer applies strip EQ and round-trips it through scene JSON",
          "[mixing][capi][eq]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 4096;

  sonare::mastering::eq::EqBand band;
  band.type = sonare::mastering::eq::EqBandType::Peak;
  band.frequency_hz = 1000.0f;
  band.gain_db = 18.0f;
  band.q = 1.0f;
  band.enabled = true;

  auto make_scene = [&](bool with_eq) {
    sonare::mixing::api::Scene scene;
    sonare::mixing::api::Strip strip;
    strip.id = "source";
    strip.pan_law = 3;  // Linear0dB so the settled unity pan does not itself
                        // change the level (see routing_capi_test's other
                        // Linear0dB cases).
    if (with_eq) {
      strip.eq.bands.push_back(band);
    }
    scene.strips.push_back(strip);
    scene.buses.push_back({"master", "master"});
    scene.connections.push_back({"source", "master"});
    return scene;
  };

  std::vector<float> input(kBlock);
  for (int i = 0; i < kBlock; ++i) {
    input[static_cast<size_t>(i)] = std::sin(sonare::constants::kTwoPi * 1000.0f *
                                             static_cast<float>(i) / static_cast<float>(kSr));
  }
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};

  auto settled_peak = [&](bool with_eq) {
    const std::string json = sonare::mixing::api::scene_to_json(make_scene(with_eq));
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);
    std::vector<float> out_l(kBlock, 0.0f);
    std::vector<float> out_r(kBlock, 0.0f);
    REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
            SONARE_OK);
    sonare_mixer_destroy(mixer);
    float peak = 0.0f;
    for (int i = kBlock / 2; i < kBlock; ++i) {
      peak = std::max(peak, std::abs(out_l[static_cast<size_t>(i)]));
    }
    return peak;
  };

  // The strip's own EQ audibly boosts a 1 kHz tone at the strip's own 1 kHz peak.
  const float flat_peak = settled_peak(false);
  const float boosted_peak = settled_peak(true);
  REQUIRE(boosted_peak > flat_peak * 1.5f);

  // Round-trip: the strip's eq spec survives sonare_mixer_to_scene_json unchanged.
  const std::string json = sonare::mixing::api::scene_to_json(make_scene(true));
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);
  char* round_trip = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &round_trip) == SONARE_OK);
  REQUIRE(round_trip != nullptr);
  const std::string round_trip_json(round_trip);
  sonare_free_string(round_trip);
  sonare_mixer_destroy(mixer);

  const auto restored = sonare::mixing::api::scene_from_json(round_trip_json);
  REQUIRE(restored.strips.size() == 1);
  REQUIRE(restored.strips[0].eq.enabled);
  REQUIRE(restored.strips[0].eq.bands.size() == 1);
  REQUIRE(restored.strips[0].eq.bands[0] == band);
}

TEST_CASE(
    "C-API scene bus (including the role-master bus) applies pan in balance, stereo, and "
    "dual pan modes",
    "[mixing][capi][pan]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 4096;

  // Distinct L/R levels so the three modes are separable: Balance leaves the
  // image alone and only attenuates the far channel, StereoPan collapses the
  // pair to mono before panning, and DualPan routes each side independently.
  const float in_l_level = 1.0f;
  const float in_r_level = 0.5f;

  auto render = [&](sonare::mixing::api::Bus master) {
    sonare::mixing::api::Scene scene;
    sonare::mixing::api::Strip strip;
    strip.id = "source";
    strip.pan_law = 3;  // Linear0dB
    scene.strips.push_back(strip);
    master.id = "master";
    master.role = "master";
    master.pan_law = 3;  // Linear0dB: exact gains at the hard sides.
    scene.buses.push_back(master);
    scene.connections.push_back({"source", "master"});

    const std::string json = sonare::mixing::api::scene_to_json(scene);
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);
    std::vector<float> in_l(kBlock, in_l_level);
    std::vector<float> in_r(kBlock, in_r_level);
    const float* inputs_l[] = {in_l.data()};
    const float* inputs_r[] = {in_r.data()};
    std::vector<float> out_l(kBlock, 0.0f);
    std::vector<float> out_r(kBlock, 0.0f);
    REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                        kBlock) == SONARE_OK);
    sonare_mixer_destroy(mixer);
    return std::pair<float, float>{out_l[kBlock - 1], out_r[kBlock - 1]};
  };

  SECTION("balance mode pans hard left, attenuating only the right channel") {
    sonare::mixing::api::Bus master;
    master.pan = -1.0f;
    master.pan_mode = SONARE_PAN_MODE_BALANCE;
    const auto [l, r] = render(master);
    REQUIRE_THAT(l, WithinAbs(in_l_level, 1e-4f));
    REQUIRE_THAT(r, WithinAbs(0.0f, 1e-4f));
  }

  SECTION("stereo pan mode collapses the pair to mono before panning") {
    sonare::mixing::api::Bus master;
    master.pan = -1.0f;
    master.pan_mode = SONARE_PAN_MODE_STEREO_PAN;
    const auto [l, r] = render(master);
    const float mono = 0.5f * (in_l_level + in_r_level);
    REQUIRE_THAT(l, WithinAbs(mono, 1e-4f));
    REQUIRE_THAT(r, WithinAbs(0.0f, 1e-4f));
  }

  SECTION("dual pan mode routes each input side independently") {
    sonare::mixing::api::Bus master;
    master.pan_mode = SONARE_PAN_MODE_DUAL_PAN;
    master.dual_pan_left = 1.0f;    // input left routed hard right
    master.dual_pan_right = -1.0f;  // input right routed hard left
    const auto [l, r] = render(master);
    REQUIRE_THAT(l, WithinAbs(in_r_level, 1e-4f));
    REQUIRE_THAT(r, WithinAbs(in_l_level, 1e-4f));
  }
}

TEST_CASE("C-API role-master scene bus pan settles immediately with no glide at render start",
          "[mixing][capi][pan]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;  // shorter than the panner's 5 ms smoothing window

  // The strip is left at its construction defaults (Balance, pan 0, Const3dB),
  // so it carries no pan transient of its own; only the bus's settle behavior
  // is under test here.
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip strip;
  strip.id = "source";
  scene.strips.push_back(strip);
  sonare::mixing::api::Bus master{"master", "master"};
  master.pan = -1.0f;
  master.pan_law = 3;  // Linear0dB: exact gains, {left=1, right=0} at pan -1.
  scene.buses.push_back(master);
  scene.connections.push_back({"source", "master"});

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
  sonare_mixer_destroy(mixer);

  // Had the bus pan glided in from center instead of settling at scene load,
  // sample 0 of this very first block would still carry most of the right
  // channel; multiplying by the settled hard-left gain (0 on the right, exact
  // for Linear0dB) is exactly zero regardless of the strip's own settle state.
  REQUIRE(out_r[0] == 0.0f);
  REQUIRE(out_l[0] > 0.5f);
}

TEST_CASE("C-API scene bus applies its own EQ to the summed signal", "[mixing][capi][eq]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 4096;

  auto make_scene = [](bool with_eq) {
    sonare::mixing::api::Scene scene;
    sonare::mixing::api::Strip strip;
    strip.id = "source";
    strip.pan_law = 3;  // Linear0dB
    scene.strips.push_back(strip);
    sonare::mixing::api::Bus master{"master", "master"};
    master.pan_law = 3;
    if (with_eq) {
      sonare::mastering::eq::EqBand band;
      band.type = sonare::mastering::eq::EqBandType::Peak;
      band.frequency_hz = 1000.0f;
      band.gain_db = 18.0f;
      band.q = 1.0f;
      band.enabled = true;
      master.eq.bands.push_back(band);
    }
    scene.buses.push_back(master);
    scene.connections.push_back({"source", "master"});
    return scene;
  };

  std::vector<float> input(kBlock);
  for (int i = 0; i < kBlock; ++i) {
    input[static_cast<size_t>(i)] = std::sin(sonare::constants::kTwoPi * 1000.0f *
                                             static_cast<float>(i) / static_cast<float>(kSr));
  }
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};

  auto settled_peak = [&](bool with_eq) {
    const std::string json = sonare::mixing::api::scene_to_json(make_scene(with_eq));
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);
    std::vector<float> out_l(kBlock, 0.0f);
    std::vector<float> out_r(kBlock, 0.0f);
    REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
            SONARE_OK);
    sonare_mixer_destroy(mixer);
    float peak = 0.0f;
    for (int i = kBlock / 2; i < kBlock; ++i) {
      peak = std::max(peak, std::abs(out_l[static_cast<size_t>(i)]));
    }
    return peak;
  };

  const float flat_peak = settled_peak(false);
  const float boosted_peak = settled_peak(true);
  REQUIRE(boosted_peak > flat_peak * 1.5f);
}

TEST_CASE("C-API default bus output is bit-identical to the summed strip inputs",
          "[mixing][capi]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 512;

  // A default Bus{} must add nothing of its own to the signal it sums (see
  // BusNode's at_rest_identity()/eq-has-no-band/width==1 skip rules): the
  // master's output for two strips must equal the exact per-lane sum, where
  // each lane's own contribution is measured by running that strip alone
  // through an identical default bus (its own default pan/width stages carry
  // whatever floating-point behavior they already have; the point here is
  // that the bus adds no further deviation on top of what its lanes hand it).
  auto make_scene = [](const std::vector<std::string>& ids) {
    sonare::mixing::api::Scene scene;
    for (const auto& id : ids) {
      sonare::mixing::api::Strip strip;
      strip.id = id;
      scene.strips.push_back(strip);
      scene.connections.push_back({id, "master"});
    }
    scene.buses.push_back({"master", "master"});
    return scene;
  };

  std::vector<float> a_in(kBlock);
  std::vector<float> b_in(kBlock);
  for (int i = 0; i < kBlock; ++i) {
    a_in[static_cast<size_t>(i)] =
        std::sin(0.3f * static_cast<float>(i)) - std::cos(0.05f * static_cast<float>(i));
    b_in[static_cast<size_t>(i)] = 0.5f * std::cos(0.7f * static_cast<float>(i));
  }
  const float* a_l[] = {a_in.data()};
  const float* a_r[] = {a_in.data()};
  const float* b_l[] = {b_in.data()};
  const float* b_r[] = {b_in.data()};

  auto render_lane = [&](const char* id, const float* const* in_l, const float* const* in_r) {
    const std::string json = sonare::mixing::api::scene_to_json(make_scene({id}));
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);
    std::vector<float> out_l(kBlock, 0.0f);
    std::vector<float> out_r(kBlock, 0.0f);
    REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
            SONARE_OK);
    sonare_mixer_destroy(mixer);
    return out_l;  // L and R inputs are identical, so only L is needed.
  };

  const std::vector<float> a_lane = render_lane("a", a_l, a_r);
  const std::vector<float> b_lane = render_lane("b", b_l, b_r);

  const std::string combined_json = sonare::mixing::api::scene_to_json(make_scene({"a", "b"}));
  SonareMixer* mixer = sonare_mixer_from_scene_json(combined_json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);
  const float* in_l[] = {a_in.data(), b_in.data()};
  const float* in_r[] = {a_in.data(), b_in.data()};
  std::vector<float> out_l(kBlock, 0.0f);
  std::vector<float> out_r(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 2, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  sonare_mixer_destroy(mixer);

  for (int i = 0; i < kBlock; ++i) {
    const float expected = a_lane[static_cast<size_t>(i)] + b_lane[static_cast<size_t>(i)];
    REQUIRE(out_l[static_cast<size_t>(i)] == expected);
    REQUIRE(out_r[static_cast<size_t>(i)] == expected);
  }

  // Sensitivity twin: an active bus EQ band processes the two lanes' sum
  // through one stateful filter, which is not the same floating-point result
  // as the unfiltered per-lane sum above -- proving the `==` comparison is not
  // vacuously true (e.g. from a mixer that silently skips summing altogether).
  sonare::mixing::api::Scene eq_scene = make_scene({"a", "b"});
  sonare::mastering::eq::EqBand band;
  band.type = sonare::mastering::eq::EqBandType::Peak;
  band.frequency_hz = 1000.0f;
  band.gain_db = 18.0f;
  band.q = 1.0f;
  band.enabled = true;
  eq_scene.buses[0].eq.bands.push_back(band);
  const std::string eq_json = sonare::mixing::api::scene_to_json(eq_scene);
  SonareMixer* eq_mixer = sonare_mixer_from_scene_json(eq_json.c_str(), kSr, kBlock);
  REQUIRE(eq_mixer != nullptr);
  std::vector<float> eq_out_l(kBlock, 0.0f);
  std::vector<float> eq_out_r(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(eq_mixer, in_l, in_r, 2, eq_out_l.data(), eq_out_r.data(),
                                      kBlock) == SONARE_OK);
  sonare_mixer_destroy(eq_mixer);

  bool any_differs = false;
  for (int i = 0; i < kBlock; ++i) {
    const float expected = a_lane[static_cast<size_t>(i)] + b_lane[static_cast<size_t>(i)];
    if (eq_out_l[static_cast<size_t>(i)] != expected) {
      any_differs = true;
      break;
    }
  }
  REQUIRE(any_differs);
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH
