/// @file routing_scene_test.cpp
/// @brief Routed mixer scene topology tests.

#include "routing_test_helpers.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

#include "util/exception.h"

TEST_CASE("Routed mixer scene round-trip preserves topology", "[mixing][routing]") {
  // from_scene_json(vocalReverbSend) -> to_scene_json must preserve buses,
  // strips, sends (with destination_bus_id), and connections, including the
  // bus -> return edge that makes the send audible.
  constexpr int kSr = 48000;
  constexpr int kBlock = 512;

  char* preset_json = nullptr;
  REQUIRE(sonare_mixing_scene_preset_json("vocalReverbSend", &preset_json) == SONARE_OK);
  REQUIRE(preset_json != nullptr);

  SonareMixer* mixer = sonare_mixer_from_scene_json(preset_json, kSr, kBlock);
  sonare_free_string(preset_json);
  REQUIRE(mixer != nullptr);

  char* round_trip = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &round_trip) == SONARE_OK);
  REQUIRE(round_trip != nullptr);
  const std::string scene(round_trip);
  sonare_free_string(round_trip);

  // Buses: master plus the vocal-verb aux.
  REQUIRE(scene.find("\"id\":\"master\"") != std::string::npos);
  REQUIRE(scene.find("\"role\":\"master\"") != std::string::npos);
  REQUIRE(scene.find("\"vocal-verb\"") != std::string::npos);

  // Strips: the vocal source and the reverb return.
  REQUIRE(scene.find("\"id\":\"vocal\"") != std::string::npos);
  REQUIRE(scene.find("\"id\":\"vocal-verb-return\"") != std::string::npos);

  // Send survives with its destination bus and timing.
  REQUIRE(scene.find("\"destinationBusId\":\"vocal-verb\"") != std::string::npos);
  REQUIRE(scene.find("\"timing\":\"post\"") != std::string::npos);

  // Connections survive, including the bus -> return edge that routes the send.
  // Key order follows the alphabetic ordering of util::json::Object (std::map),
  // so "destination" precedes "source" in the serialized JSON.
  REQUIRE(scene.find("\"destination\":\"vocal-verb-return\",\"source\":\"vocal-verb\"") !=
          std::string::npos);
  REQUIRE(scene.find("\"destination\":\"master\",\"source\":\"vocal-verb-return\"") !=
          std::string::npos);
  REQUIRE(scene.find("\"destination\":\"master\",\"source\":\"vocal\"") != std::string::npos);

  // Re-parsing the round-tripped JSON must rebuild a working mixer.
  SonareMixer* restored = sonare_mixer_from_scene_json(scene.c_str(), kSr, kBlock);
  REQUIRE(restored != nullptr);
  REQUIRE(sonare_mixer_compile(restored) == SONARE_OK);
  sonare_mixer_destroy(restored);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("Routed mixer applies scene bus inserts", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip lead;
  lead.id = "lead";
  scene.strips.push_back(lead);
  sonare::mixing::api::Bus master("master", "master");
  master.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PostFader, "saturation.hardClipper", "{\"ceiling\":0.25}"});
  scene.buses.push_back(master);
  scene.connections.push_back({"lead", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);

  std::array<float, kBlock> input{};
  input.fill(1.0f);
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);

  REQUIRE_THAT(out_l[0], WithinAbs(0.25f, 0.001f));
  REQUIRE_THAT(out_r[0], WithinAbs(0.25f, 0.001f));
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Removing a bus drops connections that referenced it", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip lead;
  lead.id = "lead";
  scene.strips.push_back(lead);
  scene.buses.push_back({"master", "master"});
  scene.buses.push_back({"reverb", "aux"});
  scene.connections.push_back({"lead", "master"});
  scene.connections.push_back({"reverb", "master"});  // bus -> master edge

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);

  // Removing the bus must also drop the reverb->master edge; otherwise the next
  // compile would try to wire an edge from a node that no longer exists.
  REQUIRE(sonare_mixer_remove_bus(mixer, "reverb") == SONARE_OK);
  REQUIRE(sonare_mixer_compile(mixer) == SONARE_OK);

  std::array<float, kBlock> input{};
  input.fill(0.5f);
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Removing a bus drops strip sends that targeted it", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;

  SonareMixer* mixer = sonare_mixer_create(kSr, kBlock);
  REQUIRE(mixer != nullptr);
  REQUIRE(sonare_mixer_add_bus(mixer, "reverb", "aux") == SONARE_OK);
  SonareStrip* lead = sonare_mixer_add_strip(mixer, "lead");
  REQUIRE(lead != nullptr);
  size_t send_index = 999;
  REQUIRE(sonare_strip_add_send(lead, "lead-to-verb", "reverb", -6.0f,
                                SONARE_SEND_TIMING_POST_FADER, &send_index) == SONARE_OK);

  // Removing the reverb bus must also drop the strip send that fed it, instead of
  // leaving it dangling to be re-materialized as an implicit aux bus to master.
  REQUIRE(sonare_mixer_remove_bus(mixer, "reverb") == SONARE_OK);

  char* json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  const std::string scene_json(json);
  sonare_free_string(json);
  REQUIRE(scene_json.find("\"lead-to-verb\"") == std::string::npos);
  REQUIRE(scene_json.find("\"reverb\"") == std::string::npos);

  // The strip's send was removed, so index 0 is now out of range.
  REQUIRE(sonare_strip_set_send_db(lead, 0, -3.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_compile(mixer) == SONARE_OK);
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Routed mixer delivers scene sidechain keys to strip inserts", "[mixing][routing]") {
  static constexpr int kSr = 48000;
  static constexpr int kBlock = 512;

  auto render = [&](bool with_key) {
    sonare::mixing::api::Scene scene;
    scene.buses.push_back({"master", "master"});
    scene.buses.push_back({"discard", "aux"});

    sonare::mixing::api::Strip host;
    host.id = "host";
    scene.strips.push_back(host);

    sonare::mixing::api::Strip bed;
    bed.id = "bed";
    bed.inserts.push_back({sonare::mixing::api::InsertSlot::PostFader, "dynamics.sidechainRouter",
                           "{\"thresholdDb\":-10,\"rangeDb\":18,\"attackMs\":0,\"releaseMs\":50}",
                           with_key ? "host" : ""});
    scene.strips.push_back(bed);
    scene.connections.push_back({"host", "discard"});
    scene.connections.push_back({"bed", "master"});

    const std::string json = sonare::mixing::api::scene_to_json(scene);
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);

    std::array<float, kBlock> host_l{};
    std::array<float, kBlock> host_r{};
    std::array<float, kBlock> bed_l{};
    std::array<float, kBlock> bed_r{};
    host_l.fill(1.0f);
    host_r.fill(1.0f);
    bed_l.fill(0.05f);
    bed_r.fill(0.05f);

    const float* in_l[] = {host_l.data(), bed_l.data()};
    const float* in_r[] = {host_r.data(), bed_r.data()};
    std::array<float, kBlock> out_l{};
    std::array<float, kBlock> out_r{};
    REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 2, out_l.data(), out_r.data(), kBlock) ==
            SONARE_OK);

    SonareStrip* bed_strip = sonare_mixer_strip_by_id(mixer, "bed");
    REQUIRE(bed_strip != nullptr);
    SonareMixMeterSnapshot meter{};
    REQUIRE(sonare_strip_meter(bed_strip, &meter) == SONARE_OK);
    const double energy = block_energy({out_l.begin(), out_l.end()}, {out_r.begin(), out_r.end()});
    sonare_mixer_destroy(mixer);
    return std::pair<double, float>{energy, meter.gain_reduction_db};
  };

  const auto without_key = render(false);
  const auto with_key = render(true);
  REQUIRE(with_key.first < without_key.first * 0.35);
  REQUIRE(with_key.second < -3.0f);
  REQUIRE(without_key.second > -0.5f);
}

TEST_CASE("Routed mixer delivers scene sidechain keys to bus inserts", "[mixing][routing]") {
  static constexpr int kSr = 48000;
  static constexpr int kBlock = 512;

  auto render = [&](bool with_key) {
    sonare::mixing::api::Scene scene;
    sonare::mixing::api::Bus master("master", "master");
    master.inserts.push_back(
        {sonare::mixing::api::InsertSlot::PostFader, "dynamics.sidechainRouter",
         "{\"thresholdDb\":-10,\"rangeDb\":18,\"attackMs\":0,\"releaseMs\":50}",
         with_key ? "host" : ""});
    scene.buses.push_back(master);
    scene.buses.push_back({"discard", "aux"});

    sonare::mixing::api::Strip host;
    host.id = "host";
    scene.strips.push_back(host);
    sonare::mixing::api::Strip bed;
    bed.id = "bed";
    scene.strips.push_back(bed);
    scene.connections.push_back({"host", "discard"});
    scene.connections.push_back({"bed", "master"});

    const std::string json = sonare::mixing::api::scene_to_json(scene);
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);

    std::array<float, kBlock> host_l{};
    std::array<float, kBlock> host_r{};
    std::array<float, kBlock> bed_l{};
    std::array<float, kBlock> bed_r{};
    host_l.fill(1.0f);
    host_r.fill(1.0f);
    bed_l.fill(0.05f);
    bed_r.fill(0.05f);

    const float* in_l[] = {host_l.data(), bed_l.data()};
    const float* in_r[] = {host_r.data(), bed_r.data()};
    std::array<float, kBlock> out_l{};
    std::array<float, kBlock> out_r{};
    REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 2, out_l.data(), out_r.data(), kBlock) ==
            SONARE_OK);

    const double energy = block_energy({out_l.begin(), out_l.end()}, {out_r.begin(), out_r.end()});
    sonare_mixer_destroy(mixer);
    return energy;
  };

  const double without_key = render(false);
  const double with_key = render(true);
  REQUIRE(with_key < without_key * 0.35);
}

TEST_CASE("Scene-loaded mixer exposes strip meter and goniometer snapshots", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 512;
  constexpr int kBlocks = 96;  // > 1 second, enough for momentary/integrated LUFS.

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip vocal;
  vocal.id = "vocal";
  vocal.input_trim_db = 3.0f;
  vocal.inserts.push_back({sonare::mixing::api::InsertSlot::PreFader, "dynamics.compressor",
                           "{\"thresholdDb\":-30,\"ratio\":4,\"attackMs\":0,\"releaseMs\":50}"});
  scene.strips.push_back(vocal);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"vocal", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_strip_by_id(mixer, "vocal");
  REQUIRE(strip != nullptr);

  std::array<float, kBlock> in_l{};
  std::array<float, kBlock> in_r{};
  const float* inputs_l[] = {in_l.data()};
  const float* inputs_r[] = {in_r.data()};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  for (int block = 0; block < kBlocks; ++block) {
    for (int i = 0; i < kBlock; ++i) {
      const int n = block * kBlock + i;
      const float s = 0.7f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(n) /
                                      static_cast<float>(kSr));
      in_l[static_cast<size_t>(i)] = s;
      in_r[static_cast<size_t>(i)] = -0.5f * s;
    }
    REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                        kBlock) == SONARE_OK);
  }

  SonareMixMeterSnapshot meter{};
  REQUIRE(sonare_strip_meter(strip, &meter) == SONARE_OK);
  REQUIRE(meter.seq >= static_cast<uint64_t>(kBlocks));
  REQUIRE(meter.gain_reduction_db < -0.1f);
  REQUIRE(meter.max_true_peak_db > sonare::constants::kFloorDb);
  REQUIRE(std::isfinite(meter.momentary_lufs));
  REQUIRE(meter.momentary_lufs > -70.0f);
  REQUIRE(std::isfinite(meter.integrated_lufs));
  REQUIRE(meter.integrated_lufs > -70.0f);

  std::array<SonareMixGoniometerPoint, 8> points{};
  const size_t count = sonare_strip_read_goniometer_latest(strip, points.data(), points.size());
  REQUIRE(count == points.size());
  REQUIRE(std::isfinite(points[0].left));
  REQUIRE(std::isfinite(points[0].right));
  REQUIRE(std::abs(points[0].left) > 0.0f);
  REQUIRE(std::abs(points[0].right) > 0.0f);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("Scene channel layout round-trips and defaults to stereo", "[mixing][routing]") {
  using sonare::ChannelLayout;

  // A surround master plus a 5.1-source strip serialize their layout fields and
  // parse back to the same enum values.
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip src;
  src.id = "src";
  src.source_layout = ChannelLayout::FivePointOne;
  scene.strips.push_back(src);
  sonare::mixing::api::Bus master("master", "master");
  master.layout = ChannelLayout::SevenPointOne;
  scene.buses.push_back(master);
  scene.connections.push_back({"src", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  REQUIRE(json.find("\"sourceLayout\":\"5.1\"") != std::string::npos);
  REQUIRE(json.find("\"layout\":\"7.1\"") != std::string::npos);

  const auto restored = sonare::mixing::api::scene_from_json(json);
  REQUIRE(restored.strips.size() == 1);
  REQUIRE(restored.strips[0].source_layout == ChannelLayout::FivePointOne);
  REQUIRE(restored.buses.size() == 1);
  REQUIRE(restored.buses[0].layout == ChannelLayout::SevenPointOne);
}

TEST_CASE("Scene omits the layout fields when stereo (byte-compat)", "[mixing][routing]") {
  using sonare::ChannelLayout;

  // Default (stereo) layouts must not emit the new fields, so existing stereo
  // scenes serialize byte-identically and absent fields parse back as stereo.
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip src;
  src.id = "src";
  scene.strips.push_back(src);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"src", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  REQUIRE(json.find("sourceLayout") == std::string::npos);
  REQUIRE(json.find("\"layout\"") == std::string::npos);

  const auto restored = sonare::mixing::api::scene_from_json(json);
  REQUIRE(restored.strips[0].source_layout == ChannelLayout::Stereo);
  REQUIRE(restored.buses[0].layout == ChannelLayout::Stereo);
}

TEST_CASE("Scene surround pan round-trips and omits at the default", "[mixing][routing]") {
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip moved;
  moved.id = "moved";
  moved.surround_pan.azimuth = -45.0f;
  moved.surround_pan.divergence = 0.25f;
  moved.surround_pan.lfe = 0.5f;
  scene.strips.push_back(moved);
  sonare::mixing::api::Strip centered;  // all defaults -> must not emit the object
  centered.id = "centered";
  scene.strips.push_back(centered);

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  REQUIRE(json.find("surroundPan") != std::string::npos);

  const auto restored = sonare::mixing::api::scene_from_json(json);
  REQUIRE(restored.strips.size() == 2);
  REQUIRE(restored.strips[0].surround_pan.azimuth == -45.0f);
  REQUIRE(restored.strips[0].surround_pan.divergence == 0.25f);
  REQUIRE(restored.strips[0].surround_pan.lfe == 0.5f);
  REQUIRE(restored.strips[0].surround_pan.distance == 1.0f);  // reserved default
  // The centered strip carries no surroundPan and parses back to the default.
  REQUIRE(restored.strips[1].surround_pan.azimuth == 0.0f);
  REQUIRE(restored.strips[1].surround_pan.divergence == 0.0f);
  REQUIRE(restored.strips[1].surround_pan.distance == 1.0f);
}

TEST_CASE("Scene JSON rejects an unknown channel layout string", "[mixing][routing]") {
  const std::string bad_bus = R"({
    "version": 1,
    "buses": [{"id": "master", "role": "master", "layout": "9.2"}],
    "strips": [{"id": "vocal"}],
    "connections": [{"source": "vocal", "destination": "master"}]
  })";
  REQUIRE_THROWS_AS(sonare::mixing::api::scene_from_json(bad_bus), sonare::SonareException);

  const std::string bad_type = R"({
    "version": 1,
    "buses": [{"id": "master", "role": "master", "layout": 6}],
    "strips": [{"id": "vocal"}],
    "connections": [{"source": "vocal", "destination": "master"}]
  })";
  REQUIRE_THROWS_AS(sonare::mixing::api::scene_from_json(bad_type), sonare::SonareException);
}

TEST_CASE("Routed mixer scene round-trip preserves VCA groups", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip lead;
  lead.id = "lead";
  scene.strips.push_back(lead);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"lead", "master"});
  scene.vca_groups.push_back({"lead-vca", -6.0f, {"lead"}});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);

  char* round_trip = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &round_trip) == SONARE_OK);
  REQUIRE(round_trip != nullptr);
  const std::string restored_json(round_trip);
  sonare_free_string(round_trip);
  sonare_mixer_destroy(mixer);

  const auto restored = sonare::mixing::api::scene_from_json(restored_json);
  REQUIRE(restored.vca_groups.size() == 1);
  REQUIRE(restored.vca_groups[0].id == "lead-vca");
  REQUIRE_THAT(restored.vca_groups[0].gain_db, WithinAbs(-6.0f, 0.001f));
  REQUIRE(restored.vca_groups[0].members == std::vector<std::string>{"lead"});
}

TEST_CASE("Routed mixer applies scene solo to audio output", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip solo;
  solo.id = "solo";
  solo.soloed = true;
  sonare::mixing::api::Strip muted_by_solo;
  muted_by_solo.id = "muted-by-solo";
  scene.strips.push_back(solo);
  scene.strips.push_back(muted_by_solo);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"solo", "master"});
  scene.connections.push_back({"muted-by-solo", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);

  std::array<float, kBlock> silent{};
  std::array<float, kBlock> loud{};
  loud.fill(1.0f);
  const float* in_l[] = {silent.data(), loud.data()};
  const float* in_r[] = {silent.data(), loud.data()};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 2, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);

  REQUIRE_THAT(
      static_cast<float>(block_energy({out_l.begin(), out_l.end()}, {out_r.begin(), out_r.end()})),
      WithinAbs(0.0f, 1.0e-6f));
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Routed mixer rejects duplicate scene connections", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip lead;
  lead.id = "lead";
  scene.strips.push_back(lead);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"lead", "master"});
  scene.connections.push_back({"lead", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer == nullptr);
}

TEST_CASE("Routed mixer does not auto-route explicit unpatched aux buses", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip lead;
  lead.id = "lead";
  lead.fader_db = -120.0f;
  lead.sends.push_back(
      {"lead-to-aux", "explicit-aux", 0.0f, sonare::mixing::api::SendTiming::PreFader});
  scene.strips.push_back(lead);
  scene.buses.push_back({"master", "master"});
  scene.buses.push_back({"explicit-aux", "aux"});
  scene.connections.push_back({"lead", "explicit-aux"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);

  std::array<float, kBlock> input{};
  input.fill(1.0f);
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);

  REQUIRE(block_energy({out_l.begin(), out_l.end()}, {out_r.begin(), out_r.end()}) < 1.0e-8);
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Scene JSON rejects non-string insert slot and send timing", "[mixing][routing]") {
  // `slot` and `timing` are string enums ("pre"/"post"). A wrong-typed value
  // must fail the parse instead of silently falling back to the default.
  const std::string bad_slot = R"({
    "version": 1,
    "buses": [{"id": "master", "role": "master"}],
    "strips": [{"id": "vocal", "inserts": [{"slot": 1, "processor": "eq.tilt"}]}],
    "connections": [{"source": "vocal", "destination": "master"}]
  })";
  REQUIRE_THROWS_AS(sonare::mixing::api::scene_from_json(bad_slot), sonare::SonareException);

  const std::string bad_timing = R"({
    "version": 1,
    "buses": [{"id": "master", "role": "master"}, {"id": "verb", "role": "aux"}],
    "strips": [{"id": "vocal", "sends": [
      {"id": "to-verb", "destinationBusId": "verb", "sendDb": -12, "timing": 1}
    ]}],
    "connections": [{"source": "vocal", "destination": "master"}]
  })";
  REQUIRE_THROWS_AS(sonare::mixing::api::scene_from_json(bad_timing), sonare::SonareException);

  // The string forms still parse.
  const std::string good = R"({
    "version": 1,
    "buses": [{"id": "master", "role": "master"}],
    "strips": [{"id": "vocal", "inserts": [{"slot": "post", "processor": "eq.tilt"}]}],
    "connections": [{"source": "vocal", "destination": "master"}]
  })";
  const auto scene = sonare::mixing::api::scene_from_json(good);
  REQUIRE(scene.strips.size() == 1);
  REQUIRE(scene.strips[0].inserts.size() == 1);
  REQUIRE(scene.strips[0].inserts[0].slot == sonare::mixing::api::InsertSlot::PostFader);
}

TEST_CASE("Scene load surfaces silently-ignored insert params as a warning",
          "[mixing][routing][warning]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 512;

  // eq.parametric reads only band{i}.* fields, so flat highPassHz/presenceDb keys
  // take no effect. The scene still loads, but the ignored keys are reported on
  // the dedicated warning channel (NOT last_error, which stays empty on success).
  const std::string with_unknown = R"({
    "version": 1,
    "buses": [{"id": "master", "role": "master"}],
    "strips": [{"id": "vocal", "inserts": [
      {"slot": "post", "processor": "eq.parametric",
       "params": "{\"highPassHz\":80,\"presenceDb\":4}"}
    ]}],
    "connections": [{"source": "vocal", "destination": "master"}]
  })";
  SonareMixer* mixer = sonare_mixer_from_scene_json(with_unknown.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);
  const std::string warning = sonare_last_warning_message();
  REQUIRE(warning.find("eq.parametric") != std::string::npos);
  REQUIRE(warning.find("vocal") != std::string::npos);
  REQUIRE(warning.find("highPassHz") != std::string::npos);
  REQUIRE(warning.find("presenceDb") != std::string::npos);
  REQUIRE(std::string(sonare_last_error_message()).empty());
  sonare_mixer_destroy(mixer);

  // A scene whose insert params are all consumed leaves the warning channel clear
  // (and clears any stale warning from the previous load on this thread).
  const std::string all_known = R"({
    "version": 1,
    "buses": [{"id": "master", "role": "master"}],
    "strips": [{"id": "vocal", "inserts": [
      {"slot": "post", "processor": "eq.parametric",
       "params": "{\"band0.frequencyHz\":1000,\"band0.gainDb\":3}"}
    ]}],
    "connections": [{"source": "vocal", "destination": "master"}]
  })";
  SonareMixer* clean = sonare_mixer_from_scene_json(all_known.c_str(), kSr, kBlock);
  REQUIRE(clean != nullptr);
  REQUIRE(std::string(sonare_last_warning_message()).empty());
  sonare_mixer_destroy(clean);
}

TEST_CASE("sonare_mastering_insert_param_names enumerates an insert's keys",
          "[mixing][routing][mastering]") {
  const std::string comp = sonare_mastering_insert_param_names("dynamics.compressor");
  REQUIRE(comp.find("thresholdDb") != std::string::npos);
  REQUIRE(comp.find("ratio") != std::string::npos);

  // Band-indexed processors enumerate their band{i}.<field> keys.
  const std::string parametric = sonare_mastering_insert_param_names("eq.parametric");
  REQUIRE(parametric.find("band0.frequencyHz") != std::string::npos);

  // Unknown name -> empty string (no params, no crash).
  REQUIRE(std::string(sonare_mastering_insert_param_names("not.a.real.processor")).empty());
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH
