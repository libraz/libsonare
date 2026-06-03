/// @file project_default_sr_and_uri_test.cpp
/// @brief Regression tests for the headless-project default sample rate (48 kHz)
///        and audio-source URI preservation across a serialize -> deserialize
///        round-trip.
///
/// These cover two audit fixes:
///   1. The default project sample rate was raised from the lo-fi 22.05 kHz
///      analysis default to the conventional 48 kHz DAW render rate. Both the
///      in-memory Project constructor and the serializer's "sample_rate"
///      fallback (for a document that omits the field) must agree.
///   2. Decoded PCM is intentionally NEVER embedded in the project JSON, so the
///      ONLY render-time link to the underlying audio is the audio source's
///      uri / storage_handle_id / content_hash reference triple. Those must
///      survive a round-trip so a host can detect an unresolved source and
///      re-decode it instead of silently bouncing silence.

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "arrangement/edit_command.h"
#include "arrangement/edit_model.h"
#include "arrangement/edit_source.h"
#include "serialize/project_serializer.h"

using namespace sonare;
using namespace sonare::arrangement;
using sonare::serialize::project_from_json;
using sonare::serialize::project_to_json;

namespace {

// Returns the single AudioSourceRef in a project (the tests register exactly
// one), or nullptr if the first source is not audio.
const AudioSourceRef* first_audio_source(const Project& p) {
  if (p.sources().empty()) return nullptr;
  return std::get_if<AudioSourceRef>(&p.sources().front());
}

}  // namespace

TEST_CASE("Project default sample rate is 48 kHz", "[serialize][arrangement]") {
  // In-memory constructor default.
  Project fresh;
  CHECK(fresh.sample_rate() == 48000.0);

  // Serializer fallback: a document that OMITS "sample_rate" must deserialize to
  // the same 48 kHz default as an in-memory project (not the old 22.05 kHz).
  const std::string json_without_sr = R"({"version": 1})";
  auto result = project_from_json(json_without_sr);
  REQUIRE(result.ok());
  CHECK(result.project->sample_rate() == 48000.0);
}

TEST_CASE("Explicit sample rate survives round-trip", "[serialize][arrangement]") {
  Project p;
  p.set_sample_rate(96000.0);
  MidiContentStore midi;
  const std::string json = project_to_json(p, midi);
  auto result = project_from_json(json);
  REQUIRE(result.ok());
  CHECK(result.project->sample_rate() == 96000.0);
}

TEST_CASE("Audio source URI reference is preserved across round-trip", "[serialize][arrangement]") {
  // A host that drops the in-memory PCM after save must be able to re-resolve the
  // source from its reference triple. Decoded samples are NOT embedded in JSON;
  // these references are the only re-decode link, so they must round-trip exactly.
  Project p;
  AudioSourceRef src;
  src.uri = "file:///host/local/take 01 & \"mix\".wav";
  src.channel_count = 2;
  src.sample_rate_hint = 44100.0;
  src.storage_handle_id = 7;
  src.content_hash = "sha256:deadbeef";
  p.add_audio_source(src);

  MidiContentStore midi;
  const std::string json = project_to_json(p, midi);
  auto result = project_from_json(json);
  REQUIRE(result.ok());

  const AudioSourceRef* loaded = first_audio_source(*result.project);
  REQUIRE(loaded != nullptr);
  CHECK(loaded->uri == src.uri);
  CHECK(loaded->channel_count == src.channel_count);
  CHECK(loaded->sample_rate_hint == src.sample_rate_hint);
  CHECK(loaded->storage_handle_id == src.storage_handle_id);
  CHECK(loaded->content_hash == src.content_hash);
}
