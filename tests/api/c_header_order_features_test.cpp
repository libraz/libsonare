// A refused C-ABI call names the earliest parameter of the public signature that is wrong, and
// leaves every out parameter cleared.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? std::string(message) : std::string();
}

template <typename T>
T* garbage_pointer() {
  return reinterpret_cast<T*>(static_cast<std::uintptr_t>(0x10));
}

}  // namespace

TEST_CASE("inverse entries name the input before the outputs", "[c_api][header_order]") {
  SECTION("mel_to_audio_ex names mel before out") {
    REQUIRE(sonare_mel_to_audio_ex(nullptr, 4, 4, 22050, 512, 0, 0.0f, 0.0f, 0, 4, nullptr,
                                   nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "mel must not be NULL");

    float* out = garbage_pointer<float>();
    size_t out_length = 7;
    REQUIRE(sonare_mel_to_audio_ex(nullptr, 4, 4, 22050, 512, 0, 0.0f, 0.0f, 0, 4, &out,
                                   &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "mel must not be NULL");
    CHECK(out == nullptr);
    CHECK(out_length == 0);
  }

  SECTION("griffin_lim names magnitude before out") {
    REQUIRE(sonare_griffin_lim(nullptr, 0, 257, 4, 512, 128, 22050, 4, 0.99f, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "magnitude must not be NULL");

    float* out = garbage_pointer<float>();
    size_t out_length = 7;
    REQUIRE(sonare_griffin_lim(nullptr, 0, 257, 4, 512, 128, 22050, 4, 0.99f, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(out == nullptr);
    CHECK(out_length == 0);
  }

  SECTION("mfcc_to_mel_ex names coefficients before out") {
    REQUIRE(sonare_mfcc_to_mel_ex(nullptr, 4, 4, 8, 0.0f, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "coefficients must not be NULL");

    SonareInverseResult result;
    result.rows = 5;
    result.n_frames = 5;
    result.data = garbage_pointer<float>();
    REQUIRE(sonare_mfcc_to_mel_ex(nullptr, 4, 4, 8, 0.0f, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(result.rows == 0);
    CHECK(result.n_frames == 0);
    CHECK(result.data == nullptr);
  }
}

TEST_CASE("spectral and pitch entries name the earlier parameter first", "[c_api][header_order]") {
  SECTION("clicks names times before out") {
    REQUIRE(sonare_clicks(nullptr, 3, 22050, 0, 1000.0f, 0.1f, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "times must not be NULL when time_count is not 0");

    float* out = garbage_pointer<float>();
    size_t out_length = 7;
    REQUIRE(sonare_clicks(nullptr, 3, 22050, 0, 1000.0f, 0.1f, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(out == nullptr);
    CHECK(out_length == 0);
  }

  SECTION("zero_crossings names samples before out") {
    REQUIRE(sonare_zero_crossings(nullptr, 4, 0.0f, 1, 1, 1, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "samples must not be NULL when length is not 0");

    int* out = garbage_pointer<int>();
    size_t out_count = 7;
    REQUIRE(sonare_zero_crossings(nullptr, 4, 0.0f, 1, 1, 1, &out, &out_count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(out == nullptr);
    CHECK(out_count == 0);
  }

  SECTION("onset_backtrack names events before out") {
    REQUIRE(sonare_onset_backtrack(nullptr, 2, nullptr, 0, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "events must not be NULL when event_count is not 0");
  }

  SECTION("pitch_tuning names frequencies before out_tuning") {
    REQUIRE(sonare_pitch_tuning(nullptr, 2, 0.01f, 12, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "frequencies must not be NULL when length is not 0");

    float tuning = 3.0f;
    REQUIRE(sonare_pitch_tuning(nullptr, 2, 0.01f, 12, &tuning) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(tuning == 0.0f);
  }

  SECTION("spectral_centroid names samples before out") {
    REQUIRE(sonare_spectral_centroid(nullptr, 0, 22050, 2048, 512, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "samples must be a non-empty buffer");

    float* out = garbage_pointer<float>();
    size_t out_count = 7;
    REQUIRE(sonare_spectral_centroid(nullptr, 0, 22050, 2048, 512, &out, &out_count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(out == nullptr);
    CHECK(out_count == 0);
  }

#if defined(SONARE_WITH_PITCH_EDITOR)
  SECTION("note_segments names f0_hz before out") {
    REQUIRE(sonare_note_segments(nullptr, nullptr, nullptr, 10, 100.0f, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "f0_hz must not be NULL");

    SonareNoteSegmentsResult result;
    result.segments = garbage_pointer<SonareNoteSegment>();
    result.count = 7;
    REQUIRE(sonare_note_segments(nullptr, nullptr, nullptr, 10, 100.0f, nullptr, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(result.segments == nullptr);
    CHECK(result.count == 0);
  }
#endif
}

TEST_CASE("CQT entries name the audio buffer before out", "[c_api][header_order]") {
  REQUIRE(sonare_cqt(nullptr, 0, 22050, 512, 32.7f, 84, 12, 1.0f, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must be a non-empty buffer");

  SonareCqtResult result;
  result.n_bins = 5;
  result.n_frames = 5;
  result.magnitude = garbage_pointer<float>();
  result.frequencies = garbage_pointer<float>();
  REQUIRE(sonare_cqt(nullptr, 0, 22050, 512, 32.7f, 84, 12, 1.0f, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(result.n_bins == 0);
  CHECK(result.n_frames == 0);
  CHECK(result.magnitude == nullptr);
  CHECK(result.frequencies == nullptr);

  REQUIRE(sonare_vqt(nullptr, 0, 22050, 512, 32.7f, 84, 12, 1.0f, 0.0f, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must be a non-empty buffer");
}

TEST_CASE("quick entries name the audio buffer before out", "[c_api][header_order]") {
  SECTION("detect_bpm") {
    REQUIRE(sonare_detect_bpm(nullptr, 0, 22050, 60.0f, 200.0f, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "samples must be a non-empty buffer");

    float bpm = 3.0f;
    REQUIRE(sonare_detect_bpm(nullptr, 0, 22050, 60.0f, 200.0f, &bpm) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(bpm == 0.0f);
  }

  SECTION("detect_beats") {
    REQUIRE(sonare_detect_beats(nullptr, 0, 22050, 60.0f, 200.0f, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "samples must be a non-empty buffer");

    float* times = garbage_pointer<float>();
    size_t count = 7;
    REQUIRE(sonare_detect_beats(nullptr, 0, 22050, 60.0f, 200.0f, &times, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(times == nullptr);
    CHECK(count == 0);
  }

  SECTION("analyze") {
    REQUIRE(sonare_analyze(nullptr, 0, 22050, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "samples must be a non-empty buffer");
  }
}

TEST_CASE("polyphony entries name the analysis before the outputs", "[c_api][header_order]") {
  size_t count = 7;
#if defined(SONARE_WITH_PITCH_EDITOR)
  REQUIRE(sonare_polyphonic_note_count(nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "analysis must not be NULL");

  REQUIRE(sonare_polyphonic_note_count(nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(count == 0);

  float* out = garbage_pointer<float>();
  size_t out_length = 7;
  REQUIRE(sonare_polyphonic_render(nullptr, nullptr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "analysis must not be NULL");
  CHECK(out == nullptr);
  CHECK(out_length == 0);
#else
  REQUIRE(sonare_polyphonic_note_count(nullptr, &count) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(count == 0);
#endif
}

TEST_CASE("transcribe entries name the audio buffer before out", "[c_api][header_order]") {
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
  REQUIRE(sonare_transcribe(nullptr, 0, 22050, 120.0f, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples must be a non-empty buffer");

  SonareTranscribeResult result;
  result.events = garbage_pointer<SonareMidiEventPod>();
  result.count = 7;
  result.note_count = 7;
  REQUIRE(sonare_transcribe(nullptr, 0, 22050, 120.0f, nullptr, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(result.events == nullptr);
  CHECK(result.count == 0);
  CHECK(result.note_count == 0);

  size_t note_count = 7;
  REQUIRE(sonare_project_transcribe_to_clip(nullptr, 1, nullptr, 0, 22050, nullptr, &note_count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "project must not be NULL");
  CHECK(note_count == 0);
#else
  size_t note_count = 7;
  REQUIRE(sonare_project_transcribe_to_clip(nullptr, 1, nullptr, 0, 22050, nullptr, &note_count) ==
          SONARE_ERROR_NOT_SUPPORTED);
  CHECK(note_count == 0);
#endif
}
