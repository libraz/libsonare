/// @file project_deserialize_fuzz_test.cpp
/// @brief project serializer fuzz / robustness tests: malformed, truncated, and garbage input
///        to project_from_json must return a diagnostic WITHOUT crashing, reading
///        out of bounds, or letting any exception escape.

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_model.h"
#include "arrangement/edit_source.h"
#include "serialize/project_serializer.h"

using namespace sonare::arrangement;
using sonare::serialize::DeserializeResult;
using sonare::serialize::project_from_json;
using sonare::serialize::project_to_json;

namespace {

// Calls project_from_json and asserts no exception escapes. Returns the result
// so the caller can inspect diagnostics. The whole point is that the function is
// noexcept-in-practice: any failure is surfaced as a diagnostic, never a throw.
DeserializeResult safe_parse(const std::string& input) {
  bool threw = false;
  DeserializeResult result;
  try {
    result = project_from_json(input);
  } catch (...) {
    threw = true;
  }
  CHECK_FALSE(threw);
  return result;
}

}  // namespace

TEST_CASE("malformed and garbage input never crashes and yields diagnostics", "[serialize]") {
  const std::vector<std::string> bad_inputs = {
      "",                                                 // empty
      " ",                                                // whitespace only
      "\xEF\xBB\xBF",                                     // BOM only
      "not json at all",                                  // non-JSON garbage
      "{",                                                // truncated object open
      "}",                                                // stray close
      "[",                                                // truncated array
      "{\"version\":",                                    // truncated mid-value
      "{\"version\": 1",                                  // missing closing brace
      "{\"version\": 1,}",                                // trailing comma
      "{\"version\": }",                                  // missing value
      "{version: 1}",                                     // unquoted key
      "{\"version\": 01}",                                // leading zero
      "{\"version\": 1.2.3}",                             // malformed number
      "{\"version\": \"one\"}",                           // version wrong type (string)
      "{\"version\": true}",                              // version wrong type (bool)
      "{\"version\": -5}",                                // negative version
      "{\"version\": 999999}",                            // unsupported future schema version
      "[1, 2, 3]",                                        // top-level array, not object
      "\"just a string\"",                                // top-level string
      "42",                                               // top-level number
      "null",                                             // top-level null
      "true",                                             // top-level bool
      "{\"version\": 1, \"sample_rate\": \"x\"}",         // wrong-typed scalar (tolerated)
      "{\"version\": 1, \"tracks\": \"notarray\"}",       // wrong-typed array (tolerated)
      "{\"version\": 1, \"clips\": [\"notobject\", 5]}",  // bad array elements
      // Sidecar with malformed base64 payload => diagnostic, not crash.
      "{\"version\": 1, \"assist_sidecars\": [{\"module_id\": \"m\", "
      "\"payload_b64\": \"!!!notbase64\"}]}",
      "{\"version\": 1, \"assist_sidecars\": [{\"payload_b64\": \"ABC\"}]}",  // bad length
      "\xFF\xFE\x00\x01garbage",                                              // raw binary bytes
      std::string(1, '\0'),                                                   // a single NUL byte
  };

  for (const auto& input : bad_inputs) {
    INFO("input: " << input);
    auto result = safe_parse(input);
    // Either it failed to produce a project, or (for tolerated wrong-typed
    // optional fields) it produced one with no errors — but it must never crash.
    if (!result.ok()) {
      CHECK_FALSE(result.diagnostics.empty());
    }
  }
}

TEST_CASE("deeply nested input is rejected without stack overflow", "[serialize]") {
  std::string deep;
  for (int i = 0; i < 5000; ++i) deep += "[";
  // No closing brackets: malformed AND deep. Must not crash.
  auto result = safe_parse(deep);
  CHECK_FALSE(result.ok());
  CHECK_FALSE(result.diagnostics.empty());

  std::string deep_obj = "{\"version\": 1, \"x\":";
  for (int i = 0; i < 5000; ++i) deep_obj += "[";
  auto result2 = safe_parse(deep_obj);
  CHECK_FALSE(result2.ok());
}

TEST_CASE("huge and degenerate numbers do not crash", "[serialize]") {
  const std::vector<std::string> inputs = {
      "{\"version\": 1, \"sample_rate\": 1e400}",  // overflow -> inf (parser may reject)
      "{\"version\": 1, \"sample_rate\": -1e400}",
      "{\"version\": 1, \"sample_rate\": 1e-400}",  // underflow -> 0
      "{\"version\": 1, \"tempo_segments\": [{\"bpm\": 1e308}]}",
      "{\"version\": 1.0e1}",  // version 10 -> unsupported
  };
  for (const auto& input : inputs) {
    INFO("input: " << input);
    auto result = safe_parse(input);
    (void)result;  // Only requirement: no crash / no escaped exception.
  }
}

TEST_CASE("every truncated prefix of a valid project never crashes", "[serialize]") {
  // Build a representative valid project, then feed every byte-prefix of its
  // serialization to the deserializer. All but the full string are malformed;
  // none may crash, read OOB, or throw.
  Project p;
  p.set_sample_rate(48000.0);
  AudioSourceRef a;
  a.uri = "file://x.wav";
  const SourceId sid = p.add_audio_source(a);
  Track t;
  t.name = "T";
  const TrackId tid = p.add_track(t);
  EditClip c;
  c.track_id = tid;
  c.source_id = sid;
  c.start_ppq = 0.0;
  c.length_ppq = 960.0;
  p.add_clip(c);
  p.add_marker(0.0, "M");
  AssistSidecar sc;
  sc.module_id = "m";
  sc.payload = {1, 2, 3, 4, 5};
  p.add_assist_sidecar(sc);

  MidiContentStore midi;
  const std::string full = project_to_json(p, midi);

  for (size_t len = 0; len < full.size(); ++len) {
    const std::string prefix = full.substr(0, len);
    auto result = safe_parse(prefix);
    // A strict prefix of valid JSON is malformed: it must not yield a project.
    // (The full string is exercised by the round-trip test.)
    if (len > 0) {
      CHECK_FALSE(result.ok());
    }
  }

  // Sanity: the full document still parses.
  auto full_result = safe_parse(full);
  CHECK(full_result.ok());
}
