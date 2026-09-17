/// @file sonare_c_assist_test.cpp
/// @brief The composition-assist C ABI: the module ids, the preview/apply pair,
///        and the shape AND CONTENT of the result document. Tag:
///        [c_api][assist].
///
/// The JSON document is the product this surface ships, so the patches are
/// decoded back to note numbers rather than only counted, and preview and apply
/// are held to returning the same one. The apply path is pinned on the property
/// a caller depends on and cannot see from outside: a run commits as a SINGLE
/// undo transaction, so one undo puts the clip back. A run that declines is
/// pinned on its reason text, which is the only account of why it declined.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "util/json.h"

namespace {

namespace json = sonare::util::json;

/// Owns the heap document the assist calls return.
class Document {
 public:
  ~Document() { sonare_free_string(text_); }
  char** out() noexcept { return &text_; }
  const char* c_str() const noexcept { return text_; }
  json::Value parsed() const {
    REQUIRE(text_ != nullptr);
    return json::parse_strict(std::string(text_));
  }

 private:
  char* text_ = nullptr;
};

/// Owns a project handle plus the ids the assist request needs.
struct ProjectFixture {
  SonareProject* project = nullptr;
  uint32_t track_id = 0;
  uint32_t clip_id = 0;

  ~ProjectFixture() { sonare_project_destroy(project); }
};

void add_c_major_line(SonareProject* project, uint32_t clip_id) {
  const uint8_t notes[] = {60, 64, 67, 72};
  std::vector<SonareMidiEventPod> events;
  for (int i = 0; i < 4; ++i) {
    SonareMidiEventPod on{};
    SonareMidiEventPod off{};
    REQUIRE(sonare_midi_note_on(i * 480.0, 0, 0, notes[i], 100, &on) == SONARE_OK);
    REQUIRE(sonare_midi_note_off(i * 480.0 + 240.0, 0, 0, notes[i], 0, &off) == SONARE_OK);
    events.push_back(on);
    events.push_back(off);
  }
  REQUIRE(sonare_project_set_midi_events(project, clip_id, events.data(), events.size()) ==
          SONARE_OK);
}

void annotate_c_major(SonareProject* project) {
  SonareProjectKeySegment key{};
  key.start_ppq = 0.0;
  key.end_ppq = 1920.0;
  key.tonic_pc = 0;
  key.mode = 1;  // major
  REQUIRE(sonare_project_annotate_keys(project, &key, 1) == SONARE_OK);

  SonareProjectChordSymbol chord{};
  chord.start_ppq = 0.0;
  chord.end_ppq = 1920.0;
  chord.root_pc = 0;
  chord.quality = 1;  // major
  chord.slash_bass_pc = 255;
  REQUIRE(sonare_project_annotate_chords(project, &chord, 1) == SONARE_OK);
}

/// One MIDI track holding one empty MIDI clip: no notes and no annotation, so
/// neither module has anything to read until a test supplies it.
void make_bare_project(ProjectFixture* fixture) {
  REQUIRE(sonare_project_create(&fixture->project) == SONARE_OK);

  SonareProjectTrackDesc track{};
  track.kind = SONARE_TRACK_MIDI;
  track.name = "lead";
  REQUIRE(sonare_project_add_track(fixture->project, &track, &fixture->track_id) == SONARE_OK);

  SonareProjectClipDesc clip{};
  clip.track_id = fixture->track_id;
  clip.is_midi = 1;
  clip.start_ppq = 0.0;
  clip.length_ppq = 1920.0;
  clip.gain = 1.0f;
  REQUIRE(sonare_project_add_clip(fixture->project, &clip, &fixture->clip_id) == SONARE_OK);
}

/// The bare project plus a C major line and a C major key + chord annotation --
/// everything both built-in modules need.
void make_project(ProjectFixture* fixture) {
  make_bare_project(fixture);
  add_c_major_line(fixture->project, fixture->clip_id);
  annotate_c_major(fixture->project);
}

std::string request_for(uint32_t clip_id, const std::string& modules = {}) {
  std::string request = "{";
  if (!modules.empty()) request += "\"modules\": " + modules + ", ";
  request += "\"params\": {\"target_clip_id\": " + std::to_string(clip_id) + "}}";
  return request;
}

std::string serialize_project(const SonareProject* project) {
  char* text = nullptr;
  size_t length = 0;
  REQUIRE(sonare_project_serialize(project, &text, &length) == SONARE_OK);
  REQUIRE(text != nullptr);
  std::string out(text, length);
  sonare_free_string(text);
  return out;
}

/// Note numbers of the note-ons a patch adds, in document order.
std::vector<int> patch_note_ons(const json::Value& patch) {
  std::vector<int> notes;
  const json::Value* add = patch.find("add");
  REQUIRE(add != nullptr);
  REQUIRE(add->is_array());
  for (const json::Value& event : add->as_array()) {
    const json::Value* data0 = event.find("data0");
    REQUIRE(data0 != nullptr);
    REQUIRE(event.find("ppq") != nullptr);
    REQUIRE(event.find("data1") != nullptr);
    const auto word = static_cast<uint32_t>(data0->as_number());
    const uint32_t status = (word >> 20) & 0x0Fu;
    const int note = static_cast<int>((word >> 8) & 0x7Fu);
    const int velocity = static_cast<int>(word & 0x7Fu);
    if (status == 0x9u && velocity != 0) notes.push_back(note);
  }
  return notes;
}

std::vector<std::string> split_on_newline(const char* text) {
  std::vector<std::string> parts;
  std::string current;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor == '\n') {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(*cursor);
    }
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

bool text_contains(const std::string& text, const std::string& fragment) {
  return text.find(fragment) != std::string::npos;
}

/// MIDI events in a serialized project: the serializer writes one "data0" key
/// per event, so counting them counts the clip content without a getter.
size_t count_midi_events(const std::string& serialized) {
  size_t count = 0;
  const std::string key = "\"data0\"";
  for (size_t at = serialized.find(key); at != std::string::npos;
       at = serialized.find(key, at + key.size())) {
    ++count;
  }
  return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// Module ids
// ---------------------------------------------------------------------------

TEST_CASE("sonare_assist_module_ids names both built-in modules", "[c_api][assist]") {
  const char* ids = sonare_assist_module_ids();
  REQUIRE(ids != nullptr);
  const std::vector<std::string> lines = split_on_newline(ids);
  REQUIRE(lines.size() == 2u);
  CHECK(lines[0] == "sonare.builtin.chord_tone_generator");
  CHECK(lines[1] == "sonare.builtin.diatonic_harmonizer");

  // The ids are what a request selects a module by, so a run naming each of them
  // has to be accepted.
  ProjectFixture fixture;
  make_project(&fixture);
  for (const std::string& id : lines) {
    Document document;
    const std::string request = request_for(fixture.clip_id, "[\"" + id + "\"]");
    INFO("module: " << id);
    CHECK(sonare_project_assist_preview_json(fixture.project, request.c_str(), document.out()) ==
          SONARE_OK);
  }
}

// ---------------------------------------------------------------------------
// The result document
// ---------------------------------------------------------------------------

TEST_CASE("the preview document carries the notes the modules propose", "[c_api][assist]") {
  ProjectFixture fixture;
  make_project(&fixture);

  Document document;
  const std::string request =
      request_for(fixture.clip_id, "[\"sonare.builtin.diatonic_harmonizer\"]");
  REQUIRE(sonare_project_assist_preview_json(fixture.project, request.c_str(), document.out()) ==
          SONARE_OK);
  const json::Value root = document.parsed();

  REQUIRE(root.is_object());
  REQUIRE(root.find("status") != nullptr);
  CHECK(root["status"].as_string() == "ok");
  REQUIRE(root.find("reason") != nullptr);
  CHECK(root["reason"].is_string());
  REQUIRE(root.find("iterationsConsumed") != nullptr);
  CHECK(root["iterationsConsumed"].as_number() == 4.0);
  REQUIRE(root.find("slotsDiscarded") != nullptr);
  CHECK(root["slotsDiscarded"].as_number() == 0.0);

  // unrenderedCommands is present and empty: the built-ins emit nothing this
  // surface cannot describe.
  REQUIRE(root.find("unrenderedCommands") != nullptr);
  REQUIRE(root["unrenderedCommands"].is_array());
  CHECK(root["unrenderedCommands"].as_array().empty());

  REQUIRE(root.find("patches") != nullptr);
  REQUIRE(root["patches"].is_array());
  REQUIRE(root["patches"].as_array().size() == 1u);
  const json::Value& patch = root["patches"][0];
  REQUIRE(patch.find("clipId") != nullptr);
  CHECK(static_cast<uint32_t>(patch["clipId"].as_number()) == fixture.clip_id);
  // The document is the product: decode it rather than counting its entries.
  CHECK(patch_note_ons(patch) == std::vector<int>{57, 60, 64, 69});
  CHECK(patch["add"].as_array().size() == 8u);  // one note-off per note-on

  // The payloads carry the per-note reasons, one string per contributing module.
  REQUIRE(root.find("payloads") != nullptr);
  REQUIRE(root["payloads"].is_array());
  REQUIRE(root["payloads"].as_array().size() == 1u);
  const std::string payload = root["payloads"][0].as_string();
  CHECK(text_contains(payload, "sonare.builtin.diatonic_harmonizer"));
  CHECK(text_contains(payload, "moved -2 scale steps"));
}

TEST_CASE("running both modules reports one patch each", "[c_api][assist]") {
  ProjectFixture fixture;
  make_project(&fixture);

  Document document;
  const std::string request = request_for(fixture.clip_id);
  REQUIRE(sonare_project_assist_preview_json(fixture.project, request.c_str(), document.out()) ==
          SONARE_OK);
  const json::Value root = document.parsed();

  CHECK(root["status"].as_string() == "ok");
  REQUIRE(root["patches"].as_array().size() == 2u);
  // Fixed dispatch order: the generator runs before the counterpoint engine.
  CHECK(patch_note_ons(root["patches"][0]) == std::vector<int>{36, 40, 43});
  CHECK(patch_note_ons(root["patches"][1]) == std::vector<int>{57, 60, 64, 69});
  CHECK(root["payloads"].as_array().size() == 2u);
}

// ---------------------------------------------------------------------------
// preview does not mutate; apply does, as one undo step
// ---------------------------------------------------------------------------

TEST_CASE("preview leaves the project byte-identical", "[c_api][assist]") {
  ProjectFixture fixture;
  make_project(&fixture);
  const std::string before = serialize_project(fixture.project);

  Document document;
  const std::string request = request_for(fixture.clip_id);
  REQUIRE(sonare_project_assist_preview_json(fixture.project, request.c_str(), document.out()) ==
          SONARE_OK);
  // The run did propose something, so the unchanged project below is a preview
  // holding back rather than a run that found nothing.
  CHECK(document.parsed()["status"].as_string() == "ok");
  CHECK(serialize_project(fixture.project) == before);
}

TEST_CASE("apply reports what it commits and one undo takes all of it back", "[c_api][assist]") {
  ProjectFixture fixture;
  make_project(&fixture);
  const std::string before = serialize_project(fixture.project);
  const std::string request = request_for(fixture.clip_id);
  // The fixture's own four notes, as the serializer writes them.
  REQUIRE(count_midi_events(before) == 8u);

  // Both modules contribute, so the commit carries TWO patches and the apply
  // document has to describe both of them.
  Document applied;
  REQUIRE(sonare_project_assist_apply_json(fixture.project, request.c_str(), applied.out()) ==
          SONARE_OK);
  const json::Value apply_root = applied.parsed();
  CHECK(apply_root["status"].as_string() == "ok");
  REQUIRE(apply_root["patches"].as_array().size() == 2u);
  CHECK(patch_note_ons(apply_root["patches"][0]) == std::vector<int>{36, 40, 43});
  CHECK(patch_note_ons(apply_root["patches"][1]) == std::vector<int>{57, 60, 64, 69});
  CHECK(apply_root["iterationsConsumed"].as_number() == 5.0);

  const std::string after = serialize_project(fixture.project);
  CHECK(after != before);
  // Seven notes proposed, fourteen events committed: the document describes the
  // edit that landed rather than reporting on a run whose commands are gone.
  CHECK(count_midi_events(after) == 8u + 14u);

  // The header's claim that apply returns the same document as preview, read as
  // an assertion. Re-previewing on the post-apply project would propose against
  // the new content, so the comparison is made against a fresh project in the
  // same starting state.
  ProjectFixture twin;
  make_project(&twin);
  REQUIRE(serialize_project(twin.project) == before);
  Document previewed;
  const std::string twin_request = request_for(twin.clip_id);
  REQUIRE(sonare_project_assist_preview_json(twin.project, twin_request.c_str(), previewed.out()) ==
          SONARE_OK);
  const json::Value preview_root = previewed.parsed();
  CHECK(json::dump(preview_root["patches"]) == json::dump(apply_root["patches"]));
  CHECK(json::dump(preview_root["payloads"]) == json::dump(apply_root["payloads"]));
  CHECK(preview_root["status"].as_string() == apply_root["status"].as_string());
  // ... and the preview did not commit any of it.
  CHECK(serialize_project(twin.project) == before);

  // One undo, not seven and not two: the whole run is a single transaction.
  REQUIRE(sonare_project_undo(fixture.project) == SONARE_OK);
  CHECK(serialize_project(fixture.project) == before);

  // And it redoes as one step too.
  REQUIRE(sonare_project_redo(fixture.project) == SONARE_OK);
  CHECK(serialize_project(fixture.project) == after);

  // A second undo goes past the assist run into the setup, which is what makes
  // the single undo above a transaction boundary rather than an empty history.
  REQUIRE(sonare_project_undo(fixture.project) == SONARE_OK);
  REQUIRE(sonare_project_undo(fixture.project) == SONARE_OK);
  CHECK(serialize_project(fixture.project) != before);
}

TEST_CASE("a mixed run succeeds and still accounts for the module that declined",
          "[c_api][assist]") {
  const std::string no_notes_fragment = "carries no MIDI events";
  const std::string no_chords =
      "the project annotates no chords, and this module does not guess harmony";

  SECTION("the generator contributes and the harmonizer declines") {
    // Chords annotated, but the clip the harmonizer is pointed at has no notes
    // to derive from.
    ProjectFixture fixture;
    make_bare_project(&fixture);
    annotate_c_major(fixture.project);

    Document document;
    const std::string request = request_for(fixture.clip_id);
    REQUIRE(sonare_project_assist_preview_json(fixture.project, request.c_str(), document.out()) ==
            SONARE_OK);
    const json::Value root = document.parsed();

    // A success: one module did its job.
    CHECK(root["status"].as_string() == "ok");
    REQUIRE(root["patches"].as_array().size() == 1u);
    CHECK(patch_note_ons(root["patches"][0]) == std::vector<int>{36, 40, 43});
    // ... and the run still says what the other one did, which is the whole
    // point of carrying a reason on an "ok" run.
    CHECK(text_contains(root["reason"].as_string(), no_notes_fragment));
    CHECK_FALSE(root["reason"].as_string().empty());
    // The contributor had nothing to complain about, so only one voice is here.
    CHECK_FALSE(text_contains(root["reason"].as_string(), no_chords));
    CHECK(root["payloads"].as_array().size() == 1u);
  }

  SECTION("the harmonizer contributes and the generator declines") {
    // The mirror image: a line and a key, but no chord symbols to voice.
    ProjectFixture fixture;
    make_bare_project(&fixture);
    add_c_major_line(fixture.project, fixture.clip_id);
    SonareProjectKeySegment key{};
    key.start_ppq = 0.0;
    key.end_ppq = 1920.0;
    key.tonic_pc = 0;
    key.mode = 1;  // major
    REQUIRE(sonare_project_annotate_keys(fixture.project, &key, 1) == SONARE_OK);

    Document document;
    const std::string request = request_for(fixture.clip_id);
    REQUIRE(sonare_project_assist_preview_json(fixture.project, request.c_str(), document.out()) ==
            SONARE_OK);
    const json::Value root = document.parsed();

    CHECK(root["status"].as_string() == "ok");
    REQUIRE(root["patches"].as_array().size() == 1u);
    CHECK(patch_note_ons(root["patches"][0]) == std::vector<int>{57, 60, 64, 69});
    CHECK(text_contains(root["reason"].as_string(), no_chords));
    CHECK_FALSE(text_contains(root["reason"].as_string(), no_notes_fragment));
    CHECK(root["payloads"].as_array().size() == 1u);
  }
}

TEST_CASE("a run whose status is not ok commits nothing", "[c_api][assist]") {
  ProjectFixture fixture;
  // No MIDI content and no harmony annotation: neither module has anything to
  // work from, so the run comes back empty.
  make_bare_project(&fixture);
  const std::string before = serialize_project(fixture.project);

  Document document;
  const std::string request = request_for(fixture.clip_id);
  REQUIRE(sonare_project_assist_apply_json(fixture.project, request.c_str(), document.out()) ==
          SONARE_OK);
  const json::Value root = document.parsed();
  CHECK(root["status"].as_string() == "empty");
  CHECK(root["patches"].as_array().empty());
  // Both modules declined for their own separate reasons, and both survive the
  // apply path: an empty run that commits nothing still accounts for itself.
  CHECK(text_contains(root["reason"].as_string(),
                      "the project annotates no chords, and this module does not guess harmony"));
  CHECK(text_contains(root["reason"].as_string(), "carries no MIDI events"));
  CHECK(serialize_project(fixture.project) == before);

  // Nothing was pushed onto the history either: the undo is refused because
  // only the setup commands are there to take back.
  REQUIRE(sonare_project_undo(fixture.project) == SONARE_OK);  // undoes the AddClip
  CHECK(serialize_project(fixture.project) != before);
}

// ---------------------------------------------------------------------------
// Request validation
// ---------------------------------------------------------------------------

TEST_CASE("an unknown module id is an error, not a silent skip", "[c_api][assist]") {
  ProjectFixture fixture;
  make_project(&fixture);

  Document document;
  const std::string request = request_for(fixture.clip_id, "[\"sonare.builtin.nope\"]");
  CHECK(sonare_project_assist_preview_json(fixture.project, request.c_str(), document.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(document.c_str() == nullptr);
  CHECK(text_contains(std::string(sonare_last_error_message()), "unknown module id"));

  // A well-formed selection of the SAME shape is accepted, so the refusal is
  // the id and not the field.
  Document accepted;
  const std::string good =
      request_for(fixture.clip_id, "[\"sonare.builtin.chord_tone_generator\"]");
  CHECK(sonare_project_assist_preview_json(fixture.project, good.c_str(), accepted.out()) ==
        SONARE_OK);

  // A modules field of the wrong shape is refused by name too.
  Document wrong_shape;
  const std::string not_an_array = request_for(fixture.clip_id, "\"chord_tone_generator\"");
  CHECK(sonare_project_assist_preview_json(fixture.project, not_an_array.c_str(),
                                           wrong_shape.out()) == SONARE_ERROR_INVALID_PARAMETER);

  Document wrong_element;
  const std::string not_strings = request_for(fixture.clip_id, "[7]");
  CHECK(sonare_project_assist_preview_json(fixture.project, not_strings.c_str(),
                                           wrong_element.out()) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("the assist entry points reject a malformed call", "[c_api][assist]") {
  ProjectFixture fixture;
  make_project(&fixture);
  const std::string request = request_for(fixture.clip_id);

  Document document;
  CHECK(sonare_project_assist_preview_json(nullptr, request.c_str(), document.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_assist_apply_json(nullptr, request.c_str(), document.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_assist_preview_json(fixture.project, request.c_str(), nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_assist_apply_json(fixture.project, request.c_str(), nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_assist_preview_json(fixture.project, nullptr, document.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_assist_preview_json(fixture.project, "", document.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_assist_preview_json(fixture.project, "{", document.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_assist_preview_json(fixture.project, "[]", document.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_assist_preview_json(fixture.project, "{\"scope\": 1}", document.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_assist_preview_json(fixture.project, "{\"budget\": 1}", document.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_assist_preview_json(fixture.project, "{\"params\": 1}", document.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_assist_preview_json(fixture.project, "{\"seed\": -1}", document.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  // A refused call leaves nothing to free.
  CHECK(document.c_str() == nullptr);

  // The same project accepts the valid request, so the refusals are the request.
  CHECK(sonare_project_assist_preview_json(fixture.project, request.c_str(), document.out()) ==
        SONARE_OK);
  CHECK(document.c_str() != nullptr);
}

TEST_CASE("a params document naming no target clip is REFUSED with an error return",
          "[c_api][assist]") {
  ProjectFixture fixture;
  make_project(&fixture);
  const std::string before = serialize_project(fixture.project);
  const std::string declined =
      "params_json must name target_clip_id: this module writes into an existing clip and does "
      "not create one";

  Document document;
  // A caller that named no clip made a mistake, and the call says so in its
  // return code rather than leaving the reason text as the only way to find it.
  CHECK(sonare_project_assist_preview_json(fixture.project, "{\"params\": {}}", document.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(text_contains(std::string(sonare_last_error_message()), declined));

  // The document is still written on this path -- and it is empty throughout,
  // because the modules refuse before doing any work.
  REQUIRE(document.c_str() != nullptr);
  const json::Value root = document.parsed();
  CHECK(root["status"].as_string() == "rejected");
  CHECK(root["patches"].as_array().empty());
  CHECK(root["payloads"].as_array().empty());
  CHECK(root["unrenderedCommands"].as_array().empty());
  CHECK(root["iterationsConsumed"].as_number() == 0.0);
  // Both modules read the same params blob and refuse it identically, and the
  // reason is reported ONCE rather than once per registered module: two copies
  // of one sentence reads to a caller like a library bug, not their own typo.
  CHECK(text_contains(root["reason"].as_string(), declined));
  CHECK(root["reason"].as_string() == declined);
  CHECK(std::string(sonare_last_error_message()) == declined);

  // With one module selected the reason is that module's words and nothing else.
  Document single;
  const std::string one_module =
      "{\"modules\": [\"sonare.builtin.diatonic_harmonizer\"], \"params\": {}}";
  CHECK(sonare_project_assist_preview_json(fixture.project, one_module.c_str(), single.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(single.parsed()["reason"].as_string() == declined);

  // Applying a refused request commits nothing.
  Document applied;
  CHECK(sonare_project_assist_apply_json(fixture.project, "{\"params\": {}}", applied.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(applied.parsed()["status"].as_string() == "rejected");
  CHECK(serialize_project(fixture.project) == before);
}

TEST_CASE("a refusal and an empty outcome differ in status AND in return code", "[c_api][assist]") {
  ProjectFixture fixture;
  make_project(&fixture);

  // A caller's mistake: a field outside its domain.
  Document refused;
  const std::string bad_params =
      "{\"params\": {\"target_clip_id\": " + std::to_string(fixture.clip_id) +
      ", \"velocity_scale\": 9}}";
  const SonareError refused_rc =
      sonare_project_assist_preview_json(fixture.project, bad_params.c_str(), refused.out());

  // A musical nothing: a well-formed request over a project with nothing to act
  // on. Same empty patches, same non-empty reason -- and that is exactly why the
  // status and the return code have to separate them.
  ProjectFixture bare;
  make_bare_project(&bare);
  Document empty;
  const std::string good_params = request_for(bare.clip_id);
  const SonareError empty_rc =
      sonare_project_assist_preview_json(bare.project, good_params.c_str(), empty.out());

  CHECK(refused_rc == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(empty_rc == SONARE_OK);
  CHECK(refused_rc != empty_rc);

  const json::Value refused_root = refused.parsed();
  const json::Value empty_root = empty.parsed();
  CHECK(refused_root["status"].as_string() == "rejected");
  CHECK(empty_root["status"].as_string() == "empty");
  CHECK(refused_root["status"].as_string() != empty_root["status"].as_string());

  // The two are indistinguishable on every other axis, which is the point.
  CHECK(refused_root["patches"].as_array().empty());
  CHECK(empty_root["patches"].as_array().empty());
  CHECK_FALSE(refused_root["reason"].as_string().empty());
  CHECK_FALSE(empty_root["reason"].as_string().empty());
}

// ---------------------------------------------------------------------------
// Scope and budget
// ---------------------------------------------------------------------------

TEST_CASE("the request's scope and budget reach the modules", "[c_api][assist]") {
  ProjectFixture fixture;
  make_project(&fixture);

  Document scoped;
  const std::string scope_request =
      "{\"modules\": [\"sonare.builtin.diatonic_harmonizer\"], \"scope\": {\"startPpq\": 480, "
      "\"endPpq\": 1440}, \"params\": {\"target_clip_id\": " +
      std::to_string(fixture.clip_id) + "}}";
  REQUIRE(sonare_project_assist_preview_json(fixture.project, scope_request.c_str(),
                                             scoped.out()) == SONARE_OK);
  const json::Value scope_root = scoped.parsed();
  CHECK(scope_root["status"].as_string() == "ok");
  CHECK(patch_note_ons(scope_root["patches"][0]) == std::vector<int>{60, 64});

  // A LONE module stops itself on the iteration cap. The driver's gate only
  // fires between slots and there is no later slot here, so the module's own
  // report is the only account of the truncation and it has to reach the status.
  Document budgeted;
  const std::string budget_request =
      "{\"modules\": [\"sonare.builtin.diatonic_harmonizer\"], \"budget\": {\"maxIterations\": 2}, "
      "\"params\": {\"target_clip_id\": " +
      std::to_string(fixture.clip_id) + "}}";
  REQUIRE(sonare_project_assist_preview_json(fixture.project, budget_request.c_str(),
                                             budgeted.out()) == SONARE_OK);
  const json::Value budget_root = budgeted.parsed();
  CHECK(budget_root["status"].as_string() == "budgetTruncated");
  // One module ran, so the reason is that module's own words exactly.
  CHECK(budget_root["reason"].as_string() == "stopped after the request's iteration budget");
  CHECK(budget_root["iterationsConsumed"].as_number() == 2.0);
  CHECK(patch_note_ons(budget_root["patches"][0]) == std::vector<int>{57, 60});

  // Without the cap the same request derives all four, so the two above are the
  // budget acting and not the source being short.
  Document unbudgeted;
  const std::string whole_request =
      request_for(fixture.clip_id, "[\"sonare.builtin.diatonic_harmonizer\"]");
  REQUIRE(sonare_project_assist_preview_json(fixture.project, whole_request.c_str(),
                                             unbudgeted.out()) == SONARE_OK);
  const json::Value whole_root = unbudgeted.parsed();
  CHECK(whole_root["status"].as_string() == "ok");
  CHECK(patch_note_ons(whole_root["patches"][0]) == std::vector<int>{57, 60, 64, 69});

  // The driver's own gate is the OTHER path to the same status: once the
  // generator has spent the budget, the counterpoint slot is not dispatched at
  // all, and the reason is the driver's rather than a module's.
  Document gated;
  const std::string gated_request =
      "{\"budget\": {\"maxIterations\": 1}, \"params\": {\"target_clip_id\": " +
      std::to_string(fixture.clip_id) + "}}";
  REQUIRE(sonare_project_assist_preview_json(fixture.project, gated_request.c_str(), gated.out()) ==
          SONARE_OK);
  const json::Value gated_root = gated.parsed();
  CHECK(gated_root["status"].as_string() == "budgetTruncated");
  CHECK(gated_root["reason"].as_string() == "iteration budget exhausted");
  // Only the generator's patch is there: the harmonizer never ran.
  REQUIRE(gated_root["patches"].as_array().size() == 1u);
  CHECK(patch_note_ons(gated_root["patches"][0]) == std::vector<int>{36, 40, 43});
}

TEST_CASE("assist params reach the modules through the opaque blob", "[c_api][assist]") {
  ProjectFixture fixture;
  make_project(&fixture);

  Document document;
  const std::string request =
      "{\"modules\": [\"sonare.builtin.chord_tone_generator\"], \"params\": {\"target_clip_id\": " +
      std::to_string(fixture.clip_id) + ", \"low_note\": 60, \"base_velocity\": 40}}";
  REQUIRE(sonare_project_assist_preview_json(fixture.project, request.c_str(), document.out()) ==
          SONARE_OK);
  const json::Value root = document.parsed();
  CHECK(patch_note_ons(root["patches"][0]) == std::vector<int>{60, 64, 67});

  // A params field outside its domain is REFUSED by name -- not a silently
  // substituted default, and not an empty run the caller has to read prose to
  // understand.
  Document refused;
  const std::string bad =
      "{\"modules\": [\"sonare.builtin.diatonic_harmonizer\"], \"params\": {\"target_clip_id\": " +
      std::to_string(fixture.clip_id) + ", \"velocity_scale\": 0}}";
  CHECK(sonare_project_assist_preview_json(fixture.project, bad.c_str(), refused.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  const json::Value bad_root = refused.parsed();
  CHECK(bad_root["status"].as_string() == "rejected");
  CHECK(bad_root["patches"].as_array().empty());
  CHECK(bad_root["reason"].as_string() == "velocity_scale must be within (0, 2]");
  CHECK(std::string(sonare_last_error_message()) == "velocity_scale must be within (0, 2]");

  // A field of the wrong type is reported by name too, rather than defaulted.
  Document mistyped;
  const std::string broken =
      "{\"modules\": [\"sonare.builtin.diatonic_harmonizer\"], \"params\": {\"target_clip_id\": " +
      std::to_string(fixture.clip_id) + ", \"low_note\": \"48\"}}";
  CHECK(sonare_project_assist_preview_json(fixture.project, broken.c_str(), mistyped.out()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mistyped.parsed()["status"].as_string() == "rejected");
  CHECK(mistyped.parsed()["reason"].as_string() == "low_note must be a number");

  // A module that declines because the project says nothing it can use is a
  // different thing entirely: it succeeds, and reports in its own words.
  ProjectFixture bare;
  make_bare_project(&bare);

  Document no_chords;
  const std::string comp_request =
      request_for(bare.clip_id, "[\"sonare.builtin.chord_tone_generator\"]");
  REQUIRE(sonare_project_assist_preview_json(bare.project, comp_request.c_str(), no_chords.out()) ==
          SONARE_OK);
  const json::Value bare_root = no_chords.parsed();
  CHECK(bare_root["status"].as_string() == "empty");
  CHECK(bare_root["reason"].as_string() ==
        "the project annotates no chords, and this module does not guess harmony");
}
