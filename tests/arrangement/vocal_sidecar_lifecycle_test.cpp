#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_history.h"
#include "arrangement/edit_model.h"
#include "arrangement/vocal_edit_sidecar.h"

namespace {

namespace arr = sonare::arrangement;
namespace vocal = arr::vocal_sidecar;

arr::Project make_audio_project(arr::ClipId* clip_out = nullptr,
                                arr::SourceId* source_a_out = nullptr,
                                arr::SourceId* source_b_out = nullptr) {
  arr::Project project;
  const arr::SourceId source_a = project.add_audio_source(arr::AudioSourceRef{});
  const arr::SourceId source_b = project.add_audio_source(arr::AudioSourceRef{});
  arr::Track track;
  track.kind = arr::Track::Kind::kAudio;
  const arr::TrackId track_id = project.add_track(track);
  arr::EditClip clip;
  clip.track_id = track_id;
  clip.source_id = source_a;
  clip.length_ppq = 10.0;
  const arr::ClipId clip_id = project.add_clip(clip);
  if (clip_out != nullptr) *clip_out = clip_id;
  if (source_a_out != nullptr) *source_a_out = source_a;
  if (source_b_out != nullptr) *source_b_out = source_b;
  return project;
}

arr::AssistSidecar vocal_sidecar(arr::ClipId clip_id, arr::TakeId take_id,
                                 std::vector<uint8_t> payload = {0x01, 0x02, 0x03}) {
  arr::AssistSidecar sidecar;
  sidecar.module_id = vocal::make_key({clip_id, take_id});
  sidecar.schema_version = vocal::kSchemaVersion;
  sidecar.payload = std::move(payload);
  return sidecar;
}

arr::AssistSidecar non_vocal_sidecar(std::string module_id, uint8_t value) {
  arr::AssistSidecar sidecar;
  sidecar.module_id = std::move(module_id);
  sidecar.schema_version = 7;
  sidecar.payload = {value};
  sidecar.target_track_id = 3;
  sidecar.region_start_ppq = 4.0;
  sidecar.region_end_ppq = 8.0;
  return sidecar;
}

arr::AssistSidecar malformed_vocal_prefix() {
  arr::AssistSidecar sidecar;
  sidecar.module_id = std::string(vocal::kPrefix) + "not-a-key";
  sidecar.schema_version = vocal::kSchemaVersion;
  sidecar.payload = {0xff};
  return sidecar;
}

void put_u16(std::vector<uint8_t>* bytes, size_t offset, uint16_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) (*bytes)[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

void put_u64(std::vector<uint8_t>* bytes, size_t offset, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) (*bytes)[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

std::vector<uint8_t> valid_payload(arr::SourceId original, arr::SourceId derived) {
  std::vector<uint8_t> payload(116, 0);
  payload[0] = 'S';
  payload[1] = 'V';
  payload[2] = 'P';
  payload[3] = '1';
  put_u16(&payload, 4, 1);
  put_u16(&payload, 6, 1);
  put_u32(&payload, 8, original);
  put_u32(&payload, 12, derived);
  put_u32(&payload, 16, 48000);
  put_u32(&payload, 20, 11);
  put_u64(&payload, 24, 1);
  put_u64(&payload, 32, 9);
  put_u64(&payload, 104, 4);
  payload[112] = 0x53;
  payload[113] = 0x56;
  payload[114] = 0x45;
  payload[115] = 0x31;
  return payload;
}

bool same_sidecars(const std::vector<arr::AssistSidecar>& lhs,
                   const std::vector<arr::AssistSidecar>& rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].module_id != rhs[i].module_id || lhs[i].schema_version != rhs[i].schema_version ||
        lhs[i].payload != rhs[i].payload || lhs[i].target_track_id != rhs[i].target_track_id ||
        lhs[i].region_start_ppq != rhs[i].region_start_ppq ||
        lhs[i].region_end_ppq != rhs[i].region_end_ppq) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("vocal sidecar keys are strict canonical decimal identities", "[arrangement][vocal]") {
  const auto parsed = vocal::parse_key("libsonare.vocal-edit/clip/7/take/0");
  REQUIRE(parsed.has_value());
  CHECK(parsed->clip_id == 7);
  CHECK(parsed->take_id == 0);
  CHECK(vocal::make_key({7, 0}) == "libsonare.vocal-edit/clip/7/take/0");

  CHECK_FALSE(vocal::parse_key("libsonare.vocal-edit/clip/07/take/0").has_value());
  CHECK_FALSE(vocal::parse_key("libsonare.vocal-edit/clip/7/take/00").has_value());
  CHECK_FALSE(vocal::parse_key("libsonare.vocal-edit/clip/0/take/0").has_value());
  CHECK_FALSE(vocal::parse_key("libsonare.vocal-edit/clip/7/take/0/suffix").has_value());
}

TEST_CASE("vocal envelope round-trips and rejects trailing or unsupported bytes",
          "[arrangement][vocal]") {
  arr::vocal_sidecar::Envelope expected;
  expected.original_source_id = 3;
  expected.derived_source_id = 4;
  expected.source_sample_rate = 44100;
  expected.profile_id = 17;
  expected.source_sample_count = 1234;
  expected.committed_revision = 99;
  expected.original_digest[0] = 0xa1;
  expected.derived_digest[31] = 0xb2;
  expected.sve1 = {0x53, 0x56, 0x45, 0x31, 0x01};
  const arr::AssistSidecar encoded = vocal::encode_envelope({8, 2}, expected);
  CHECK(encoded.module_id == vocal::make_key({8, 2}));
  CHECK(encoded.schema_version == vocal::kSchemaVersion);

  arr::vocal_sidecar::Envelope decoded;
  REQUIRE(vocal::decode_envelope(encoded, &decoded));
  CHECK(decoded.original_source_id == expected.original_source_id);
  CHECK(decoded.derived_source_id == expected.derived_source_id);
  CHECK(decoded.source_sample_rate == expected.source_sample_rate);
  CHECK(decoded.profile_id == expected.profile_id);
  CHECK(decoded.source_sample_count == expected.source_sample_count);
  CHECK(decoded.committed_revision == expected.committed_revision);
  CHECK(decoded.original_digest == expected.original_digest);
  CHECK(decoded.derived_digest == expected.derived_digest);
  CHECK(decoded.sve1 == expected.sve1);

  arr::AssistSidecar trailing = encoded;
  trailing.payload.push_back(0x00);
  arr::vocal_sidecar::Envelope untouched = expected;
  untouched.profile_id = 123;
  CHECK_FALSE(vocal::decode_envelope(trailing, &untouched));
  CHECK(untouched.profile_id == 123);
}

TEST_CASE("lifecycle only changes parseable vocal keys and preserves non-vocal order",
          "[arrangement][vocal]") {
  arr::ClipId clip = 0;
  arr::Project project = make_audio_project(&clip);
  const arr::AssistSidecar first = non_vocal_sidecar("other/first", 1);
  const arr::AssistSidecar base = vocal_sidecar(clip, 0, {1, 2});
  const arr::AssistSidecar middle = non_vocal_sidecar("other/middle", 2);
  const arr::AssistSidecar take = vocal_sidecar(clip, 4, {3, 4});
  const arr::AssistSidecar malformed = malformed_vocal_prefix();
  const arr::AssistSidecar last = non_vocal_sidecar("other/last", 3);
  project.add_assist_sidecar(first);
  project.add_assist_sidecar(base);
  project.add_assist_sidecar(middle);
  project.add_assist_sidecar(take);
  project.add_assist_sidecar(malformed);
  project.add_assist_sidecar(last);
  const std::vector<arr::AssistSidecar> before = project.assist_sidecars();

  vocal::clone_clip_sidecars(&project, clip, clip + 1);
  REQUIRE(project.assist_sidecars().size() == 8);
  CHECK(project.assist_sidecars()[6].module_id == vocal::make_key({clip + 1, 0}));
  CHECK(project.assist_sidecars()[7].module_id == vocal::make_key({clip + 1, 4}));
  CHECK(project.assist_sidecars()[6].payload == base.payload);
  CHECK(project.assist_sidecars()[7].payload == take.payload);

  vocal::remove_clip_sidecars(&project, clip);
  REQUIRE(project.assist_sidecars().size() == 6);
  CHECK(project.assist_sidecars()[0].module_id == first.module_id);
  CHECK(project.assist_sidecars()[1].module_id == middle.module_id);
  CHECK(project.assist_sidecars()[2].module_id == malformed.module_id);
  CHECK(project.assist_sidecars()[3].module_id == last.module_id);
  CHECK(project.assist_sidecars()[4].module_id == vocal::make_key({clip + 1, 0}));
  CHECK(project.assist_sidecars()[5].module_id == vocal::make_key({clip + 1, 4}));
  CHECK_FALSE(same_sidecars(project.assist_sidecars(), before));
}

TEST_CASE("remove clip sidecars are exact across undo and redo", "[arrangement][vocal]") {
  arr::ClipId clip = 0;
  arr::Project project = make_audio_project(&clip);
  project.add_assist_sidecar(non_vocal_sidecar("other/a", 1));
  project.add_assist_sidecar(vocal_sidecar(clip, 0));
  project.add_assist_sidecar(vocal_sidecar(clip, 1));
  project.add_assist_sidecar(non_vocal_sidecar("other/b", 2));
  const auto before = project.assist_sidecars();

  arr::EditHistory history{std::move(project)};
  REQUIRE(history.apply(std::make_unique<arr::RemoveClip>(clip)));
  REQUIRE(history.project().assist_sidecars().size() == 2);
  const auto after = history.project().assist_sidecars();
  REQUIRE(history.undo());
  CHECK(same_sidecars(history.project().assist_sidecars(), before));
  REQUIRE(history.redo());
  CHECK(same_sidecars(history.project().assist_sidecars(), after));
}

TEST_CASE("duplicate and split clone vocal payloads byte-for-byte", "[arrangement][vocal]") {
  arr::ClipId clip = 0;
  arr::Project project = make_audio_project(&clip);
  project.set_overlap_policy(arr::OverlapPolicy::kAllow);
  project.add_assist_sidecar(non_vocal_sidecar("other/a", 1));
  project.add_assist_sidecar(vocal_sidecar(clip, 0, {0x10, 0x11}));
  project.add_assist_sidecar(vocal_sidecar(clip + 50, 0, {0x30}));
  project.add_assist_sidecar(vocal_sidecar(clip, 9, {0x20, 0x21}));
  project.add_assist_sidecar(non_vocal_sidecar("other/b", 2));
  const auto before_duplicate = project.assist_sidecars();
  arr::EditHistory history{std::move(project)};

  REQUIRE(history.apply(std::make_unique<arr::DuplicateClip>(clip, 20.0)));
  const auto after_duplicate = history.project().assist_sidecars();
  arr::ClipId duplicate = 0;
  for (const arr::EditClip& item : history.project().clips()) {
    if (item.id != clip) duplicate = item.id;
  }
  REQUIRE(duplicate != 0);
  CHECK(std::count_if(history.project().assist_sidecars().begin(),
                      history.project().assist_sidecars().end(),
                      [duplicate](const arr::AssistSidecar& sidecar) {
                        const auto key = vocal::parse_key(sidecar.module_id);
                        return key.has_value() && key->clip_id == duplicate;
                      }) == 2);
  REQUIRE(history.undo());
  CHECK_FALSE(history.project().has_clip(duplicate));
  CHECK(same_sidecars(history.project().assist_sidecars(), before_duplicate));
  REQUIRE(history.redo());
  CHECK(history.project().has_clip(duplicate));
  CHECK(same_sidecars(history.project().assist_sidecars(), after_duplicate));

  const auto before_split = history.project().assist_sidecars();
  REQUIRE(history.apply(std::make_unique<arr::SplitClip>(clip, 5.0)));
  const auto after_split = history.project().assist_sidecars();
  arr::ClipId right = 0;
  for (const arr::EditClip& item : history.project().clips()) {
    if (item.id != clip && item.start_ppq == 5.0) right = item.id;
  }
  REQUIRE(right != 0);
  CHECK(std::count_if(history.project().assist_sidecars().begin(),
                      history.project().assist_sidecars().end(),
                      [right](const arr::AssistSidecar& sidecar) {
                        const auto key = vocal::parse_key(sidecar.module_id);
                        return key.has_value() && key->clip_id == right;
                      }) == 2);
  REQUIRE(history.undo());
  CHECK_FALSE(history.project().has_clip(right));
  CHECK(same_sidecars(history.project().assist_sidecars(), before_split));
  REQUIRE(history.redo());
  CHECK(history.project().has_clip(right));
  CHECK(same_sidecars(history.project().assist_sidecars(), after_split));
}

TEST_CASE("source and take rebind prune only changed resolved bindings", "[arrangement][vocal]") {
  arr::ClipId clip = 0;
  arr::SourceId source_a = 0;
  arr::SourceId source_b = 0;
  arr::Project project = make_audio_project(&clip, &source_a, &source_b);
  arr::EditClip* model = project.find_clip_mutable(clip);
  REQUIRE(model != nullptr);
  model->takes = {{1, 0, 1.0, "inherited"}, {2, source_b, 2.0, "explicit"}};
  project.add_assist_sidecar(vocal_sidecar(clip, 0));
  project.add_assist_sidecar(non_vocal_sidecar("other/a", 1));
  project.add_assist_sidecar(vocal_sidecar(clip, 1));
  project.add_assist_sidecar(vocal_sidecar(clip, 2));
  const auto before = project.assist_sidecars();
  arr::EditHistory history{std::move(project)};

  REQUIRE(history.apply(std::make_unique<arr::SetClipSource>(clip, source_b)));
  const auto after_source = history.project().assist_sidecars();
  CHECK_FALSE(std::any_of(history.project().assist_sidecars().begin(),
                          history.project().assist_sidecars().end(),
                          [clip](const arr::AssistSidecar& sidecar) {
                            const auto key = vocal::parse_key(sidecar.module_id);
                            return key.has_value() && key->clip_id == clip && key->take_id <= 1;
                          }));
  CHECK(std::any_of(history.project().assist_sidecars().begin(),
                    history.project().assist_sidecars().end(),
                    [clip](const arr::AssistSidecar& sidecar) {
                      const auto key = vocal::parse_key(sidecar.module_id);
                      return key.has_value() && key->clip_id == clip && key->take_id == 2;
                    }));
  REQUIRE(history.undo());
  CHECK(same_sidecars(history.project().assist_sidecars(), before));
  REQUIRE(history.redo());
  CHECK(same_sidecars(history.project().assist_sidecars(), after_source));
  REQUIRE(history.undo());
  CHECK(same_sidecars(history.project().assist_sidecars(), before));

  std::vector<arr::ClipTake> next_takes = {{2, source_b, 3.0, "renamed"}};
  REQUIRE(history.apply(std::make_unique<arr::SetClipTakes>(clip, next_takes, 2)));
  const auto after_takes = history.project().assist_sidecars();
  CHECK_FALSE(std::any_of(history.project().assist_sidecars().begin(),
                          history.project().assist_sidecars().end(),
                          [clip](const arr::AssistSidecar& sidecar) {
                            const auto key = vocal::parse_key(sidecar.module_id);
                            return key.has_value() && key->clip_id == clip && key->take_id == 1;
                          }));
  // Take 2 keeps its source and only slips its offset, so its sidecar survives.
  CHECK(std::any_of(history.project().assist_sidecars().begin(),
                    history.project().assist_sidecars().end(),
                    [clip](const arr::AssistSidecar& sidecar) {
                      const auto key = vocal::parse_key(sidecar.module_id);
                      return key.has_value() && key->clip_id == clip && key->take_id == 2;
                    }));
  CHECK(std::any_of(history.project().assist_sidecars().begin(),
                    history.project().assist_sidecars().end(),
                    [clip](const arr::AssistSidecar& sidecar) {
                      const auto key = vocal::parse_key(sidecar.module_id);
                      return key.has_value() && key->clip_id == clip && key->take_id == 0;
                    }));
  REQUIRE(history.undo());
  CHECK(same_sidecars(history.project().assist_sidecars(), before));
  REQUIRE(history.redo());
  CHECK(same_sidecars(history.project().assist_sidecars(), after_takes));
}

TEST_CASE("source-offset changes keep the whole-source vocal sidecar",
          "[arrangement][vocal][vocal_offset]") {
  struct Row {
    const char* name;
    arr::TakeId take_id;
  };
  const Row rows[] = {{"trim", 0}, {"split", 0}, {"take slip", 1}};
  for (const Row& row : rows) {
    DYNAMIC_SECTION(row.name) {
      arr::ClipId clip = 0;
      arr::SourceId source_a = 0;
      arr::SourceId source_b = 0;
      arr::Project project = make_audio_project(&clip, &source_a, &source_b);
      project.find_clip_mutable(clip)->takes = {{1, source_b, 0.0, "take"}};
      project.add_assist_sidecar(vocal_sidecar(clip, row.take_id, {0x41, 0x42}));
      const auto before = project.assist_sidecars();
      arr::EditHistory history{std::move(project)};
      const double offset_before =
          row.take_id == 0 ? history.project().find_clip(clip)->source_offset_ppq
                           : history.project().find_clip(clip)->takes[0].source_offset_ppq;

      arr::EditCommandPtr command;
      if (std::string(row.name) == "trim") {
        command = std::make_unique<arr::TrimClip>(clip, 2.0, 8.0);
      } else if (std::string(row.name) == "split") {
        command = std::make_unique<arr::SplitClip>(clip, 4.0);
      } else {
        command = std::make_unique<arr::SetClipTakes>(
            clip, std::vector<arr::ClipTake>{{1, source_b, 1.5, "take"}}, 1);
      }
      REQUIRE(history.apply(std::move(command)));
      const arr::EditClip* edited = history.project().find_clip(clip);
      REQUIRE(edited != nullptr);
      const double offset_after =
          row.take_id == 0 ? edited->source_offset_ppq : edited->takes[0].source_offset_ppq;
      // Split moves the offset of the right half; the others move this clip's.
      if (std::string(row.name) != "split") CHECK(offset_after != offset_before);
      const auto key = vocal::make_key({clip, row.take_id});
      CHECK(std::any_of(
          history.project().assist_sidecars().begin(), history.project().assist_sidecars().end(),
          [&](const arr::AssistSidecar& sidecar) {
            return sidecar.module_id == key && sidecar.payload == std::vector<uint8_t>{0x41, 0x42};
          }));
      const auto after = history.project().assist_sidecars();
      REQUIRE(history.undo());
      CHECK(same_sidecars(history.project().assist_sidecars(), before));
      REQUIRE(history.redo());
      CHECK(same_sidecars(history.project().assist_sidecars(), after));
    }
  }
}

TEST_CASE("removing a track removes every clip vocal key and undo restores it",
          "[arrangement][vocal]") {
  arr::ClipId clip = 0;
  arr::Project project = make_audio_project(&clip);
  arr::EditClip second = *project.find_clip(clip);
  second.start_ppq = 20.0;
  const arr::ClipId second_clip = project.add_clip(second);
  REQUIRE(second_clip != 0);
  const arr::TrackId track_id = project.find_clip(clip)->track_id;
  project.add_assist_sidecar(vocal_sidecar(clip, 0));
  project.add_assist_sidecar(vocal_sidecar(second_clip, 0));
  project.add_assist_sidecar(non_vocal_sidecar("other/track", 9));
  const auto before = project.assist_sidecars();
  arr::EditHistory history{std::move(project)};

  REQUIRE(history.apply(std::make_unique<arr::RemoveTrack>(track_id)));
  REQUIRE(history.project().assist_sidecars().size() == 1);
  const auto after = history.project().assist_sidecars();
  REQUIRE(history.undo());
  CHECK(same_sidecars(history.project().assist_sidecars(), before));
  REQUIRE(history.redo());
  CHECK(same_sidecars(history.project().assist_sidecars(), after));
}

TEST_CASE("source collection includes valid sidecar references and blocks malformed data",
          "[arrangement][vocal]") {
  arr::ClipId clip = 0;
  arr::SourceId original = 0;
  arr::SourceId derived = 0;
  arr::Project project = make_audio_project(&clip, &original, &derived);
  arr::AssistSidecar valid = vocal_sidecar(clip, 0, valid_payload(original, derived));
  project.assist_sidecars_mutable().push_back(valid);

  const std::vector<arr::SourceId> orphaned =
      vocal::collect_orphaned_sources_after_removing_clips(project, {clip});
  CHECK(orphaned == std::vector<arr::SourceId>{original, derived});

  arr::EditClip remaining = *project.find_clip(clip);
  remaining.start_ppq = 20.0;
  remaining.source_id = derived;
  const arr::ClipId remaining_clip = project.add_clip(remaining);
  REQUIRE(remaining_clip != 0);
  CHECK(vocal::collect_orphaned_sources_after_removing_clips(project, {clip}) ==
        std::vector<arr::SourceId>{original});

  project.assist_sidecars_mutable().push_back(malformed_vocal_prefix());
  CHECK(vocal::collect_orphaned_sources_after_removing_clips(project, {clip}).empty());
}

TEST_CASE("upsert uses module key identity and restores vocal anchors on undo",
          "[arrangement][vocal]") {
  arr::ClipId clip = 0;
  arr::Project project = make_audio_project(&clip);
  project.add_assist_sidecar(non_vocal_sidecar("other/before", 1));
  project.add_assist_sidecar(vocal_sidecar(clip, 2, {0x01}));
  project.add_assist_sidecar(non_vocal_sidecar("other/middle", 2));
  project.add_assist_sidecar(vocal_sidecar(clip, 2, {0x02}));
  project.add_assist_sidecar(vocal_sidecar(clip, 0, {0x03}));
  project.add_assist_sidecar(non_vocal_sidecar("other/after", 3));
  const auto before = project.assist_sidecars();

  arr::AssistSidecar canonical = vocal_sidecar(clip, 2, {0xaa, 0xbb});
  arr::EditHistory history{std::move(project)};
  REQUIRE(history.apply(vocal::make_upsert_command(canonical)));
  REQUIRE(history.project().assist_sidecars().size() == 5);
  const auto after = history.project().assist_sidecars();
  CHECK(history.project().assist_sidecars()[0].module_id == "other/before");
  CHECK(history.project().assist_sidecars()[1].module_id == "other/middle");
  CHECK(history.project().assist_sidecars()[2].module_id == vocal::make_key({clip, 0}));
  CHECK(history.project().assist_sidecars()[4].payload == canonical.payload);
  REQUIRE(history.undo());
  CHECK(same_sidecars(history.project().assist_sidecars(), before));
  REQUIRE(history.redo());
  CHECK(same_sidecars(history.project().assist_sidecars(), after));
}

TEST_CASE("wrapped inverses retain only the sidecars of the clips they affect",
          "[arrangement][vocal]") {
  auto inverse_bytes = [](size_t unrelated_clips) {
    arr::ClipId clip = 0;
    arr::Project project = make_audio_project(&clip);
    project.add_assist_sidecar(vocal_sidecar(clip, 0));
    for (size_t i = 0; i < unrelated_clips; ++i) {
      const arr::ClipId other = clip + 100 + static_cast<arr::ClipId>(i);
      project.add_assist_sidecar(vocal_sidecar(other, 0, std::vector<uint8_t>(256, 0x5a)));
    }
    const arr::MidiContentStore store;
    std::vector<size_t> bytes;
    for (const arr::EditCommandPtr& command :
         {arr::EditCommandPtr(std::make_unique<arr::RemoveClip>(clip)),
          arr::EditCommandPtr(vocal::make_upsert_command(vocal_sidecar(clip, 0, {0x7f})))}) {
      const arr::EditCommandPtr inverse = command->invert(project, store);
      REQUIRE(inverse != nullptr);
      bytes.push_back(inverse->retained_bytes());
    }
    return bytes;
  };
  CHECK(inverse_bytes(64) == inverse_bytes(0));
}
