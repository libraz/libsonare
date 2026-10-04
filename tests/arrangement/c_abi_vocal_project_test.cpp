#include <sonare/sonare_c_project.h>
#include <sonare/sonare_c_vocal_project.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "arrangement/vocal_edit_sidecar.h"
#include "c_api/project_internal.h"
#if defined(SONARE_WITH_MIXING)
#include "support/alloc_guard.h"
#endif
#include "util/constants.h"
#include "util/sha256.h"

namespace {

#if defined(SONARE_WITH_PITCH_EDITOR)
constexpr int kSampleRate = 16000;
constexpr int64_t kSampleCount = 3200;

std::array<uint8_t, 32> float_digest(const std::vector<float>& samples) {
  sonare::util::Sha256 sha;
  sha.update(reinterpret_cast<const uint8_t*>(samples.data()), samples.size() * sizeof(float));
  return sha.finalize();
}

std::string canonical_hash(const std::vector<float>& samples) {
  return "sha256:" + sonare::util::sha256_hex(reinterpret_cast<const uint8_t*>(samples.data()),
                                              samples.size() * sizeof(float));
}

struct VocalStateFixture {
  std::vector<float> source;
  std::vector<float> rendered;
  std::vector<uint8_t> state;
  SonareVocalStateToken token{};

  explicit VocalStateFixture(bool nonidentity = false) : source(static_cast<size_t>(kSampleCount)) {
    for (int64_t i = 0; i < kSampleCount; ++i) {
      source[static_cast<size_t>(i)] = static_cast<float>(
          0.2 * std::sin(sonare::constants::kTwoPiD * sonare::constants::kA4Hz * i / kSampleRate));
    }

    std::array<float, 20> f0{};
    std::array<uint8_t, 20> voiced{};
    f0.fill(static_cast<float>(sonare::constants::kA4Hz));
    voiced.fill(1);
    SonareVocalAnalysis analysis;
    sonare_vocal_analysis_init(&analysis);
    analysis.samples_per_frame = 160.0;
    analysis.frame_length_samples = 320;
    analysis.f0_hz = f0.data();
    analysis.voiced = voiced.data();
    analysis.frame_count = f0.size();

    SonareVocalCreateOptions options;
    sonare_vocal_create_options_init(&options);
    options.analysis = &analysis;
    options.edge_fade_ms = 0.0;

    SonareVocalEditSession* raw_session = nullptr;
    if (sonare_vocal_session_create(source.data(), kSampleCount, 1, kSampleRate, &options,
                                    &raw_session) != SONARE_OK ||
        raw_session == nullptr) {
      throw std::runtime_error("could not create vocal state fixture");
    }
    std::unique_ptr<SonareVocalEditSession, decltype(&sonare_vocal_session_destroy)> session(
        raw_session, sonare_vocal_session_destroy);
    if (nonidentity) {
      SonareVocalNotesResult notes;
      sonare_vocal_notes_result_init(&notes);
      if (sonare_vocal_session_notes(session.get(), &notes) != SONARE_OK || notes.note_count == 0) {
        sonare_vocal_free_notes(&notes);
        throw std::runtime_error("could not read vocal notes for edited fixture");
      }
      SonareVocalEditDraft* raw_draft = nullptr;
      if (sonare_vocal_session_begin_edit(session.get(), 0, &raw_draft) != SONARE_OK ||
          raw_draft == nullptr) {
        sonare_vocal_free_notes(&notes);
        throw std::runtime_error("could not begin edited fixture");
      }
      std::unique_ptr<SonareVocalEditDraft, decltype(&sonare_vocal_draft_destroy)> draft(
          raw_draft, sonare_vocal_draft_destroy);
      SonareVocalStateToken draft_token{};
      SonareVocalOperation operation;
      sonare_vocal_operation_init(&operation);
      operation.note_id = notes.notes[0].id;
      operation.edit = notes.notes[0].edit;
      operation.edit.gain_db = -6.0;
      operation.kind = SONARE_VOCAL_SET_EDIT;
      SonareVocalEditResult edit_result;
      sonare_vocal_edit_result_init(&edit_result);
      if (sonare_vocal_draft_token(draft.get(), &draft_token) != SONARE_OK ||
          sonare_vocal_draft_apply(draft.get(), draft_token.generation, &operation, 1,
                                   &edit_result) != SONARE_OK) {
        sonare_vocal_free_edit_result(&edit_result);
        sonare_vocal_free_notes(&notes);
        throw std::runtime_error("could not apply edited fixture");
      }
      sonare_vocal_free_edit_result(&edit_result);
      if (sonare_vocal_draft_commit(draft.get(), 0, &edit_result) != SONARE_OK) {
        sonare_vocal_free_edit_result(&edit_result);
        sonare_vocal_free_notes(&notes);
        throw std::runtime_error("could not commit edited fixture");
      }
      sonare_vocal_free_edit_result(&edit_result);
      sonare_vocal_free_notes(&notes);
    }
    if (sonare_vocal_session_token(session.get(), &token) != SONARE_OK) {
      throw std::runtime_error("could not read vocal state token");
    }

    SonareVocalStateBytes bytes;
    sonare_vocal_state_bytes_init(&bytes);
    if (sonare_vocal_session_export_state(session.get(), &bytes) != SONARE_OK ||
        bytes.data == nullptr) {
      throw std::runtime_error("could not export vocal state");
    }
    state.assign(bytes.data, bytes.data + bytes.size);
    sonare_vocal_free_state_bytes(&bytes);

    SonareVocalRenderSnapshot* raw_snapshot = nullptr;
    if (sonare_vocal_session_capture_snapshot(session.get(), &raw_snapshot) != SONARE_OK ||
        raw_snapshot == nullptr) {
      throw std::runtime_error("could not capture vocal snapshot");
    }
    std::unique_ptr<SonareVocalRenderSnapshot, decltype(&sonare_vocal_snapshot_destroy)> snapshot(
        raw_snapshot, sonare_vocal_snapshot_destroy);
    SonareVocalRenderResult render;
    sonare_vocal_render_result_init(&render);
    if (sonare_vocal_snapshot_render(snapshot.get(), {0, kSampleCount}, 1, nullptr, nullptr,
                                     &render) != SONARE_OK ||
        render.samples == nullptr || render.sample_count != kSampleCount) {
      sonare_vocal_free_render_result(&render);
      throw std::runtime_error("could not render vocal snapshot");
    }
    rendered.assign(render.samples, render.samples + render.sample_count);
    sonare_vocal_free_render_result(&render);
  }
};

struct ProjectFixture {
  SonareProject* project = nullptr;
  uint32_t track_id = 0;
  uint32_t clip_id = 0;
  uint32_t source_id = 0;
  VocalStateFixture vocal;

  explicit ProjectFixture(bool nonidentity = false) : vocal(nonidentity) {
    if (sonare_project_create(&project) != SONARE_OK || project == nullptr) {
      throw std::runtime_error("could not create project fixture");
    }
    SonareProjectTrackDesc track_desc{SONARE_TRACK_AUDIO, "lead vocal"};
    if (sonare_project_add_track(project, &track_desc, &track_id) != SONARE_OK) {
      sonare_project_destroy(project);
      project = nullptr;
      throw std::runtime_error("could not create audio track fixture");
    }
    SonareProjectClipDesc clip_desc{};
    clip_desc.track_id = track_id;
    clip_desc.start_ppq = 0.0;
    clip_desc.length_ppq = 1.0;
    clip_desc.source_offset_ppq = 0.0;
    clip_desc.gain = 1.0f;
    clip_desc.audio_interleaved = vocal.source.data();
    clip_desc.audio_frames = kSampleCount;
    clip_desc.audio_channels = 1;
    clip_desc.audio_sample_rate = kSampleRate;
    if (sonare_project_add_clip(project, &clip_desc, &clip_id) != SONARE_OK) {
      sonare_project_destroy(project);
      project = nullptr;
      throw std::runtime_error("could not create audio clip fixture");
    }
    source_id = project->history.project().find_clip(clip_id)->source_id;
    const std::string hash = canonical_hash(vocal.source);
    if (sonare_project_set_audio_source_metadata(project, source_id, hash.c_str(), "") !=
        SONARE_OK) {
      sonare_project_destroy(project);
      project = nullptr;
      throw std::runtime_error("could not set source digest fixture");
    }
  }

  ~ProjectFixture() {
    if (project != nullptr) sonare_project_destroy(project);
  }

  SonareProjectVocalEditApplyDesc apply_desc() const {
    SonareProjectVocalEditApplyDesc desc;
    sonare_project_vocal_edit_apply_desc_init(&desc);
    desc.clip_id = clip_id;
    desc.take_id = 0;
    desc.expected_source_id = source_id;
    desc.expected_source_sample_rate = kSampleRate;
    desc.expected_source_sample_count = kSampleCount;
    const auto digest = float_digest(vocal.source);
    std::memcpy(desc.expected_source_sha256, digest.data(), digest.size());
    desc.expected_clip_length_ppq = 1.0;
    desc.expected_source_offset_ppq = 0.0;
    desc.rendered_mono = vocal.rendered.data();
    desc.rendered_sample_count = kSampleCount;
    desc.rendered_sample_rate = kSampleRate;
    desc.rendered_start_sample = 0;
    desc.render_token = vocal.token;
    desc.sve1 = vocal.state.data();
    desc.sve1_size = vocal.state.size();
    return desc;
  }
};

#if defined(SONARE_WITH_MIXING)
std::string serialize_project_copy(const SonareProject* project) {
  char* json = nullptr;
  size_t json_size = 0;
  if (sonare_project_serialize(project, &json, &json_size) != SONARE_OK || json == nullptr) {
    throw std::runtime_error("could not serialize project snapshot");
  }
  std::string copy(json, json_size);
  sonare_free_string(json);
  return copy;
}

#endif

struct CancelAfterFirstCall {
  int calls = 0;
};

int cancel_after_first_call(void* user_data) {
  auto* state = static_cast<CancelAfterFirstCall*>(user_data);
  return ++state->calls > 1 ? 1 : 0;
}

#endif
}  // namespace

TEST_CASE("vocal project dependency API has initialized output ownership", "[project][vocal]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectVocalEditDependenciesResult result;
  sonare_project_vocal_edit_dependencies_result_init(&result);
  REQUIRE(result.struct_size == sizeof(result));
  REQUIRE(result.schema_version == 1u);
  REQUIRE(sonare_project_get_vocal_edit_dependencies(project, &result) == SONARE_OK);
  CHECK(result.dependencies == nullptr);
  CHECK(result.dependency_count == 0);
  sonare_project_free_vocal_edit_dependencies(&result);
  CHECK(result.dependencies == nullptr);
  CHECK(result.dependency_count == 0);
  CHECK(result.struct_size == sizeof(result));
  CHECK(result.schema_version == 1u);
  sonare_project_destroy(project);
}

TEST_CASE("vocal project descriptor and rehydrate results require ABI initialization",
          "[project][vocal]") {
  SonareProjectVocalEditApplyDesc desc;
  sonare_project_vocal_edit_apply_desc_init(&desc);
  CHECK(desc.struct_size == sizeof(desc));
  CHECK(desc.schema_version == 1u);

  SonareProjectVocalEditApplyResult apply_result;
  sonare_project_vocal_edit_apply_result_init(&apply_result);
  CHECK(apply_result.struct_size == sizeof(apply_result));
  CHECK(apply_result.schema_version == 1u);

  SonareProjectVocalOriginalSource original;
  sonare_project_vocal_original_source_init(&original);
  CHECK(original.struct_size == sizeof(original));
  CHECK(original.schema_version == 1u);

  SonareProjectVocalRehydrateResult rehydrate_result;
  sonare_project_vocal_rehydrate_result_init(&rehydrate_result);
  CHECK(rehydrate_result.struct_size == sizeof(rehydrate_result));
  CHECK(rehydrate_result.schema_version == 1u);
  sonare_project_free_vocal_rehydrate_result(&rehydrate_result);
}

#if defined(SONARE_WITH_PITCH_EDITOR)
TEST_CASE("vocal project apply atomically binds derived PCM and undoes exactly",
          "[project][vocal][apply]") {
  ProjectFixture fixture;
  const SonareProjectVocalEditApplyDesc desc = fixture.apply_desc();
  SonareProjectVocalEditApplyResult result;
  sonare_project_vocal_edit_apply_result_init(&result);
  const size_t before_depth = fixture.project->history.undo_depth();
  REQUIRE(sonare_project_apply_vocal_edit(fixture.project, &desc, &result) == SONARE_OK);
  REQUIRE(result.original_source_id == fixture.source_id);
  REQUIRE(result.derived_source_id != 0);
  CHECK(fixture.project->history.undo_depth() == before_depth + 1);
  CHECK(fixture.project->history.project().find_clip(fixture.clip_id)->source_id ==
        result.derived_source_id);
  REQUIRE(fixture.project->audio.sources.find(result.derived_source_id) !=
          fixture.project->audio.sources.end());
  REQUIRE(fixture.project->audio.sources.at(result.derived_source_id).channels.size() == 1);
  CHECK(fixture.project->audio.sources.at(result.derived_source_id).channels[0] ==
        fixture.vocal.rendered);
  REQUIRE(fixture.project->history.project().assist_sidecars().size() == 1);
  CHECK(std::string(result.sidecar_key) ==
        "libsonare.vocal-edit/clip/" + std::to_string(fixture.clip_id) + "/take/0");

  REQUIRE(sonare_project_undo(fixture.project) == SONARE_OK);
  CHECK(fixture.project->history.project().find_clip(fixture.clip_id)->source_id ==
        fixture.source_id);
  CHECK(fixture.project->audio.sources.find(result.derived_source_id) ==
        fixture.project->audio.sources.end());
  CHECK(fixture.project->history.project().assist_sidecars().empty());
  REQUIRE(sonare_project_redo(fixture.project) == SONARE_OK);
  CHECK(fixture.project->history.project().find_clip(fixture.clip_id)->source_id ==
        result.derived_source_id);
  CHECK(fixture.project->audio.sources.find(result.derived_source_id) !=
        fixture.project->audio.sources.end());
  CHECK(fixture.project->history.project().assist_sidecars().size() == 1);
}

TEST_CASE("vocal project Apply materializes inherited takes when rebinding the base",
          "[project][vocal][apply]") {
  ProjectFixture fixture;
  auto* clip = fixture.project->history.project().find_clip_mutable(fixture.clip_id);
  REQUIRE(clip != nullptr);
  clip->takes.push_back({1, 0, 0.0, "inherited"});
  const SonareProjectVocalEditApplyDesc desc = fixture.apply_desc();
  SonareProjectVocalEditApplyResult result;
  sonare_project_vocal_edit_apply_result_init(&result);
  REQUIRE(sonare_project_apply_vocal_edit(fixture.project, &desc, &result) == SONARE_OK);
  const auto* updated = fixture.project->history.project().find_clip(fixture.clip_id);
  REQUIRE(updated != nullptr);
  REQUIRE(updated->takes.size() == 1);
  CHECK(updated->takes[0].source_id == fixture.source_id);
  CHECK(updated->source_id == result.derived_source_id);
}

TEST_CASE("vocal project persistence reports dependencies and rehydrates derived PCM",
          "[project][vocal][rehydrate]") {
  ProjectFixture fixture(true);
  REQUIRE(fixture.vocal.rendered != fixture.vocal.source);
  const SonareProjectVocalEditApplyDesc desc = fixture.apply_desc();
  SonareProjectVocalEditApplyResult applied;
  sonare_project_vocal_edit_apply_result_init(&applied);
  REQUIRE(sonare_project_apply_vocal_edit(fixture.project, &desc, &applied) == SONARE_OK);

  char* json = nullptr;
  size_t json_size = 0;
  REQUIRE(sonare_project_serialize(fixture.project, &json, &json_size) == SONARE_OK);
  REQUIRE(json != nullptr);
  SonareProject* loaded = nullptr;
  char* diagnostics = nullptr;
  REQUIRE(sonare_project_deserialize(json, json_size, &loaded, &diagnostics) == SONARE_OK);
  sonare_free_string(json);
  sonare_free_string(diagnostics);
  REQUIRE(loaded != nullptr);

  SonareProjectVocalEditDependenciesResult dependencies;
  sonare_project_vocal_edit_dependencies_result_init(&dependencies);
  REQUIRE(sonare_project_get_vocal_edit_dependencies(loaded, &dependencies) == SONARE_OK);
  REQUIRE(dependencies.dependency_count == 1);
  CHECK(dependencies.dependencies[0].clip_id == fixture.clip_id);
  CHECK(dependencies.dependencies[0].take_id == 0);
  CHECK(dependencies.dependencies[0].original_pcm_available == 0);
  CHECK(dependencies.dependencies[0].derived_pcm_available == 0);
  sonare_project_free_vocal_edit_dependencies(&dependencies);

  SonareProjectVocalOriginalSource original;
  sonare_project_vocal_original_source_init(&original);
  original.source_id = fixture.source_id;
  original.mono = fixture.vocal.source.data();
  original.sample_count = kSampleCount;
  original.sample_rate = kSampleRate;
  SonareProjectVocalRehydrateResult rehydrated;
  sonare_project_vocal_rehydrate_result_init(&rehydrated);
  REQUIRE(sonare_project_rehydrate_vocal_edits(loaded, &original, 1, nullptr, nullptr,
                                               &rehydrated) == SONARE_OK);
  REQUIRE(rehydrated.item_count == 1);
  CHECK(rehydrated.items[0].status == SONARE_VOCAL_REHYDRATE_REHYDRATED);
  CHECK(rehydrated.items[0].reason == SONARE_VOCAL_REASON_NONE);
  REQUIRE(loaded->audio.sources.find(applied.derived_source_id) != loaded->audio.sources.end());
  CHECK(loaded->audio.sources.at(applied.derived_source_id).channels[0] == fixture.vocal.rendered);
  sonare_project_free_vocal_rehydrate_result(&rehydrated);
  sonare_project_destroy(loaded);
}

TEST_CASE("vocal project accepts empty and opaque original metadata across persistence",
          "[project][vocal][rehydrate]") {
  for (const char* content_hash : {"", "host:opaque-source-id"}) {
    DYNAMIC_SECTION("original content hash: " << content_hash) {
      ProjectFixture fixture(true);
      REQUIRE(sonare_project_set_audio_source_metadata(fixture.project, fixture.source_id,
                                                       content_hash, "") == SONARE_OK);
      const SonareProjectVocalEditApplyDesc desc = fixture.apply_desc();
      SonareProjectVocalEditApplyResult applied;
      sonare_project_vocal_edit_apply_result_init(&applied);
      REQUIRE(sonare_project_apply_vocal_edit(fixture.project, &desc, &applied) == SONARE_OK);

      char* json = nullptr;
      size_t json_size = 0;
      REQUIRE(sonare_project_serialize(fixture.project, &json, &json_size) == SONARE_OK);
      REQUIRE(json != nullptr);
      SonareProject* loaded = nullptr;
      char* diagnostics = nullptr;
      REQUIRE(sonare_project_deserialize(json, json_size, &loaded, &diagnostics) == SONARE_OK);
      sonare_free_string(json);
      sonare_free_string(diagnostics);
      REQUIRE(loaded != nullptr);

      SonareProjectVocalEditDependenciesResult dependencies;
      sonare_project_vocal_edit_dependencies_result_init(&dependencies);
      REQUIRE(sonare_project_get_vocal_edit_dependencies(loaded, &dependencies) == SONARE_OK);
      REQUIRE(dependencies.dependency_count == 1);
      CHECK(dependencies.dependencies[0].reason == SONARE_VOCAL_REASON_NONE);
      CHECK(dependencies.dependencies[0].original_pcm_available == 0);
      CHECK(dependencies.dependencies[0].derived_pcm_available == 0);
      sonare_project_free_vocal_edit_dependencies(&dependencies);

      SonareProjectVocalOriginalSource original;
      sonare_project_vocal_original_source_init(&original);
      original.source_id = fixture.source_id;
      original.mono = fixture.vocal.source.data();
      original.sample_count = kSampleCount;
      original.sample_rate = kSampleRate;
      SonareProjectVocalRehydrateResult rehydrated;
      sonare_project_vocal_rehydrate_result_init(&rehydrated);
      REQUIRE(sonare_project_rehydrate_vocal_edits(loaded, &original, 1, nullptr, nullptr,
                                                   &rehydrated) == SONARE_OK);
      REQUIRE(rehydrated.item_count == 1);
      CHECK(rehydrated.items[0].status == SONARE_VOCAL_REHYDRATE_REHYDRATED);
      CHECK(rehydrated.items[0].reason == SONARE_VOCAL_REASON_NONE);
      REQUIRE(loaded->audio.sources.find(applied.derived_source_id) != loaded->audio.sources.end());
      CHECK(loaded->audio.sources.at(applied.derived_source_id).channels[0] ==
            fixture.vocal.rendered);
      sonare_project_free_vocal_rehydrate_result(&rehydrated);
      sonare_project_destroy(loaded);
    }
  }
}

TEST_CASE("vocal project rejects mismatched derived source metadata during rehydrate",
          "[project][vocal][rehydrate]") {
  ProjectFixture fixture;
  const SonareProjectVocalEditApplyDesc desc = fixture.apply_desc();
  SonareProjectVocalEditApplyResult applied;
  sonare_project_vocal_edit_apply_result_init(&applied);
  REQUIRE(sonare_project_apply_vocal_edit(fixture.project, &desc, &applied) == SONARE_OK);
  REQUIRE(sonare_project_set_audio_source_metadata(fixture.project, applied.derived_source_id,
                                                   "host:tampered-derived", "") == SONARE_OK);
  REQUIRE(fixture.project->audio.sources.erase(applied.derived_source_id) == 1);

  SonareProjectVocalOriginalSource original;
  sonare_project_vocal_original_source_init(&original);
  original.source_id = fixture.source_id;
  original.mono = fixture.vocal.source.data();
  original.sample_count = kSampleCount;
  original.sample_rate = kSampleRate;
  SonareProjectVocalRehydrateResult result;
  sonare_project_vocal_rehydrate_result_init(&result);
  REQUIRE(sonare_project_rehydrate_vocal_edits(fixture.project, &original, 1, nullptr, nullptr,
                                               &result) == SONARE_OK);
  REQUIRE(result.item_count == 1);
  CHECK(result.items[0].status == SONARE_VOCAL_REHYDRATE_UNRESOLVED);
  CHECK(result.items[0].reason == SONARE_VOCAL_REASON_SOURCE_MISMATCH);
  CHECK(fixture.project->audio.sources.find(applied.derived_source_id) ==
        fixture.project->audio.sources.end());
  sonare_project_free_vocal_rehydrate_result(&result);
}

TEST_CASE("vocal project rejects a late PCM collision without partial mutation",
          "[project][vocal][apply]") {
  ProjectFixture fixture;
  const auto colliding_id = fixture.project->history.project().next_source_id();
  sonare::arrangement::AudioSourceSamples collision;
  collision.sample_rate = kSampleRate;
  collision.channels = {{0.25f}};
  fixture.project->audio.sources.emplace(colliding_id, std::move(collision));
  const size_t depth = fixture.project->history.undo_depth();
  const size_t sidecars = fixture.project->history.project().assist_sidecars().size();
  const SonareProjectVocalEditApplyDesc desc = fixture.apply_desc();
  SonareProjectVocalEditApplyResult result;
  sonare_project_vocal_edit_apply_result_init(&result);
  CHECK(sonare_project_apply_vocal_edit(fixture.project, &desc, &result) != SONARE_OK);
  CHECK(fixture.project->history.undo_depth() == depth);
  CHECK(fixture.project->history.project().assist_sidecars().size() == sidecars);
  CHECK(fixture.project->history.project().find_clip(fixture.clip_id)->source_id ==
        fixture.source_id);
  CHECK(fixture.project->audio.sources.at(colliding_id).channels[0] == std::vector<float>{0.25f});
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("vocal project Apply leaves no mutation when any allocation fails",
          "[project][vocal][apply][allocation]") {
  size_t out_of_memory_failures = 0;
  bool succeeded = false;
  for (size_t allocation = 1; allocation < 256; ++allocation) {
    ProjectFixture fixture;
    const SonareProjectVocalEditApplyDesc desc = fixture.apply_desc();
    const std::string before_json = serialize_project_copy(fixture.project);
    const size_t before_depth = fixture.project->history.undo_depth();
    const size_t before_source_count = fixture.project->audio.sources.size();
    SonareProjectVocalEditApplyResult result;
    sonare_project_vocal_edit_apply_result_init(&result);
    SonareError error;
    {
      sonare::test::AllocationFailureAtGuard guard(allocation);
      error = sonare_project_apply_vocal_edit(fixture.project, &desc, &result);
    }
    if (error == SONARE_ERROR_OUT_OF_MEMORY) {
      ++out_of_memory_failures;
      CHECK(serialize_project_copy(fixture.project) == before_json);
      CHECK(fixture.project->history.undo_depth() == before_depth);
      CHECK(fixture.project->audio.sources.size() == before_source_count);
      CHECK(fixture.project->history.project().find_clip(fixture.clip_id)->source_id ==
            fixture.source_id);
      CHECK(fixture.project->history.project().assist_sidecars().empty());
      CHECK(result.derived_source_id == 0);
      CHECK(result.sidecar_key[0] == '\0');
    } else {
      REQUIRE(error == SONARE_OK);
      succeeded = true;
      break;
    }
  }
  CHECK(out_of_memory_failures > 0);
  CHECK(succeeded);
}
#endif

TEST_CASE("vocal project rejects a mismatching existing original during rehydrate",
          "[project][vocal][rehydrate]") {
  ProjectFixture fixture;
  const SonareProjectVocalEditApplyDesc desc = fixture.apply_desc();
  SonareProjectVocalEditApplyResult applied;
  sonare_project_vocal_edit_apply_result_init(&applied);
  REQUIRE(sonare_project_apply_vocal_edit(fixture.project, &desc, &applied) == SONARE_OK);

  REQUIRE(fixture.project->audio.sources.erase(applied.derived_source_id) == 1);
  auto& existing_original = fixture.project->audio.sources.at(fixture.source_id).channels[0];
  REQUIRE(existing_original.size() == fixture.vocal.source.size());
  existing_original[1] += 0.125f;

  SonareProjectVocalOriginalSource original;
  sonare_project_vocal_original_source_init(&original);
  original.source_id = fixture.source_id;
  original.mono = fixture.vocal.source.data();
  original.sample_count = kSampleCount;
  original.sample_rate = kSampleRate;
  SonareProjectVocalRehydrateResult result;
  sonare_project_vocal_rehydrate_result_init(&result);
  REQUIRE(sonare_project_rehydrate_vocal_edits(fixture.project, &original, 1, nullptr, nullptr,
                                               &result) == SONARE_OK);
  REQUIRE(result.item_count == 1);
  CHECK(result.items[0].status == SONARE_VOCAL_REHYDRATE_UNRESOLVED);
  CHECK(result.items[0].reason == SONARE_VOCAL_REASON_SOURCE_MISMATCH);
  CHECK(fixture.project->audio.sources.find(applied.derived_source_id) ==
        fixture.project->audio.sources.end());
  sonare_project_free_vocal_rehydrate_result(&result);
}

TEST_CASE("vocal project cancellation publishes no staged PCM", "[project][vocal][rehydrate]") {
  ProjectFixture fixture;
  const SonareProjectVocalEditApplyDesc desc = fixture.apply_desc();
  SonareProjectVocalEditApplyResult applied;
  sonare_project_vocal_edit_apply_result_init(&applied);
  REQUIRE(sonare_project_apply_vocal_edit(fixture.project, &desc, &applied) == SONARE_OK);
  REQUIRE(fixture.project->audio.sources.erase(applied.derived_source_id) == 1);
  const size_t history_depth = fixture.project->history.undo_depth();

  SonareProjectVocalOriginalSource original;
  sonare_project_vocal_original_source_init(&original);
  original.source_id = fixture.source_id;
  original.mono = fixture.vocal.source.data();
  original.sample_count = kSampleCount;
  original.sample_rate = kSampleRate;
  CancelAfterFirstCall cancellation;
  SonareProjectVocalRehydrateResult result;
  sonare_project_vocal_rehydrate_result_init(&result);
  CHECK(sonare_project_rehydrate_vocal_edits(fixture.project, &original, 1, cancel_after_first_call,
                                             &cancellation, &result) == SONARE_ERROR_CANCELLED);
  CHECK(cancellation.calls == 2);
  CHECK(result.items == nullptr);
  CHECK(result.item_count == 0);
  CHECK(fixture.project->history.undo_depth() == history_depth);
  CHECK(fixture.project->audio.sources.find(applied.derived_source_id) ==
        fixture.project->audio.sources.end());
  sonare_project_free_vocal_rehydrate_result(&result);
}

TEST_CASE("vocal project does not publish original PCM for an invalid SVE1",
          "[project][vocal][rehydrate]") {
  ProjectFixture fixture;
  const SonareProjectVocalEditApplyDesc desc = fixture.apply_desc();
  SonareProjectVocalEditApplyResult applied;
  sonare_project_vocal_edit_apply_result_init(&applied);
  REQUIRE(sonare_project_apply_vocal_edit(fixture.project, &desc, &applied) == SONARE_OK);
  REQUIRE(fixture.project->audio.sources.erase(fixture.source_id) == 1);
  REQUIRE(fixture.project->audio.sources.erase(applied.derived_source_id) == 1);
  auto& payload = fixture.project->history.project().assist_sidecars_mutable()[0].payload;
  REQUIRE(payload.size() > 112);
  payload[112] = 'X';

  SonareProjectVocalOriginalSource original;
  sonare_project_vocal_original_source_init(&original);
  original.source_id = fixture.source_id;
  original.mono = fixture.vocal.source.data();
  original.sample_count = kSampleCount;
  original.sample_rate = kSampleRate;
  SonareProjectVocalRehydrateResult result;
  sonare_project_vocal_rehydrate_result_init(&result);
  REQUIRE(sonare_project_rehydrate_vocal_edits(fixture.project, &original, 1, nullptr, nullptr,
                                               &result) == SONARE_OK);
  REQUIRE(result.item_count == 1);
  CHECK(result.items[0].status == SONARE_VOCAL_REHYDRATE_UNRESOLVED);
  CHECK(result.items[0].reason == SONARE_VOCAL_REASON_INVALID_STATE);
  CHECK(fixture.project->audio.sources.find(fixture.source_id) ==
        fixture.project->audio.sources.end());
  CHECK(fixture.project->audio.sources.find(applied.derived_source_id) ==
        fixture.project->audio.sources.end());
  sonare_project_free_vocal_rehydrate_result(&result);
}

TEST_CASE("vocal project validates SVE1 before reporting an already-ready entry",
          "[project][vocal][rehydrate]") {
  ProjectFixture fixture;
  const SonareProjectVocalEditApplyDesc desc = fixture.apply_desc();
  SonareProjectVocalEditApplyResult applied;
  sonare_project_vocal_edit_apply_result_init(&applied);
  REQUIRE(sonare_project_apply_vocal_edit(fixture.project, &desc, &applied) == SONARE_OK);
  auto& payload = fixture.project->history.project().assist_sidecars_mutable()[0].payload;
  REQUIRE(payload.size() > 112);
  payload[112] = 'X';

  SonareProjectVocalOriginalSource original;
  sonare_project_vocal_original_source_init(&original);
  original.source_id = fixture.source_id;
  original.mono = fixture.vocal.source.data();
  original.sample_count = kSampleCount;
  original.sample_rate = kSampleRate;
  SonareProjectVocalRehydrateResult result;
  sonare_project_vocal_rehydrate_result_init(&result);
  REQUIRE(sonare_project_rehydrate_vocal_edits(fixture.project, &original, 1, nullptr, nullptr,
                                               &result) == SONARE_OK);
  REQUIRE(result.item_count == 1);
  CHECK(result.items[0].status == SONARE_VOCAL_REHYDRATE_UNRESOLVED);
  CHECK(result.items[0].reason == SONARE_VOCAL_REASON_INVALID_STATE);
  CHECK(fixture.project->audio.sources.at(applied.derived_source_id).channels[0] ==
        fixture.vocal.rendered);
  sonare_project_free_vocal_rehydrate_result(&result);
}

#endif

TEST_CASE("vocal project dependency enumeration reports malformed vocal payloads",
          "[project][vocal][dependencies]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  sonare::arrangement::AssistSidecar malformed;
  malformed.module_id = "libsonare.vocal-edit/clip/1/take/0";
  malformed.schema_version = 1;
  malformed.payload = {'S', 'V', 'P', '1', 1, 0, 1, 0};
  REQUIRE(
      project->history.apply(std::make_unique<sonare::arrangement::SetAssistSidecar>(malformed)));
  SonareProjectVocalEditDependenciesResult dependencies;
  sonare_project_vocal_edit_dependencies_result_init(&dependencies);
  REQUIRE(sonare_project_get_vocal_edit_dependencies(project, &dependencies) == SONARE_OK);
  REQUIRE(dependencies.dependency_count == 1);
  CHECK(dependencies.dependencies[0].reason == SONARE_VOCAL_REASON_INVALID_STATE);
  CHECK(std::string(dependencies.dependencies[0].sidecar_key) == malformed.module_id);
  sonare_project_free_vocal_edit_dependencies(&dependencies);
  sonare_project_destroy(project);
}

#if !defined(SONARE_WITH_PITCH_EDITOR)
TEST_CASE("vocal project preserves initialized feature-off outputs", "[project][vocal][stub]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  SonareProjectVocalEditApplyDesc desc;
  sonare_project_vocal_edit_apply_desc_init(&desc);
  SonareProjectVocalEditApplyResult applied;
  sonare_project_vocal_edit_apply_result_init(&applied);
  CHECK(sonare_project_apply_vocal_edit(project, &desc, &applied) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(applied.struct_size == sizeof(applied));
  CHECK(applied.schema_version == 1u);
  CHECK(applied.derived_source_id == 0);
  SonareProjectVocalRehydrateResult result;
  sonare_project_vocal_rehydrate_result_init(&result);
  CHECK(sonare_project_rehydrate_vocal_edits(project, nullptr, 0, nullptr, nullptr, &result) ==
        SONARE_ERROR_NOT_SUPPORTED);
  CHECK(result.item_count == 0);
  CHECK(result.items == nullptr);
  CHECK(result.struct_size == sizeof(result));
  sonare_project_destroy(project);
}
#endif
