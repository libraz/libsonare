#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace {

struct ProjectHandle {
  SonareProject* value = nullptr;

  ProjectHandle() { REQUIRE(sonare_project_create(&value) == SONARE_OK); }
  ~ProjectHandle() { sonare_project_destroy(value); }
  ProjectHandle(const ProjectHandle&) = delete;
  ProjectHandle& operator=(const ProjectHandle&) = delete;
};

TEST_CASE("track routes require nonempty strings when supplied",
          "[c_api][project][writers][route]") {
  ProjectHandle project;
  SonareProjectTrackDesc desc{};
  desc.kind = SONARE_TRACK_AUDIO;
  uint32_t track_id = 0;
  REQUIRE(sonare_project_add_track(project.value, &desc, &track_id) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project.value, track_id, "lead", "master") == SONARE_OK);

  CHECK(sonare_project_set_track_route(project.value, track_id, "", "master") ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("channel_strip_ref") == 0);
  CHECK(sonare_project_set_track_route(project.value, track_id, "lead", "") ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("output_target") == 0);
  REQUIRE(sonare_project_set_track_route(project.value, track_id, nullptr, nullptr) == SONARE_OK);
}

TEST_CASE("chord annotations require a nonempty Roman numeral when supplied",
          "[c_api][project][writers][chords]") {
  ProjectHandle project;
  const SonareProjectChordSymbol chords[] = {
      {0, 1, 0, 0, nullptr, 0, 255, nullptr, 0},
      {1, 2, 0, 0, nullptr, 0, 255, "", 0},
  };
  REQUIRE(sonare_project_annotate_chords(project.value, chords, 1) == SONARE_OK);
  CHECK(sonare_project_annotate_chords(project.value, chords, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("chords[1].roman_numeral") == 0);
}

#if defined(SONARE_WITH_MASTERING)
TEST_CASE("project scene documents refuse unread insert parameters",
          "[c_api][project][writers][insert-keys]") {
  ProjectHandle project;
  constexpr const char* kScene =
      R"({"version":2,"strips":[{"id":"lead","inserts":[{"processor":"saturation.softClipper","params":{"bogus":1}}]}]})";
  constexpr const char* kMessage =
      "unknown key strips[0].inserts[0].params.bogus (expected aliasing, ceiling, driveDb, mix)";
  REQUIRE(sonare_project_set_mixer_scene_json(project.value, kScene) ==
          SONARE_ERROR_INVALID_FORMAT);
  CHECK(std::string(sonare_last_error_message()) == kMessage);

  const std::string document = std::string(R"({"version":7,"scene":)") + kScene + "}";
  const std::string message =
      "unknown key scene.strips[0].inserts[0].params.bogus (expected aliasing, ceiling, driveDb, "
      "mix)";
  SonareProject* loaded = nullptr;
  char* diagnostics = nullptr;
  CHECK(sonare_project_deserialize(document.data(), document.size(), &loaded, &diagnostics) ==
        SONARE_ERROR_INVALID_FORMAT);
  const std::unique_ptr<SonareProject, decltype(&sonare_project_destroy)> owned(
      loaded, sonare_project_destroy);
  const std::unique_ptr<char, decltype(&sonare_free_string)> owned_diagnostics(diagnostics,
                                                                               sonare_free_string);
  CHECK(std::string(sonare_last_error_message()) == message);
  CHECK(owned == nullptr);
  REQUIRE(owned_diagnostics != nullptr);
  CHECK(std::string(owned_diagnostics.get()) == "invalid_format: " + message);
}

TEST_CASE("project scene documents refuse an unknown insert processor as the mixer does",
          "[c_api][project][writers][insert-keys]") {
  ProjectHandle project;
  constexpr const char* kScene =
      R"({"version":2,"strips":[{"id":"lead","inserts":[{"processor":"not.a.processor"}]}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project.value, kScene) ==
          SONARE_ERROR_INVALID_FORMAT);
  CHECK(std::string(sonare_last_error_message()) ==
        "strips[0].inserts[0].processor must name a known processor");

  const std::string document = std::string(R"({"version":7,"scene":)") + kScene + "}";
  SonareProject* loaded = nullptr;
  CHECK(sonare_project_deserialize(document.data(), document.size(), &loaded, nullptr) ==
        SONARE_ERROR_INVALID_FORMAT);
  const std::unique_ptr<SonareProject, decltype(&sonare_project_destroy)> owned(
      loaded, sonare_project_destroy);
  CHECK(owned == nullptr);
  CHECK(std::string(sonare_last_error_message()) ==
        "scene.strips[0].inserts[0].processor must name a known processor");

  SonareMixer* mixer = sonare_mixer_from_scene_json(kScene, 48000, 512);
  CHECK(mixer == nullptr);
  CHECK(std::string(sonare_last_error_message()) ==
        "strips[0].inserts[0].processor must name a known processor");
}
#endif

std::string serialize(const SonareProject* project) {
  char* json = nullptr;
  size_t length = 0;
  REQUIRE(sonare_project_serialize(project, &json, &length) == SONARE_OK);
  std::string result(json, length);
  sonare_free_string(json);
  return result;
}

size_t source_count(const SonareProject* project) {
  size_t count = 0;
  REQUIRE(sonare_project_source_count(project, &count) == SONARE_OK);
  return count;
}

size_t unresolved_audio_source_count(const SonareProject* project) {
  size_t count = 0;
  REQUIRE(sonare_project_unresolved_audio_source_count(project, &count) == SONARE_OK);
  return count;
}

SonareProjectSource source_by_index(const SonareProject* project, size_t index) {
  SonareProjectSource source{};
  REQUIRE(sonare_project_source_by_index(project, index, &source) == SONARE_OK);
  return source;
}

SonareProjectClip clip_by_index(const SonareProject* project, size_t index) {
  SonareProjectClip clip{};
  REQUIRE(sonare_project_clip_by_index(project, index, &clip) == SONARE_OK);
  return clip;
}

std::string source_uri(const SonareProject* project, uint32_t source_id) {
  char* uri = nullptr;
  REQUIRE(sonare_project_get_audio_source_uri(project, source_id, &uri) == SONARE_OK);
  std::string result(uri);
  sonare_free_string(uri);
  return result;
}

std::pair<std::string, std::string> source_metadata(const SonareProject* project,
                                                    uint32_t source_id) {
  SonareProjectAudioSourceMetadata metadata{};
  REQUIRE(sonare_project_get_audio_source_metadata(project, source_id, &metadata) == SONARE_OK);
  std::pair<std::string, std::string> result{metadata.content_hash, metadata.external_stem_role};
  sonare_free_project_audio_source_metadata(&metadata);
  return result;
}

size_t source_index(const SonareProject* project, uint32_t source_id) {
  for (size_t i = 0; i < source_count(project); ++i) {
    if (source_by_index(project, i).id == source_id) return i;
  }
  FAIL("source id was not present in the project");
  return 0;
}

uint32_t add_audio_track(SonareProject* project) {
  const SonareProjectTrackDesc desc{SONARE_TRACK_AUDIO, "audio"};
  uint32_t track_id = 0;
  REQUIRE(sonare_project_add_track(project, &desc, &track_id) == SONARE_OK);
  return track_id;
}

struct AutomationLaneList {
  SonareProjectAutomationLaneList value{};

  explicit AutomationLaneList(const SonareProject* project) {
    REQUIRE(sonare_project_automation_lanes(project, &value) == SONARE_OK);
  }
  ~AutomationLaneList() { sonare_free_project_automation_lane_list(&value); }
  AutomationLaneList(const AutomationLaneList&) = delete;
  AutomationLaneList& operator=(const AutomationLaneList&) = delete;
};

SonareAutomationLaneDesc automation_lane_desc(SonareAutomationTargetKind kind,
                                              uint32_t target_param_id,
                                              const SonareAutomationPoint* points,
                                              size_t point_count) {
  SonareAutomationLaneDesc desc{};
  sonare_automation_lane_desc_init(&desc);
  desc.target_kind = kind;
  desc.target_param_id = target_param_id;
  desc.points = points;
  desc.point_count = point_count;
  return desc;
}

void expect_lane_add_refusal(SonareProject* project, uint32_t track_id,
                             const SonareAutomationLaneDesc& desc, const char* subject) {
  uint32_t out_lane_id = 99;
  CHECK(sonare_project_add_automation_lane(project, track_id, &desc, &out_lane_id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out_lane_id == 0);
  CHECK(std::string(sonare_last_error_message()).find(subject) != std::string::npos);
}

}  // namespace

TEST_CASE("project section annotations accept overlap and undo as one edit",
          "[c_api][project][writers][sections]") {
  ProjectHandle project;
  const std::string initial = serialize(project.value);
  const SonareProjectSection sections[] = {{0.0, 8.0, "intro"}, {4.0, 12.0, "verse"}};

  REQUIRE(sonare_project_annotate_sections(project.value, sections, 2) == SONARE_OK);
  const std::string annotated = serialize(project.value);
  CHECK(annotated.find("\"intro\"") != std::string::npos);
  CHECK(annotated.find("\"verse\"") != std::string::npos);
  CHECK(annotated != initial);

  REQUIRE(sonare_project_undo(project.value) == SONARE_OK);
  CHECK(serialize(project.value) == initial);
  REQUIRE(sonare_project_redo(project.value) == SONARE_OK);
  CHECK(serialize(project.value) == annotated);

  REQUIRE(sonare_project_annotate_sections(project.value, nullptr, 0) == SONARE_OK);
  CHECK(serialize(project.value).find("\"intro\"") == std::string::npos);
  REQUIRE(sonare_project_undo(project.value) == SONARE_OK);
  CHECK(serialize(project.value) == annotated);
}

TEST_CASE("project section annotations refuse invalid fields without changing state",
          "[c_api][project][writers][sections]") {
  ProjectHandle project;
  const SonareProjectSection accepted = {0.0, 4.0, "intro"};
  REQUIRE(sonare_project_annotate_sections(project.value, &accepted, 1) == SONARE_OK);
  const std::string before = serialize(project.value);

  const SonareProjectSection reversed = {4.0, 4.0, "verse"};
  CHECK(sonare_project_annotate_sections(project.value, &reversed, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("sections[0].end_ppq") != std::string::npos);
  CHECK(serialize(project.value) == before);

  const SonareProjectSection invalid_start = {-1.0, 4.0, "verse"};
  CHECK(sonare_project_annotate_sections(project.value, &invalid_start, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("sections[0].start_ppq") !=
        std::string::npos);
  CHECK(serialize(project.value) == before);

  const SonareProjectSection blank = {4.0, 8.0, " \t\n"};
  CHECK(sonare_project_annotate_sections(project.value, &blank, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("sections[0].label") != std::string::npos);
  CHECK(serialize(project.value) == before);

  const SonareProjectSection empty = {4.0, 8.0, ""};
  CHECK(sonare_project_annotate_sections(project.value, &empty, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("sections[0].label") != std::string::npos);
  CHECK(serialize(project.value) == before);

  const SonareProjectSection missing = {4.0, 8.0, nullptr};
  CHECK(sonare_project_annotate_sections(project.value, &missing, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("sections[0].label") != std::string::npos);
  CHECK(serialize(project.value) == before);
}

TEST_CASE("audio source add and relink preserve identity and PCM through undo",
          "[c_api][project][writers][sources]") {
  ProjectHandle project;
  uint32_t first_source_id = 0;
  uint32_t second_source_id = 0;
  REQUIRE(sonare_project_add_audio_source(project.value, "same.wav", &first_source_id) ==
          SONARE_OK);
  REQUIRE(sonare_project_add_audio_source(project.value, "same.wav", &second_source_id) ==
          SONARE_OK);
  CHECK(first_source_id != second_source_id);
  CHECK(source_count(project.value) == 2);
  CHECK(unresolved_audio_source_count(project.value) == 2);

  REQUIRE(sonare_project_undo(project.value) == SONARE_OK);
  CHECK(source_count(project.value) == 1);
  REQUIRE(sonare_project_redo(project.value) == SONARE_OK);
  CHECK(source_count(project.value) == 2);
  CHECK(source_by_index(project.value, 1).id == second_source_id);
  REQUIRE(sonare_project_undo(project.value) == SONARE_OK);

  const float source_audio[] = {0.1f, -0.2f, 0.3f, -0.4f};
  REQUIRE(sonare_project_set_source_audio(project.value, first_source_id, source_audio, 4, 1,
                                          48000) == SONARE_OK);
  CHECK(unresolved_audio_source_count(project.value) == 0);
  SonareProjectSource source = source_by_index(project.value, 0);
  CHECK(source.id == first_source_id);
  CHECK(source.num_channels == 1);
  CHECK(source.sample_rate_hint == 48000.0);

  REQUIRE(sonare_project_undo(project.value) == SONARE_OK);
  CHECK(unresolved_audio_source_count(project.value) == 1);
  source = source_by_index(project.value, 0);
  CHECK(source.id == first_source_id);
  CHECK(source.num_channels == 0);
  CHECK(source.sample_rate_hint == 0.0);
  REQUIRE(sonare_project_redo(project.value) == SONARE_OK);
  CHECK(unresolved_audio_source_count(project.value) == 0);

  REQUIRE(sonare_project_relink_source(project.value, first_source_id, "same.wav") == SONARE_OK);
  CHECK(source_uri(project.value, first_source_id) == "same.wav");
  CHECK(source_by_index(project.value, 0).num_channels == 0);
  CHECK(unresolved_audio_source_count(project.value) == 1);
  REQUIRE(sonare_project_undo(project.value) == SONARE_OK);
  CHECK(source_by_index(project.value, 0).num_channels == 1);
  CHECK(unresolved_audio_source_count(project.value) == 0);

  const uint32_t track_id = add_audio_track(project.value);
  const float clip_audio[] = {0.25f, -0.25f, 0.5f, -0.5f};
  SonareProjectClipDesc desc{};
  desc.track_id = track_id;
  desc.start_ppq = 0.0;
  desc.length_ppq = 4.0;
  desc.gain = 1.0f;
  desc.audio_interleaved = clip_audio;
  desc.audio_frames = 4;
  desc.audio_channels = 1;
  desc.audio_sample_rate = 48000;
  desc.source_uri = "original.wav";
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_clip(project.value, &desc, &clip_id) == SONARE_OK);
  const SonareProjectClip original_clip = clip_by_index(project.value, 0);
  REQUIRE(original_clip.id == clip_id);
  const uint32_t clip_source_id = original_clip.source_id;
  REQUIRE(sonare_project_set_audio_source_metadata(project.value, clip_source_id, "sha256",
                                                   "lead") == SONARE_OK);
  const std::pair<std::string, std::string> original_metadata{"sha256", "lead"};
  const std::pair<std::string, std::string> relinked_metadata{"", "lead"};
  CHECK(source_uri(project.value, clip_source_id) == "original.wav");
  CHECK(source_metadata(project.value, clip_source_id) == original_metadata);
  CHECK(unresolved_audio_source_count(project.value) == 0);

  REQUIRE(sonare_project_relink_source(project.value, clip_source_id, "relocated.wav") ==
          SONARE_OK);
  const SonareProjectClip relinked_clip = clip_by_index(project.value, 0);
  CHECK(relinked_clip.id == clip_id);
  CHECK(relinked_clip.source_id == clip_source_id);
  CHECK(source_uri(project.value, clip_source_id) == "relocated.wav");
  CHECK(source_metadata(project.value, clip_source_id) == relinked_metadata);
  CHECK(source_by_index(project.value, source_index(project.value, clip_source_id)).num_channels ==
        0);
  CHECK(unresolved_audio_source_count(project.value) == 1);

  REQUIRE(sonare_project_undo(project.value) == SONARE_OK);
  const SonareProjectClip undone_clip = clip_by_index(project.value, 0);
  CHECK(undone_clip.id == clip_id);
  CHECK(undone_clip.source_id == clip_source_id);
  CHECK(source_uri(project.value, clip_source_id) == "original.wav");
  CHECK(source_metadata(project.value, clip_source_id) == original_metadata);
  CHECK(source_by_index(project.value, source_index(project.value, clip_source_id)).num_channels ==
        1);
  CHECK(unresolved_audio_source_count(project.value) == 0);

  REQUIRE(sonare_project_redo(project.value) == SONARE_OK);
  CHECK(source_uri(project.value, clip_source_id) == "relocated.wav");
  CHECK(source_metadata(project.value, clip_source_id) == relinked_metadata);
  CHECK(unresolved_audio_source_count(project.value) == 1);
}

TEST_CASE("audio source writers refuse invalid URIs and non-audio source ids",
          "[c_api][project][writers][sources]") {
  ProjectHandle project;
  uint32_t source_id = 123;

  CHECK(sonare_project_add_audio_source(project.value, nullptr, &source_id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(source_id == 0);
  CHECK(std::string(sonare_last_error_message()).find("uri") != std::string::npos);
  source_id = 123;
  CHECK(sonare_project_add_audio_source(project.value, "", &source_id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(source_id == 0);
  CHECK(std::string(sonare_last_error_message()).find("uri") != std::string::npos);
  CHECK(source_count(project.value) == 0);

  REQUIRE(sonare_project_add_audio_source(project.value, "audio.wav", &source_id) == SONARE_OK);
  CHECK(sonare_project_relink_source(project.value, source_id, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("uri") != std::string::npos);
  CHECK(sonare_project_relink_source(project.value, source_id, "") ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("uri") != std::string::npos);
  CHECK(sonare_project_relink_source(project.value, UINT32_C(123456), "other.wav") ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("source_id") != std::string::npos);

  uint32_t midi_track_id = 0;
  uint32_t midi_clip_id = 0;
  REQUIRE(sonare_project_add_midi_clip(project.value, 0.0, 4.0, &midi_track_id, &midi_clip_id) ==
          SONARE_OK);
  CHECK(midi_track_id != 0);
  const SonareProjectClip midi_clip = clip_by_index(project.value, 0);
  CHECK(midi_clip.id == midi_clip_id);
  CHECK(sonare_project_relink_source(project.value, midi_clip.source_id, "midi.wav") ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("source_id") != std::string::npos);
  CHECK(source_uri(project.value, source_id) == "audio.wav");
}

TEST_CASE("project deserialization exposes serializer error reasons",
          "[c_api][project][writers][error_reason]") {
#if defined(SONARE_WITH_ARRANGEMENT)
  SonareProject* project = nullptr;
  char* diagnostics = nullptr;
  const char malformed[] = "{";
  CHECK(sonare_project_deserialize(malformed, sizeof(malformed) - 1, &project, &diagnostics) ==
        SONARE_ERROR_INVALID_FORMAT);
  CHECK(project == nullptr);
  REQUIRE(sonare_last_error_reason() != nullptr);
  CHECK(std::string(sonare_last_error_reason()) == "malformedJson");
  sonare_free_string(diagnostics);

  diagnostics = nullptr;
  const char unsupported[] = R"({"version":999})";
  CHECK(sonare_project_deserialize(unsupported, sizeof(unsupported) - 1, &project, &diagnostics) ==
        SONARE_ERROR_INVALID_FORMAT);
  CHECK(project == nullptr);
  REQUIRE(sonare_last_error_reason() != nullptr);
  CHECK(std::string(sonare_last_error_reason()) == "unsupportedSchemaVersion");
  sonare_free_string(diagnostics);
#endif
}

TEST_CASE("persistent synth enum overrides round trip by token",
          "[c_api][project][writers][instruments]") {
  ProjectHandle project;
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project.value, 0, 1, &track, &clip) == SONARE_OK);
  struct EnumField {
    int SonareSynthPatch::*member;
    const char* encoded;
  };
  const EnumField fields[] = {
      {&SonareSynthPatch::waveform, R"("waveform":"sine")"},
      {&SonareSynthPatch::filter_model, R"("filterModel":"svf")"},
      {&SonareSynthPatch::filter_output, R"("filterOutput":"lowPass")"},
      {&SonareSynthPatch::body, R"("body":"none")"},
      {&SonareSynthPatch::retrigger, R"("retrigger":"free")"},
      {&SonareSynthPatch::lfo_shape, R"("lfoShape":"triangle")"},
      {&SonareSynthPatch::sample_loop, R"("sampleLoop":"none")"},
      {&SonareSynthPatch::sample_key_track, R"("sampleKeyTrack":"on")"},
  };
  for (const auto& field : fields) {
    CAPTURE(field.encoded);
    SonareSynthPatch patch{};
    sonare_synth_patch_init(&patch);
    patch.*field.member = 1;
    patch.present_fields = SONARE_SYNTH_FIELD_MOD_ROUTINGS | SONARE_SYNTH_FIELD_AMP_SUSTAIN;
    SonareProjectInstrument instrument{};
    sonare_project_instrument_init(&instrument);
    instrument.synth = &patch;
    REQUIRE(sonare_project_set_destination_instrument(project.value, 0, &instrument) == SONARE_OK);

    const auto document = serialize(project.value);
    CHECK(document.find(field.encoded) != std::string::npos);
    CHECK(document.find(R"("modRoutings":[])") != std::string::npos);
    SonareProject* loaded = nullptr;
    REQUIRE(sonare_project_deserialize(document.data(), document.size(), &loaded, nullptr) ==
            SONARE_OK);
    const std::unique_ptr<SonareProject, decltype(&sonare_project_destroy)> reloaded(
        loaded, sonare_project_destroy);
    CHECK(serialize(reloaded.get()) == document);

    SonareProjectInstrumentList list{};
    REQUIRE(sonare_project_destination_instruments(reloaded.get(), &list) == SONARE_OK);
    REQUIRE(list.count == 1);
    REQUIRE(list.items[0].instrument.synth != nullptr);
    const auto& read_patch = *list.items[0].instrument.synth;
    CHECK(read_patch.*field.member == 1);
    CHECK((read_patch.present_fields & SONARE_SYNTH_FIELD_MOD_ROUTINGS) != 0);
    CHECK((read_patch.present_fields & SONARE_SYNTH_FIELD_AMP_SUSTAIN) != 0);
    REQUIRE(sonare_project_set_destination_instrument(reloaded.get(), 0,
                                                      &list.items[0].instrument) == SONARE_OK);
    sonare_free_project_instrument_list(&list);
    CHECK(serialize(reloaded.get()) == document);
  }
}

TEST_CASE("persistent synth names nonfinite dormant sample fields",
          "[c_api][project][writers][instruments]") {
  ProjectHandle project;
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project.value, 0, 1, &track, &clip) == SONARE_OK);
  const std::string before = serialize(project.value);
  SonareSynthPatch patch{};
  sonare_synth_patch_init(&patch);
  patch.engine_mode = SONARE_SYNTH_ENGINE_REED;
  SonareProjectInstrument instrument{};
  sonare_project_instrument_init(&instrument);
  instrument.synth = &patch;
  patch.sample_level = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_project_set_destination_instrument(project.value, 0, &instrument) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("instrument.synth.sample_level") !=
        std::string::npos);
  patch.sample_level = 0.0f;
  patch.sample_start_offset = std::numeric_limits<float>::infinity();
  CHECK(sonare_project_set_destination_instrument(project.value, 0, &instrument) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("instrument.synth.sample_start_offset") !=
        std::string::npos);
  CHECK(serialize(project.value) == before);
}

TEST_CASE("destination instrument model validator names the C argument field",
          "[c_api][project][writers][instruments]") {
  ProjectHandle project;
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project.value, 0, 1, &track, &clip) == SONARE_OK);

  SonareBuiltinSynthConfig builtin{};
  sonare_builtin_synth_config_init(&builtin);
  builtin.gain = 0.0f;
  SonareProjectInstrument instrument{};
  sonare_project_instrument_init(&instrument);
  instrument.kind = SONARE_PROJECT_INSTRUMENT_BUILTIN;
  instrument.builtin = &builtin;

  CHECK(sonare_project_set_destination_instrument(project.value, 0, &instrument) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("instrument.builtin.gain") == 0);
}

TEST_CASE("bounce names an unresolved persistent SoundFont URI",
          "[c_api][project][writers][instruments]") {
  ProjectHandle project;
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project.value, 0, 1, &track, &clip) == SONARE_OK);
  SonareMidiEventPod note{};
  REQUIRE(sonare_midi_note_on(0, 0, 0, 60, 100, &note) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project.value, clip, &note, 1) == SONARE_OK);
  SonareSf2InstrumentConfig sf2{};
  sonare_sf2_instrument_config_init(&sf2);
  SonareProjectInstrument instrument{};
  sonare_project_instrument_init(&instrument);
  instrument.kind = SONARE_PROJECT_INSTRUMENT_SF2;
  instrument.sf2 = &sf2;
  instrument.sound_font_uri = "fonts/project.sf2";
  REQUIRE(sonare_project_set_destination_instrument(project.value, 0, &instrument) == SONARE_OK);

  SonareStringList uris{};
  REQUIRE(sonare_project_unresolved_sound_font_uris(project.value, &uris) == SONARE_OK);
  REQUIRE(uris.count == 1);
  CHECK(std::string(uris.items[0]) == instrument.sound_font_uri);
  sonare_free_string_list(&uris);

  float* audio = nullptr;
  size_t frames = 0;
  int channels = 0;
  REQUIRE(sonare_project_bounce(project.value, nullptr, &audio, &frames, &channels) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(sonare_last_error_reason() != nullptr);
  CHECK(std::string(sonare_last_error_reason()) == "unresolvedSoundFont");
  CHECK(std::string(sonare_last_error_message()).find(instrument.sound_font_uri) !=
        std::string::npos);
  CHECK(audio == nullptr);
  CHECK(frames == 0);
  CHECK(channels == 0);
}

TEST_CASE("project automation lanes round-trip kinds and keep their ids",
          "[c_api][project][writers][automation]") {
#if defined(SONARE_WITH_ARRANGEMENT)
  ProjectHandle project;
  const uint32_t track_id = add_audio_track(project.value);
  const SonareAutomationTargetKind kinds[] = {
      SONARE_AUTOMATION_TARGET_OPAQUE,      SONARE_AUTOMATION_TARGET_TRACK_FADER_DB,
      SONARE_AUTOMATION_TARGET_TRACK_PAN,   SONARE_AUTOMATION_TARGET_TRACK_SEND_DB,
      SONARE_AUTOMATION_TARGET_TRACK_WIDTH, SONARE_AUTOMATION_TARGET_TRACK_INSERT_PARAM,
  };
  const float values[] = {0.5f, -3.0f, 0.25f, -6.0f, 1.25f, 0.5f};
  uint32_t lane_ids[6]{};

  for (size_t i = 0; i < 6; ++i) {
    const std::string before_add = serialize(project.value);
    const SonareAutomationPoint point{0.0, values[i], SONARE_CURVE_LINEAR};
    SonareAutomationLaneDesc desc = automation_lane_desc(kinds[i], i == 0 ? 101u : 0u, &point, 1);
    if (kinds[i] == SONARE_AUTOMATION_TARGET_TRACK_SEND_DB) desc.send_id = "send-main";
    if (kinds[i] == SONARE_AUTOMATION_TARGET_TRACK_INSERT_PARAM) {
      desc.insert_index = 0;
      desc.processor = "utility.gain";
      desc.param_name = "levelDb";
    }

    REQUIRE(sonare_project_add_automation_lane(project.value, track_id, &desc, &lane_ids[i]) ==
            SONARE_OK);
    REQUIRE(lane_ids[i] != 0);
    const std::string after_add = serialize(project.value);
    AutomationLaneList lanes(project.value);
    REQUIRE(lanes.value.count == i + 1);
    const SonareProjectAutomationLane& row = lanes.value.items[i];
    CHECK(row.track_id == track_id);
    CHECK(row.lane_id == lane_ids[i]);
    CHECK(row.desc.target_kind == kinds[i]);
    CHECK(row.desc.target_param_id == (i == 0 ? 101u : 0u));
    CHECK(row.desc.point_count == 1);
    CHECK(row.desc.points[0].value == values[i]);
    if (kinds[i] == SONARE_AUTOMATION_TARGET_TRACK_SEND_DB) {
      REQUIRE(row.desc.send_id != nullptr);
      CHECK(std::string(row.desc.send_id) == "send-main");
    }
    if (kinds[i] == SONARE_AUTOMATION_TARGET_TRACK_INSERT_PARAM) {
      CHECK(row.desc.insert_index == 0);
      REQUIRE(row.desc.processor != nullptr);
      REQUIRE(row.desc.param_name != nullptr);
      CHECK(std::string(row.desc.processor) == "utility.gain");
      CHECK(std::string(row.desc.param_name) == "levelDb");
    }

    REQUIRE(sonare_project_edit_automation_lane(project.value, track_id, lane_ids[i], &row.desc) ==
            SONARE_OK);
    CHECK(serialize(project.value) == after_add);
    REQUIRE(sonare_project_undo(project.value) == SONARE_OK);
    CHECK(serialize(project.value) == before_add);
    REQUIRE(sonare_project_redo(project.value) == SONARE_OK);
    CHECK(serialize(project.value) == after_add);
    AutomationLaneList redone(project.value);
    REQUIRE(redone.value.count == i + 1);
    CHECK(redone.value.items[i].lane_id == lane_ids[i]);
  }

  const SonareAutomationPoint changed_point{0.0, 1.5f, SONARE_CURVE_LINEAR};
  SonareAutomationLaneDesc changed =
      automation_lane_desc(SONARE_AUTOMATION_TARGET_OPAQUE, 101, &changed_point, 1);
  REQUIRE(sonare_project_edit_automation_lane(project.value, track_id, lane_ids[0], &changed) ==
          SONARE_OK);
  {
    AutomationLaneList edited(project.value);
    REQUIRE(edited.value.count == 6);
    CHECK(edited.value.items[0].lane_id == lane_ids[0]);
    CHECK(edited.value.items[0].desc.points[0].value == changed_point.value);
  }
  REQUIRE(sonare_project_undo(project.value) == SONARE_OK);
  {
    AutomationLaneList undone(project.value);
    CHECK(undone.value.items[0].lane_id == lane_ids[0]);
    CHECK(undone.value.items[0].desc.points[0].value == values[0]);
  }
  REQUIRE(sonare_project_redo(project.value) == SONARE_OK);
  const std::string serialized = serialize(project.value);

  SonareProject* loaded = nullptr;
  char* diagnostics = nullptr;
  REQUIRE(sonare_project_deserialize(serialized.data(), serialized.size(), &loaded, &diagnostics) ==
          SONARE_OK);
  sonare_free_string(diagnostics);
  {
    AutomationLaneList loaded_lanes(loaded);
    REQUIRE(loaded_lanes.value.count == 6);
    CHECK(loaded_lanes.value.items[0].lane_id == lane_ids[0]);
    CHECK(loaded_lanes.value.items[0].desc.points[0].value == changed_point.value);
  }
  sonare_project_destroy(loaded);
#endif
}

TEST_CASE("project automation lane descriptors name invalid fields",
          "[c_api][project][writers][automation]") {
#if defined(SONARE_WITH_ARRANGEMENT)
  ProjectHandle project;
  const uint32_t track_id = add_audio_track(project.value);
  const SonareAutomationPoint point{0.0, 0.5f, SONARE_CURVE_LINEAR};
  const std::string before = serialize(project.value);
  SonareAutomationLaneDesc desc =
      automation_lane_desc(SONARE_AUTOMATION_TARGET_OPAQUE, 17, &point, 1);

  SonareAutomationLaneDesc bad = desc;
  bad.struct_size = sizeof(SonareAutomationLaneDesc) - 1;
  expect_lane_add_refusal(project.value, track_id, bad, "desc.struct_size");
  bad = desc;
  bad.reserved = 1;
  expect_lane_add_refusal(project.value, track_id, bad, "desc.reserved");
  bad = desc;
  bad.target_param_id = 0;
  expect_lane_add_refusal(project.value, track_id, bad, "desc.target_param_id");
  bad = automation_lane_desc(SONARE_AUTOMATION_TARGET_TRACK_FADER_DB, 17, &point, 1);
  expect_lane_add_refusal(project.value, track_id, bad, "desc.target_param_id");
  bad = desc;
  bad.target_kind = static_cast<SonareAutomationTargetKind>(6);
  expect_lane_add_refusal(project.value, track_id, bad, "desc.target_kind");
  bad = desc;
  bad.send_id = "";
  expect_lane_add_refusal(project.value, track_id, bad, "desc.send_id");

  bad = automation_lane_desc(SONARE_AUTOMATION_TARGET_TRACK_SEND_DB, 0, &point, 1);
  expect_lane_add_refusal(project.value, track_id, bad, "desc.send_id");
  bad.send_id = "";
  expect_lane_add_refusal(project.value, track_id, bad, "desc.send_id");
  bad.send_id = "send-main";
  bad.insert_index = 0;
  expect_lane_add_refusal(project.value, track_id, bad, "desc.insert_index");
  bad = automation_lane_desc(SONARE_AUTOMATION_TARGET_TRACK_SEND_DB, 0, &point, 1);
  bad.send_id = "send-main";
  SonareAutomationPoint too_loud_send{0.0, 25.0f, SONARE_CURVE_LINEAR};
  bad.points = &too_loud_send;
  expect_lane_add_refusal(project.value, track_id, bad, "desc.points[0].value");

  bad = automation_lane_desc(SONARE_AUTOMATION_TARGET_TRACK_INSERT_PARAM, 0, &point, 1);
  bad.processor = "utility.gain";
  bad.param_name = "levelDb";
  expect_lane_add_refusal(project.value, track_id, bad, "desc.insert_index");
  bad.insert_index = -2;
  expect_lane_add_refusal(project.value, track_id, bad, "desc.insert_index");
  bad.insert_index = 0;
  bad.processor = "";
  expect_lane_add_refusal(project.value, track_id, bad, "desc.processor");
  bad.processor = "utility.gain";
  bad.param_name = "";
  expect_lane_add_refusal(project.value, track_id, bad, "desc.param_name");
  bad.param_name = "levelDb";
  bad.send_id = "unexpected";
  expect_lane_add_refusal(project.value, track_id, bad, "desc.send_id");

  SonareAutomationPoint bad_curve{0.0, 0.5f, 4};
  bad = automation_lane_desc(SONARE_AUTOMATION_TARGET_OPAQUE, 17, &bad_curve, 1);
  expect_lane_add_refusal(project.value, track_id, bad, "desc.points[0].curve_to_next");
  CHECK(serialize(project.value) == before);

  uint32_t lane_id = 0;
  REQUIRE(sonare_project_add_automation_lane(project.value, track_id, &desc, &lane_id) ==
          SONARE_OK);
  CHECK(sonare_project_edit_automation_lane(project.value, track_id, lane_id + 100, &desc) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("lane_id") != std::string::npos);
  CHECK(sonare_project_remove_automation_lane(project.value, track_id, lane_id + 100) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("lane_id") != std::string::npos);
#endif
}

TEST_CASE("project automation compile refuses unresolved send and insert targets",
          "[c_api][project][writers][automation]") {
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_MIXING)
  ProjectHandle project;
  const uint32_t track_id = add_audio_track(project.value);
  REQUIRE(sonare_project_set_track_route(project.value, track_id, "main-strip", "master") ==
          SONARE_OK);
  REQUIRE(
      sonare_project_set_mixer_scene_json(
          project.value,
          R"({"version":2,"strips":[{"id":"main-strip"}],"buses":[{"id":"master","role":"master"}]})") ==
      SONARE_OK);

  const SonareAutomationPoint point{0.0, -6.0f, SONARE_CURVE_LINEAR};
  SonareAutomationLaneDesc send =
      automation_lane_desc(SONARE_AUTOMATION_TARGET_TRACK_SEND_DB, 0, &point, 1);
  send.send_id = "missing-send";
  uint32_t lane_id = 0;
  REQUIRE(sonare_project_add_automation_lane(project.value, track_id, &send, &lane_id) ==
          SONARE_OK);

  const auto expect_compile_refusal = [&](const char* message_text) {
    SonareProjectCompileResult result{};
    SonareProjectTimeline* timeline = nullptr;
    REQUIRE(sonare_project_compile(project.value, &result, &timeline) == SONARE_OK);
    CHECK(timeline == nullptr);
    CHECK(result.has_timeline == 0);
    REQUIRE(result.diagnostic_count > 0);
    CHECK(std::string(result.messages).find(message_text) != std::string::npos);
    bool found_unrouted = false;
    for (size_t i = 0; i < result.diagnostic_count; ++i) {
      const char* name = sonare_project_diagnostic_code_name(result.diagnostics[i].code);
      if (name != nullptr && std::string(name) == "automationTargetUnrouted") {
        found_unrouted = true;
      }
    }
    CHECK(found_unrouted);
    sonare_free_project_compile_result(&result);
  };
  expect_compile_refusal("missing-send");

  REQUIRE(sonare_project_remove_automation_lane(project.value, track_id, lane_id) == SONARE_OK);
  REQUIRE(
      sonare_project_set_mixer_scene_json(
          project.value,
          R"({"version":2,"strips":[{"id":"main-strip","inserts":[{"slot":"pre","processor":"utility.gain","params":{"levelDb":0}}]}],"buses":[{"id":"master","role":"master"}]})") ==
      SONARE_OK);

  const SonareAutomationPoint insert_point{0.0, 0.0f, SONARE_CURVE_LINEAR};
  SonareAutomationLaneDesc insert =
      automation_lane_desc(SONARE_AUTOMATION_TARGET_TRACK_INSERT_PARAM, 0, &insert_point, 1);
  insert.insert_index = 1;
  insert.processor = "utility.gain";
  insert.param_name = "levelDb";
  REQUIRE(sonare_project_add_automation_lane(project.value, track_id, &insert, &lane_id) ==
          SONARE_OK);
  expect_compile_refusal("insert[1]");

  insert.insert_index = 0;
  insert.processor = "eq.parametric";
  REQUIRE(sonare_project_edit_automation_lane(project.value, track_id, lane_id, &insert) ==
          SONARE_OK);
  expect_compile_refusal("must be processor 'eq.parametric'");

#if defined(SONARE_WITH_MASTERING)
  const SonareAutomationPoint outside_insert_range{0.0, 1000.0f, SONARE_CURVE_LINEAR};
  insert.processor = "utility.gain";
  insert.points = &outside_insert_range;
  REQUIRE(sonare_project_edit_automation_lane(project.value, track_id, lane_id, &insert) ==
          SONARE_OK);
  expect_compile_refusal("outside the 'utility.gain' parameter range");

  insert.points = &insert_point;
  REQUIRE(sonare_project_edit_automation_lane(project.value, track_id, lane_id, &insert) ==
          SONARE_OK);
  SonareProjectCompileResult result{};
  SonareProjectTimeline* timeline = nullptr;
  REQUIRE(sonare_project_compile(project.value, &result, &timeline) == SONARE_OK);
  CHECK(result.has_timeline != 0);
  CHECK(timeline != nullptr);
  sonare_project_timeline_destroy(timeline);
  sonare_free_project_compile_result(&result);
#endif
#endif
}
