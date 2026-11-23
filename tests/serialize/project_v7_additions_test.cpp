#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "arrangement/destination_instrument.h"
#include "arrangement/edit_model.h"
#include "automation/automation_lane.h"
#include "mixing/api/scene.h"
#include "serialize/project_serializer.h"
#include "util/exception.h"
#include "util/json.h"

namespace {

using sonare::arrangement::DestinationBuiltinInstrument;
using sonare::arrangement::DestinationInstrumentBinding;
using sonare::arrangement::DestinationMidiBank;
using sonare::arrangement::DestinationSf2Instrument;
using sonare::arrangement::DestinationSynthInstrument;

sonare::arrangement::Track track_for_destination(uint32_t destination_id) {
  sonare::arrangement::Track track;
  track.midi_destination_id = destination_id;
  return track;
}

}  // namespace

TEST_CASE("project v7 round-trips all destination instrument variants in map order",
          "[serialize][project_v7]") {
  sonare::arrangement::Project project;
  REQUIRE(project.add_track(track_for_destination(9)) != 0);
  REQUIRE(project.add_track(track_for_destination(2)) != 0);
  REQUIRE(project.add_track(track_for_destination(5)) != 0);

  DestinationSynthInstrument synth;
  synth.preset = "warm-pad";
  synth.engine_mode = "fm";
  synth.fields.emplace("gain", 0.75);
  synth.engine_params.emplace("filter.cutoffHz", 1200.0);
  synth.mod_routings.push_back({1, 2, 0.5f});
  synth.use_gm_programs = true;
  project.set_destination_instrument(
      9, DestinationInstrumentBinding{std::move(synth), DestinationMidiBank{1, 2}, 3});

  DestinationBuiltinInstrument builtin;
  builtin.waveform = 3;
  builtin.gain = 0.8f;
  builtin.attack_ms = 12.0f;
  builtin.decay_ms = 80.0f;
  builtin.sustain = 0.6f;
  builtin.release_ms = 240.0f;
  builtin.polyphony = 24;
  project.set_destination_instrument(2, DestinationInstrumentBinding{builtin, {}, {}});

  DestinationSf2Instrument sf2;
  sf2.gain = 0.9f;
  sf2.polyphony = 32;
  sf2.prefer_model_for_modeled_families = 1;
  sf2.clear_bank_rig = 1;
  sf2.gs_efx_realization = 1;
  sf2.sound_font_uri = "fonts/orchestra.sf2";
  project.set_destination_instrument(
      5, DestinationInstrumentBinding{std::move(sf2), DestinationMidiBank{4, 5}, 6});

  DestinationBuiltinInstrument dangling_builtin;
  dangling_builtin.waveform = 1;
  project.restore_destination_instrument(12,
                                         DestinationInstrumentBinding{dangling_builtin, {}, {}});

  const auto root = sonare::util::json::parse(sonare::serialize::project_to_json(project, {}));
  REQUIRE(root["version"].as_int() == 7);
  REQUIRE(root["instruments"].as_array().size() == 4);
  const auto& instruments = root["instruments"].as_array();
  CHECK(instruments[0]["destinationId"].as_int() == 2);
  CHECK(instruments[0]["kind"].as_string() == "builtin");
  CHECK(instruments[0]["builtin"].contains("attackMs"));
  CHECK(instruments[0]["builtin"].contains("releaseMs"));
  CHECK(instruments[1]["destinationId"].as_int() == 5);
  CHECK(instruments[1]["kind"].as_string() == "sf2");
  CHECK(instruments[1]["sf2"].contains("preferModelForModeledFamilies"));
  CHECK(instruments[1]["sf2"].contains("gsEfxRealization"));
  CHECK(instruments[1]["sf2"]["soundFontUri"].as_string() == "fonts/orchestra.sf2");
  CHECK(instruments[2]["destinationId"].as_int() == 9);
  CHECK(instruments[2]["kind"].as_string() == "synth");
  CHECK(instruments[2]["synth"]["fields"].contains("gain"));
  CHECK(instruments[2]["synth"]["engineParams"].contains("filter.cutoffHz"));
  CHECK(instruments[2]["synth"]["modRoutings"].as_array().size() == 1);
  CHECK(instruments[3]["destinationId"].as_int() == 12);

  const auto decoded =
      sonare::serialize::project_from_json(sonare::serialize::project_to_json(project, {}));
  REQUIRE(decoded.ok());
  REQUIRE(decoded.project->destination_instruments().size() == 4);
  CHECK(decoded.project->destination_instruments() == project.destination_instruments());
  CHECK(sonare::serialize::project_to_json(*decoded.project, decoded.midi) ==
        sonare::serialize::project_to_json(project, {}));
}

TEST_CASE("project v7 preserves synth enum fields and explicit modulation clears",
          "[serialize][project_v7]") {
  sonare::arrangement::Project project;
  REQUIRE(project.add_track(track_for_destination(1)) != 0);
  REQUIRE(project.add_track(track_for_destination(2)) != 0);
  REQUIRE(project.add_track(track_for_destination(3)) != 0);

  DestinationSynthInstrument absent;
  absent.preset = "patch";
  absent.fields.emplace("gain", 0.75);
  absent.fields.emplace("waveform", std::string("saw"));
  project.set_destination_instrument(1, DestinationInstrumentBinding{std::move(absent), {}, {}});

  DestinationSynthInstrument explicit_empty;
  explicit_empty.preset = "patch";
  explicit_empty.clears_base_mod_routings = true;
  project.set_destination_instrument(
      2, DestinationInstrumentBinding{std::move(explicit_empty), {}, {}});

  DestinationSynthInstrument replacement;
  replacement.preset = "patch";
  replacement.mod_routings.push_back({1, 2, 0.5f});
  project.set_destination_instrument(3,
                                     DestinationInstrumentBinding{std::move(replacement), {}, {}});

  const std::string json = sonare::serialize::project_to_json(project, {});
  const auto root = sonare::util::json::parse(json);
  const auto& instruments = root["instruments"].as_array();
  REQUIRE(instruments.size() == 3);
  const auto& absent_synth = instruments[0]["synth"];
  CHECK(absent_synth["fields"]["gain"].as_number() == 0.75);
  CHECK(absent_synth["fields"]["waveform"].as_string() == "saw");
  CHECK_FALSE(absent_synth.contains("modRoutings"));
  const auto& cleared_synth = instruments[1]["synth"];
  REQUIRE(cleared_synth.contains("modRoutings"));
  CHECK(cleared_synth["modRoutings"].as_array().empty());
  REQUIRE(instruments[2]["synth"]["modRoutings"].as_array().size() == 1);

  const auto decoded = sonare::serialize::project_from_json(json);
  REQUIRE(decoded.ok());
  const auto* absent_decoded = std::get_if<DestinationSynthInstrument>(
      &decoded.project->destination_instruments().at(1).instrument);
  REQUIRE(absent_decoded != nullptr);
  CHECK(std::get<std::string>(absent_decoded->fields.at("waveform")) == "saw");
  CHECK(std::get<double>(absent_decoded->fields.at("gain")) == 0.75);
  CHECK(absent_decoded->mod_routings.empty());
  CHECK_FALSE(absent_decoded->clears_base_mod_routings);

  const auto* cleared_decoded = std::get_if<DestinationSynthInstrument>(
      &decoded.project->destination_instruments().at(2).instrument);
  REQUIRE(cleared_decoded != nullptr);
  CHECK(cleared_decoded->mod_routings.empty());
  CHECK(cleared_decoded->clears_base_mod_routings);

  const auto* replacement_decoded = std::get_if<DestinationSynthInstrument>(
      &decoded.project->destination_instruments().at(3).instrument);
  REQUIRE(replacement_decoded != nullptr);
  REQUIRE(replacement_decoded->mod_routings.size() == 1);
  CHECK_FALSE(replacement_decoded->clears_base_mod_routings);
  CHECK(sonare::serialize::project_to_json(*decoded.project, decoded.midi) == json);
}

TEST_CASE("project v7 preserves typed lane identities in document order",
          "[serialize][project_v7]") {
  sonare::arrangement::Project project;
  auto track = track_for_destination(1);
  using sonare::automation::AutomationLane;
  using sonare::automation::AutomationTargetIdentity;
  using sonare::automation::AutomationTargetKind;
  track.automation_lanes.emplace_back(0, AutomationTargetKind::kTrackInsertParam,
                                      AutomationTargetIdentity{{}, 1, "eq", "gain"});
  track.automation_lanes.emplace_back(0, AutomationTargetKind::kTrackSendDb,
                                      AutomationTargetIdentity{"z-room", {}, {}, {}});
  track.automation_lanes.emplace_back(0, AutomationTargetKind::kTrackSendDb,
                                      AutomationTargetIdentity{"a-room", {}, {}, {}});
  track.automation_lanes.emplace_back(0, AutomationTargetKind::kTrackInsertParam,
                                      AutomationTargetIdentity{{}, 0, "eq", "gain"});
  track.automation_lanes.emplace_back(0, AutomationTargetKind::kTrackInsertParam,
                                      AutomationTargetIdentity{{}, 0, "eq", "frequency"});
  track.automation_lanes.emplace_back(0, AutomationTargetKind::kTrackWidth);
  track.automation_lanes.back().set_points({{0.0, 1.25f}});
  track.automation_lanes[0].set_points({{0.0, -2.0f}});
  REQUIRE(project.add_track(std::move(track)) != 0);

  const std::string json = sonare::serialize::project_to_json(project, {});
  const auto root = sonare::util::json::parse(json);
  const auto& lanes = root["tracks"].as_array()[0]["automationLanes"].as_array();
  REQUIRE(lanes.size() == 6);
  for (std::size_t i = 0; i < lanes.size(); ++i) {
    CHECK(lanes[i]["id"].as_int() == static_cast<int64_t>(i + 1));
  }
  CHECK(lanes[0]["targetKind"].as_string() == "trackInsertParam");
  CHECK(lanes[0]["insertIndex"].as_int() == 1);
  CHECK(lanes[1]["targetKind"].as_string() == "trackSendDb");
  CHECK(lanes[1]["sendId"].as_string() == "z-room");
  CHECK(lanes[2]["sendId"].as_string() == "a-room");
  CHECK(lanes[2]["targetParamId"].as_int() == 0);
  CHECK(lanes[3]["insertIndex"].as_int() == 0);
  CHECK(lanes[3]["paramName"].as_string() == "gain");
  CHECK(lanes[4]["insertIndex"].as_int() == 0);
  CHECK(lanes[4]["paramName"].as_string() == "frequency");
  CHECK(lanes[5]["targetKind"].as_string() == "trackWidth");

  const auto decoded = sonare::serialize::project_from_json(json);
  REQUIRE(decoded.ok());
  const auto& loaded_lanes = decoded.project->tracks()[0].automation_lanes;
  REQUIRE(loaded_lanes.size() == 6);
  for (std::size_t i = 0; i < loaded_lanes.size(); ++i) {
    CHECK(loaded_lanes[i].lane_id() == i + 1);
  }
  CHECK(loaded_lanes[0].target_identity().insert_index == 1);
  CHECK(loaded_lanes[1].target_identity().send_id == "z-room");
  CHECK(loaded_lanes[2].target_identity().send_id == "a-room");
  CHECK(loaded_lanes[3].target_identity().param_name == "gain");
  CHECK(loaded_lanes[4].target_identity().param_name == "frequency");
  CHECK(loaded_lanes[5].target_kind() == AutomationTargetKind::kTrackWidth);
  CHECK(loaded_lanes[5].value_at(0.0) == 1.25f);
  CHECK(sonare::serialize::project_to_json(*decoded.project, decoded.midi) == json);
}

TEST_CASE("project v7 reserves explicit automation lane ids before assigning legacy ids",
          "[serialize][project_v7]") {
  const auto decoded = sonare::serialize::project_from_json(
      R"({"version":7,"tracks":[{"id":1,"automationLanes":[)"
      R"({"id":8,"targetParamId":80},{"targetParamId":81},)"
      R"({"id":2,"targetParamId":82},{"targetParamId":83}]}]})");
  REQUIRE(decoded.ok());
  const auto& lanes = decoded.project->tracks()[0].automation_lanes;
  REQUIRE(lanes.size() == 4);
  CHECK(lanes[0].lane_id() == 8);
  CHECK(lanes[1].lane_id() == 9);
  CHECK(lanes[2].lane_id() == 2);
  CHECK(lanes[3].lane_id() == 10);
  const auto encoded =
      sonare::util::json::parse(sonare::serialize::project_to_json(*decoded.project, decoded.midi));
  const auto& encoded_lanes = encoded["tracks"].as_array()[0]["automationLanes"].as_array();
  REQUIRE(encoded_lanes.size() == 4);
  CHECK(encoded_lanes[0]["id"].as_int() == 8);
  CHECK(encoded_lanes[1]["id"].as_int() == 9);
  CHECK(encoded_lanes[2]["id"].as_int() == 2);
  CHECK(encoded_lanes[3]["id"].as_int() == 10);
}

TEST_CASE("project v7 refuses duplicate automation lane ids and typed target ids",
          "[serialize][project_v7][refusal]") {
  const auto duplicate = sonare::serialize::project_from_json(
      R"({"version":7,"tracks":[)"
      R"({"id":1,"automationLanes":[{"id":4,"targetParamId":40}]},)"
      R"({"id":2,"automationLanes":[{"id":4,"targetParamId":41}]}]})");
  CHECK_FALSE(duplicate.ok());
  REQUIRE_FALSE(duplicate.diagnostics.empty());
  CHECK(duplicate.diagnostics.back().message.find("automationLanes[0].id") != std::string::npos);

  const auto typed_id = sonare::serialize::project_from_json(
      R"({"version":7,"tracks":[{"id":1,"automationLanes":[)"
      R"({"id":4,"targetParamId":40,"targetKind":"trackFaderDb"}]}]})");
  CHECK_FALSE(typed_id.ok());
  REQUIRE_FALSE(typed_id.diagnostics.empty());
  CHECK(typed_id.diagnostics.back().message.find("targetParamId") != std::string::npos);
}

TEST_CASE("project v7 refuses an insert index outside the signed 32-bit domain",
          "[serialize][project_v7][refusal]") {
  constexpr std::size_t kTooLarge =
      static_cast<std::size_t>(std::numeric_limits<int32_t>::max()) + 1;
  using sonare::automation::AutomationTargetIdentity;
  using sonare::automation::AutomationTargetKind;
  sonare::arrangement::Project project;
  auto track = track_for_destination(1);
  track.automation_lanes.emplace_back(0, AutomationTargetKind::kTrackInsertParam,
                                      AutomationTargetIdentity{{}, kTooLarge, "eq", "gain"});
  REQUIRE(project.add_track(std::move(track)) != 0);
  try {
    (void)sonare::serialize::project_to_json(project, {});
    FAIL("expected an insert index refusal");
  } catch (const sonare::SonareException& error) {
    CHECK(error.code() == sonare::ErrorCode::InvalidState);
    CHECK(std::string(error.what()).find("insertIndex") == 0);
  }

  const auto loaded = sonare::serialize::project_from_json(
      R"({"version":7,"tracks":[{"id":1,"automationLanes":[{"targetParamId":1,"targetKind":"trackInsertParam","insertIndex":2147483648,"processor":"eq","paramName":"gain"}]}]})");
  CHECK_FALSE(loaded.ok());
  REQUIRE_FALSE(loaded.diagnostics.empty());
  CHECK(loaded.diagnostics.back().message.find("tracks[0].automationLanes[0].insertIndex") == 0);
}

TEST_CASE("project v7 deduplicates typed lanes by full identity in their first slot",
          "[serialize][project_v7][normalization]") {
  const auto decoded = sonare::serialize::project_from_json(
      R"({"version":7,"tracks":[{"id":1,"automationLanes":[{"targetParamId":1,"targetKind":"trackSendDb","sendId":"hall","points":[{"ppq":0,"value":-3}]},{"targetParamId":2,"targetKind":"trackSendDb","sendId":"room","points":[{"ppq":0,"value":-4}]},{"targetParamId":3,"targetKind":"trackSendDb","sendId":"hall","points":[{"ppq":0,"value":-6}]}]}]})");
  REQUIRE(decoded.ok());
  const auto& lanes = decoded.project->tracks()[0].automation_lanes;
  REQUIRE(lanes.size() == 2);
  CHECK(lanes[0].target_identity().send_id == "hall");
  CHECK(lanes[0].target_param_id() == 0);
  REQUIRE(lanes[0].points().size() == 1);
  CHECK(lanes[0].points()[0].value == -6.0f);
  CHECK(lanes[1].target_identity().send_id == "room");
}

TEST_CASE("project v7 scene persistence distinguishes absent from explicit default",
          "[serialize][project_v7]") {
  sonare::arrangement::Project absent_scene;
  const std::string absent_json = sonare::serialize::project_to_json(absent_scene, {});
  const auto absent_root = sonare::util::json::parse(absent_json);
  CHECK_FALSE(absent_root.contains("scene"));
  const auto absent_decoded = sonare::serialize::project_from_json(absent_json);
  REQUIRE(absent_decoded.ok());
  CHECK(absent_decoded.project->find_scene() == nullptr);

  sonare::arrangement::Project explicit_scene;
  explicit_scene.set_scene(sonare::mixing::api::Scene{});
  const std::string explicit_json = sonare::serialize::project_to_json(explicit_scene, {});
  const auto explicit_root = sonare::util::json::parse(explicit_json);
  CHECK(explicit_root.contains("scene"));
  const auto explicit_decoded = sonare::serialize::project_from_json(explicit_json);
  REQUIRE(explicit_decoded.ok());
  CHECK(explicit_decoded.project->find_scene() != nullptr);
}

TEST_CASE("project v7 deserialization keeps JSON refusal reasons",
          "[serialize][project_v7][error_reason]") {
  const auto malformed = sonare::serialize::project_from_json("{");
  CHECK_FALSE(malformed.ok());
  CHECK(malformed.reason == sonare::ErrorReason::MalformedJson);

  const auto unsupported = sonare::serialize::project_from_json(R"({"version":8})");
  CHECK_FALSE(unsupported.ok());
  CHECK(unsupported.reason == sonare::ErrorReason::UnsupportedSchemaVersion);

  const auto scene_reason =
      sonare::serialize::project_from_json(R"({"version":7,"scene":{"version":3}})");
  CHECK_FALSE(scene_reason.ok());
  CHECK(scene_reason.reason == sonare::ErrorReason::UnsupportedSchemaVersion);
}

TEST_CASE("project v7 refuses malformed lane identities and unknown instrument keys",
          "[serialize][project_v7][refusal]") {
  const auto missing_send = sonare::serialize::project_from_json(
      R"({"version":7,"tracks":[{"id":1,"automationLanes":[{"targetParamId":1,"targetKind":"trackSendDb"}]}]})");
  CHECK_FALSE(missing_send.ok());
  REQUIRE_FALSE(missing_send.diagnostics.empty());
  CHECK(missing_send.diagnostics.back().message ==
        "tracks[0].automationLanes[0].sendId must be given");

  const auto wrong_identity = sonare::serialize::project_from_json(
      R"({"version":7,"tracks":[{"id":1,"automationLanes":[{"targetParamId":1,"targetKind":"trackSendDb","sendId":"hall","processor":"eq"}]}]})");
  CHECK_FALSE(wrong_identity.ok());
  REQUIRE_FALSE(wrong_identity.diagnostics.empty());
  CHECK(wrong_identity.diagnostics.back().message ==
        "tracks[0].automationLanes[0].processor must be absent");

  const auto unknown_key = sonare::serialize::project_from_json(
      R"({"version":7,"instruments":[{"destinationId":1,"kind":"builtin","builtin":{"waveform":0,"futureField":1}}]})");
  CHECK_FALSE(unknown_key.ok());
  REQUIRE_FALSE(unknown_key.diagnostics.empty());
  CHECK(unknown_key.diagnostics.back().message == "unknown key instruments[0].builtin.futureField");

  const auto invalid_width = sonare::serialize::project_from_json(
      R"({"version":7,"tracks":[{"id":1,"automationLanes":[{"targetParamId":1,"targetKind":"trackWidth","points":[{"ppq":0,"value":2.5}]}]}]})");
  CHECK_FALSE(invalid_width.ok());
  REQUIRE_FALSE(invalid_width.diagnostics.empty());
  CHECK(invalid_width.diagnostics.back().message.find(
            "tracks[0].automationLanes[0].points[0].value") == 0);
}

TEST_CASE("project v7 refuses unknown or mistyped synth patch fields",
          "[serialize][project_v7][refusal]") {
  struct Refusal {
    const char* fields;
    const char* subject;
  };
  const Refusal refusals[] = {
      {R"("waveform":"bogus")", "waveform"}, {R"("waveform":1)", "waveform"},
      {R"("futureField":1)", "futureField"}, {R"("gain":"loud")", "gain"},
      {R"("unison":1.5)", "unison"},
  };
  for (const auto& refusal : refusals) {
    CAPTURE(refusal.fields);
    const std::string document =
        std::string(
            R"({"version":7,"instruments":[{"destinationId":3,"kind":"synth","synth":{"preset":"","fields":{)") +
        refusal.fields + R"(},"engineParams":{},"useGmPrograms":false}}]})";
    const auto result = sonare::serialize::project_from_json(document);
    CHECK_FALSE(result.ok());
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics.back().message.find(std::string("instruments[0].synth.fields.") +
                                                 refusal.subject) != std::string::npos);
  }
}

TEST_CASE("project v7 refuses non-persistable sample synth patches with their reason",
          "[serialize][project_v7][error_reason]") {
  sonare::arrangement::Project project;
  REQUIRE(project.add_track(track_for_destination(3)) != 0);
  DestinationSynthInstrument synth;
  synth.preset = "sample-patch";
  synth.engine_mode = "sample";
  project.set_destination_instrument(3, DestinationInstrumentBinding{std::move(synth), {}, {}});

  try {
    (void)sonare::serialize::project_to_json(project, {});
    FAIL("expected a non-persistable sample patch refusal");
  } catch (const sonare::SonareException& error) {
    CHECK(error.code() == sonare::ErrorCode::NotSupported);
    CHECK(error.reason() == sonare::ErrorReason::NotPersistable);
  }

  const auto refused = sonare::serialize::project_from_json(
      R"({"version":7,"instruments":[{"destinationId":3,"kind":"synth","synth":{"preset":"sample-patch","engineMode":"sample","fields":{},"engineParams":{},"modRoutings":[],"useGmPrograms":false}}]})");
  CHECK_FALSE(refused.ok());
  CHECK_FALSE(refused.reason.has_value());
  REQUIRE_FALSE(refused.diagnostics.empty());
  CHECK(refused.diagnostics.back().code == "invalid_format");
  CHECK(refused.diagnostics.back().message.find("instruments[0].synth.engineMode") == 0);
}
