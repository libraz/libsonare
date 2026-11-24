/// @file c_header_order_effects_test.cpp
/// @brief The effects, voice changer and vocal edit C entries clear every out parameter first and
///        then refuse arguments in public-header parameter order, whatever else is wrong.

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_vocal_edit.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

constexpr int kRate = 22050;
constexpr size_t kLength = 256;

std::vector<float> make_samples() {
  std::vector<float> samples(kLength);
  for (size_t i = 0; i < kLength; ++i) {
    samples[i] = 0.1f * std::sin(0.05f * static_cast<float>(i));
  }
  return samples;
}

/// A pointer value no entry may dereference or free; a cleared out parameter must not keep it.
template <typename T>
T* garbage_pointer() {
  return reinterpret_cast<T*>(static_cast<std::uintptr_t>(0x10));
}

}  // namespace

TEST_CASE("effects entries refuse the input buffer before a NULL output", "[c_api][header_order]") {
  const std::vector<float> samples = make_samples();

  float* out = garbage_pointer<float>();
  size_t out_length = 99;
  REQUIRE(sonare_harmonic(nullptr, kLength, kRate, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must be a non-empty buffer");
  CHECK(out == nullptr);
  CHECK(out_length == 0);

  REQUIRE(sonare_harmonic(nullptr, kLength, kRate, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must be a non-empty buffer");

  REQUIRE(sonare_harmonic(samples.data(), kLength, kRate, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");

  REQUIRE(sonare_harmonic(samples.data(), kLength, kRate, &out, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_length must not be NULL");
  CHECK(out == nullptr);

  SonareHpssResult hpss_result{garbage_pointer<float>(), garbage_pointer<float>(), 99, 99};
  float* residual = garbage_pointer<float>();
  REQUIRE(sonare_hpss(nullptr, kLength, kRate, 31, 31, nullptr, &hpss_result, &residual) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must be a non-empty buffer");
  CHECK(hpss_result.harmonic == nullptr);
  CHECK(hpss_result.percussive == nullptr);
  CHECK(residual == nullptr);
  CHECK(hpss_result.length == 0);
  CHECK(hpss_result.sample_rate == 0);

  REQUIRE(sonare_hpss(nullptr, kLength, kRate, 31, 31, nullptr, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must be a non-empty buffer");

  int* intervals_out = garbage_pointer<int>();
  size_t interval_count_out = 99;
  REQUIRE(sonare_remix_aligned_intervals(samples.data(), kLength, kRate, nullptr, 1, 0,
                                         &intervals_out,
                                         &interval_count_out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "intervals must not be NULL when interval_count is not 0");
  CHECK(intervals_out == nullptr);
  CHECK(interval_count_out == 0);
}

TEST_CASE("decompose and nn_filter refuse the input matrix before a NULL output",
          "[c_api][header_order]") {
  float* w = garbage_pointer<float>();
  size_t w_length = 99;
  float* h = garbage_pointer<float>();
  size_t h_length = 99;
  REQUIRE(sonare_decompose(nullptr, 4, 4, 2, 10, 0.0f, nullptr, &w, &w_length, &h, &h_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "s must not be NULL");
  CHECK(w == nullptr);
  CHECK(w_length == 0);
  CHECK(h == nullptr);
  CHECK(h_length == 0);

  REQUIRE(sonare_decompose(nullptr, 4, 4, 2, 10, 0.0f, nullptr, nullptr, nullptr, nullptr,
                           nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "s must not be NULL");

  float* out = garbage_pointer<float>();
  size_t out_length = 99;
  REQUIRE(sonare_nn_filter(nullptr, 4, 4, "mean", 2, 1, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "s must not be NULL");
  CHECK(out == nullptr);
  CHECK(out_length == 0);

  REQUIRE(sonare_nn_filter(nullptr, 4, 4, "mean", 2, 1, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "s must not be NULL");
}

TEST_CASE("stereo normalize refuses the left channel before a NULL result",
          "[c_api][header_order]") {
  float sentinel = 0.0f;
  float* out_left = &sentinel;
  float* out_right = &sentinel;
  size_t out_length = 99;
  REQUIRE(sonare_normalize_stereo(nullptr, nullptr, kLength, kRate, -3.0f, &out_left, &out_right,
                                  &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "left must be a non-empty buffer");
  CHECK(out_left == nullptr);
  CHECK(out_right == nullptr);
  CHECK(out_length == 0);

  REQUIRE(sonare_normalize_stereo(nullptr, nullptr, kLength, kRate, -3.0f, nullptr, nullptr,
                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "left must be a non-empty buffer");
}

#ifdef SONARE_WITH_VOICE_CHANGER

TEST_CASE("voice changer one-shots refuse the input buffer before a NULL output",
          "[c_api][header_order]") {
  const std::vector<float> samples = make_samples();

  float* out = garbage_pointer<float>();
  size_t out_length = 99;
  REQUIRE(sonare_voice_change(nullptr, kLength, kRate, nullptr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must be a non-empty buffer");
  CHECK(out == nullptr);
  CHECK(out_length == 0);

  REQUIRE(sonare_voice_change(nullptr, kLength, kRate, nullptr, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must be a non-empty buffer");

  out = garbage_pointer<float>();
  out_length = 99;
  REQUIRE(sonare_realtime_voice_changer_render(nullptr, kLength, kRate, nullptr, 3, &out,
                                               &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must be a non-empty buffer");
  CHECK(out == nullptr);
  CHECK(out_length == 0);

  // A refused channel count precedes the NULL outputs.
  REQUIRE(sonare_realtime_voice_changer_render(samples.data(), kLength, kRate, nullptr, 3, nullptr,
                                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "channels must be an integer in [1, 2]");
}

TEST_CASE("voice changer creation refuses its arguments before a NULL handle output",
          "[c_api][header_order]") {
  SonareRealtimeVoiceChanger* handle = garbage_pointer<SonareRealtimeVoiceChanger>();
  REQUIRE(sonare_realtime_voice_changer_create(nullptr, 0, 0, 1, &handle) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(handle == nullptr);
  const std::string rate_message = last_message();
  CHECK_FALSE(rate_message.empty());
  CHECK(rate_message != "out must not be NULL");

  REQUIRE(sonare_realtime_voice_changer_create(nullptr, 0, 0, 1, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == rate_message);

  SonareRealtimeVoiceChangerConfig bad_config;
  std::memset(&bad_config, 0, sizeof(bad_config));
  handle = garbage_pointer<SonareRealtimeVoiceChanger>();
  REQUIRE(sonare_realtime_voice_changer_create(&bad_config, kRate, 0, 1, &handle) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(handle == nullptr);
  const std::string config_message = last_message();
  CHECK_FALSE(config_message.empty());

  // The config precedes the sample rate, so its refusal wins over a bad rate and a NULL output.
  REQUIRE(sonare_realtime_voice_changer_create(&bad_config, 0, 0, 1, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == config_message);

  SonareRealtimeVoiceChangerConfig parsed;
  REQUIRE(sonare_realtime_voice_changer_config_from_json("neutralMonitor", &parsed) == SONARE_OK);
  handle = garbage_pointer<SonareRealtimeVoiceChanger>();
  REQUIRE(sonare_realtime_voice_changer_create(&parsed, 0, 0, 1, &handle) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(handle == nullptr);
  const std::string parsed_rate_message = last_message();
  REQUIRE(sonare_realtime_voice_changer_create(&parsed, 0, 0, 1, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == parsed_rate_message);
  CHECK(parsed_rate_message != "out must not be NULL");

  // A document the loader refuses is its own refusal.
  REQUIRE(sonare_realtime_voice_changer_config_from_json("{", &parsed) ==
          SONARE_ERROR_INVALID_FORMAT);
}

#endif  // SONARE_WITH_VOICE_CHANGER

#ifdef SONARE_WITH_PITCH_EDITOR

TEST_CASE("vocal edit entries refuse the handle before a NULL or invalid output",
          "[c_api][header_order]") {
  SonareVocalStateToken token;
  std::memset(&token, 0xAB, sizeof(token));
  REQUIRE(sonare_vocal_session_token(nullptr, &token) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "session");
  CHECK(token.session_epoch == 0);
  CHECK(token.revision == 0);
  CHECK(token.profile_id == 0);

  REQUIRE(sonare_vocal_session_token(nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "session");

  int can_undo = 7;
  int can_redo = 7;
  REQUIRE(sonare_vocal_session_history(nullptr, &can_undo, &can_redo) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "session");
  CHECK(can_undo == 0);
  CHECK(can_redo == 0);

  REQUIRE(sonare_vocal_session_history(nullptr, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "session");

  SonareVocalNotesResult notes;
  sonare_vocal_notes_result_init(&notes);
  notes.note_count = 7;
  REQUIRE(sonare_vocal_session_notes(nullptr, &notes) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "session");
  CHECK(notes.note_count == 0);

  // A result whose header was never initialised is refused after the session, not before it.
  SonareVocalNotesResult uninitialised;
  std::memset(&uninitialised, 0, sizeof(uninitialised));
  REQUIRE(sonare_vocal_session_notes(nullptr, &uninitialised) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "session");
  REQUIRE(sonare_vocal_session_notes(nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "session");
}

TEST_CASE("vocal session creation refuses the source buffer before a NULL session output",
          "[c_api][header_order]") {
  SonareVocalSession* session = garbage_pointer<SonareVocalSession>();
  REQUIRE(sonare_vocal_session_create(nullptr, 1024, 1, kRate, nullptr, &session) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples");
  CHECK(session == nullptr);

  REQUIRE(sonare_vocal_session_create(nullptr, 1024, 1, kRate, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples");

  session = garbage_pointer<SonareVocalSession>();
  REQUIRE(sonare_vocal_session_restore(nullptr, 1024, 1, kRate, nullptr, 0, nullptr, &session) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples");
  CHECK(session == nullptr);

  REQUIRE(sonare_vocal_session_restore(nullptr, 1024, 1, kRate, nullptr, 0, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples");
}

TEST_CASE("vocal source and options refusals name the first invalid argument",
          "[c_api][header_order][vocal_subjects]") {
  const auto samples = make_samples();
  SonareVocalCreateOptions options;
  sonare_vocal_create_options_init(&options);
  options.struct_size = 0;
  SonareVocalRestoreOptions restore_options;
  sonare_vocal_restore_options_init(&restore_options);
  restore_options.struct_size = 0;
  const auto check_source_detail = [](const char* expected) {
    SonareVocalErrorDetail detail;
    sonare_vocal_error_detail_init(&detail);
    sonare_vocal_last_error_detail(&detail);
    CHECK(detail.reason == SONARE_VOCAL_REASON_INVALID_INPUT);
    CHECK(std::string(detail.field) == expected);
    CHECK(last_message() == "positive mono source required");
  };
  REQUIRE(sonare_vocal_session_create(samples.data(), 0, 2, 0, &options, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  check_source_detail("frames");
  REQUIRE(sonare_vocal_session_restore(samples.data(), samples.size(), 2, 0, nullptr, 1,
                                       &restore_options,
                                       nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  check_source_detail("channels");
  REQUIRE(sonare_vocal_session_create(samples.data(), samples.size(), 1, kRate, &options,
                                      nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.struct_size");
  REQUIRE(sonare_vocal_session_restore(samples.data(), samples.size(), 1, kRate, nullptr, 0,
                                       &restore_options,
                                       nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.struct_size");
  REQUIRE(sonare_vocal_session_restore(samples.data(), samples.size(), 1, kRate, nullptr, 0,
                                       nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_session");
}

TEST_CASE("vocal output and stale token refusals use declared names",
          "[c_api][header_order][vocal_subjects]") {
  const auto samples = make_samples();
  SonareVocalSession* raw = nullptr;
  REQUIRE(sonare_vocal_session_create(samples.data(), samples.size(), 1, kRate, nullptr, &raw) ==
          SONARE_OK);
  const std::unique_ptr<SonareVocalSession, decltype(&sonare_vocal_session_destroy)> session(
      raw, sonare_vocal_session_destroy);
  const auto check_refusal = [](SonareError code, const char* subject) {
    CHECK(code == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == subject);
  };
  const auto check_conflict = [](SonareError code, const char* subject, uint64_t actual) {
    CHECK(code == SONARE_ERROR_INVALID_STATE);
    CHECK(last_message() == "revision or generation conflict");
    SonareVocalErrorDetail detail;
    sonare_vocal_error_detail_init(&detail);
    sonare_vocal_last_error_detail(&detail);
    CHECK(detail.reason == SONARE_VOCAL_REASON_REVISION_CONFLICT);
    CHECK(std::string(detail.field) == subject);
    CHECK(detail.expected == 99);
    CHECK(detail.actual == actual);
  };
  check_refusal(sonare_vocal_session_output_length(raw, nullptr), "samples");
  check_refusal(sonare_vocal_session_notes(raw, nullptr), "result");
  SonareVocalNotesResult notes{};
  notes.note_count = 7;
  check_refusal(sonare_vocal_session_notes(raw, &notes), "result.struct_size");
  CHECK(notes.note_count == 7);
  check_refusal(sonare_vocal_session_export_state(raw, nullptr), "state");
  SonareVocalStateBytes state{};
  check_refusal(sonare_vocal_session_export_state(raw, &state), "state.struct_size");
  check_refusal(sonare_vocal_session_begin_edit(raw, 99, nullptr), "out_draft");
  check_refusal(sonare_vocal_session_source_to_destination(raw, 0, 0, nullptr),
                "destination_sample");
  check_refusal(sonare_vocal_session_destination_to_source(raw, 0, 0, nullptr), "source_sample");
  check_refusal(sonare_vocal_session_capture_snapshot(raw, nullptr), "snapshot");

  SonareVocalSnapshot* raw_snapshot = nullptr;
  REQUIRE(sonare_vocal_session_capture_snapshot(raw, &raw_snapshot) == SONARE_OK);
  const std::unique_ptr<SonareVocalSnapshot, decltype(&sonare_vocal_snapshot_destroy)> snapshot(
      raw_snapshot, sonare_vocal_snapshot_destroy);
  check_refusal(sonare_vocal_snapshot_output_length(raw_snapshot, nullptr), "samples");
  check_refusal(sonare_vocal_render_job_begin(raw_snapshot, {0, 1}, 0, nullptr), "job");

  SonareVocalDraft* raw_draft = nullptr;
  check_conflict(sonare_vocal_session_begin_edit(raw, 99, &raw_draft), "expected_revision", 0);
  REQUIRE(raw_draft == nullptr);
  SonareVocalEditResult result;
  sonare_vocal_edit_result_init(&result);
  check_conflict(sonare_vocal_session_undo(raw, 99, &result), "expected_revision", 0);
  check_conflict(sonare_vocal_session_redo(raw, 99, &result), "expected_revision", 0);
  REQUIRE(sonare_vocal_session_begin_edit(raw, 0, &raw_draft) == SONARE_OK);
  const std::unique_ptr<SonareVocalDraft, decltype(&sonare_vocal_draft_destroy)> draft(
      raw_draft, sonare_vocal_draft_destroy);
  check_refusal(sonare_vocal_draft_source_to_destination(raw_draft, 0, 0, nullptr),
                "destination_sample");
  check_refusal(sonare_vocal_draft_destination_to_source(raw_draft, 0, 0, nullptr),
                "source_sample");
  check_refusal(sonare_vocal_draft_capture_snapshot(raw_draft, nullptr), "snapshot");
  check_conflict(sonare_vocal_draft_apply(raw_draft, 99, nullptr, 0, &result),
                 "expected_generation", 1);
  check_conflict(sonare_vocal_draft_commit(raw_draft, 99, &result), "expected_revision", 0);
}

#endif  // SONARE_WITH_PITCH_EDITOR
