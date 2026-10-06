#include <sonare/sonare_c_vocal_edit.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/alloc_guard.h"
#include "util/constants.h"

TEST_CASE("vocal C options preserve literal zero controls", "[vocal_c_api]") {
  SonareVocalCreateOptions options;
  sonare_vocal_create_options_init(&options);
  CHECK(options.struct_size == sizeof(options));
  CHECK(options.schema_version == SONARE_VOCAL_EDIT_API_VERSION);
  CHECK(options.edge_fade_ms == 5.0);
  SonareVocalNoteEdit edit;
  sonare_vocal_note_edit_init(&edit);
  CHECK(edit.amount == 0.0);
  CHECK(edit.speed_ms == 0.0);
  CHECK(edit.drift_scale == 1.0);
  CHECK(edit.vibrato_scale == 1.0);
  CHECK(edit.formant_mode == SONARE_VOCAL_FORMANT_PRESERVE);
}

#if defined(SONARE_WITH_PITCH_EDITOR)
TEST_CASE("vocal C session accepts authoritative supplied analysis", "[vocal_c_api]") {
  constexpr int kSampleRate = 16000;
  constexpr int kFrames = 1600;
  std::vector<float> samples(kFrames);
  for (int i = 0; i < kFrames; ++i) {
    samples[static_cast<size_t>(i)] = static_cast<float>(
        0.2 * std::sin(sonare::constants::kTwoPiD * sonare::constants::kA4Hz * i / kSampleRate));
  }
  std::array<float, 10> f0;
  f0.fill(sonare::constants::kA4Hz);
  std::array<uint8_t, 10> voiced;
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
  SonareVocalEditSession* session = nullptr;
  const auto error = sonare_vocal_session_create(samples.data(), samples.size(), 1, kSampleRate,
                                                 &options, &session);
  CHECK(error == SONARE_OK);
  CHECK(session != nullptr);
  sonare_vocal_session_destroy(session);
}

TEST_CASE("vocal C session rejects stereo and clears poisoned output", "[vocal_c_api]") {
  std::vector<float> samples(64, 0.0f);
  auto* session = reinterpret_cast<SonareVocalEditSession*>(static_cast<uintptr_t>(1));
  const auto error = sonare_vocal_session_create(samples.data(), 32, 2, 16000, nullptr, &session);
  CHECK(error == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(session == nullptr);
}

namespace {
struct VocalFixture {
  std::vector<float> samples = std::vector<float>(3200);
  std::array<float, 20> f0{};
  std::array<uint8_t, 20> voiced{};
  SonareVocalAnalysis analysis{};
  SonareVocalCreateOptions options{};
  VocalFixture() {
    for (size_t i = 0; i < samples.size(); ++i)
      samples[i] = static_cast<float>(0.2 * std::sin(sonare::constants::kTwoPiD * 440 * i / 16000));
    f0.fill(440);
    voiced.fill(1);
    sonare_vocal_analysis_init(&analysis);
    analysis.samples_per_frame = 160;
    analysis.frame_length_samples = 320;
    analysis.f0_hz = f0.data();
    analysis.voiced = voiced.data();
    analysis.frame_count = f0.size();
    sonare_vocal_create_options_init(&options);
    options.analysis = &analysis;
    options.edge_fade_ms = 0;
  }
  std::unique_ptr<SonareVocalEditSession, decltype(&sonare_vocal_session_destroy)> create() {
    SonareVocalEditSession* session = nullptr;
    REQUIRE(sonare_vocal_session_create(samples.data(), samples.size(), 1, 16000, &options,
                                        &session) == SONARE_OK);
    REQUIRE(session != nullptr);
    return {session, sonare_vocal_session_destroy};
  }
};
}  // namespace

TEST_CASE("vocal C empty commit returns null range storage", "[vocal_c_api][vocal_empty_ranges]") {
  VocalFixture f;
  auto session = f.create();
  SonareVocalEditDraft* raw = nullptr;
  REQUIRE(sonare_vocal_session_begin_edit(session.get(), 0, &raw) == SONARE_OK);
  std::unique_ptr<SonareVocalEditDraft, decltype(&sonare_vocal_draft_destroy)> draft(
      raw, sonare_vocal_draft_destroy);
  SonareVocalEditResult result;
  sonare_vocal_edit_result_init(&result);
  REQUIRE(sonare_vocal_draft_commit(raw, 0, &result) == SONARE_OK);
  CHECK(result.dirty_range_count == 0);
  CHECK(result.dirty_ranges == nullptr);
  CHECK(result.id_change_count == 0);
  CHECK(result.id_changes == nullptr);
  sonare_vocal_free_edit_result(&result);
}

TEST_CASE("vocal C draft exposes provisional notes and stale errors without publication",
          "[vocal_c_api]") {
  VocalFixture f;
  auto s = f.create();
  SonareVocalNotesResult notes;
  sonare_vocal_notes_result_init(&notes);
  REQUIRE(sonare_vocal_session_notes(s.get(), &notes) == SONARE_OK);
  REQUIRE(notes.note_count == 1);
  const auto id = notes.notes[0].id;
  const auto edit = notes.notes[0].edit;
  SonareVocalEditDraft* raw = nullptr;
  REQUIRE(sonare_vocal_session_begin_edit(s.get(), 0, &raw) == SONARE_OK);
  std::unique_ptr<SonareVocalEditDraft, decltype(&sonare_vocal_draft_destroy)> draft(
      raw, sonare_vocal_draft_destroy);
  SonareVocalStateToken before{};
  REQUIRE(sonare_vocal_draft_token(raw, &before) == SONARE_OK);
  SonareVocalOperation operation;
  sonare_vocal_operation_init(&operation);
  operation.note_id = id;
  operation.edit = edit;
  operation.edit.transpose_semitones = 2;
  SonareVocalEditResult result;
  sonare_vocal_edit_result_init(&result);
  REQUIRE(sonare_vocal_draft_apply(raw, before.generation, &operation, 1, &result) == SONARE_OK);
  CHECK(result.token.generation == before.generation + 1);
  CHECK(result.dirty_range_count > 0);
  CHECK(result.id_change_count == 0);
  CHECK(result.id_changes == nullptr);
  sonare_vocal_free_edit_result(&result);
  CHECK(sonare_vocal_draft_apply(raw, before.generation, &operation, 1, &result) ==
        SONARE_ERROR_INVALID_STATE);
  CHECK(result.dirty_ranges == nullptr);
  CHECK(result.id_changes == nullptr);
  SonareVocalErrorDetail detail;
  sonare_vocal_error_detail_init(&detail);
  sonare_vocal_last_error_detail(&detail);
  CHECK(detail.reason == SONARE_VOCAL_REASON_REVISION_CONFLICT);
  SonareVocalPitchResult pitch;
  sonare_vocal_pitch_result_init(&pitch);
  REQUIRE(sonare_vocal_draft_evaluate_pitch(raw, id, &pitch) == SONARE_OK);
  REQUIRE(pitch.frame_count > 0);
  CHECK(pitch.effective_midi[0] - pitch.measured_midi[0] == 2);
  sonare_vocal_free_pitch_result(&pitch);
  SonareVocalStateBytes bytes;
  sonare_vocal_state_bytes_init(&bytes);
  CHECK(sonare_vocal_session_export_state(s.get(), &bytes) == SONARE_ERROR_INVALID_STATE);
  CHECK(bytes.data == nullptr);
  REQUIRE(sonare_vocal_draft_commit(raw, 0, &result) == SONARE_OK);
  CHECK(result.token.revision == 1);
  CHECK(result.dirty_range_count > 0);
  sonare_vocal_free_edit_result(&result);
  REQUIRE(sonare_vocal_session_undo(s.get(), 1, &result) == SONARE_OK);
  CHECK(result.token.revision == 2);
  CHECK(result.dirty_range_count > 0);
  sonare_vocal_free_edit_result(&result);
  REQUIRE(sonare_vocal_session_redo(s.get(), 2, &result) == SONARE_OK);
  CHECK(result.token.revision == 3);
  sonare_vocal_free_edit_result(&result);
  sonare_vocal_free_notes(&notes);
  sonare_vocal_free_notes(&notes);
}

TEST_CASE("vocal C snapshots own PCM after session destruction and jobs cancel safely",
          "[vocal_c_api]") {
  VocalFixture f;
  auto s = f.create();
  SonareVocalRenderSnapshot* raw = nullptr;
  REQUIRE(sonare_vocal_session_capture_snapshot(s.get(), &raw) == SONARE_OK);
  std::unique_ptr<SonareVocalRenderSnapshot, decltype(&sonare_vocal_snapshot_destroy)> snapshot(
      raw, sonare_vocal_snapshot_destroy);
  s.reset();
  SonareVocalRenderResult full;
  sonare_vocal_render_result_init(&full);
  REQUIRE(sonare_vocal_snapshot_render(raw, {0, 3200}, UINT64_MAX, nullptr, nullptr, &full) ==
          SONARE_OK);
  CHECK(full.sample_count == 3200);
  CHECK(full.token.request_id == UINT64_MAX);
  CHECK(std::memcmp(full.samples, f.samples.data(), f.samples.size() * sizeof(float)) == 0);
  sonare_vocal_free_render_result(&full);
  SonareVocalRenderJob* job = nullptr;
  REQUIRE(sonare_vocal_render_job_begin(raw, {17, 501}, 7, &job) == SONARE_OK);
  int complete = 8;
  auto cancel = [](void*) -> int { return 1; };
  CHECK(sonare_vocal_render_job_next(job, cancel, nullptr, &complete) == SONARE_ERROR_CANCELLED);
  CHECK(complete == 0);
  CHECK(sonare_vocal_render_job_finalize(job, nullptr, nullptr, &full) != SONARE_OK);
  CHECK(full.samples == nullptr);
  sonare_vocal_render_job_abort(job);
  sonare_vocal_render_job_abort(job);
  sonare_vocal_render_job_destroy(job);
}

TEST_CASE("vocal C state restores exact source and reports digest mismatch", "[vocal_c_api]") {
  VocalFixture f;
  auto s = f.create();
  SonareVocalStateBytes state;
  sonare_vocal_state_bytes_init(&state);
  REQUIRE(sonare_vocal_session_export_state(s.get(), &state) == SONARE_OK);
  REQUIRE(state.size > 4);
  CHECK(std::memcmp(state.data, "SVE1", 4) == 0);
  SonareVocalEditSession* restored = nullptr;
  REQUIRE(sonare_vocal_session_restore(f.samples.data(), 3200, 1, 16000, state.data, state.size,
                                       nullptr, &restored) == SONARE_OK);
  SonareVocalStateToken original_token{}, restored_token{};
  REQUIRE(sonare_vocal_session_token(s.get(), &original_token) == SONARE_OK);
  REQUIRE(sonare_vocal_session_token(restored, &restored_token) == SONARE_OK);
  CHECK(original_token.session_epoch != restored_token.session_epoch);
  sonare_vocal_session_destroy(restored);
  f.samples[0] = -0.0f;
  restored = reinterpret_cast<SonareVocalEditSession*>(static_cast<uintptr_t>(1));
  CHECK(sonare_vocal_session_restore(f.samples.data(), 3200, 1, 16000, state.data, state.size,
                                     nullptr, &restored) != SONARE_OK);
  CHECK(restored == nullptr);
  SonareVocalErrorDetail detail;
  sonare_vocal_error_detail_init(&detail);
  sonare_vocal_last_error_detail(&detail);
  CHECK(detail.reason == SONARE_VOCAL_REASON_SOURCE_MISMATCH);
  CHECK(std::strlen(detail.expected_text) == 64);
  CHECK(std::strlen(detail.actual_text) == 64);
  sonare_vocal_free_state_bytes(&state);
}

TEST_CASE("vocal C mutations publish only after result allocations succeed",
          "[vocal_c_api][vocal_allocation]") {
  for (int action = 0; action < 5; ++action) {
    size_t failures = 0;
    bool succeeded = false;
    for (size_t point = 1; point < 128; ++point) {
      VocalFixture f;
      auto session = f.create();
      SonareVocalNotesResult notes;
      sonare_vocal_notes_result_init(&notes);
      REQUIRE(sonare_vocal_session_notes(session.get(), &notes) == SONARE_OK);
      REQUIRE(notes.note_count == 1);
      SonareVocalEditDraft* draft = nullptr;
      REQUIRE(sonare_vocal_session_begin_edit(session.get(), 0, &draft) == SONARE_OK);
      std::unique_ptr<SonareVocalEditDraft, decltype(&sonare_vocal_draft_destroy)> holder(
          draft, sonare_vocal_draft_destroy);
      SonareVocalOperation op;
      sonare_vocal_operation_init(&op);
      op.note_id = notes.notes[0].id;
      op.edit = notes.notes[0].edit;
      op.edit.gain_db = -6;
      if (action == 4) {
        op.kind = SONARE_VOCAL_SPLIT;
        op.cut_source_sample = 1600;
      }
      SonareVocalEditResult result;
      sonare_vocal_edit_result_init(&result);
      SonareVocalStateToken before{};
      REQUIRE(sonare_vocal_draft_token(draft, &before) == SONARE_OK);
      if (action != 0 && action != 4) {
        REQUIRE(sonare_vocal_draft_apply(draft, before.generation, &op, 1, &result) == SONARE_OK);
        sonare_vocal_free_edit_result(&result);
      }
      if (action >= 2 && action != 4) {
        REQUIRE(sonare_vocal_draft_commit(draft, 0, &result) == SONARE_OK);
        sonare_vocal_free_edit_result(&result);
      }
      if (action == 3) {
        REQUIRE(sonare_vocal_session_undo(session.get(), 1, &result) == SONARE_OK);
        sonare_vocal_free_edit_result(&result);
      }
      const uint64_t revision = (action < 2 || action == 4) ? 0 : (action == 2 ? 1 : 2);
      SonareError error;
      {
        sonare::test::AllocationFailureAtGuard guard(point);
        switch (action) {
          case 4:
          case 0:
            error = sonare_vocal_draft_apply(draft, before.generation, &op, 1, &result);
            break;
          case 1:
            error = sonare_vocal_draft_commit(draft, 0, &result);
            break;
          case 2:
            error = sonare_vocal_session_undo(session.get(), 1, &result);
            break;
          default:
            error = sonare_vocal_session_redo(session.get(), 2, &result);
            break;
        }
      }
      INFO("action=" << action << ", allocation=" << point);
      if (error == SONARE_ERROR_OUT_OF_MEMORY) {
        ++failures;
        uint64_t after = UINT64_MAX;
        REQUIRE(sonare_vocal_session_revision(session.get(), &after) == SONARE_OK);
        CHECK(after == revision);
        CHECK(result.dirty_ranges == nullptr);
        CHECK(result.id_changes == nullptr);
        CHECK(result.token.revision == 0);
        if (action == 0 || action == 4) {
          SonareVocalStateToken token{};
          REQUIRE(sonare_vocal_draft_token(draft, &token) == SONARE_OK);
          CHECK(token.generation == before.generation);
        }
        SonareVocalNotesResult unchanged;
        sonare_vocal_notes_result_init(&unchanged);
        REQUIRE(((action == 0 || action == 4)
                     ? sonare_vocal_draft_notes(draft, &unchanged)
                     : sonare_vocal_session_notes(session.get(), &unchanged)) == SONARE_OK);
        REQUIRE(unchanged.note_count == 1);
        CHECK(unchanged.notes[0].edit.gain_db == (action == 2 ? -6 : 0));
        sonare_vocal_free_notes(&unchanged);
      } else {
        REQUIRE(error == SONARE_OK);
        if (action == 4) {
          REQUIRE(result.id_change_count == 1);
          CHECK(result.id_changes[0].first_new_id == 2);
          CHECK(result.id_changes[0].second_new_id == 3);
        }
        succeeded = true;
      }
      sonare_vocal_free_edit_result(&result);
      sonare_vocal_free_notes(&notes);
      if (succeeded) break;
    }
    CHECK(failures > 0);
    CHECK(succeeded);
  }
}

TEST_CASE("vocal C analysis settings survive authoritative roundtrip",
          "[vocal_c_api][vocal_analysis_metadata]") {
  VocalFixture fixture;
  fixture.analysis.fmin_hz = 80.0;
  fixture.analysis.fmax_hz = 1000.0;
  fixture.analysis.yin_threshold = 0.2;
  fixture.analysis.voiced_threshold = 0.7;
  fixture.analysis.centered = 0;
  fixture.analysis.segmentation_threshold_cents = 60.0;
  fixture.analysis.min_note_ms = 15.0;
  fixture.analysis.reference_hz = 442.0;
  SonareVocalEditSession* session = nullptr;
  REQUIRE(sonare_vocal_session_create(fixture.samples.data(), fixture.samples.size(), 1, 16000,
                                      &fixture.options, &session) == SONARE_OK);
  SonareVocalAnalysisResult result;
  sonare_vocal_analysis_result_init(&result);
  REQUIRE(sonare_vocal_session_analysis(session, &result) == SONARE_OK);
  CHECK(result.analysis.fmin_hz == fixture.analysis.fmin_hz);
  CHECK(result.analysis.fmax_hz == fixture.analysis.fmax_hz);
  CHECK(result.analysis.yin_threshold == fixture.analysis.yin_threshold);
  CHECK(result.analysis.voiced_threshold == fixture.analysis.voiced_threshold);
  CHECK(result.analysis.centered == fixture.analysis.centered);
  CHECK(result.analysis.segmentation_threshold_cents ==
        fixture.analysis.segmentation_threshold_cents);
  CHECK(result.analysis.min_note_ms == fixture.analysis.min_note_ms);
  CHECK(result.analysis.reference_hz == fixture.analysis.reference_hz);
  auto options = fixture.options;
  options.analysis = &result.analysis;
  SonareVocalEditSession* roundtrip = nullptr;
  REQUIRE(sonare_vocal_session_create(fixture.samples.data(), fixture.samples.size(), 1, 16000,
                                      &options, &roundtrip) == SONARE_OK);
  SonareVocalAnalysisResult again;
  sonare_vocal_analysis_result_init(&again);
  REQUIRE(sonare_vocal_session_analysis(roundtrip, &again) == SONARE_OK);
  CHECK(std::strcmp(result.analysis_sha256, again.analysis_sha256) == 0);
  sonare_vocal_free_analysis(&again);
  sonare_vocal_free_analysis(&result);
  sonare_vocal_session_destroy(roundtrip);
  sonare_vocal_session_destroy(session);
}

#else
TEST_CASE("vocal C entry points report unsupported without the pitch editor", "[vocal_c_api]") {
  CHECK(sonare_vocal_available() == 0);
  const std::vector<float> samples(1600, 0.0f);
  SonareVocalCreateOptions options;
  sonare_vocal_create_options_init(&options);
  SonareVocalEditSession* session = nullptr;
  CHECK(sonare_vocal_session_create(samples.data(), static_cast<int64_t>(samples.size()), 1, 16000,
                                    &options, &session) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(session == nullptr);
  SonareVocalErrorDetail detail;
  sonare_vocal_error_detail_init(&detail);
  sonare_vocal_last_error_detail(&detail);
  CHECK(detail.reason == SONARE_VOCAL_REASON_UNSUPPORTED);
}
#endif

TEST_CASE("vocal error detail has a valid header on a fresh thread", "[vocal_c_api]") {
  SonareVocalErrorDetail result{};
  std::thread reader([&] {
    sonare_vocal_error_detail_init(&result);
    sonare_vocal_last_error_detail(&result);
  });
  reader.join();
  CHECK(result.struct_size == sizeof(result));
  CHECK(result.schema_version == SONARE_VOCAL_EDIT_API_VERSION);
  CHECK(result.reason == SONARE_VOCAL_REASON_NONE);
}

#if defined(SONARE_WITH_PITCH_EDITOR)
TEST_CASE("vocal C create and restore enforce the supported sample-rate range",
          "[vocal_c_api][vocal_sample_rate]") {
  VocalFixture f;
  auto s = f.create();
  SonareVocalStateBytes state;
  sonare_vocal_state_bytes_init(&state);
  REQUIRE(sonare_vocal_session_export_state(s.get(), &state) == SONARE_OK);
  // Supplied analysis is rate independent, so only the rate gate decides acceptance.
  for (const int rate : {7999, 8000, 384000, 384001}) {
    CAPTURE(rate);
    const bool supported = rate >= 8000 && rate <= 384000;
    SonareVocalEditSession* created = nullptr;
    const auto create_error = sonare_vocal_session_create(f.samples.data(), f.samples.size(), 1,
                                                          rate, &f.options, &created);
    CHECK((create_error == SONARE_OK) == supported);
    if (!supported) {
      CHECK(create_error == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(created == nullptr);
      SonareVocalErrorDetail detail;
      sonare_vocal_error_detail_init(&detail);
      sonare_vocal_last_error_detail(&detail);
      CHECK(std::string(detail.field) == "sample_rate");
    }
    sonare_vocal_session_destroy(created);
    SonareVocalEditSession* restored = nullptr;
    const auto restore_error = sonare_vocal_session_restore(
        f.samples.data(), 3200, 1, rate, state.data, state.size, nullptr, &restored);
    if (supported) {
      // In range, the rate gate passes and the recorded source rate decides instead.
      CHECK(restore_error == (rate == 16000 ? SONARE_OK : SONARE_ERROR_INVALID_STATE));
    } else {
      CHECK(restore_error == SONARE_ERROR_INVALID_PARAMETER);
    }
    CHECK(restored == nullptr);
  }
  sonare_vocal_free_state_bytes(&state);
}

TEST_CASE("vocal C restore options carry runtime limits and validate their header",
          "[vocal_c_api][vocal_restore_options]") {
  VocalFixture f;
  auto s = f.create();
  SonareVocalStateBytes state;
  sonare_vocal_state_bytes_init(&state);
  REQUIRE(sonare_vocal_session_export_state(s.get(), &state) == SONARE_OK);
  const auto restore = [&](const SonareVocalRestoreOptions* options, SonareVocalEditSession** out) {
    return sonare_vocal_session_restore(f.samples.data(), 3200, 1, 16000, state.data, state.size,
                                        options, out);
  };
  const auto begin_jobs = [](SonareVocalEditSession* session, int* accepted) {
    SonareVocalRenderSnapshot* snapshot = nullptr;
    REQUIRE(sonare_vocal_session_capture_snapshot(session, &snapshot) == SONARE_OK);
    std::vector<SonareVocalRenderJob*> jobs;
    *accepted = 0;
    for (int i = 0; i < 6; ++i) {
      SonareVocalRenderJob* job = nullptr;
      if (sonare_vocal_render_job_begin(snapshot, {0, 320}, 1, &job) != SONARE_OK) break;
      jobs.push_back(job);
      ++*accepted;
    }
    for (auto* job : jobs) sonare_vocal_render_job_destroy(job);
    sonare_vocal_snapshot_destroy(snapshot);
  };

  SonareVocalRestoreOptions defaults;
  sonare_vocal_restore_options_init(&defaults);
  CHECK(defaults.struct_size == sizeof(defaults));
  CHECK(defaults.max_render_jobs == 4);

  SECTION("NULL options restore with the default job limit") {
    SonareVocalEditSession* restored = nullptr;
    REQUIRE(restore(nullptr, &restored) == SONARE_OK);
    int accepted = 0;
    begin_jobs(restored, &accepted);
    CHECK(accepted == static_cast<int>(defaults.max_render_jobs));
    sonare_vocal_session_destroy(restored);
  }
  SECTION("a custom job limit is honoured") {
    SonareVocalRestoreOptions options = defaults;
    options.max_render_jobs = 2;
    SonareVocalEditSession* restored = nullptr;
    REQUIRE(restore(&options, &restored) == SONARE_OK);
    int accepted = 0;
    begin_jobs(restored, &accepted);
    CHECK(accepted == 2);
    sonare_vocal_session_destroy(restored);
  }
  SECTION("malformed options are refused") {
    SonareVocalRestoreOptions small = defaults;
    small.struct_size = sizeof(small) - 1;
    SonareVocalRestoreOptions schema = defaults;
    schema.schema_version = SONARE_VOCAL_EDIT_API_VERSION + 1;
    SonareVocalRestoreOptions zero_jobs = defaults;
    zero_jobs.max_render_jobs = 0;
    for (const auto* options : {&small, &schema, &zero_jobs}) {
      auto* restored = reinterpret_cast<SonareVocalEditSession*>(static_cast<uintptr_t>(1));
      CHECK(restore(options, &restored) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(restored == nullptr);
    }
  }
  sonare_vocal_free_state_bytes(&state);
}
#endif
