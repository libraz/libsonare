/// @file sonare_c_project_midi_meter_test.cpp
/// @brief SMF and MIDI Clip File export/import keep a project's meter map as it is, through the
///        C ABI: no implied signature on export, and an import replacing the meter as it replaces
///        the tempo.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

struct OwnedTimeSignatureList {
  SonareProjectTimeSignatureList value{};
  ~OwnedTimeSignatureList() { sonare_free_project_time_signature_list(&value); }
};

using ExportFn = SonareError (*)(const SonareProject*, uint8_t**, size_t*);
using ImportFn = SonareError (*)(SonareProject*, const uint8_t*, size_t, uint32_t*);

struct Container {
  const char* name;
  ExportFn export_bytes;
  ImportFn import_bytes;
};

const Container kContainers[] = {
    {"SMF", sonare_project_export_smf, sonare_project_import_smf},
    {"MIDI Clip File", sonare_project_export_clip_file, sonare_project_import_clip_file},
};

// A project holding one MIDI clip with a single note, and @p meter as its meter map.
SonareProject* project_with_meter(const std::vector<SonareProjectTimeSignatureSegment>& meter) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  uint32_t track_id = 0;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track_id, &clip_id) == SONARE_OK);
  SonareMidiEventPod events[2] = {};
  REQUIRE(sonare_midi_note_on(0.0, 0, 0, 60, 100, &events[0]) == SONARE_OK);
  REQUIRE(sonare_midi_note_off(1.0, 0, 0, 60, 0, &events[1]) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip_id, events, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_time_signatures(project, meter.data(), meter.size()) == SONARE_OK);
  return project;
}

std::vector<SonareProjectTimeSignatureSegment> read_meter(const SonareProject* project) {
  OwnedTimeSignatureList list;
  REQUIRE(sonare_project_time_signatures(project, &list.value) == SONARE_OK);
  std::vector<SonareProjectTimeSignatureSegment> meter;
  meter.reserve(list.value.count);
  for (size_t i = 0; i < list.value.count; ++i) {
    meter.push_back(list.value.items[i]);
  }
  return meter;
}

std::vector<uint8_t> export_project(const Container& container, const SonareProject* project) {
  uint8_t* bytes = nullptr;
  size_t len = 0;
  REQUIRE(container.export_bytes(project, &bytes, &len) == SONARE_OK);
  std::vector<uint8_t> out(bytes, bytes + len);
  sonare_free_bytes(bytes);
  return out;
}

// Exports @p meter from one project and imports the bytes into a fresh one.
std::vector<SonareProjectTimeSignatureSegment> round_trip(
    const Container& container, const std::vector<SonareProjectTimeSignatureSegment>& meter) {
  SonareProject* source = project_with_meter(meter);
  const std::vector<uint8_t> bytes = export_project(container, source);
  sonare_project_destroy(source);
  SonareProject* target = nullptr;
  REQUIRE(sonare_project_create(&target) == SONARE_OK);
  REQUIRE(container.import_bytes(target, bytes.data(), bytes.size(), nullptr) == SONARE_OK);
  std::vector<SonareProjectTimeSignatureSegment> read = read_meter(target);
  sonare_project_destroy(target);
  return read;
}

}  // namespace

TEST_CASE("an exported project's meter map imports back unchanged", "[c_api][project][midi]") {
  for (const Container& container : kContainers) {
    INFO(container.name);

    // An empty meter map exports no signature and imports as empty.
    CHECK(round_trip(container, {}).empty());

    // A first signature after zero gains nothing at zero.
    const std::vector<SonareProjectTimeSignatureSegment> read =
        round_trip(container, {{0.5, 3, 4}});
    REQUIRE(read.size() == 1);
    CHECK(read[0].start_ppq == 0.5);
    CHECK(read[0].numerator == 3);
    CHECK(read[0].denominator == 4);
  }
}

TEST_CASE("an import replaces the project's meter map as it replaces the tempo map",
          "[c_api][project][midi]") {
  for (const Container& container : kContainers) {
    INFO(container.name);
    SonareProject* meterless = project_with_meter({});
    const std::vector<uint8_t> bytes = export_project(container, meterless);
    sonare_project_destroy(meterless);

    SonareProject* project = project_with_meter({{0.0, 3, 4}});
    const SonareProjectTempoSegment tempo = {0.0, 90.0, 0.0};
    REQUIRE(sonare_project_set_tempo_segments(project, &tempo, 1) == SONARE_OK);
    REQUIRE(container.import_bytes(project, bytes.data(), bytes.size(), nullptr) == SONARE_OK);

    CHECK(read_meter(project).empty());
    SonareProjectTempoSegmentList imported_tempos{};
    REQUIRE(sonare_project_tempo_segments(project, &imported_tempos) == SONARE_OK);
    REQUIRE(imported_tempos.count == 1);
    CHECK(imported_tempos.items[0].bpm == 120.0);
    sonare_free_project_tempo_segment_list(&imported_tempos);

    // One undo restores the meter the import replaced.
    REQUIRE(sonare_project_undo(project) == SONARE_OK);
    const std::vector<SonareProjectTimeSignatureSegment> restored = read_meter(project);
    REQUIRE(restored.size() == 1);
    CHECK(restored[0].numerator == 3);
    sonare_project_destroy(project);
  }
}
