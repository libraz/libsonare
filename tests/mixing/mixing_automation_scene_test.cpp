/// @file mixing_automation_scene_test.cpp
/// @brief Mixing automation and scene JSON tests.

#include "mixing_test_helpers.h"

TEST_CASE("AutomationLane enforces bounded monotonic SPSC events", "[mixing]") {
  sonare::mixing::AutomationLane lane(2);
  sonare::mixing::AutomationEvent first;
  first.sample_pos = 10;
  first.value = 0.25f;
  first.target = {sonare::mixing::AutomationTargetKind::Fader, 1, 0, 0};
  sonare::mixing::AutomationEvent second = first;
  second.sample_pos = 10;
  second.value = 0.5f;
  second.curve = sonare::mixing::AutomationCurveType::Exponential;
  sonare::mixing::AutomationEvent third = first;
  third.sample_pos = 12;
  third.value = 0.75f;

  REQUIRE(lane.capacity() == 2);
  REQUIRE(lane.push(first));
  REQUIRE(lane.push(second));
  REQUIRE_FALSE(lane.push(third));

  std::vector<sonare::mixing::AutomationBlockEvent> consumed;
  const size_t count =
      lane.consume_block(8, 4, [&](const auto& event) { consumed.push_back(event); });

  REQUIRE(count == 2);
  REQUIRE(consumed.size() == 2);
  REQUIRE(consumed[0].offset == 2);
  REQUIRE(consumed[1].offset == 2);
  REQUIRE_THAT(consumed[0].event.value, WithinAbs(0.25f, 0.0001f));
  REQUIRE_THAT(consumed[1].event.value, WithinAbs(0.5f, 0.0001f));
  REQUIRE(consumed[1].event.curve == sonare::mixing::AutomationCurveType::Exponential);
  REQUIRE(lane.empty());

  REQUIRE(lane.push(third));
  sonare::mixing::AutomationEvent stale = first;
  stale.sample_pos = 11;
  REQUIRE_FALSE(lane.push(stale));
}

TEST_CASE("AutomationLane consumes only the requested block and drops stale events", "[mixing]") {
  sonare::mixing::AutomationLane lane(4);
  for (int64_t sample : {4, 8, 16}) {
    sonare::mixing::AutomationEvent event;
    event.sample_pos = sample;
    event.value = static_cast<float>(sample);
    REQUIRE(lane.push(event));
  }

  std::vector<sonare::mixing::AutomationBlockEvent> first_block;
  REQUIRE(lane.consume_block(8, 4, [&](const auto& event) { first_block.push_back(event); }) == 4);
  REQUIRE(first_block.size() == 4);
  REQUIRE(first_block[0].offset == 0);
  REQUIRE_THAT(first_block[0].event.value, WithinAbs(8.0f, 0.0001f));
  REQUIRE(first_block[3].offset == 3);
  REQUIRE_THAT(first_block[3].event.value, WithinAbs(11.0f, 0.0001f));

  std::vector<sonare::mixing::AutomationBlockEvent> second_block;
  REQUIRE(lane.consume_block(12, 4, [&](const auto& event) { second_block.push_back(event); }) ==
          4);
  REQUIRE(second_block.size() == 4);
  REQUIRE_FALSE(lane.empty());

  REQUIRE(lane.consume_block(16, 1, [&](const auto& event) { second_block.push_back(event); }) ==
          1);
  REQUIRE(second_block.size() == 5);
  REQUIRE(second_block.back().offset == 0);

  lane.clear();
  REQUIRE(lane.empty());
  sonare::mixing::AutomationEvent reset_event;
  reset_event.sample_pos = 1;
  REQUIRE(lane.push(reset_event));
}

TEST_CASE("AutomationLane emits exponential curve events between positive breakpoints",
          "[mixing]") {
  sonare::mixing::AutomationLane lane(8);
  sonare::mixing::AutomationEvent first;
  first.sample_pos = 10;
  first.value = 1.0f;
  first.curve = sonare::mixing::AutomationCurveType::Exponential;
  first.target = {sonare::mixing::AutomationTargetKind::Width, 1, 0, 0};
  sonare::mixing::AutomationEvent second = first;
  second.sample_pos = 14;
  second.value = 4.0f;

  REQUIRE(lane.push(first));
  REQUIRE(lane.push(second));

  std::vector<sonare::mixing::AutomationBlockEvent> consumed;
  const size_t count =
      lane.consume_block(8, 8, [&](const auto& event) { consumed.push_back(event); });

  REQUIRE(count == 5);
  REQUIRE(consumed.size() == 5);
  REQUIRE(consumed[0].offset == 2);
  REQUIRE(consumed[1].offset == 3);
  REQUIRE(consumed[2].offset == 4);
  REQUIRE(consumed[3].offset == 5);
  REQUIRE(consumed[4].offset == 6);
  REQUIRE_THAT(consumed[2].event.value, WithinAbs(2.0f, 0.0001f));
}

TEST_CASE("AutomationLane supports linear hold and s-curve interpolation", "[mixing]") {
  using sonare::mixing::AutomationBlockEvent;
  using sonare::mixing::AutomationCurveType;
  using sonare::mixing::AutomationEvent;
  using sonare::mixing::AutomationLane;
  using sonare::mixing::AutomationTargetKind;

  auto collect_curve = [](AutomationCurveType curve) {
    AutomationLane lane(8);
    AutomationEvent first;
    first.sample_pos = 0;
    first.value = 0.0f;
    first.curve = curve;
    first.target = {AutomationTargetKind::Fader, 1, 0, 0};
    AutomationEvent second = first;
    second.sample_pos = 4;
    second.value = 1.0f;
    REQUIRE(lane.push(first));
    REQUIRE(lane.push(second));
    std::vector<AutomationBlockEvent> consumed;
    lane.consume_block(0, 5, [&](const auto& event) { consumed.push_back(event); });
    return consumed;
  };

  const auto linear = collect_curve(AutomationCurveType::Linear);
  REQUIRE(linear.size() == 5);
  REQUIRE_THAT(linear[2].event.value, WithinAbs(0.5f, 0.0001f));

  const auto hold = collect_curve(AutomationCurveType::Hold);
  REQUIRE(hold.size() == 2);
  REQUIRE_THAT(hold[0].event.value, WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(hold[1].event.value, WithinAbs(1.0f, 0.0001f));

  const auto s_curve = collect_curve(AutomationCurveType::SCurve);
  REQUIRE(s_curve.size() == 5);
  REQUIRE_THAT(s_curve[1].event.value, WithinAbs(0.15625f, 0.0001f));
  REQUIRE_THAT(s_curve[2].event.value, WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(s_curve[3].event.value, WithinAbs(0.84375f, 0.0001f));
}

TEST_CASE("AutomationLane sees a producer push before peeking the next curve event", "[mixing]") {
  sonare::mixing::AutomationLane lane(4);
  sonare::mixing::AutomationEvent first;
  first.sample_pos = 0;
  first.value = 1.0f;
  first.curve = sonare::mixing::AutomationCurveType::Exponential;
  first.target = {sonare::mixing::AutomationTargetKind::Fader, 1, 0, 0};
  sonare::mixing::AutomationEvent second = first;
  second.sample_pos = 8;
  second.value = 16.0f;

  REQUIRE(lane.push(first));

  bool pushed_second = false;
  std::vector<sonare::mixing::AutomationBlockEvent> consumed;
  const size_t count = lane.consume_block(0, 8, [&](const auto& event) {
    consumed.push_back(event);
    if (!pushed_second && event.event.sample_pos == 0) {
      pushed_second = true;
      REQUIRE(lane.push(second));
    }
  });

  REQUIRE(pushed_second);
  REQUIRE(count == 8);
  REQUIRE(consumed.size() == 8);
  REQUIRE(consumed.front().offset == 0);
  REQUIRE(consumed.back().offset == 7);
  REQUIRE_THAT(consumed[4].event.value, WithinAbs(4.0f, 0.0001f));
}

TEST_CASE("AutomationLane continues exponential curves across blocks", "[mixing]") {
  sonare::mixing::AutomationLane lane(4);
  sonare::mixing::AutomationEvent first;
  first.sample_pos = 0;
  first.value = 1.0f;
  first.curve = sonare::mixing::AutomationCurveType::Exponential;
  first.target = {sonare::mixing::AutomationTargetKind::Fader, 1, 0, 0};
  sonare::mixing::AutomationEvent second = first;
  second.sample_pos = 100;
  second.value = 16.0f;

  REQUIRE(lane.push(first));
  REQUIRE(lane.push(second));

  std::vector<sonare::mixing::AutomationBlockEvent> first_block;
  REQUIRE(lane.consume_block(0, 8, [&](const auto& event) { first_block.push_back(event); }) == 8);
  REQUIRE(first_block.front().offset == 0);
  REQUIRE(first_block.back().offset == 7);

  std::vector<sonare::mixing::AutomationBlockEvent> second_block;
  REQUIRE(lane.consume_block(48, 5, [&](const auto& event) { second_block.push_back(event); }) ==
          5);
  REQUIRE(second_block.front().offset == 0);
  REQUIRE(second_block.back().offset == 4);
  REQUIRE_THAT(second_block[2].event.value, WithinAbs(4.0f, 0.0001f));
}

TEST_CASE("Mixing Scene JSON round-trips pure data without graph dependency", "[mixing]") {
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip vocal;
  vocal.id = "vocal";
  vocal.input_trim_db = 2.0f;
  vocal.fader_db = -3.0f;
  vocal.pan = -0.25f;
  vocal.width = 1.2f;
  vocal.muted = false;
  vocal.soloed = true;
  vocal.solo_safe = false;
  vocal.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "dynamics.compressor", "{\"thresholdDb\":-18}"});
  vocal.inserts.push_back({sonare::mixing::api::InsertSlot::PostFader, "eq.equalizer",
                           "{\"band0.externalSidechain\":1,\"band0.gainDb\":-3}", "kick"});
  vocal.sends.push_back({"verb-send", "verb", -12.0f, sonare::mixing::api::SendTiming::PostFader});
  scene.strips.push_back(vocal);
  sonare::mixing::api::Bus verb_bus;
  verb_bus.id = "verb";
  verb_bus.role = "aux";
  verb_bus.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PostFader, "effects.reverb.plate", "{\"decaySec\":1.2}"});
  scene.buses.push_back(verb_bus);
  scene.vca_groups.push_back({"lead", -1.5f, {"vocal"}});
  scene.connections.push_back({"vocal", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  REQUIRE(json.find("\"vcaOffsetDb\":0") != std::string::npos);
  const auto parsed = sonare::mixing::api::scene_from_json(json);

  REQUIRE(parsed.version == 1);
  REQUIRE(parsed.strips.size() == 1);
  REQUIRE(parsed.strips[0].id == "vocal");
  REQUIRE_THAT(parsed.strips[0].input_trim_db, WithinAbs(2.0f, 0.0001f));
  REQUIRE_THAT(parsed.strips[0].fader_db, WithinAbs(-3.0f, 0.0001f));
  REQUIRE_THAT(parsed.strips[0].pan, WithinAbs(-0.25f, 0.0001f));
  REQUIRE_THAT(parsed.strips[0].width, WithinAbs(1.2f, 0.0001f));
  REQUIRE(parsed.strips[0].soloed);
  REQUIRE(parsed.strips[0].inserts.size() == 2);
  REQUIRE(parsed.strips[0].inserts[0].slot == sonare::mixing::api::InsertSlot::PreFader);
  REQUIRE(parsed.strips[0].inserts[0].processor_name == "dynamics.compressor");
  REQUIRE(parsed.strips[0].inserts[0].params_json == "{\"thresholdDb\":-18}");
  REQUIRE(parsed.strips[0].inserts[1].processor_name == "eq.equalizer");
  REQUIRE(parsed.strips[0].inserts[1].params_json.find("externalSidechain") != std::string::npos);
  REQUIRE(parsed.strips[0].inserts[1].sidechain_key == "kick");
  REQUIRE(parsed.strips[0].sends.size() == 1);
  REQUIRE(parsed.strips[0].sends[0].destination_bus_id == "verb");
  REQUIRE(parsed.strips[0].sends[0].timing == sonare::mixing::api::SendTiming::PostFader);
  REQUIRE(parsed.buses[0].role == "aux");
  REQUIRE(parsed.buses[0].inserts.size() == 1);
  REQUIRE(parsed.buses[0].inserts[0].processor_name == "effects.reverb.plate");
  REQUIRE(parsed.vca_groups[0].members[0] == "vocal");
  REQUIRE(parsed.connections[0].destination == "master");
}

TEST_CASE("Mixing Scene JSON parses exponent-form numbers it can emit", "[mixing]") {
  const std::string json =
      "{\"version\":1,\"strips\":[{\"id\":\"tiny\",\"inputTrimDb\":1e-05,\"faderDb\":-3E+00,"
      "\"pan\":0,\"width\":1,\"muted\":false,\"soloed\":false,\"soloSafe\":false,"
      "\"panMode\":0,\"dualPanLeft\":-1.25e-01,\"dualPanRight\":1.25E-01,"
      "\"polarityInvertLeft\":false,\"polarityInvertRight\":false,\"panLaw\":0,"
      "\"channelDelaySamples\":0,\"inserts\":[],\"sends\":[]}],\"buses\":[],"
      "\"vcaGroups\":[],\"connections\":[]}";

  const auto parsed = sonare::mixing::api::scene_from_json(json);

  REQUIRE(parsed.strips.size() == 1);
  REQUIRE_THAT(parsed.strips[0].input_trim_db, WithinAbs(1e-05f, 1e-09f));
  REQUIRE_THAT(parsed.strips[0].fader_db, WithinAbs(-3.0f, 0.0001f));
  REQUIRE_THAT(parsed.strips[0].dual_pan_left, WithinAbs(-0.125f, 0.0001f));
  REQUIRE_THAT(parsed.strips[0].dual_pan_right, WithinAbs(0.125f, 0.0001f));

  const auto reparsed =
      sonare::mixing::api::scene_from_json(sonare::mixing::api::scene_to_json(parsed));
  REQUIRE(reparsed.strips.size() == 1);
  REQUIRE_THAT(reparsed.strips[0].input_trim_db, WithinAbs(1e-05f, 1e-09f));
}

TEST_CASE("Mixing Scene JSON round-trips all extended strip fields", "[mixing]") {
  // Every field added to api::Strip beyond the original set must survive a
  // scene_to_json -> scene_from_json round-trip. Build a strip whose extended
  // fields all carry non-default values so a dropped field would change the
  // observed value.
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip strip;
  strip.id = "extended";
  strip.pan_mode = 2;  // DualPan
  strip.dual_pan_left = -0.4f;
  strip.dual_pan_right = 0.7f;
  strip.polarity_invert_left = true;
  strip.polarity_invert_right = true;
  strip.pan_law = 3;  // Linear0dB
  strip.channel_delay_samples = 17;
  scene.strips.push_back(strip);

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  const auto parsed = sonare::mixing::api::scene_from_json(json);

  REQUIRE(parsed.strips.size() == 1);
  const auto& out = parsed.strips[0];
  REQUIRE(out.pan_mode == 2);
  REQUIRE_THAT(out.dual_pan_left, WithinAbs(-0.4f, 0.0001f));
  REQUIRE_THAT(out.dual_pan_right, WithinAbs(0.7f, 0.0001f));
  REQUIRE(out.polarity_invert_left);
  REQUIRE(out.polarity_invert_right);
  REQUIRE(out.pan_law == 3);
  REQUIRE(out.channel_delay_samples == 17);
}

TEST_CASE("Mixing Scene JSON ignores unknown forward-compat fields", "[mixing]") {
  // A scene authored by a newer version may carry fields this parser does not
  // know. Unknown scalars, arrays, and nested objects at both the scene level
  // and inside a strip object must be skipped without throwing, while known
  // fields still parse correctly.
  const std::string json =
      "{"
      "\"version\":1,"
      "\"futureScalar\":42,"
      "\"unknownArray\":[1,2,3],"
      "\"futureField\":{\"nested\":{\"deep\":[true,false],\"s\":\"x\"}},"
      "\"strips\":[{"
      "\"id\":\"vox\","
      "\"faderDb\":-4.5,"
      "\"panLaw\":2,"
      "\"channelDelaySamples\":9,"
      "\"futureStripScalar\":\"hello\","
      "\"futureStripObject\":{\"a\":1,\"b\":[{\"c\":2}]},"
      "\"futureStripArray\":[{\"x\":1},{\"y\":2}]"
      "}],"
      "\"buses\":[],"
      "\"vcaGroups\":[],"
      "\"connections\":[]"
      "}";

  sonare::mixing::api::Scene parsed;
  REQUIRE_NOTHROW(parsed = sonare::mixing::api::scene_from_json(json));
  REQUIRE(parsed.version == 1);
  REQUIRE(parsed.strips.size() == 1);
  REQUIRE(parsed.strips[0].id == "vox");
  REQUIRE_THAT(parsed.strips[0].fader_db, WithinAbs(-4.5f, 0.0001f));
  REQUIRE(parsed.strips[0].pan_law == 2);
  REQUIRE(parsed.strips[0].channel_delay_samples == 9);
}

TEST_CASE("Mixing Scene JSON rejects unsupported version", "[mixing]") {
  const std::string json = "{\"version\":2,\"strips\":[],\"buses\":[]}";
  REQUIRE_THROWS_AS(sonare::mixing::api::scene_from_json(json), sonare::SonareException);
}

TEST_CASE("Mixing scene presets expose planned templates and JSON round-trip", "[mixing]") {
  const auto names = sonare::mixing::api::scene_preset_names();
  REQUIRE(names.size() == 3);
  REQUIRE(names[0] == "vocalReverbSend");
  REQUIRE(names[1] == "drumBusSubgroup");
  REQUIRE(names[2] == "commentaryDucking");

  for (const auto& name : names) {
    CAPTURE(name);
    const auto preset = sonare::mixing::api::scene_preset_from_string(name);
    REQUIRE(std::string(sonare::mixing::api::scene_preset_to_string(preset)) == name);
    const auto scene = sonare::mixing::api::scene_preset(preset);
    REQUIRE_FALSE(scene.strips.empty());
    REQUIRE_FALSE(scene.buses.empty());
    const auto parsed =
        sonare::mixing::api::scene_from_json(sonare::mixing::api::scene_to_json(scene));
    REQUIRE(parsed.strips.size() == scene.strips.size());
    REQUIRE(parsed.buses.size() == scene.buses.size());
  }

  REQUIRE_THROWS_AS(sonare::mixing::api::scene_preset_from_string("missing"),
                    sonare::SonareException);
}

TEST_CASE("Mixing scene presets contain expected routing intent", "[mixing]") {
  const auto vocal =
      sonare::mixing::api::scene_preset(sonare::mixing::api::ScenePreset::VocalReverbSend);
  REQUIRE(vocal.strips[0].id == "vocal");
  REQUIRE(vocal.strips[0].sends[0].destination_bus_id == "vocal-verb");
  REQUIRE(vocal.strips[0].inserts[0].processor_name == "eq.parametric");

  const auto drums =
      sonare::mixing::api::scene_preset(sonare::mixing::api::ScenePreset::DrumBusSubgroup);
  REQUIRE(drums.vca_groups.size() == 1);
  REQUIRE(drums.vca_groups[0].members.size() == 4);
  REQUIRE(drums.connections[0].destination == "drum-bus");

  const auto commentary =
      sonare::mixing::api::scene_preset(sonare::mixing::api::ScenePreset::CommentaryDucking);
  REQUIRE(commentary.strips[2].id == "music-bed");
  REQUIRE(commentary.strips[2].inserts[0].processor_name == "dynamics.sidechainRouter");
  REQUIRE(commentary.vca_groups[0].id == "voices");
}
