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
  CHECK(last_message() == "harmonic: samples must be a non-empty buffer");
  CHECK(out == nullptr);
  CHECK(out_length == 0);

  REQUIRE(sonare_harmonic(nullptr, kLength, kRate, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "harmonic: samples must be a non-empty buffer");

  REQUIRE(sonare_harmonic(samples.data(), kLength, kRate, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");

  REQUIRE(sonare_harmonic(samples.data(), kLength, kRate, &out, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_length must not be NULL");
  CHECK(out == nullptr);

  float* harmonic = garbage_pointer<float>();
  float* percussive = garbage_pointer<float>();
  float* residual = garbage_pointer<float>();
  size_t length = 99;
  int sample_rate = 99;
  REQUIRE(sonare_hpss_with_residual(nullptr, kLength, kRate, 31, 31, &harmonic, &percussive,
                                    &residual, &length,
                                    &sample_rate) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "hpss_ex: samples must be a non-empty buffer");
  CHECK(harmonic == nullptr);
  CHECK(percussive == nullptr);
  CHECK(residual == nullptr);
  CHECK(length == 0);
  CHECK(sample_rate == 0);

  REQUIRE(sonare_hpss_with_residual(nullptr, kLength, kRate, 31, 31, nullptr, nullptr, nullptr,
                                    nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "hpss_ex: samples must be a non-empty buffer");

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
  REQUIRE(sonare_decompose_with_init(nullptr, 4, 4, 2, 10, 0.0f, "random", &w, &w_length, &h,
                                     &h_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "s must not be NULL");
  CHECK(w == nullptr);
  CHECK(w_length == 0);
  CHECK(h == nullptr);
  CHECK(h_length == 0);

  REQUIRE(sonare_decompose_with_init(nullptr, 4, 4, 2, 10, 0.0f, "random", nullptr, nullptr,
                                     nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
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
  SonareNormalizeStereoResult result;
  std::memset(&result, 0xAB, sizeof(result));
  REQUIRE(sonare_normalize_stereo(nullptr, nullptr, kLength, kRate, -3.0f, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "normalize_stereo: left must be a non-empty buffer");
  CHECK(result.left == nullptr);
  CHECK(result.right == nullptr);
  CHECK(result.length == 0);

  REQUIRE(sonare_normalize_stereo(nullptr, nullptr, kLength, kRate, -3.0f, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "normalize_stereo: left must be a non-empty buffer");
}

#ifdef SONARE_WITH_VOICE_CHANGER

TEST_CASE("voice changer one-shots refuse the input buffer before a NULL output",
          "[c_api][header_order]") {
  const std::vector<float> samples = make_samples();

  float* out = garbage_pointer<float>();
  size_t out_length = 99;
  REQUIRE(sonare_voice_change(nullptr, kLength, kRate, 0.0f, 1.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "voice_change: samples must be a non-empty buffer");
  CHECK(out == nullptr);
  CHECK(out_length == 0);

  REQUIRE(sonare_voice_change(nullptr, kLength, kRate, 0.0f, 1.0f, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "voice_change: samples must be a non-empty buffer");

  out = garbage_pointer<float>();
  out_length = 99;
  REQUIRE(sonare_voice_change_realtime(nullptr, kLength, kRate, nullptr, 3, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "voice_change_realtime: samples must be a non-empty buffer");
  CHECK(out == nullptr);
  CHECK(out_length == 0);

  // A refused channel count precedes the NULL outputs and records no message of its own.
  REQUIRE(sonare_voice_change_realtime(samples.data(), kLength, kRate, nullptr, 3, nullptr,
                                       nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().empty());
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

  handle = garbage_pointer<SonareRealtimeVoiceChanger>();
  REQUIRE(sonare_realtime_voice_changer_create_json("neutralMonitor", 0, 0, 1, &handle) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(handle == nullptr);
  const std::string json_rate_message = last_message();
  REQUIRE(sonare_realtime_voice_changer_create_json("neutralMonitor", 0, 0, 1, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == json_rate_message);
  CHECK(json_rate_message != "out must not be NULL");

  // A document the loader refuses precedes the sample rate.
  REQUIRE(sonare_realtime_voice_changer_create_json("{", 0, 0, 1, nullptr) ==
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

#endif  // SONARE_WITH_PITCH_EDITOR
