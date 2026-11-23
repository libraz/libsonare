/// @file sonare_c_daw_refusal_test.cpp
/// @brief The pitch-editor and percussive-event C entries refuse their own arguments in
///        "<parameter> <predicate>" form.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

constexpr int kRate = 22050;
constexpr size_t kLength = 2048;
constexpr size_t kFrames = 10;

std::vector<float> make_samples() {
  std::vector<float> samples(kLength);
  for (size_t i = 0; i < kLength; ++i) {
    samples[i] = 0.1f * std::sin(0.05f * static_cast<float>(i));
  }
  return samples;
}

}  // namespace

TEST_CASE("the vector-output pitch entries refuse a NULL output by name",
          "[c_api][engine_refusal]") {
  const std::vector<float> samples = make_samples();
  float* out = nullptr;
  size_t out_length = 0;

  CHECK(sonare_note_stretch(samples.data(), kLength, kRate, 0, 100, 1.0f, nullptr, &out_length) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  CHECK(sonare_note_move(samples.data(), kLength, kRate, 0, 100, 50, &out, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_length must not be NULL");
  CHECK(sonare_pitch_correct_to_midi(samples.data(), kLength, kRate, 60.0f, 62.0f, nullptr,
                                     &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
}

TEST_CASE("the note region entries refuse each position by its parameter name",
          "[c_api][engine_refusal]") {
  const std::vector<float> samples = make_samples();
  float* out = reinterpret_cast<float*>(0x1);
  size_t out_length = 7;

  CHECK(sonare_note_stretch(samples.data(), kLength, kRate, 1200, 400, 1.5f, &out, &out_length) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "offset_sample must be greater than onset_sample");
  REQUIRE(sonare_last_refusal_subject_count() == 2);
  CHECK(std::string(sonare_last_refusal_subject(0)) == "offset_sample");
  CHECK(std::string(sonare_last_refusal_subject(1)) == "onset_sample");
  CHECK(out == nullptr);
  CHECK(out_length == 0);

  CHECK(sonare_note_stretch(samples.data(), kLength, kRate, -1, 400, 1.5f, &out, &out_length) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "onset_sample must be an integer in [0, 2047]");
  CHECK(sonare_note_stretch(samples.data(), kLength, kRate, 0, 400, 0.0f, &out, &out_length) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "stretch_ratio must be finite and greater than 0");

  CHECK(sonare_note_move(samples.data(), kLength, kRate, 0, 400, 24000, &out, &out_length) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "target_onset_sample must be an integer in [0, 2047]");
  REQUIRE(sonare_last_refusal_subject_count() == 1);
  CHECK(std::string(sonare_last_refusal_subject(0)) == "target_onset_sample");
  CHECK(sonare_note_move(samples.data(), kLength, kRate, 2048, 4000, 0, &out, &out_length) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "onset_sample must be an integer in [0, 2047]");
}

TEST_CASE("a refused sample position renders its domain in the unit its caller gave",
          "[c_api][engine_refusal]") {
  const std::vector<float> samples = make_samples();
  float* out = nullptr;
  size_t out_length = 0;
  char* text = nullptr;

  REQUIRE(sonare_note_move(samples.data(), kLength, kRate, 0, 400, 24000, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const char* seconds[] = {"target_onset_sec"};
  const double rate[] = {static_cast<double>(kRate)};
  REQUIRE(sonare_last_refusal_render(seconds, rate, 1, SONARE_PRESENCE_SPELLING_C, &text) ==
          SONARE_OK);
  CHECK(std::string(text) == "target_onset_sec must be finite and in [0, 0.09283]");
  sonare_free_string(text);
  const double exact[] = {2047.0};
  REQUIRE(sonare_last_refusal_render(seconds, exact, 1, SONARE_PRESENCE_SPELLING_C, &text) ==
          SONARE_OK);
  CHECK(std::string(text) == "target_onset_sec must be finite and in [0, 1]");
  sonare_free_string(text);
  const double unscaled[] = {1.0};
  REQUIRE(sonare_last_refusal_render(seconds, unscaled, 1, SONARE_PRESENCE_SPELLING_C, &text) ==
          SONARE_OK);
  CHECK(std::string(text) == "target_onset_sec must be an integer in [0, 2047]");
  sonare_free_string(text);
  for (const double bad : {0.0, -1.0, std::numeric_limits<double>::infinity()}) {
    const double scale[] = {bad};
    CHECK(sonare_last_refusal_render(seconds, scale, 1, SONARE_PRESENCE_SPELLING_C, &text) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(text == nullptr);
  }

  REQUIRE(sonare_note_move(samples.data(), kLength, kRate, 1200, 400, 0, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const char* pair[] = {"offset_sec", "onset_sec"};
  const double rates[] = {static_cast<double>(kRate), static_cast<double>(kRate)};
  REQUIRE(sonare_last_refusal_render(pair, rates, 2, SONARE_PRESENCE_SPELLING_C, &text) ==
          SONARE_OK);
  CHECK(std::string(text) == "offset_sec must be greater than onset_sec");
  sonare_free_string(text);
}

TEST_CASE("the percussive entries refuse their arguments by name", "[c_api][engine_refusal]") {
  const std::vector<float> samples = make_samples();

  CHECK(sonare_extract_percussive_events(samples.data(), kLength, kRate, nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");

  SonarePercussiveEventsResult result{};
  SonarePercussiveEventConfig config{};
  CHECK(sonare_extract_percussive_events(samples.data(), kLength, kRate, &config, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.struct_size must be set by sonare_percussive_event_config_init");
  sonare_percussive_event_config_init(&config);
  config.n_fft = 0;
  CHECK(sonare_extract_percussive_events(samples.data(), kLength, kRate, &config, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.n_fft must be an even integer in [4, 524288]");
  sonare_percussive_event_config_init(&config);
  config.hpss_kernel_harmonic = 0;
  CHECK(sonare_extract_percussive_events(samples.data(), kLength, kRate, &config, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.hpss_kernel_harmonic must be an integer in [1, 2147483647]");

  float* out = nullptr;
  size_t out_length = 0;
  CHECK(sonare_render_percussive_events(samples.data(), kLength, kRate, nullptr, 1, nullptr, &out,
                                        &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "events must not be NULL when count is not 0");
  CHECK(sonare_render_percussive_events(samples.data(), kLength, kRate, nullptr, 0, nullptr,
                                        nullptr, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");

  SonarePercussiveRenderConfig render_config{};
  CHECK(sonare_render_percussive_events(samples.data(), kLength, kRate, nullptr, 0, &render_config,
                                        &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.struct_size must be set by sonare_percussive_render_config_init");
  sonare_percussive_render_config_init(&render_config);
  render_config.fade_ms = 0.0f;
  CHECK(sonare_render_percussive_events(samples.data(), kLength, kRate, nullptr, 0, &render_config,
                                        &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.fade_ms must be finite and greater than 0");
  render_config.fade_ms = 5.0f;
  render_config.hpss_kernel_percussive = 0;
  CHECK(sonare_render_percussive_events(samples.data(), kLength, kRate, nullptr, 0, &render_config,
                                        &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.hpss_kernel_percussive must be an integer in [1, 2147483647]");
}

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("sonare_note_targets_from_smf refuses a NULL output by name", "[c_api][engine_refusal]") {
  SonareNoteTarget* targets = nullptr;
  size_t count = 0;
  CHECK(sonare_note_targets_from_smf(nullptr, 0, 0, nullptr, &count) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  CHECK(sonare_note_targets_from_smf(nullptr, 0, 0, &targets, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_count must not be NULL");
}
#endif

#if defined(SONARE_WITH_PITCH_EDITOR)

TEST_CASE("the time-varying correction entries refuse the track arguments by name",
          "[c_api][engine_refusal]") {
  const std::vector<float> samples = make_samples();
  std::vector<float> f0(kFrames, 220.0f);
  std::vector<float> prob(kFrames, 0.9f);
  float* out = nullptr;
  size_t out_length = 0;

  CHECK(sonare_pitch_correct_to_midi_timevarying(samples.data(), kLength, kRate, nullptr,
                                                 prob.data(), nullptr, kFrames, 512, 60.0f, &out,
                                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "f0_hz must not be NULL");
  CHECK(sonare_pitch_correct_to_midi_timevarying(samples.data(), kLength, kRate, f0.data(),
                                                 prob.data(), nullptr, 0, 512, 60.0f, &out,
                                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_frames must be an integer of at least 1");
  CHECK(sonare_pitch_correct_to_midi_timevarying(samples.data(), kLength, kRate, f0.data(),
                                                 prob.data(), nullptr, kFrames, 0, 60.0f, &out,
                                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "hop_length must be an integer in [1, 2147483647]");
  CHECK(sonare_pitch_correct_to_midi_timevarying(samples.data(), kLength, kRate, f0.data(),
                                                 prob.data(), nullptr, kFrames, 512, 200.0f, &out,
                                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "target_midi must be finite and in [0, 127]");

  std::vector<float> high(kFrames, 20000.0f);
  CHECK(sonare_pitch_correct_to_midi_timevarying(samples.data(), kLength, kRate, high.data(),
                                                 prob.data(), nullptr, kFrames, 512, 60.0f, &out,
                                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "f0_hz must be finite and in [0, sample_rate / 2], or NaN on an unvoiced frame");

  std::vector<float> bad_prob(kFrames, 2.0f);
  CHECK(sonare_pitch_correct_to_midi_timevarying(
            samples.data(), kLength, kRate, f0.data(), bad_prob.data(), nullptr, kFrames, 512,
            60.0f, &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "voiced_prob must be finite and in [0, 1]");
}

TEST_CASE("the correction config is refused member by member", "[c_api][engine_refusal]") {
  sonare_pitch_correction_config_init(nullptr);

  const std::vector<float> samples = make_samples();
  std::vector<float> f0(kFrames, 220.0f);
  std::vector<float> prob(kFrames, 0.9f);
  float* out = nullptr;
  size_t out_length = 0;
  SonarePitchCorrectionConfig config;
  sonare_pitch_correction_config_init(&config);
  CHECK(config.struct_size == sizeof(SonarePitchCorrectionConfig));
  CHECK(config.scale_reference_midi == 69.0f);

  auto run = [&](const SonarePitchCorrectionConfig& c) {
    return sonare_pitch_correct_timevarying(samples.data(), kLength, kRate, f0.data(), prob.data(),
                                            nullptr, kFrames, 512, &c, &out, &out_length);
  };

  CHECK(sonare_pitch_correct_timevarying(samples.data(), kLength, kRate, nullptr, prob.data(),
                                         nullptr, kFrames, 512, &config, &out,
                                         &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "f0_hz must not be NULL");

  SonarePitchCorrectionConfig bad{};
  CHECK(run(bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.struct_size must be set by sonare_pitch_correction_config_init");

  bad = config;
  bad.scale_reference_midi = 0.0f;
  bad.target_mode = SONARE_PITCH_TARGET_SCALE;
  CHECK(run(bad) == SONARE_OK);

  bad = config;
  bad.target_mode = 5;
  CHECK(run(bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.target_mode must be one of 0, 1");

  bad = config;
  bad.target_midi = 200.0f;
  CHECK(run(bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.target_midi must be finite and in [0, 127]");

  bad = config;
  bad.scale_root = 12;
  CHECK(run(bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.scale_root must be an integer in [0, 11]");

  bad = config;
  bad.scale_mode_mask = 0;
  CHECK(run(bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.scale_mode_mask must be an integer in [1, 4095]");

  bad = config;
  bad.retune_amount = 2.0f;
  CHECK(run(bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.retune_amount must be finite and in [0, 1]");

  bad = config;
  bad.max_correction_semitones = -1.0f;
  CHECK(run(bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.max_correction_semitones must be finite and at least 0");

  bad = config;
  bad.retune_speed_ms = -1.0f;
  CHECK(run(bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.retune_speed_ms must be finite and at least 0");

  bad = config;
  bad.vibrato_threshold_cents = -1.0f;
  CHECK(run(bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.vibrato_threshold_cents must be finite and at least 0");

  bad = config;
  bad.scale_reference_midi = std::numeric_limits<float>::quiet_NaN();
  CHECK(run(bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "config.scale_reference_midi must be finite and in [-3.4028235e38, 3.4028235e38]");
}

TEST_CASE("sonare_auto_tune refuses its key, config and outputs by name",
          "[c_api][engine_refusal]") {
  const std::vector<float> samples = make_samples();
  float* out = nullptr;
  size_t out_length = 0;
  SonareKey out_key{};

  CHECK(sonare_auto_tune(samples.data(), kLength, kRate, nullptr, nullptr, nullptr, &out_length,
                         &out_key) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  CHECK(sonare_auto_tune(samples.data(), kLength, kRate, nullptr, nullptr, &out, nullptr,
                         &out_key) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_length must not be NULL");
  CHECK(sonare_auto_tune(samples.data(), kLength, kRate, nullptr, nullptr, &out, &out_length,
                         nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_key must not be NULL");

  SonareKey key{};
  key.root = static_cast<SonarePitchClass>(12);
  CHECK(sonare_auto_tune(samples.data(), kLength, kRate, &key, nullptr, &out, &out_length,
                         &out_key) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "key.root must name a declared value");
  key.root = SONARE_PITCH_C;
  key.mode = static_cast<SonareMode>(99);
  CHECK(sonare_auto_tune(samples.data(), kLength, kRate, &key, nullptr, &out, &out_length,
                         &out_key) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "key.mode must name a declared value");

  SonarePitchCorrectionConfig config;
  sonare_pitch_correction_config_init(&config);
  config.retune_speed_ms = -1.0f;
  CHECK(sonare_auto_tune(samples.data(), kLength, kRate, nullptr, &config, &out, &out_length,
                         &out_key) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.retune_speed_ms must be finite and at least 0");
}

TEST_CASE("the note extraction entries refuse the track and config by name",
          "[c_api][engine_refusal]") {
  const std::vector<float> samples = make_samples();
  std::vector<float> f0(kFrames, 220.0f);
  std::vector<int32_t> voiced(kFrames, 1);
  std::vector<float> prob(kFrames, 0.9f);
  SonareNoteObjectsResult result{};

  CHECK(sonare_extract_notes(samples.data(), kLength, kRate, f0.data(), nullptr, voiced.data(),
                             kFrames, 100.0f, nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  CHECK(sonare_extract_notes(samples.data(), kLength, kRate, f0.data(), nullptr, nullptr, kFrames,
                             100.0f, nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "voiced_prob and voiced must not both be NULL");
  CHECK(sonare_extract_notes(samples.data(), kLength, kRate, nullptr, nullptr, voiced.data(),
                             kFrames, 100.0f, nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "f0_hz must not be NULL");
  CHECK(sonare_extract_notes(samples.data(), kLength, kRate, f0.data(), nullptr, voiced.data(), 0,
                             100.0f, nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_frames must be an integer in [1, 2147483647]");
  CHECK(sonare_extract_notes(samples.data(), kLength, kRate, f0.data(), nullptr, voiced.data(),
                             kFrames, 0.0f, nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "frame_rate must be finite and greater than 0");
  std::vector<float> bad_prob(kFrames, 1.5f);
  CHECK(sonare_extract_notes(samples.data(), kLength, kRate, f0.data(), bad_prob.data(), nullptr,
                             kFrames, 100.0f, nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "voiced_prob must be finite and in [0, 1]");

  SonareNoteExtractorConfig config{};
  CHECK(sonare_extract_notes(samples.data(), kLength, kRate, f0.data(), nullptr, voiced.data(),
                             kFrames, 100.0f, &config, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.struct_size must be set by sonare_note_extractor_config_init");
  sonare_note_extractor_config_init(&config);
  config.segmentation_threshold_cents = 0.0f;
  CHECK(sonare_extract_notes(samples.data(), kLength, kRate, f0.data(), nullptr, voiced.data(),
                             kFrames, 100.0f, &config, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.segmentation_threshold_cents must be finite and greater than 0");
  config.segmentation_threshold_cents = 50.0f;
  config.min_note_ms = -1.0f;
  CHECK(sonare_extract_notes(samples.data(), kLength, kRate, f0.data(), nullptr, voiced.data(),
                             kFrames, 100.0f, &config, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.min_note_ms must be finite and at least 0");
  config.min_note_ms = 30.0f;
  config.reference_hz = 0.0f;
  CHECK(sonare_extract_notes(samples.data(), kLength, kRate, f0.data(), nullptr, voiced.data(),
                             kFrames, 100.0f, &config, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.reference_hz must be finite and greater than 0");
  config.reference_hz = 440.0f;
  config.voiced_threshold = 2.0f;
  CHECK(sonare_extract_notes(samples.data(), kLength, kRate, f0.data(), nullptr, voiced.data(),
                             kFrames, 100.0f, &config, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.voiced_threshold must be finite and in [0, 1]");
}

TEST_CASE("split and merge refuse their indices, notes and edits by name",
          "[c_api][engine_refusal]") {
  const std::vector<float> samples = make_samples();
  std::vector<float> f0(kFrames, 220.0f);
  std::vector<int32_t> voiced(kFrames, 1);
  SonareNoteObjectsResult result{};
  SonareNoteObject note{};
  sonare_note_edit_init(&note.edit);
  note.frame_start = 0;
  note.frame_end = 8;

  auto split = [&](const SonareNoteObject* notes, size_t note_count, const float* envelopes,
                   size_t envelope_count, size_t index) {
    return sonare_split_note(samples.data(), kLength, kRate, f0.data(), nullptr, voiced.data(),
                             kFrames, 100.0f, nullptr, notes, note_count, envelopes, envelope_count,
                             index, 4, &result);
  };
  auto merge = [&](size_t note_count, size_t first, size_t last) {
    return sonare_merge_notes(samples.data(), kLength, kRate, f0.data(), nullptr, voiced.data(),
                              kFrames, 100.0f, nullptr, &note, note_count, nullptr, 0, first, last,
                              &result);
  };

  CHECK(sonare_split_note(samples.data(), kLength, kRate, f0.data(), nullptr, voiced.data(),
                          kFrames, 100.0f, nullptr, &note, 1, nullptr, 0, 0, 4,
                          nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  CHECK(split(&note, 1, nullptr, 0, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "index must be less than note_count");
  CHECK(split(nullptr, 1, nullptr, 0, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "notes must not be NULL when note_count is not 0");

  CHECK(merge(1, 1, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "first must be less than last");
  CHECK(merge(1, 0, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "last must be less than note_count");

  const float pool[2] = {1.0f, 1.0f};
  SonareNoteObject edited = note;
  edited.edit.envelope_count = 2;
  CHECK(split(&edited, 1, nullptr, 2, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "envelopes must not be NULL when notes[0].edit.envelope_count is not 0");
  edited.edit.envelope_offset = -1;
  CHECK(split(&edited, 1, pool, 2, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "notes[0].edit.envelope_offset must be an integer of at least 0");
  edited.edit.envelope_offset = 3;
  CHECK(split(&edited, 1, pool, 2, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "notes[0].edit.envelope_offset must not exceed envelope_count");
  edited.edit.envelope_offset = 1;
  CHECK(split(&edited, 1, pool, 2, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "notes[0].edit.envelope_offset + notes[0].edit.envelope_count must not exceed "
        "envelope_count");

  SonareNoteObject invalid = note;
  invalid.edit.gain_db = std::numeric_limits<float>::quiet_NaN();
  CHECK(split(&invalid, 1, nullptr, 0, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "notes[0].edit must hold finite values, a positive time_stretch_ratio and a non-negative "
        "envelope whose product with the gain stays finite");
}

TEST_CASE("sonare_render_notes refuses its notes and track by name", "[c_api][engine_refusal]") {
  const std::vector<float> samples = make_samples();
  std::vector<float> f0(kFrames, 220.0f);
  std::vector<int32_t> voiced(kFrames, 1);
  float* out = nullptr;
  size_t out_length = 0;
  SonareNoteObject note{};
  sonare_note_edit_init(&note.edit);
  note.onset_sample = 0;
  note.offset_sample = 1000;

  auto render = [&](const SonareNoteObject* notes, size_t note_count, const float* f0_hz,
                    const int32_t* voiced_flags, size_t n_frames) {
    return sonare_render_notes(samples.data(), kLength, kRate, notes, note_count, nullptr, 0, f0_hz,
                               voiced_flags, n_frames, 100.0f, nullptr, &out, &out_length);
  };

  CHECK(render(nullptr, 1, nullptr, nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "notes must not be NULL when note_count is not 0");
  CHECK(render(nullptr, 0, nullptr, voiced.data(), 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "f0_hz must not be NULL when voiced is not NULL");
  CHECK(render(nullptr, 0, nullptr, nullptr, kFrames) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "f0_hz must not be NULL when n_frames is not 0");

  note.frame_start = -1;
  note.frame_end = 4;
  CHECK(render(&note, 1, f0.data(), voiced.data(), kFrames) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "notes[0].frame_start must be an integer in [0, 2147483647]");
  note.frame_start = 3;
  note.frame_end = 2;
  CHECK(render(&note, 1, f0.data(), voiced.data(), kFrames) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "notes[0].frame_end must not be less than notes[0].frame_start");
  note.frame_start = 0;
  note.frame_end = 20;
  CHECK(render(&note, 1, f0.data(), voiced.data(), kFrames) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "notes[0].frame_end must not exceed n_frames");

  note.frame_end = 4;
  note.edit.vibrato_depth_change = 1.0f;
  CHECK(render(&note, 1, nullptr, nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "notes[0].edit.vibrato_depth_change and notes[0].edit.drift_change must be 0 when f0_hz is "
        "NULL");
}

TEST_CASE("sonare_decompose_note_pitch refuses its arguments by name", "[c_api][engine_refusal]") {
  std::vector<float> f0(kFrames, 220.0f);
  std::vector<int32_t> voiced(kFrames, 1);
  SonarePitchDecompositionResult result{};

  CHECK(sonare_decompose_note_pitch(f0.data(), voiced.data(), kFrames, 100.0f, 220.0f, 0.0f,
                                    nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  CHECK(sonare_decompose_note_pitch(nullptr, voiced.data(), kFrames, 100.0f, 220.0f, 0.0f,
                                    &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "f0_hz must not be NULL when voiced is not NULL");
  CHECK(sonare_decompose_note_pitch(f0.data(), voiced.data(), kFrames, 100.0f, -1.0f, 0.0f,
                                    &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "median_hz must be finite and at least 0");
  CHECK(sonare_decompose_note_pitch(f0.data(), voiced.data(), kFrames, 100.0f, 220.0f, -1.0f,
                                    &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "vibrato_cutoff_hz must be finite and at least 0");
}

TEST_CASE("the note-target entries refuse their arguments by name", "[c_api][engine_refusal]") {
  sonare_note_target_assign_config_init(nullptr);

  SonareNoteTargetAssignConfig config;
  sonare_note_target_assign_config_init(&config);
  SonareNoteObject note{};
  sonare_note_edit_init(&note.edit);
  SonareNoteTarget target{0.0, 1.0, 60.0f};
  size_t assigned = 0;

  CHECK(sonare_assign_note_targets(&note, 1, kRate, &target, 1, &config, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_assigned_count must not be NULL");
  CHECK(sonare_assign_note_targets(nullptr, 1, kRate, &target, 1, &config, &assigned) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "notes must not be NULL when note_count is not 0");
  CHECK(sonare_assign_note_targets(&note, 1, 0, &target, 1, &config, &assigned) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "sample_rate must be an integer in [1, 2147483647]");
  CHECK(sonare_assign_note_targets(&note, 1, kRate, nullptr, 1, &config, &assigned) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "targets must not be NULL when target_count is not 0");

  SonareNoteTargetAssignConfig bad{};
  CHECK(sonare_assign_note_targets(&note, 1, kRate, &target, 1, &bad, &assigned) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "config.struct_size must be set by sonare_note_target_assign_config_init");
  bad = config;
  bad.unmatched_policy = 7;
  CHECK(sonare_assign_note_targets(&note, 1, kRate, &target, 1, &bad, &assigned) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.unmatched_policy must be one of 0, 1, 2");
  bad = config;
  bad.min_overlap_ratio = 2.0f;
  CHECK(sonare_assign_note_targets(&note, 1, kRate, &target, 1, &bad, &assigned) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.min_overlap_ratio must be finite and in [0, 1]");
  bad = config;
  bad.max_correction_semitones = -1.0f;
  CHECK(sonare_assign_note_targets(&note, 1, kRate, &target, 1, &bad, &assigned) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.max_correction_semitones must be finite and at least 0");

  SonareNoteTarget broken = target;
  broken.start_sec = std::numeric_limits<double>::quiet_NaN();
  CHECK(sonare_assign_note_targets(&note, 1, kRate, &broken, 1, &config, &assigned) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "targets[0].start_sec must be finite");
  broken = target;
  broken.end_sec = std::numeric_limits<double>::infinity();
  CHECK(sonare_assign_note_targets(&note, 1, kRate, &broken, 1, &config, &assigned) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "targets[0].end_sec must be finite");
  broken = target;
  broken.target_midi = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_assign_note_targets(&note, 1, kRate, &broken, 1, &config, &assigned) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "targets[0].target_midi must be finite and in [-3.4028235e38, 3.4028235e38]");
}

TEST_CASE("a refusal of a value left out renders NULL, None or omitted by the spelling asked for",
          "[c_api][engine_refusal]") {
  const int32_t voiced[] = {1, 1};
  SonarePitchDecompositionResult result{};
  REQUIRE(sonare_decompose_note_pitch(nullptr, voiced, 2, 100.0f, 0.0f, 5.0f, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "f0_hz must not be NULL when voiced is not NULL");
  const char* subjects[] = {"f0_hz", "voiced"};
  const struct {
    SonarePresenceSpelling spelling;
    const char* text;
  } kRows[] = {
      {SONARE_PRESENCE_SPELLING_C, "f0_hz must not be NULL when voiced is not NULL"},
      {SONARE_PRESENCE_SPELLING_PYTHON, "f0_hz must not be None when voiced is not None"},
      {SONARE_PRESENCE_SPELLING_JS, "f0_hz must be given when voiced is given"},
  };
  for (const auto& row : kRows) {
    char* text = nullptr;
    REQUIRE(sonare_last_refusal_render(subjects, nullptr, 2, row.spelling, &text) == SONARE_OK);
    CHECK(std::string(text) == row.text);
    sonare_free_string(text);
  }
  char* text = reinterpret_cast<char*>(0x1);
  CHECK(sonare_last_refusal_render(subjects, nullptr, 2, static_cast<SonarePresenceSpelling>(3),
                                   &text) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(text == nullptr);
  CHECK(last_message() == "f0_hz must not be NULL when voiced is not NULL");
}

#endif  // defined(SONARE_WITH_PITCH_EDITOR)
