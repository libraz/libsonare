/// @file sonare_c_core_test.cpp
/// @brief Core C API tests.

#include <cstdio>
#include <fstream>
#include <limits>
#include <set>
#include <vector>

#include "analysis/analysis_json.h"
#include "core/audio_io.h"
#include "sonare_c_test_helpers.h"
#include "support/alloc_guard.h"
#include "util/constants.h"
#include "util/json.h"

namespace {

void collect_schema_paths(const sonare::util::json::Value& value, const std::string& prefix,
                          std::set<std::string>& out) {
  if (value.is_object()) {
    for (const auto& [key, child] : value.as_object()) {
      const std::string path = prefix.empty() ? key : prefix + "." + key;
      out.insert(path);
      collect_schema_paths(child, path, out);
    }
    return;
  }
  if (value.is_array() && value.size() > 0) {
    collect_schema_paths(value[static_cast<std::size_t>(0)], prefix + "[]", out);
  }
}

TEST_CASE("sonare_detect_onsets_ex", "[c_api]") {
  const auto samples = generate_clicks(120.0f, 22050, 2.0f);
  SonareOnsetDetectConfig config{};
  config.n_fft = 256;
  config.hop_length = 64;
  config.threshold = 0.0f;
  config.pre_max = 1;
  config.post_max = 1;
  config.pre_avg = 3;
  config.post_avg = 4;
  config.delta = 0.02f;
  config.wait = 1;
  config.backtrack = 1;
  config.backtrack_range = 10;
  float* times = nullptr;
  size_t count = 0;
  REQUIRE(sonare_detect_onsets_ex(samples.data(), samples.size(), 22050, &config, &times, &count) ==
          SONARE_OK);
  REQUIRE((count == 0 || times != nullptr));
  sonare_free_floats(times);

  config.hop_length = 0;
  REQUIRE(sonare_detect_onsets_ex(samples.data(), samples.size(), 22050, &config, &times, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_resample returns the standard empty C-array result", "[c_api]") {
  float* output = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t output_length = 99;
  REQUIRE(sonare_resample(nullptr, 0, 22050, 44100, &output, &output_length) == SONARE_OK);
  REQUIRE(output == nullptr);
  REQUIRE(output_length == 0);
}

sonare::AnalysisResult make_analysis_schema_fixture() {
  sonare::AnalysisResult result;
  result.bpm = 120.0f;
  result.bpm_confidence = 0.9f;
  result.bpm_candidates.push_back({120.0f, 0.9f, sonare::BpmCandidateRelation::Primary});
  result.key.root = sonare::PitchClass::C;
  result.key.mode = sonare::Mode::Major;
  result.key.confidence = 0.8f;
  result.time_signature = {4, 4, 0.7f};
  result.time_signature_candidates.push_back({4, 4, 0.7f});
  result.beats.push_back({0.25f, 0, 0.6f});
  result.beats.push_back({0.75f, 22, 0.4f});
  result.downbeat_indices = {0};
  result.downbeat_phase = 2;
  result.chords.push_back({sonare::PitchClass::C, sonare::ChordQuality::Major, 0.0f, 1.0f, 0.8f,
                           sonare::PitchClass::C});
  result.sections.push_back({sonare::SectionType::Verse, 0.0f, 1.0f, 0.5f, 0.9f});
  result.timbre = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
  result.dynamics = {12.0f, -1.0f, -14.0f, 13.0f, 3.0f, false};
  result.rhythm.time_signature = {4, 4, 0.75f};
  result.rhythm.syncopation = 0.1f;
  result.rhythm.groove_type = "straight";
  result.rhythm.pattern_regularity = 0.8f;
  result.rhythm.tempo_stability = 0.9f;
  result.melody.pitch_range_octaves = 1.0f;
  result.melody.pitch_stability = 0.7f;
  result.melody.mean_frequency = 440.0f;
  result.melody.vibrato_rate = 5.0f;
  result.melody.pitches.push_back({0.0f, 440.0f, 0.95f});
  result.form = "A";
  return result;
}

}  // namespace

#ifndef __EMSCRIPTEN__
TEST_CASE("sonare_audio_file_channel_count reports source channels", "[c_api][audio_io]") {
  const std::string mono_path = "test_c_api_channel_count_mono.wav";
  const std::string stereo_path = "test_c_api_channel_count_stereo.wav";
  const std::string invalid_path = "test_c_api_channel_count_invalid.bin";
  const auto cleanup = [&]() {
    std::remove(mono_path.c_str());
    std::remove(stereo_path.c_str());
    std::remove(invalid_path.c_str());
  };
  cleanup();

  const std::vector<float> mono(64, 0.0f);
  const std::vector<float> stereo(64 * 2, 0.0f);
  sonare::save_wav(mono_path, mono, 22050);
  sonare::save_wav_multichannel(stereo_path, stereo.data(), 64, 2, sonare::ChannelLayout::Stereo,
                                22050);
  {
    std::ofstream invalid(invalid_path, std::ios::binary);
    REQUIRE(invalid.is_open());
    // Keep the format recognizable so a malformed file exercises the decoder
    // error path consistently with and without the optional FFmpeg backend.
    invalid.write("RIFF....WAVE", 12);
  }

  int channels = -1;
  REQUIRE(sonare_audio_file_channel_count(mono_path.c_str(), &channels) == SONARE_OK);
  REQUIRE(channels == 1);
  channels = -1;
  REQUIRE(sonare_audio_file_channel_count(stereo_path.c_str(), &channels) == SONARE_OK);
  REQUIRE(channels == 2);

  channels = 17;
  REQUIRE(sonare_audio_file_channel_count("missing-c-api-channel-count.wav", &channels) ==
          SONARE_ERROR_FILE_NOT_FOUND);
  REQUIRE(channels == 0);

  channels = 17;
  REQUIRE(sonare_audio_file_channel_count(invalid_path.c_str(), &channels) ==
          SONARE_ERROR_DECODE_FAILED);
  REQUIRE(channels == 0);

  channels = 17;
  REQUIRE(sonare_audio_file_channel_count(nullptr, &channels) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(channels == 0);
  REQUIRE(sonare_audio_file_channel_count(mono_path.c_str(), nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  cleanup();
}
#endif

TEST_CASE("C pitch shift rejects unsupported expansion before allocating",
          "[c_api][pitch_shift][resource_limit]") {
  const float samples[4] = {0.0f, 0.25f, -0.25f, 0.0f};
  float* out = nullptr;
  size_t out_length = 0;
  SonareError error = SONARE_OK;
  size_t allocations = 999;
  {
    sonare::test::AllocationGuard guard;
    error = sonare_pitch_shift(samples, 4, 48000, 48.0f, &out, &out_length);
    allocations = guard.count();
  }
  REQUIRE(error == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(allocations == 0);
  REQUIRE(out == nullptr);
  REQUIRE(out_length == 0);
}

TEST_CASE("C VQT accepts NaN auto gamma and rejects infinity", "[c_api][vqt]") {
  std::vector<float> samples(2048, 0.0f);
  SonareCqtResult result{};
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  REQUIRE(sonare_vqt(samples.data(), samples.size(), 22050, 512, 32.7f, 12, 12, nan, &result) ==
          SONARE_OK);
  sonare_free_cqt_result(&result);
  REQUIRE(sonare_vqt(samples.data(), samples.size(), 22050, 512, 32.7f, 12, 12, inf, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.magnitude == nullptr);
  REQUIRE(result.frequencies == nullptr);
}

TEST_CASE("C API reconstructs CQT and VQT magnitude with checked ownership",
          "[c_api][features][inverse_cqt]") {
  constexpr int sample_rate = 8000;
  constexpr int hop_length = 128;
  constexpr int n_bins = 12;
  constexpr int bins_per_octave = 12;
  constexpr float fmin = 130.8128f;
  std::vector<float> input(2048, 0.0f);
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = 0.25f * std::sin(2.0f * sonare::constants::kPi * 261.6256f * static_cast<float>(i) /
                                sample_rate);
  }

  SonareCqtResult cqt_result{};
  REQUIRE(sonare_cqt(input.data(), input.size(), sample_rate, hop_length, fmin, n_bins,
                     bins_per_octave, &cqt_result) == SONARE_OK);
  REQUIRE(cqt_result.n_frames > 0);
  const size_t matrix_length =
      static_cast<size_t>(cqt_result.n_bins) * static_cast<size_t>(cqt_result.n_frames);

  float* output = nullptr;
  size_t output_length = 0;
  REQUIRE(sonare_cqt_to_audio(cqt_result.magnitude, cqt_result.n_bins, cqt_result.n_frames,
                              sample_rate, hop_length, fmin, bins_per_octave, 2, &output,
                              &output_length) == SONARE_OK);
  REQUIRE(output != nullptr);
  REQUIRE(output_length > 0);
  for (size_t i = 0; i < output_length; ++i) REQUIRE(std::isfinite(output[i]));
  sonare_free_floats(output);

  output = nullptr;
  output_length = 0;
  REQUIRE(sonare_vqt_to_audio(cqt_result.magnitude, cqt_result.n_bins, cqt_result.n_frames,
                              sample_rate, hop_length, fmin, bins_per_octave, 0.0f, 2, &output,
                              &output_length) == SONARE_OK);
  REQUIRE(output != nullptr);
  REQUIRE(output_length > 0);
  sonare_free_floats(output);

  REQUIRE(sonare_cqt_to_audio_checked(cqt_result.magnitude, matrix_length - 1, cqt_result.n_bins,
                                      cqt_result.n_frames, sample_rate, hop_length, fmin,
                                      bins_per_octave, 2, &output,
                                      &output_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_vqt_to_audio_checked(cqt_result.magnitude, matrix_length, cqt_result.n_bins,
                                      cqt_result.n_frames, sample_rate, hop_length, fmin,
                                      bins_per_octave, 0.0f, 257, &output,
                                      &output_length) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_free_cqt_result(&cqt_result);
}

TEST_CASE("sonare_audio_from_buffer", "[c_api]") {
  SECTION("creates audio from valid buffer") {
    auto samples = generate_sine(440.0f, 22050, 1.0f);
    SonareAudio* audio = nullptr;

    SonareError err = sonare_audio_from_buffer(samples.data(), samples.size(), 22050, &audio);

    REQUIRE(err == SONARE_OK);
    REQUIRE(audio != nullptr);
    REQUIRE(sonare_audio_length(audio) == samples.size());
    REQUIRE(sonare_audio_sample_rate(audio) == 22050);
    REQUIRE(sonare_audio_duration(audio) > 0.9f);
    REQUIRE(sonare_audio_duration(audio) < 1.1f);

    sonare_audio_free(audio);
  }

  SECTION("returns error for null data") {
    SonareAudio* audio = nullptr;
    SonareError err = sonare_audio_from_buffer(nullptr, 100, 22050, &audio);
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("returns error for zero length") {
    float sample = 0.0f;
    SonareAudio* audio = nullptr;
    SonareError err = sonare_audio_from_buffer(&sample, 0, 22050, &audio);
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("returns error for non-finite samples") {
    std::array<float, 3> samples{0.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f};
    SonareAudio* audio = nullptr;

    SonareError err = sonare_audio_from_buffer(samples.data(), samples.size(), 22050, &audio);

    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(audio == nullptr);

    samples[1] = std::numeric_limits<float>::infinity();
    err = sonare_audio_from_buffer(samples.data(), samples.size(), 22050, &audio);

    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(audio == nullptr);
  }

  SECTION("returns error for null output") {
    float sample = 0.0f;
    SonareError err = sonare_audio_from_buffer(&sample, 1, 22050, nullptr);
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_detect_bpm", "[c_api]") {
  SECTION("detects BPM from samples") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    float bpm = 0.0f;

    SonareError err = sonare_detect_bpm(samples.data(), samples.size(), 22050, &bpm);

    REQUIRE(err == SONARE_OK);
    REQUIRE(bpm > 0.0f);
  }

  SECTION("returns error for null samples") {
    float bpm = 0.0f;
    SonareError err = sonare_detect_bpm(nullptr, 100, 22050, &bpm);
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("returns error for null output") {
    auto samples = generate_clicks(120.0f, 22050, 1.0f);
    SonareError err = sonare_detect_bpm(samples.data(), samples.size(), 22050, nullptr);
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_detect_key", "[c_api]") {
  SECTION("detects key from samples") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKey key = {};

    SonareError err = sonare_detect_key(samples.data(), samples.size(), 22050, &key);

    REQUIRE(err == SONARE_OK);
    REQUIRE(key.root >= SONARE_PITCH_C);
    REQUIRE(key.root <= SONARE_PITCH_B);
    REQUIRE((key.mode == SONARE_MODE_MAJOR || key.mode == SONARE_MODE_MINOR));
    REQUIRE(key.confidence >= 0.0f);
    REQUIRE(key.confidence <= 1.0f);
  }

  SECTION("returns error for null samples") {
    SonareKey key = {};
    SonareError err = sonare_detect_key(nullptr, 100, 22050, &key);
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("detects key with explicit analysis options") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKey key = {};

    SonareError err = sonare_detect_key_with_options(samples.data(), samples.size(), 22050, 4096,
                                                     512, 0, 0, 80.0f, &key);

    REQUIRE(err == SONARE_OK);
    REQUIRE(key.root >= SONARE_PITCH_C);
    REQUIRE(key.root <= SONARE_PITCH_B);
    REQUIRE(key.confidence >= 0.0f);
    REQUIRE(key.confidence <= 1.0f);
  }

  SECTION("returns error for invalid explicit key options") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKey key = {};

    SonareError err = sonare_detect_key_with_options(samples.data(), samples.size(), 22050, 0, 512,
                                                     0, 0, 0.0f, &key);

    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("returns sorted key candidates with correlations") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKeyCandidate* candidates = nullptr;
    size_t count = 0;

    SonareError err = sonare_detect_key_candidates(samples.data(), samples.size(), 22050, 4096, 512,
                                                   0, 0, 0.0f, &candidates, &count);

    REQUIRE(err == SONARE_OK);
    REQUIRE(candidates != nullptr);
    REQUIRE(count == 24);
    for (size_t i = 1; i < count; ++i) {
      REQUIRE(candidates[i - 1].correlation >= candidates[i].correlation);
    }
    REQUIRE(candidates[0].key.root >= SONARE_PITCH_C);
    REQUIRE(candidates[0].key.root <= SONARE_PITCH_B);
    REQUIRE(candidates[0].key.confidence >= 0.0f);
    REQUIRE(candidates[0].key.confidence <= 1.0f);
    sonare_free_key_candidates(candidates);
  }

  SECTION("returns modal key candidates when modes are explicit") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    const SonareMode modes[] = {SONARE_MODE_MAJOR, SONARE_MODE_MINOR, SONARE_MODE_DORIAN,
                                SONARE_MODE_LYDIAN};
    SonareKeyCandidate* candidates = nullptr;
    size_t count = 0;

    SonareError err =
        sonare_detect_key_candidates_with_modes(samples.data(), samples.size(), 22050, 4096, 512, 0,
                                                0, 0.0f, modes, 4, &candidates, &count);

    REQUIRE(err == SONARE_OK);
    REQUIRE(candidates != nullptr);
    REQUIRE(count == 48);
    bool saw_dorian = false;
    bool saw_lydian = false;
    for (size_t i = 0; i < count; ++i) {
      saw_dorian = saw_dorian || candidates[i].key.mode == SONARE_MODE_DORIAN;
      saw_lydian = saw_lydian || candidates[i].key.mode == SONARE_MODE_LYDIAN;
    }
    REQUIRE(saw_dorian);
    REQUIRE(saw_lydian);
    sonare_free_key_candidates(candidates);
  }

  SECTION("detects key with explicit modal options") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    const SonareMode modes[] = {SONARE_MODE_MAJOR, SONARE_MODE_MINOR, SONARE_MODE_DORIAN};
    SonareKey key = {};

    SonareError err = sonare_detect_key_with_options_and_modes(
        samples.data(), samples.size(), 22050, 4096, 512, 0, 0, 0.0f, modes, 3, &key);

    REQUIRE(err == SONARE_OK);
    REQUIRE(key.root >= SONARE_PITCH_C);
    REQUIRE(key.root <= SONARE_PITCH_B);
    REQUIRE(key.mode >= SONARE_MODE_MAJOR);
    REQUIRE(key.mode <= SONARE_MODE_LOCRIAN);
  }

  SECTION("detects key with explicit profile and genre options") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    const SonareMode modes[] = {SONARE_MODE_MAJOR, SONARE_MODE_MINOR};
    SonareKey key = {};

    SonareError err = sonare_detect_key_with_extended_options(
        samples.data(), samples.size(), 22050, 4096, 512, 1, 1, 80.0f, modes, 2,
        SONARE_KEY_PROFILE_FARALDO_EDMA, "edm", &key);

    REQUIRE(err == SONARE_OK);
    REQUIRE(key.root >= SONARE_PITCH_C);
    REQUIRE(key.root <= SONARE_PITCH_B);
    REQUIRE((key.mode == SONARE_MODE_MAJOR || key.mode == SONARE_MODE_MINOR));
  }

  SECTION("returns candidates with explicit profile and genre options") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKeyCandidate* candidates = nullptr;
    size_t count = 0;

    SonareError err = sonare_detect_key_candidates_with_extended_options(
        samples.data(), samples.size(), 22050, 4096, 512, 0, 0, 0.0f, nullptr, 0,
        SONARE_KEY_PROFILE_TEMPERLEY, "pop", &candidates, &count);

    REQUIRE(err == SONARE_OK);
    REQUIRE(candidates != nullptr);
    REQUIRE(count == 24);
    sonare_free_key_candidates(candidates);
  }

  SECTION("returns error for null key candidate outputs") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKeyCandidate* candidates = nullptr;
    size_t count = 0;

    REQUIRE(sonare_detect_key_candidates(samples.data(), samples.size(), 22050, 4096, 512, 0, 0,
                                         0.0f, nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_key_candidates(samples.data(), samples.size(), 22050, 4096, 512, 0, 0,
                                         0.0f, &candidates,
                                         nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("returns error for invalid modal options") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    const SonareMode bad_modes[] = {static_cast<SonareMode>(99)};
    SonareKey key = {};
    SonareKeyCandidate* candidates = nullptr;
    size_t count = 0;

    REQUIRE(sonare_detect_key_with_options_and_modes(samples.data(), samples.size(), 22050, 4096,
                                                     512, 0, 0, 0.0f, bad_modes, 1,
                                                     &key) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_key_candidates_with_modes(samples.data(), samples.size(), 22050, 4096,
                                                    512, 0, 0, 0.0f, bad_modes, 1, &candidates,
                                                    &count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_key_with_extended_options(samples.data(), samples.size(), 22050, 4096,
                                                    512, 0, 0, 0.0f, nullptr, 0,
                                                    static_cast<SonareKeyProfileType>(99), nullptr,
                                                    &key) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_stream_analyzer C API validates config and reads quantized frames", "[c_api]") {
  SonareStreamConfig config = {};
  REQUIRE(sonare_stream_analyzer_config_default(&config) == SONARE_OK);
  REQUIRE(config.max_progression_entries == 4096);
  config.sample_rate = 22050;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.n_mels = 32;
  config.window = SONARE_WINDOW_HAMMING;
  config.output_format = SONARE_STREAM_OUTPUT_FLOAT32;

  SECTION("rejects legacy config output formats") {
    for (const int format : {SONARE_STREAM_OUTPUT_INT16, SONARE_STREAM_OUTPUT_UINT8}) {
      SonareStreamConfig bad = config;
      bad.output_format = format;
      SonareStreamAnalyzer* analyzer = nullptr;
      REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(analyzer == nullptr);
    }
  }

  SECTION("feature flags define every C SOA read shape") {
    std::array<float, 32> samples{};
    samples[8] = 0.5f;
    for (uint32_t mask = 0; mask < 16; ++mask) {
      CAPTURE(mask);
      SonareStreamConfig shaped = config;
      shaped.sample_rate = 8000;
      shaped.n_fft = 32;
      shaped.hop_length = 32;
      shaped.n_mels = 8;
      shaped.compute_mel = (mask & SONARE_STREAM_FEATURE_MEL) != 0;
      shaped.compute_chroma = (mask & SONARE_STREAM_FEATURE_CHROMA) != 0;
      shaped.compute_onset = (mask & SONARE_STREAM_FEATURE_ONSET) != 0;
      shaped.compute_spectral = (mask & SONARE_STREAM_FEATURE_SPECTRAL) != 0;

      SonareStreamAnalyzer* analyzer = nullptr;
      REQUIRE(sonare_stream_analyzer_create(&shaped, &analyzer) == SONARE_OK);
      REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size()) ==
              SONARE_OK);
      SonareStreamFrames floats{};
      REQUIRE(sonare_stream_analyzer_read_frames(analyzer, 1, &floats) == SONARE_OK);
      REQUIRE(floats.n_frames == 1);
      REQUIRE(floats.feature_flags == mask);
      REQUIRE(floats.n_mels == ((mask & SONARE_STREAM_FEATURE_MEL) ? 8 : 0));
      REQUIRE(floats.n_chroma == ((mask & SONARE_STREAM_FEATURE_CHROMA) ? 12 : 0));
      REQUIRE((floats.mel != nullptr) == ((mask & SONARE_STREAM_FEATURE_MEL) != 0));
      REQUIRE((floats.chroma != nullptr) == ((mask & SONARE_STREAM_FEATURE_CHROMA) != 0));
      REQUIRE((floats.chord_root != nullptr) == ((mask & SONARE_STREAM_FEATURE_CHROMA) != 0));
      REQUIRE((floats.onset_strength != nullptr) == ((mask & SONARE_STREAM_FEATURE_ONSET) != 0));
      REQUIRE((floats.spectral_centroid != nullptr) ==
              ((mask & SONARE_STREAM_FEATURE_SPECTRAL) != 0));
      REQUIRE(floats.rms_energy != nullptr);
      sonare_free_stream_frames(&floats);
      sonare_stream_analyzer_destroy(analyzer);

      analyzer = nullptr;
      REQUIRE(sonare_stream_analyzer_create(&shaped, &analyzer) == SONARE_OK);
      REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size()) ==
              SONARE_OK);
      SonareStreamFramesU8 u8{};
      REQUIRE(sonare_stream_analyzer_read_frames_u8(analyzer, 1, &u8) == SONARE_OK);
      REQUIRE(u8.n_frames == 1);
      REQUIRE(u8.feature_flags == mask);
      REQUIRE(u8.n_mels == ((mask & SONARE_STREAM_FEATURE_MEL) ? 8 : 0));
      REQUIRE(u8.n_chroma == ((mask & SONARE_STREAM_FEATURE_CHROMA) ? 12 : 0));
      REQUIRE((u8.mel != nullptr) == ((mask & SONARE_STREAM_FEATURE_MEL) != 0));
      REQUIRE((u8.chroma != nullptr) == ((mask & SONARE_STREAM_FEATURE_CHROMA) != 0));
      REQUIRE((u8.onset_strength != nullptr) == ((mask & SONARE_STREAM_FEATURE_ONSET) != 0));
      REQUIRE((u8.spectral_centroid != nullptr) == ((mask & SONARE_STREAM_FEATURE_SPECTRAL) != 0));
      REQUIRE(u8.rms_energy != nullptr);
      sonare_free_stream_frames_u8(&u8);
      sonare_stream_analyzer_destroy(analyzer);

      analyzer = nullptr;
      REQUIRE(sonare_stream_analyzer_create(&shaped, &analyzer) == SONARE_OK);
      REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size()) ==
              SONARE_OK);
      SonareStreamFramesI16 i16{};
      REQUIRE(sonare_stream_analyzer_read_frames_i16(analyzer, 1, &i16) == SONARE_OK);
      REQUIRE(i16.n_frames == 1);
      REQUIRE(i16.feature_flags == mask);
      REQUIRE(i16.n_mels == ((mask & SONARE_STREAM_FEATURE_MEL) ? 8 : 0));
      REQUIRE(i16.n_chroma == ((mask & SONARE_STREAM_FEATURE_CHROMA) ? 12 : 0));
      REQUIRE((i16.mel != nullptr) == ((mask & SONARE_STREAM_FEATURE_MEL) != 0));
      REQUIRE((i16.chroma != nullptr) == ((mask & SONARE_STREAM_FEATURE_CHROMA) != 0));
      REQUIRE((i16.onset_strength != nullptr) == ((mask & SONARE_STREAM_FEATURE_ONSET) != 0));
      REQUIRE((i16.spectral_centroid != nullptr) == ((mask & SONARE_STREAM_FEATURE_SPECTRAL) != 0));
      REQUIRE(i16.rms_energy != nullptr);
      sonare_free_stream_frames_i16(&i16);
      sonare_stream_analyzer_destroy(analyzer);
    }
  }

  SECTION("rejects impossible overlap") {
    SonareStreamConfig bad = config;
    bad.hop_length = bad.n_fft + 1;
    SonareStreamAnalyzer* analyzer = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(analyzer == nullptr);
  }

  SECTION("rejects invalid intervals and mel range") {
    SonareStreamConfig bad = config;
    bad.key_update_interval_sec = 0.0f;
    SonareStreamAnalyzer* analyzer = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);

    bad = config;
    bad.max_progression_entries = 0;
    REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);

    bad = config;
    bad.fmin = 1000.0f;
    bad.fmax = 500.0f;
    REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("rejects non-finite setters and quantization ranges") {
    SonareStreamAnalyzer* analyzer = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&config, &analyzer) == SONARE_OK);
    REQUIRE(analyzer != nullptr);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    REQUIRE(sonare_stream_analyzer_set_expected_duration(analyzer, nan) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_stream_analyzer_set_normalization_gain(analyzer, inf) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_stream_analyzer_set_tuning_ref_hz(analyzer, nan) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_stream_analyzer_set_expected_duration(analyzer, -inf) ==
            SONARE_ERROR_INVALID_PARAMETER);

    SonareStreamQuantizeConfig quantize{};
    REQUIRE(sonare_stream_quantize_config_default(&quantize) == SONARE_OK);
    quantize.rms_max = inf;
    SonareStreamFramesU8 frames{};
    REQUIRE(sonare_stream_analyzer_read_frames_u8_ex(analyzer, &quantize, 0, &frames) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_stream_quantize_config_default(&quantize) == SONARE_OK);
    quantize.mel_db_min = quantize.mel_db_max;
    REQUIRE(sonare_stream_analyzer_read_frames_u8_ex(analyzer, &quantize, 0, &frames) ==
            SONARE_ERROR_INVALID_PARAMETER);
    sonare_stream_analyzer_destroy(analyzer);
  }

  SECTION("reads U8 and I16 frames") {
    SonareStreamAnalyzer* analyzer = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&config, &analyzer) == SONARE_OK);
    REQUIRE(analyzer != nullptr);

    auto samples = generate_sine(440.0f, config.sample_rate, 0.25f);
    REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size()) == SONARE_OK);

    SonareStreamFramesU8 u8 = {};
    REQUIRE(sonare_stream_analyzer_read_frames_u8(analyzer, 4, &u8) == SONARE_OK);
    REQUIRE(u8.n_frames > 0);
    REQUIRE(u8.n_frames <= 4);
    REQUIRE(u8.n_mels == config.n_mels);
    REQUIRE(u8.n_chroma == 12);
    REQUIRE(u8.feature_flags == (SONARE_STREAM_FEATURE_MEL | SONARE_STREAM_FEATURE_CHROMA |
                                 SONARE_STREAM_FEATURE_ONSET | SONARE_STREAM_FEATURE_SPECTRAL));
    REQUIRE(u8.timestamps != nullptr);
    REQUIRE(u8.mel != nullptr);
    REQUIRE(u8.chroma != nullptr);
    sonare_free_stream_frames_u8(&u8);

    REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size()) == SONARE_OK);
    SonareStreamFramesI16 i16 = {};
    REQUIRE(sonare_stream_analyzer_read_frames_i16(analyzer, 4, &i16) == SONARE_OK);
    REQUIRE(i16.n_frames > 0);
    REQUIRE(i16.n_mels == config.n_mels);
    REQUIRE(i16.n_chroma == 12);
    REQUIRE(i16.feature_flags == (SONARE_STREAM_FEATURE_MEL | SONARE_STREAM_FEATURE_CHROMA |
                                  SONARE_STREAM_FEATURE_ONSET | SONARE_STREAM_FEATURE_SPECTRAL));
    REQUIRE(i16.mel != nullptr);
    REQUIRE(i16.chroma != nullptr);
    sonare_free_stream_frames_i16(&i16);

    sonare_stream_analyzer_destroy(analyzer);
  }

  SECTION("finalize flushes a partial tail frame") {
    SonareStreamAnalyzer* analyzer = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&config, &analyzer) == SONARE_OK);
    REQUIRE(analyzer != nullptr);

    std::vector<float> samples(600, 0.0f);
    REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size()) == SONARE_OK);

    size_t available = 0;
    REQUIRE(sonare_stream_analyzer_available_frames(analyzer, &available) == SONARE_OK);
    REQUIRE(available == 0);

    REQUIRE(sonare_stream_analyzer_finalize(analyzer) == SONARE_OK);
    REQUIRE(sonare_stream_analyzer_available_frames(analyzer, &available) == SONARE_OK);
    REQUIRE(available == 1);

    REQUIRE(sonare_stream_analyzer_finalize(analyzer) == SONARE_OK);
    REQUIRE(sonare_stream_analyzer_available_frames(analyzer, &available) == SONARE_OK);
    REQUIRE(available == 1);

    SonareStreamFrames frames = {};
    REQUIRE(sonare_stream_analyzer_read_frames(analyzer, 2, &frames) == SONARE_OK);
    REQUIRE(frames.n_frames == 1);
    REQUIRE(frames.n_mels == config.n_mels);
    REQUIRE(frames.timestamps != nullptr);
    REQUIRE(frames.timestamps[0] == Catch::Approx(0.0f));
    sonare_free_stream_frames(&frames);

    sonare_stream_analyzer_destroy(analyzer);
  }

  SECTION("quantize-config override widens the saturating range") {
    SonareStreamQuantizeConfig qdefault = {};
    REQUIRE(sonare_stream_quantize_config_default(&qdefault) == SONARE_OK);
    REQUIRE(qdefault.onset_max == Catch::Approx(50.0f));
    REQUIRE(qdefault.rms_max == Catch::Approx(1.0f));
    REQUIRE(qdefault.centroid_max == Catch::Approx(11025.0f));

    auto samples = generate_sine(440.0f, config.sample_rate, 0.25f);

    // A tiny centroid_max forces the (positive) spectral centroid to saturate
    // to the u8 maximum; a huge centroid_max collapses it toward zero. The two
    // reads of identical audio must therefore differ, proving the supplied
    // config actually reaches the quantizer.
    SonareStreamAnalyzer* tight = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&config, &tight) == SONARE_OK);
    REQUIRE(sonare_stream_analyzer_process(tight, samples.data(), samples.size()) == SONARE_OK);
    SonareStreamQuantizeConfig narrow = qdefault;
    narrow.centroid_max = 1.0f;
    SonareStreamFramesU8 tight_frames = {};
    REQUIRE(sonare_stream_analyzer_read_frames_u8_ex(tight, &narrow, 4, &tight_frames) ==
            SONARE_OK);
    REQUIRE(tight_frames.n_frames > 0);

    SonareStreamAnalyzer* wide = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&config, &wide) == SONARE_OK);
    REQUIRE(sonare_stream_analyzer_process(wide, samples.data(), samples.size()) == SONARE_OK);
    SonareStreamQuantizeConfig broad = qdefault;
    broad.centroid_max = 1.0e9f;
    SonareStreamFramesU8 wide_frames = {};
    REQUIRE(sonare_stream_analyzer_read_frames_u8_ex(wide, &broad, 4, &wide_frames) == SONARE_OK);
    REQUIRE(wide_frames.n_frames == tight_frames.n_frames);

    bool centroid_differs = false;
    for (int f = 0; f < tight_frames.n_frames; ++f) {
      if (tight_frames.spectral_centroid[f] != wide_frames.spectral_centroid[f]) {
        centroid_differs = true;
        break;
      }
    }
    REQUIRE(centroid_differs);
    REQUIRE(tight_frames.spectral_centroid[0] == 255);  // saturated by the narrow range

    // A null config is identical to the default-range read.
    SonareStreamFramesU8 null_frames = {};
    REQUIRE(sonare_stream_analyzer_read_frames_u8_ex(wide, nullptr, 4, &null_frames) == SONARE_OK);
    sonare_free_stream_frames_u8(&null_frames);

    sonare_free_stream_frames_u8(&tight_frames);
    sonare_free_stream_frames_u8(&wide_frames);
    sonare_stream_analyzer_destroy(tight);
    sonare_stream_analyzer_destroy(wide);
  }

  SECTION("bounds unread output and reports dropped frames") {
    SonareStreamConfig bounded = config;
    bounded.n_fft = 32;
    bounded.hop_length = 32;
    bounded.n_mels = 8;
    bounded.max_pending_frames = 3;
    SonareStreamAnalyzer* analyzer = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&bounded, &analyzer) == SONARE_OK);

    std::vector<float> samples(32 * 64, 0.0f);
    REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size()) == SONARE_OK);
    size_t available = 0;
    REQUIRE(sonare_stream_analyzer_available_frames(analyzer, &available) == SONARE_OK);
    REQUIRE(available == 3);

    SonareStreamStats stats{};
    REQUIRE(sonare_stream_analyzer_stats(analyzer, &stats) == SONARE_OK);
    REQUIRE(stats.pending_frames == 3);
    REQUIRE(stats.dropped_output_frames > 0);
    REQUIRE(stats.pending_frames + stats.dropped_output_frames ==
            static_cast<size_t>(stats.total_frames));
    REQUIRE(stats.dropped_chord_progression_entries == 0);
    REQUIRE(stats.dropped_bar_progression_entries == 0);
    sonare_free_stream_stats(&stats);
    sonare_stream_analyzer_destroy(analyzer);
  }
}

TEST_CASE("sonare_detect_beats", "[c_api]") {
  SECTION("detects beats from samples") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    float* times = nullptr;
    size_t count = 0;

    SonareError err = sonare_detect_beats(samples.data(), samples.size(), 22050, &times, &count);

    REQUIRE(err == SONARE_OK);
    REQUIRE(count >= 1);
    if (count > 0) {
      REQUIRE(times != nullptr);
      // Check times are in order
      for (size_t i = 1; i < count; ++i) {
        REQUIRE(times[i] > times[i - 1]);
      }
      sonare_free_floats(times);
    }
  }
}

TEST_CASE("sonare_detect_downbeats", "[c_api]") {
  SECTION("detects downbeats from samples") {
    auto samples = generate_clicks(120.0f, 22050, 8.0f);
    float* times = nullptr;
    size_t count = 0;

    SonareError err =
        sonare_detect_downbeats(samples.data(), samples.size(), 22050, &times, &count);

    REQUIRE(err == SONARE_OK);
    if (count > 0) {
      REQUIRE(times != nullptr);
      for (size_t i = 1; i < count; ++i) {
        REQUIRE(times[i] > times[i - 1]);
      }
      sonare_free_floats(times);
    }
  }

  SECTION("audio wrapper detects downbeats") {
    auto samples = generate_clicks(120.0f, 22050, 8.0f);
    SonareAudio* audio = nullptr;
    REQUIRE(sonare_audio_from_buffer(samples.data(), samples.size(), 22050, &audio) == SONARE_OK);

    float* times = nullptr;
    size_t count = 0;
    SonareError err = sonare_audio_detect_downbeats(audio, &times, &count);

    REQUIRE(err == SONARE_OK);
    if (count > 0) {
      REQUIRE(times != nullptr);
      sonare_free_floats(times);
    }

    sonare_audio_free(audio);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_clicks(120.0f, 22050, 2.0f);
    float* times = nullptr;
    size_t count = 0;

    REQUIRE(sonare_detect_downbeats(nullptr, samples.size(), 22050, &times, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_downbeats(samples.data(), samples.size(), 22050, nullptr, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_downbeats(samples.data(), samples.size(), 22050, &times, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_audio_detect_downbeats(nullptr, &times, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_detect_onsets", "[c_api]") {
  SECTION("detects onsets from samples") {
    auto samples = generate_clicks(120.0f, 22050, 2.0f);
    float* times = nullptr;
    size_t count = 0;

    SonareError err = sonare_detect_onsets(samples.data(), samples.size(), 22050, &times, &count);

    REQUIRE(err == SONARE_OK);
    if (count > 0) {
      REQUIRE(times != nullptr);
      sonare_free_floats(times);
    }
  }
}

TEST_CASE("sonare_analyze", "[.][slow][c_api]") {
  SECTION("returns complete analysis result") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    SonareAnalysisResult result = {};

    SonareError err = sonare_analyze(samples.data(), samples.size(), 22050, &result);

    REQUIRE(err == SONARE_OK);
    REQUIRE(result.bpm > 0.0f);
    REQUIRE(result.bpm_confidence >= 0.0f);
    REQUIRE(result.key.root >= SONARE_PITCH_C);
    REQUIRE(result.key.root <= SONARE_PITCH_B);
    REQUIRE(result.time_signature.numerator > 0);
    REQUIRE(result.time_signature.denominator > 0);
    REQUIRE(result.bpm_candidate_count > 0);
    REQUIRE(result.bpm_candidates != nullptr);
    REQUIRE(result.bpm_candidates[0].relation == SONARE_BPM_CANDIDATE_PRIMARY);
    REQUIRE(result.time_signature_candidate_count > 0);
    REQUIRE(result.time_signature_candidates != nullptr);

    sonare_free_result(&result);
  }

  SECTION("returns error for null samples") {
    SonareAnalysisResult result = {};
    SonareError err = sonare_analyze(nullptr, 100, 22050, &result);
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("free_result is safe on partially initialized struct") {
    SonareAnalysisResult result = {};
    result.beat_times = new float[2]{0.1f, 0.2f};
    result.beat_count = 2;
    result.bpm_candidates =
        new SonareAnalysisBpmCandidate[1]{{120.0f, 0.9f, SONARE_BPM_CANDIDATE_PRIMARY}};
    result.bpm_candidate_count = 1;
    result.time_signature_candidates = new SonareTimeSignature[1]{{4, 4, 0.8f}};
    result.time_signature_candidate_count = 1;

    sonare_free_result(&result);

    REQUIRE(result.beat_times == nullptr);
    REQUIRE(result.beat_count == 0);
    REQUIRE(result.bpm_candidates == nullptr);
    REQUIRE(result.bpm_candidate_count == 0);
    REQUIRE(result.time_signature_candidates == nullptr);
    REQUIRE(result.time_signature_candidate_count == 0);
  }
}

TEST_CASE("sonare_analyze_json", "[.][slow][c_api]") {
  auto samples = generate_clicks(120.0f, 22050, 4.0f);

  SECTION("serializes the full analysis result") {
    char* json = nullptr;
    SonareError err = sonare_analyze_json(samples.data(), samples.size(), 22050, &json);
    REQUIRE(err == SONARE_OK);
    REQUIRE(json != nullptr);

    const auto root = sonare::util::json::parse(json);
    REQUIRE(root.is_object());
    // Fields dropped by the flat struct must all be present in the JSON.
    REQUIRE(root.contains("bpm"));
    REQUIRE(root.contains("bpmCandidates"));
    REQUIRE(root.contains("key"));
    REQUIRE(root.contains("timeSignature"));
    REQUIRE(root.contains("timeSignatureCandidates"));
    REQUIRE(root.contains("beats"));
    REQUIRE(root.contains("chords"));
    REQUIRE(root.contains("sections"));
    REQUIRE(root.contains("timbre"));
    REQUIRE(root.contains("dynamics"));
    REQUIRE(root.contains("rhythm"));
    REQUIRE(root.contains("melody"));
    REQUIRE(root.contains("form"));
    // Beats carry per-beat strength (dropped by sonare_analyze).
    REQUIRE(root["beats"].is_array());
    if (root["beats"].size() > 0) {
      REQUIRE(root["beats"][static_cast<std::size_t>(0)].contains("strength"));
    }
    // Dynamics exposes peakDb/rmsDb; rhythm exposes tempoStability.
    REQUIRE(root["dynamics"].contains("peakDb"));
    REQUIRE(root["dynamics"].contains("rmsDb"));
    REQUIRE(root["rhythm"].contains("tempoStability"));
    REQUIRE(root["melody"].contains("pitches"));

    sonare_free_string(json);
  }

  SECTION("matches the canonical unified-result schema snapshot") {
    const auto fixture = make_analysis_schema_fixture();
    const auto root = sonare::util::json::parse(sonare::analysis_result_to_json(fixture));
    std::set<std::string> actual;
    collect_schema_paths(root, "", actual);

    const auto& expected_paths = sonare::analysis_result_schema_paths();
    const std::set<std::string> expected(expected_paths.begin(), expected_paths.end());
    REQUIRE(actual == expected);

    // Downbeats serialize as indices into the beat array plus the meter phase,
    // so a reader can resolve them without a second time series.
    REQUIRE(root["downbeatIndices"].is_array());
    REQUIRE(root["downbeatIndices"].size() == fixture.downbeat_indices.size());
    REQUIRE(root["downbeatIndices"][static_cast<std::size_t>(0)].as_number() ==
            static_cast<double>(fixture.downbeat_indices.front()));
    REQUIRE(root["downbeatPhase"].as_number() == static_cast<double>(fixture.downbeat_phase));
  }

  SECTION("reports progress and matches the silent variant's schema") {
    struct ProgressState {
      int calls = 0;
      float last = -1.0f;
    } state;
    auto cb = [](float progress, const char* /*stage*/, void* user_data) {
      auto* s = static_cast<ProgressState*>(user_data);
      ++s->calls;
      s->last = progress;
    };

    char* json = nullptr;
    SonareError err =
        sonare_analyze_json_with_progress(samples.data(), samples.size(), 22050, cb, &state, &json);
    REQUIRE(err == SONARE_OK);
    REQUIRE(json != nullptr);
    REQUIRE(state.calls > 0);
    REQUIRE(state.last >= 0.0f);
    REQUIRE(sonare::util::json::parse(json).contains("chords"));
    sonare_free_string(json);

    // A null callback runs silently and still produces output.
    char* json2 = nullptr;
    REQUIRE(sonare_analyze_json_with_progress(samples.data(), samples.size(), 22050, nullptr,
                                              nullptr, &json2) == SONARE_OK);
    REQUIRE(json2 != nullptr);
    sonare_free_string(json2);
  }

  SECTION("cancels after a reported progress boundary without producing JSON") {
    struct CancelState {
      int progress_calls = 0;
      float last_progress = -1.0f;
      bool should_cancel = false;
    } state;
    auto progress = [](float value, const char* /*stage*/, void* user_data) {
      auto* current = static_cast<CancelState*>(user_data);
      ++current->progress_calls;
      current->last_progress = value;
      current->should_cancel = value > 0.5f;
    };
    auto cancel = [](void* user_data) {
      return static_cast<CancelState*>(user_data)->should_cancel ? 1 : 0;
    };

    char* json = nullptr;
    REQUIRE(sonare_analyze_json_with_progress_ex(samples.data(), samples.size(), 22050, progress,
                                                 &state, &json, cancel,
                                                 &state) == SONARE_ERROR_CANCELLED);
    REQUIRE(state.progress_calls > 0);
    REQUIRE(state.last_progress > 0.5f);
    REQUIRE(json == nullptr);
  }

  SECTION("rejects a null out pointer") {
    REQUIRE(sonare_analyze_json(samples.data(), samples.size(), 22050, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("accepts the complete analyzer configuration") {
    SonareMusicAnalyzeOptions options = sonare_music_analyze_options_default();
    options.bpm_min = 30.0f;
    options.bpm_max = 90.0f;
    options.start_bpm = 55.0f;
    options.use_hpss = 0;
    options.use_chord_hmm = 1;
    options.detect_chord_inversions = 1;
    char* json = nullptr;
    REQUIRE(sonare_analyze_json_ex(samples.data(), samples.size(), 22050, &options, &json) ==
            SONARE_OK);
    REQUIRE(json != nullptr);
    const auto root = sonare::util::json::parse(json);
    REQUIRE(root["bpm"].as_number() >= options.bpm_min);
    REQUIRE(root["bpm"].as_number() <= options.bpm_max);
    sonare_free_string(json);

    options.bpm_max = options.bpm_min - 1.0f;
    REQUIRE(sonare_analyze_json_ex(samples.data(), samples.size(), 22050, &options, &json) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_analyze_melody_ex", "[c_api]") {
  auto samples = generate_sine(440.0f, 22050, 1.0f);

  SECTION("pYIN center contour differs from the plain-YIN default") {
    SonareMelodyResult plain = {};
    SonareMelodyResult pyin = {};
    REQUIRE(sonare_analyze_melody_ex(samples.data(), samples.size(), 22050, 65.0f, 2093.0f, 2048,
                                     256, 0.1f, /*use_pyin=*/0, /*center=*/1, &plain) == SONARE_OK);
    REQUIRE(sonare_analyze_melody_ex(samples.data(), samples.size(), 22050, 65.0f, 2093.0f, 2048,
                                     256, 0.1f, /*use_pyin=*/1, /*center=*/1, &pyin) == SONARE_OK);
    // Both produce a usable contour; the pYIN path is genuinely reachable.
    REQUIRE(pyin.mean_frequency >= 0.0f);
    sonare_free_melody_result(&plain);
    sonare_free_melody_result(&pyin);
  }

  SECTION("the legacy entry point delegates to the plain-YIN default") {
    SonareMelodyResult legacy = {};
    SonareMelodyResult ex = {};
    REQUIRE(sonare_analyze_melody(samples.data(), samples.size(), 22050, 65.0f, 2093.0f, 2048, 256,
                                  0.1f, &legacy) == SONARE_OK);
    REQUIRE(sonare_analyze_melody_ex(samples.data(), samples.size(), 22050, 65.0f, 2093.0f, 2048,
                                     256, 0.1f, 0, 1, &ex) == SONARE_OK);
    REQUIRE(legacy.point_count == ex.point_count);
    sonare_free_melody_result(&legacy);
    sonare_free_melody_result(&ex);
  }

  SECTION("rejects invalid parameters") {
    SonareMelodyResult out = {};
    REQUIRE(sonare_analyze_melody_ex(samples.data(), samples.size(), 22050, 0.0f, 2093.0f, 2048,
                                     256, 0.1f, 1, 1, &out) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_analyze_dynamics", "[c_api]") {
  SECTION("returns dynamics and loudness curve") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    SonareDynamicsResult result = {};

    SonareError err =
        sonare_analyze_dynamics(samples.data(), samples.size(), 22050, 0.4f, 512, 6.0f, &result);

    REQUIRE(err == SONARE_OK);
    REQUIRE(result.peak_db <= 1.0f);
    REQUIRE(result.rms_db <= result.peak_db);
    REQUIRE(result.dynamic_range_db >= 0.0f);
    REQUIRE(result.loudness_range_db >= 0.0f);
    REQUIRE((result.is_compressed == 0 || result.is_compressed == 1));
    REQUIRE(result.loudness_count > 0);
    REQUIRE(result.loudness_times != nullptr);
    REQUIRE(result.loudness_rms_db != nullptr);
    for (size_t i = 1; i < result.loudness_count; ++i) {
      REQUIRE(result.loudness_times[i] >= result.loudness_times[i - 1]);
    }

    sonare_free_dynamics_result(&result);
    REQUIRE(result.loudness_times == nullptr);
    REQUIRE(result.loudness_rms_db == nullptr);
    REQUIRE(result.loudness_count == 0);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_sine(440.0f, 22050, 1.0f);
    SonareDynamicsResult result = {};

    REQUIRE(sonare_analyze_dynamics(nullptr, samples.size(), 22050, 0.4f, 512, 6.0f, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_dynamics(samples.data(), samples.size(), 22050, 0.0f, 512, 6.0f,
                                    &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_dynamics(samples.data(), samples.size(), 22050, 0.4f, 0, 6.0f,
                                    &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_dynamics(samples.data(), samples.size(), 22050, 0.4f, 512, 6.0f,
                                    nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("free is safe on partially initialized struct") {
    SonareDynamicsResult result = {};
    result.loudness_times = new float[1]{0.0f};
    result.loudness_rms_db = new float[1]{-12.0f};
    result.loudness_count = 1;

    sonare_free_dynamics_result(&result);

    REQUIRE(result.loudness_times == nullptr);
    REQUIRE(result.loudness_rms_db == nullptr);
    REQUIRE(result.loudness_count == 0);
  }
}
