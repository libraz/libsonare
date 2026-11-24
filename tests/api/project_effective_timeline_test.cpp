#include <sonare/sonare_c.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

struct ProjectHandle {
  SonareProject* value = nullptr;

  ProjectHandle() { REQUIRE(sonare_project_create(&value) == SONARE_OK); }
  ~ProjectHandle() { sonare_project_destroy(value); }
  ProjectHandle(const ProjectHandle&) = delete;
  ProjectHandle& operator=(const ProjectHandle&) = delete;
};

double end_ppq(const SonareProject* project) {
  double result = -1.0;
  REQUIRE(sonare_project_end_ppq(project, &result) == SONARE_OK);
  return result;
}

uint32_t add_midi_clip(SonareProject* project, double start_ppq, double length_ppq) {
  uint32_t track_id = 0;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_midi_clip(project, start_ppq, length_ppq, &track_id, &clip_id) ==
          SONARE_OK);
  return track_id;
}

SonareProjectPosition position_at(const SonareProject* project, double ppq) {
  SonareProjectPosition result{};
  REQUIRE(sonare_project_position_at_ppq(project, ppq, &result) == SONARE_OK);
  return result;
}

double bar_start(const SonareProject* project, int64_t bar_index) {
  double result = -1.0;
  REQUIRE(sonare_project_bar_start_ppq(project, bar_index, &result) == SONARE_OK);
  return result;
}

std::vector<SonareProjectTempoSegment> tempo_segments(const SonareProject* project) {
  SonareProjectTempoSegmentList result{};
  REQUIRE(sonare_project_tempo_segments(project, &result) == SONARE_OK);
  std::vector<SonareProjectTempoSegment> copied;
  if (result.count > 0) copied.assign(result.items, result.items + result.count);
  sonare_free_project_tempo_segment_list(&result);
  return copied;
}

}  // namespace

TEST_CASE("project end PPQ includes audible clips only", "[c_api][project][effective_timeline]") {
  ProjectHandle project;
  CHECK(end_ppq(project.value) == 0.0);

  SonareProjectMarker marker{};
  marker.kind = SONARE_MARKER_KIND_MARKER;
  marker.ppq = 1000.0;
  uint32_t marker_id = 0;
  REQUIRE(sonare_project_set_marker(project.value, &marker, "later marker", &marker_id) ==
          SONARE_OK);
  CHECK(end_ppq(project.value) == 0.0);

  const uint32_t short_track = add_midi_clip(project.value, 8.0, 4.0);
  add_midi_clip(project.value, 2.0, 20.0);
  const uint32_t muted_track = add_midi_clip(project.value, 100.0, 50.0);
  REQUIRE(sonare_project_set_track_mute(project.value, muted_track, 1) == SONARE_OK);

  CHECK(end_ppq(project.value) == 22.0);

  // A soloed track silences every other one, so only its clips count.
  REQUIRE(sonare_project_set_track_solo(project.value, short_track, 1) == SONARE_OK);
  CHECK(end_ppq(project.value) == 12.0);
  REQUIRE(sonare_project_set_track_solo(project.value, short_track, 0) == SONARE_OK);
  CHECK(end_ppq(project.value) == 22.0);
}

TEST_CASE("project deserialization refuses sub-floor tempo at its document field",
          "[c_api][project][tempo_floor]") {
  constexpr const char* documents[] = {
      R"({"version":7,"tempoSegments":[{"startPpq":0,"bpm":1e-8},{"startPpq":0,"bpm":120}]})",
      R"({"version":7,"tempoSegments":[{"startPpq":0,"bpm":120,"endBpm":1e-8}]})",
  };
  const char* subjects[] = {"tempoSegments[0].bpm", "tempoSegments[0].endBpm"};
  for (size_t i = 0; i < 2; ++i) {
    const std::string document = documents[i];
    SonareProject* out = nullptr;
    char* diagnostics = nullptr;
    const SonareError code =
        sonare_project_deserialize(document.data(), document.size(), &out, &diagnostics);
    const std::string message = sonare_last_error_message();
    const std::string diagnostic_text = diagnostics ? diagnostics : "";
    CHECK(out == nullptr);
    sonare_free_string(diagnostics);
    sonare_project_destroy(out);

    const std::string expected = std::string(subjects[i]) + " must be finite and in [1e-6, 100000]";
    CHECK(code == SONARE_ERROR_INVALID_FORMAT);
    CHECK(message.find(expected) != std::string::npos);
    CHECK(diagnostic_text.find(expected) != std::string::npos);
  }
}

TEST_CASE("project tempo setter enforces the floor and preserves stored segments on refusal",
          "[c_api][project][effective_timeline][tempo_floor]") {
  ProjectHandle project;
  const SonareProjectTempoSegment valid[] = {{0.0, 120.0, 0.0}, {4.0, 90.0, 0.0}};
  REQUIRE(sonare_project_set_tempo_segments(project.value, valid, 2) == SONARE_OK);
  const auto before = tempo_segments(project.value);
  REQUIRE(before.size() == 2);

  const SonareProjectTempoSegment bad_start[] = {{0.0, 120.0, 0.0}, {4.0, 1.0e-8, 0.0}};
  CHECK(sonare_project_set_tempo_segments(project.value, bad_start, 2) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "segments[1].bpm must be finite and in [1e-6, 100000]");
  const auto after_start_refusal = tempo_segments(project.value);
  REQUIRE(after_start_refusal.size() == before.size());
  CHECK(after_start_refusal[0].bpm == before[0].bpm);
  CHECK(after_start_refusal[1].bpm == before[1].bpm);

  for (const double end_bpm : {1.0e-8, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
    const SonareProjectTempoSegment bad_ramp[] = {{0.0, 120.0, end_bpm}, {4.0, 90.0, 0.0}};
    CHECK(sonare_project_set_tempo_segments(project.value, bad_ramp, 2) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          "segments[0].end_bpm must be finite and in [1e-6, 100000]");
    const auto after_ramp_refusal = tempo_segments(project.value);
    REQUIRE(after_ramp_refusal.size() == before.size());
    CHECK(after_ramp_refusal[0].end_bpm == before[0].end_bpm);
    CHECK(after_ramp_refusal[1].bpm == before[1].bpm);
  }

  const SonareProjectTempoSegment floor[] = {{0.0, 120.0, 1.0e-6}, {4.0, 1.0e-6, 0.0}};
  REQUIRE(sonare_project_set_tempo_segments(project.value, floor, 2) == SONARE_OK);
  const auto accepted = tempo_segments(project.value);
  REQUIRE(accepted.size() == 2);
  CHECK(accepted[0].end_bpm == 1.0e-6);
  CHECK(accepted[1].bpm == 1.0e-6);
}

TEST_CASE("project position and bar readers follow the effective meter grid",
          "[c_api][project][effective_timeline]") {
  ProjectHandle project;

  const SonareProjectPosition empty = position_at(project.value, 12.5);
  CHECK(empty.bpm == 120.0);
  CHECK(empty.time_signature.start_ppq == 0.0);
  CHECK(empty.time_signature.numerator == 4);
  CHECK(empty.time_signature.denominator == 4);
  CHECK(empty.bar_start_ppq == 12.0);
  CHECK(empty.bar_index == 3);
  CHECK(empty.beat == 1);
  CHECK(empty.beat_fraction == Catch::Approx(0.5));

  const SonareProjectTimeSignatureSegment signatures[] = {{0.0, 4, 4}, {6.0, 3, 4}};
  REQUIRE(sonare_project_set_time_signatures(project.value, signatures, 2) == SONARE_OK);

  const SonareProjectPosition before_change = position_at(project.value, 5.5);
  CHECK(before_change.bar_index == 1);
  CHECK(before_change.bar_start_ppq == 4.0);
  CHECK(before_change.beat == 2);
  CHECK(before_change.beat_fraction == Catch::Approx(0.5));

  const SonareProjectPosition at_change = position_at(project.value, 6.0);
  CHECK(at_change.time_signature.start_ppq == 6.0);
  CHECK(at_change.time_signature.numerator == 3);
  CHECK(at_change.time_signature.denominator == 4);
  CHECK(at_change.bar_start_ppq == 6.0);
  CHECK(at_change.bar_index == 2);
  CHECK(at_change.beat == 1);
  CHECK(at_change.beat_fraction == 0.0);

  for (int64_t bar_index = 0; bar_index <= 5; ++bar_index) {
    const double start = bar_start(project.value, bar_index);
    const SonareProjectPosition round_trip = position_at(project.value, start);
    CHECK(round_trip.bar_index == bar_index);
    CHECK(round_trip.bar_start_ppq == start);
    CHECK(round_trip.beat == 1);
    CHECK(round_trip.beat_fraction == 0.0);
  }
}

TEST_CASE("project position refuses an overflowing bar index by PPQ",
          "[c_api][project][effective_timeline]") {
  ProjectHandle project;
  const SonareProjectTimeSignatureSegment signature = {0.0, 1, std::numeric_limits<int>::max()};
  REQUIRE(sonare_project_set_time_signatures(project.value, &signature, 1) == SONARE_OK);

  SonareProjectPosition result{};
  CHECK(sonare_project_position_at_ppq(project.value, 1.0e12, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  const std::string message = sonare_last_error_message();
  CHECK(message.find("ppq") != std::string::npos);
  CHECK(message.find("representable range") != std::string::npos);
  CHECK(message.find("9223372036854775807") != std::string::npos);
  CHECK(result.bpm == 0.0);
}

TEST_CASE("project position keeps fitting high bar indices and bar starts round-trip",
          "[c_api][project][effective_timeline]") {
  ProjectHandle project;
  const SonareProjectTimeSignatureSegment signature = {0.0, 1, 1000000};
  REQUIRE(sonare_project_set_time_signatures(project.value, &signature, 1) == SONARE_OK);

  const SonareProjectPosition at_end = position_at(project.value, 1.0e12);
  CHECK(at_end.bar_index > 9007199254740992LL);
  CHECK(at_end.bar_index < std::numeric_limits<int64_t>::max());
  CHECK(bar_start(project.value, at_end.bar_index) == at_end.bar_start_ppq);

  double result = 123.0;
  CHECK(sonare_project_bar_start_ppq(project.value, at_end.bar_index + 1, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(result == 0.0);
  CHECK(std::string(sonare_last_error_message()).find("bar_index") != std::string::npos);
}

TEST_CASE("project position round-trips a located bar and refuses an aliased bar index",
          "[c_api][project][effective_timeline]") {
  ProjectHandle project;
  const SonareProjectTimeSignatureSegment signature = {0.0, 1, 1000000};
  REQUIRE(sonare_project_set_time_signatures(project.value, &signature, 1) == SONARE_OK);

  const SonareProjectPosition result = position_at(project.value, 100000000.00000024);
  CHECK(result.bar_index == INT64_C(25000000000000));
  CHECK(result.bar_start_ppq == 100000000.0);
  CHECK(result.beat == 1);
  CHECK(result.beat_fraction == Catch::Approx(0.059604644775390625));
  const double start = bar_start(project.value, result.bar_index);
  CHECK(start == result.bar_start_ppq);
  const SonareProjectPosition at_start = position_at(project.value, start);
  CHECK(at_start.bar_index == result.bar_index);
  CHECK(at_start.beat == 1);
  CHECK(at_start.beat_fraction == 0.0);

  // This index rounds to its neighbor when converted to double.
  double bar_start_ppq = 123.0;
  CHECK(sonare_project_bar_start_ppq(project.value, INT64_C(25000000000000059), &bar_start_ppq) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(bar_start_ppq == 0.0);
  CHECK(std::string(sonare_last_error_message()).find("bar_index") != std::string::npos);
}

TEST_CASE("project position refuses a bar start that resolves to a different PPQ",
          "[c_api][project][effective_timeline][exactness]") {
  ProjectHandle project;
  constexpr double kSignatureChange = 1.0e-10;
  const SonareProjectTimeSignatureSegment signatures[] = {{0.0, 4, 4}, {kSignatureChange, 3, 4}};
  REQUIRE(sonare_project_set_time_signatures(project.value, signatures, 2) == SONARE_OK);

  // The tiny first span contributes zero complete grid slots at the meter transition.
  SonareProjectPosition refused{};
  CHECK(sonare_project_position_at_ppq(project.value, 0.0, &refused) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message())
            .find("ppq must identify a bar with an exactly representable PPQ start") !=
        std::string::npos);
  CHECK(refused.bpm == 0.0);

  const SonareProjectPosition after = position_at(project.value, kSignatureChange);
  CHECK(after.bar_index == 0);
  CHECK(after.bar_start_ppq == kSignatureChange);
  CHECK(bar_start(project.value, after.bar_index) == after.bar_start_ppq);
}
