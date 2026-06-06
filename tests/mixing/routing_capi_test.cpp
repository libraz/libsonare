/// @file routing_capi_test.cpp
/// @brief Routed mixer C API tests.

#include "routing_test_helpers.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

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

  // Valid calls on a real strip succeed.
  REQUIRE(sonare_strip_set_soloed(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_solo_safe(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_polarity_invert(strip, 1, 0) == SONARE_OK);
  REQUIRE(sonare_strip_set_pan_law(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_channel_delay_samples(strip, 3) == SONARE_OK);
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

  char* round_trip = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &round_trip) == SONARE_OK);
  REQUIRE(round_trip != nullptr);
  const std::string round_trip_json(round_trip);
  sonare_free_string(round_trip);
  const auto restored = sonare::mixing::api::scene_from_json(round_trip_json);
  REQUIRE(restored.vca_groups.size() == 1);
  REQUIRE_THAT(restored.vca_groups[0].gain_db, WithinAbs(-12.0f, 0.0001f));

  REQUIRE(sonare_mixer_set_vca_group_gain_db(mixer, "missing-vca", -3.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_set_vca_group_gain_db(nullptr, "lead-vca", -3.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_set_vca_group_gain_db(mixer, nullptr, -3.0f) ==
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
