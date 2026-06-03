/// @file sonare_c_test.cpp
/// @brief Tests for C API functions.

#include "sonare_c.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "util/constants.h"

namespace {

// Generate sine wave
std::vector<float> generate_sine(float freq, int sample_rate, float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    samples[i] =
        std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freq * i / sample_rate);
  }
  return samples;
}

#ifdef SONARE_WITH_MASTERING
float max_abs(const float* samples, size_t length) {
  float peak = 0.0f;
  for (size_t i = 0; i < length; ++i) {
    peak = std::max(peak, std::abs(samples[i]));
  }
  return peak;
}

float* non_null_sentinel_float_ptr() {
  return reinterpret_cast<float*>(static_cast<std::uintptr_t>(0x1));
}

std::vector<std::string> split_lines(const char* text) {
  std::vector<std::string> lines;
  std::stringstream stream(text ? text : "");
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}
#endif

// Generate click track
std::vector<float> generate_clicks(float bpm, int sample_rate, float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float samples_per_beat = (sample_rate * 60.0f) / bpm;
  int n_beats = static_cast<int>(duration * bpm / 60.0f);

  for (int beat = 0; beat < n_beats; ++beat) {
    size_t start = static_cast<size_t>(beat * samples_per_beat);
    size_t click_length = static_cast<size_t>(sample_rate * 0.01f);
    for (size_t i = 0; i < click_length && start + i < n_samples; ++i) {
      samples[start + i] = std::sin(static_cast<float>(sonare::constants::kPiD) * i / click_length);
    }
  }
  return samples;
}

std::vector<float> generate_chord(const std::vector<float>& freqs, int sample_rate,
                                  float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples, 0.0f);
  float gain = 0.8f / static_cast<float>(freqs.size());
  for (size_t i = 0; i < n_samples; ++i) {
    for (float freq : freqs) {
      samples[i] += gain * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freq * i /
                                    sample_rate);
    }
  }
  return samples;
}

std::vector<float> generate_harmonic_chord(const std::vector<float>& freqs, int sample_rate,
                                           float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples, 0.0f);
  for (size_t i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    for (float freq : freqs) {
      samples[i] += 0.5f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freq * t);
      samples[i] += 0.25f * std::sin(4.0f * static_cast<float>(sonare::constants::kPiD) * freq * t);
      samples[i] +=
          0.125f * std::sin(6.0f * static_cast<float>(sonare::constants::kPiD) * freq * t);
    }
  }
  float peak = 0.0f;
  for (float sample : samples) {
    peak = std::max(peak, std::abs(sample));
  }
  if (peak > 0.0f) {
    for (float& sample : samples) {
      sample /= peak;
    }
  }
  return samples;
}

}  // namespace

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
  config.sample_rate = 22050;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.n_mels = 32;
  config.window = SONARE_WINDOW_HAMMING;
  config.output_format = SONARE_STREAM_OUTPUT_UINT8;

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
    bad.fmin = 1000.0f;
    bad.fmax = 500.0f;
    REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
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
    REQUIRE(u8.timestamps != nullptr);
    REQUIRE(u8.mel != nullptr);
    REQUIRE(u8.chroma != nullptr);
    sonare_free_stream_frames_u8(&u8);

    REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size()) == SONARE_OK);
    SonareStreamFramesI16 i16 = {};
    REQUIRE(sonare_stream_analyzer_read_frames_i16(analyzer, 4, &i16) == SONARE_OK);
    REQUIRE(i16.n_frames > 0);
    REQUIRE(i16.n_mels == config.n_mels);
    REQUIRE(i16.mel != nullptr);
    REQUIRE(i16.chroma != nullptr);
    sonare_free_stream_frames_i16(&i16);

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

TEST_CASE("sonare_analyze", "[c_api]") {
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

    sonare_free_result(&result);

    REQUIRE(result.beat_times == nullptr);
    REQUIRE(result.beat_count == 0);
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

TEST_CASE("sonare_analyze_impulse_response", "[c_api][acoustic]") {
  const int sample_rate = 48000;
  const float expected_rt60 = 1.0f;
  std::vector<float> samples(static_cast<size_t>(sample_rate) * 4);
  const float decay = std::log(1000.0f) / expected_rt60;
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] = std::exp(-decay * t);
  }

  SonareAcousticResult result = {};
  SonareError err =
      sonare_analyze_impulse_response(samples.data(), samples.size(), sample_rate, 6, &result);

  REQUIRE(err == SONARE_OK);
  REQUIRE(std::isfinite(result.rt60));
  REQUIRE(std::isfinite(result.edt));
  REQUIRE(result.rt60 > 0.95f);
  REQUIRE(result.rt60 < 1.05f);
  REQUIRE(result.band_count == 6);
  REQUIRE(result.rt60_bands != nullptr);
  REQUIRE(result.edt_bands != nullptr);
  REQUIRE(result.c50_bands != nullptr);
  REQUIRE(result.c80_bands != nullptr);

  sonare_free_acoustic_result(&result);
  REQUIRE(result.rt60_bands == nullptr);
  REQUIRE(result.band_count == 0);
}

TEST_CASE("sonare_detect_acoustic", "[c_api][acoustic]") {
  const int sample_rate = 48000;
  const float expected_rt60 = 0.7f;
  std::vector<float> samples(static_cast<size_t>(sample_rate) * 4);
  const float decay = std::log(1000.0f) / expected_rt60;
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] = std::exp(-decay * t);
  }

  SonareAcousticResult result = {};
  SonareError err = sonare_detect_acoustic(samples.data(), samples.size(), sample_rate, 6, 24,
                                           30.0f, 10.0f, &result);

  REQUIRE(err == SONARE_OK);
  REQUIRE(result.is_blind == 1);
  REQUIRE(std::isfinite(result.rt60));
  REQUIRE(result.rt60 > 0.55f);
  REQUIRE(result.rt60 < 0.85f);
  REQUIRE(result.band_count == 6);
  REQUIRE(result.rt60_bands != nullptr);
  // Blind mode does not compute clarity bands; they are exposed as null so that
  // "not computed" is distinguishable from "computed-but-invalid".
  REQUIRE(result.c50_bands == nullptr);
  REQUIRE(result.c80_bands == nullptr);

  sonare_free_acoustic_result(&result);
}

TEST_CASE("sonare_detect_acoustic blind mode exposes null clarity bands", "[c_api][acoustic]") {
  const int sample_rate = 48000;
  const float expected_rt60 = 0.7f;
  std::vector<float> samples(static_cast<size_t>(sample_rate) * 4);
  const float decay = std::log(1000.0f) / expected_rt60;
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] = std::exp(-decay * t);
  }

  SonareAcousticResult result = {};
  SonareError err = sonare_detect_acoustic(samples.data(), samples.size(), sample_rate, 6, 24,
                                           30.0f, 10.0f, &result);

  REQUIRE(err == SONARE_OK);
  REQUIRE(result.is_blind == 1);
  REQUIRE(result.band_count > 0);
  REQUIRE(result.rt60_bands != nullptr);
  REQUIRE(result.c50_bands == nullptr);
  REQUIRE(result.c80_bands == nullptr);

  sonare_free_acoustic_result(&result);
}

TEST_CASE("sonare_analyze_timbre", "[c_api]") {
  SECTION("returns timbre scalars and spectral curves") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 2.0f);
    SonareTimbreResult result = {};

    SonareError err = sonare_analyze_timbre(samples.data(), samples.size(), 22050, 2048, 512, 128,
                                            13, 0.5f, &result);

    REQUIRE(err == SONARE_OK);
    REQUIRE(result.brightness >= 0.0f);
    REQUIRE(result.brightness <= 1.0f);
    REQUIRE(result.warmth >= 0.0f);
    REQUIRE(result.warmth <= 1.0f);
    REQUIRE(result.density >= 0.0f);
    REQUIRE(result.density <= 1.0f);
    REQUIRE(result.roughness >= 0.0f);
    REQUIRE(result.roughness <= 1.0f);
    REQUIRE(result.complexity >= 0.0f);
    REQUIRE(result.complexity <= 1.0f);
    REQUIRE(result.spectral_centroid_count > 0);
    REQUIRE(result.spectral_flatness_count == result.spectral_centroid_count);
    REQUIRE(result.spectral_rolloff_count == result.spectral_centroid_count);
    REQUIRE(result.spectral_centroid != nullptr);
    REQUIRE(result.spectral_flatness != nullptr);
    REQUIRE(result.spectral_rolloff != nullptr);

    sonare_free_timbre_result(&result);
    REQUIRE(result.spectral_centroid == nullptr);
    REQUIRE(result.spectral_flatness == nullptr);
    REQUIRE(result.spectral_rolloff == nullptr);
    REQUIRE(result.spectral_centroid_count == 0);
    REQUIRE(result.spectral_flatness_count == 0);
    REQUIRE(result.spectral_rolloff_count == 0);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_sine(440.0f, 22050, 1.0f);
    SonareTimbreResult result = {};

    REQUIRE(sonare_analyze_timbre(nullptr, samples.size(), 22050, 2048, 512, 128, 13, 0.5f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 0, 512, 128, 13, 0.5f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 2048, 0, 128, 13, 0.5f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 2048, 512, 0, 13, 0.5f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 2048, 512, 128, 0, 0.5f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 2048, 512, 128, 13, 0.0f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 2048, 512, 128, 13, 0.5f,
                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("free is safe on partially initialized struct") {
    SonareTimbreResult result = {};
    result.spectral_centroid = new float[1]{1000.0f};
    result.spectral_flatness = new float[1]{0.5f};
    result.spectral_rolloff = new float[1]{3000.0f};
    result.spectral_centroid_count = 1;
    result.spectral_flatness_count = 1;
    result.spectral_rolloff_count = 1;

    sonare_free_timbre_result(&result);

    REQUIRE(result.spectral_centroid == nullptr);
    REQUIRE(result.spectral_flatness == nullptr);
    REQUIRE(result.spectral_rolloff == nullptr);
    REQUIRE(result.spectral_centroid_count == 0);
    REQUIRE(result.spectral_flatness_count == 0);
    REQUIRE(result.spectral_rolloff_count == 0);
  }
}

TEST_CASE("sonare_detect_chords", "[c_api]") {
  SECTION("returns chord segments for a simple C major chord") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 2.0f);
    SonareChordAnalysisResult result = {};

    SonareError err = sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, 0.5f,
                                           1, 2048, 512, 0, &result);

    REQUIRE(err == SONARE_OK);
    REQUIRE(result.chord_count > 0);
    REQUIRE(result.chords != nullptr);
    REQUIRE(result.chords[0].root >= SONARE_PITCH_C);
    REQUIRE(result.chords[0].root <= SONARE_PITCH_B);
    REQUIRE(result.chords[0].quality >= SONARE_CHORD_MAJOR);
    REQUIRE(result.chords[0].quality <= SONARE_CHORD_SUS2_ADD4);
    REQUIRE(result.chords[0].end >= result.chords[0].start);
    REQUIRE(result.chords[0].confidence >= 0.0f);

    sonare_free_chord_analysis_result(&result);
    REQUIRE(result.chords == nullptr);
    REQUIRE(result.chord_count == 0);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 1.0f);
    SonareChordAnalysisResult result = {};

    REQUIRE(sonare_detect_chords(nullptr, samples.size(), 22050, 0.3f, 2.0f, 0.5f, 0, 2048, 512, 0,
                                 &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, -0.1f, 2.0f, 0.5f, 0, 2048,
                                 512, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 0.0f, 0.5f, 0, 2048,
                                 512, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, -0.1f, 0, 2048,
                                 512, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, 0.5f, 0, 0, 512,
                                 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, 0.5f, 0, 2048,
                                 0, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, 0.5f, 0, 2048,
                                 512, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("extended options enable HMM and inversion detection without changing legacy ABI") {
    auto samples = generate_harmonic_chord({82.41f, 261.63f, 329.63f, 392.00f}, 22050, 1.0f);
    SonareChordAnalysisResult result = {};
    SonareChordDetectionOptions options{};
    options.min_duration = 0.0f;
    options.smoothing_window = 2.0f;
    options.threshold = 0.5f;
    options.use_triads_only = 1;
    options.n_fft = 2048;
    options.hop_length = 512;
    options.use_beat_sync = 0;
    options.use_hmm = 1;
    options.hmm_beam_width = 8;
    options.use_key_context = 1;
    options.key_root = SONARE_PITCH_C;
    options.key_mode = SONARE_MODE_MAJOR;
    options.detect_inversions = 1;
    options.chroma_method = 1;

    SonareError err =
        sonare_detect_chords_ex(samples.data(), samples.size(), 22050, &options, &result);

    REQUIRE(err == SONARE_OK);
    REQUIRE(result.chord_count > 0);
    REQUIRE(result.chords != nullptr);
    REQUIRE(result.chords[0].root == SONARE_PITCH_C);
    REQUIRE(result.chords[0].bass == SONARE_PITCH_E);

    sonare_free_chord_analysis_result(&result);
  }

  SECTION("free is safe on partially initialized struct") {
    SonareChordAnalysisResult result = {};
    result.chords =
        new SonareChord[1]{{SONARE_PITCH_C, SONARE_CHORD_MAJOR, 0.0f, 1.0f, 1.0f, SONARE_PITCH_C}};
    result.chord_count = 1;

    sonare_free_chord_analysis_result(&result);

    REQUIRE(result.chords == nullptr);
    REQUIRE(result.chord_count == 0);
  }
}

TEST_CASE("sonare_chord_functional_analysis", "[c_api]") {
  SonareChordDetectionOptions options{};
  options.min_duration = 0.3f;
  options.smoothing_window = 2.0f;
  options.threshold = 0.5f;
  options.use_triads_only = 1;
  options.n_fft = 2048;
  options.hop_length = 512;
  options.use_beat_sync = 0;
  options.use_hmm = 0;
  options.hmm_beam_width = 8;
  options.use_key_context = 0;
  options.key_root = SONARE_PITCH_C;
  options.key_mode = SONARE_MODE_MAJOR;
  options.detect_inversions = 0;
  options.chroma_method = 0;

  SECTION("returns one Roman-numeral label per detected chord") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 2.0f);

    // Detection count for the same options, to cross-check the label count.
    SonareChordAnalysisResult chords = {};
    REQUIRE(sonare_detect_chords_ex(samples.data(), samples.size(), 22050, &options, &chords) ==
            SONARE_OK);
    const size_t expected = chords.chord_count;
    sonare_free_chord_analysis_result(&chords);
    REQUIRE(expected > 0);

    SonareStringArray labels = {};
    SonareError err =
        sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, &options,
                                         SONARE_PITCH_C, SONARE_MODE_MAJOR, &labels);

    REQUIRE(err == SONARE_OK);
    REQUIRE(labels.count == expected);
    REQUIRE(labels.items != nullptr);
    for (size_t i = 0; i < labels.count; ++i) {
      REQUIRE(labels.items[i] != nullptr);
      REQUIRE(std::string(labels.items[i]).size() > 0);
    }

    sonare_free_string_array(&labels);
    REQUIRE(labels.items == nullptr);
    REQUIRE(labels.count == 0);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 1.0f);
    SonareStringArray labels = {};

    REQUIRE(sonare_chord_functional_analysis(nullptr, samples.size(), 22050, &options,
                                             SONARE_PITCH_C, SONARE_MODE_MAJOR,
                                             &labels) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, nullptr,
                                             SONARE_PITCH_C, SONARE_MODE_MAJOR,
                                             &labels) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, &options,
                                             SONARE_PITCH_C, SONARE_MODE_MAJOR,
                                             nullptr) == SONARE_ERROR_INVALID_PARAMETER);

    SonareChordDetectionOptions bad = options;
    bad.n_fft = 0;
    REQUIRE(sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, &bad,
                                             SONARE_PITCH_C, SONARE_MODE_MAJOR,
                                             &labels) == SONARE_ERROR_INVALID_PARAMETER);

    REQUIRE(sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, &options,
                                             static_cast<SonarePitchClass>(99), SONARE_MODE_MAJOR,
                                             &labels) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, &options,
                                             SONARE_PITCH_C, static_cast<SonareMode>(99),
                                             &labels) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("free is safe on a zero-initialized struct") {
    SonareStringArray labels = {};
    sonare_free_string_array(&labels);
    REQUIRE(labels.items == nullptr);
    REQUIRE(labels.count == 0);
  }
}

#ifdef SONARE_WITH_MASTERING
TEST_CASE("sonare_mastering_process", "[c_api][mastering]") {
  SECTION("streaming EQ handle processes JSON bands and exposes spectrum") {
    SonareEq* eq = sonare_eq_create(48000.0, 512);
    REQUIRE(eq != nullptr);
    REQUIRE(sonare_eq_set_band(eq, 0,
                               "{\"type\":\"Peak\",\"frequencyHz\":1000,\"gainDb\":9,"
                               "\"q\":1,\"enabled\":true,\"coeffMode\":\"Vicanek\","
                               "\"proportionalQ\":true}") == SONARE_OK);
    REQUIRE(sonare_eq_set_band(eq, 0, "not json") == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_eq_set_band(eq, 0, "{\"type\":\"Unknown\",\"enabled\":true}") ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_eq_set_band(eq, 0, "{\"type\":\"Peak\",\"enabled\":truish}") ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_eq_set_band(eq, 0, "{\"type\":\"Peak\" \"enabled\":true}") ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_eq_set_band(eq, 0, "{\"type\":\"Peak\",\"type\":\"Notch\"}") ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_eq_set_band(eq, 0, "{\"type\":7,\"enabled\":true}") ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_eq_set_band(eq, 0, "{\"type\":\"Peak\",\"coeffMode\":\"unknown\"}") ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_eq_set_band(eq, 0,
                               "{\"note\":\"\\\"type\\\":\\\"Unknown\\\"\","
                               "\"type\":\"Peak\",\"frequency_hz\":1000,\"gain_db\":9,"
                               "\"q\":1,\"enabled\":true,\"coeff_mode\":\"vicanek\","
                               "\"proportional_q\":true}") == SONARE_OK);
    REQUIRE(sonare_eq_latency_samples(eq) == 0);
    sonare_eq_set_auto_gain(eq, 1);
    REQUIRE(sonare_eq_set_gain_scale(eq, 0.5f) == SONARE_OK);
    REQUIRE(sonare_eq_set_gain_scale(eq, -0.1f) != SONARE_OK);
    REQUIRE(sonare_eq_set_output_gain_db(eq, 3.0f) == SONARE_OK);
    REQUIRE(sonare_eq_set_output_pan(eq, 0.25f) == SONARE_OK);
    REQUIRE(sonare_eq_set_output_pan(eq, 1.5f) != SONARE_OK);

    std::vector<float> left = generate_sine(1000.0f, 48000, 512.0f / 48000.0f);
    std::vector<float> right = left;
    for (auto& sample : left) sample *= 0.2f;
    for (auto& sample : right) sample *= 0.2f;
    float* channels[] = {left.data(), right.data()};

    REQUIRE(sonare_eq_process(eq, channels, 2, static_cast<int>(left.size())) == SONARE_OK);
    REQUIRE(sonare_eq_last_auto_gain_db(eq) < 0.0f);

    SonareEqSnapshot snapshot{};
    REQUIRE(sonare_eq_spectrum(eq, &snapshot) == SONARE_OK);
    REQUIRE(snapshot.seq == 1);
    REQUIRE(snapshot.pre_count == SONARE_EQ_SPECTRUM_STREAM_CAPACITY);
    REQUIRE(snapshot.post_count == SONARE_EQ_SPECTRUM_STREAM_CAPACITY);
    REQUIRE(snapshot.band_gain_db[0] > 4.0f);
    REQUIRE(snapshot.band_gain_db[0] < 5.0f);
    REQUIRE(snapshot.last_auto_gain_db < 0.0f);

    REQUIRE(sonare_eq_set_phase_mode(eq, SONARE_EQ_PHASE_LINEAR) == SONARE_OK);
    REQUIRE(sonare_eq_latency_samples(eq) > 0);
    REQUIRE(sonare_eq_set_phase_mode(eq, 99) == SONARE_ERROR_INVALID_PARAMETER);

    sonare_eq_clear(eq);
    REQUIRE(sonare_eq_latency_samples(eq) == 0);
    sonare_eq_destroy(eq);
  }

  SECTION("streaming EQ match configures live bands from source and reference") {
    auto source = generate_sine(1000.0f, 48000, 1.0f);
    auto reference = source;
    SonareEq* eq = sonare_eq_create(48000.0, static_cast<int>(source.size()));
    REQUIRE(eq != nullptr);

    for (auto& sample : source) sample *= 0.12f;
    for (auto& sample : reference) sample *= 0.45f;

    REQUIRE(sonare_eq_match(eq, source.data(), reference.data(), source.size(), 48000, 4) ==
            SONARE_OK);

    std::vector<float> left = source;
    std::vector<float> right = source;
    float* channels[] = {left.data(), right.data()};
    REQUIRE(sonare_eq_process(eq, channels, 2, static_cast<int>(left.size())) == SONARE_OK);

    SonareEqSnapshot snapshot{};
    REQUIRE(sonare_eq_spectrum(eq, &snapshot) == SONARE_OK);
    bool has_positive_band = false;
    for (size_t i = 0; i < SONARE_EQ_MAX_BANDS; ++i) {
      has_positive_band = has_positive_band || snapshot.band_gain_db[i] > 0.5f;
    }
    REQUIRE(has_positive_band);

    REQUIRE(sonare_eq_match(eq, source.data(), reference.data(), source.size(), 48000, 0) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_eq_match(eq, source.data(), reference.data(), source.size(), 48000,
                            SONARE_EQ_MAX_BANDS + 1) == SONARE_ERROR_INVALID_PARAMETER);
    sonare_eq_destroy(eq);
  }

  SECTION("returns processed samples and loudness metadata") {
    auto samples = generate_sine(440.0f, 22050, 1.0f);
    for (auto& sample : samples) {
      sample *= 0.2f;
    }

    SonareMasteringConfig config{};
    config.target_lufs = -18.0f;
    config.ceiling_db = -1.0f;
    config.true_peak_oversample = 4;

    SonareMasteringResult result{};
    SonareError err =
        sonare_mastering_process(samples.data(), samples.size(), 22050, &config, &result);

    REQUIRE(err == SONARE_OK);
    REQUIRE(result.samples != nullptr);
    REQUIRE(result.length == samples.size());
    REQUIRE(result.sample_rate == 22050);
    REQUIRE(std::isfinite(result.input_lufs));
    REQUIRE(std::isfinite(result.output_lufs));
    REQUIRE(std::isfinite(result.applied_gain_db));
    REQUIRE(result.output_lufs > -18.2f);
    REQUIRE(result.output_lufs < -17.8f);

    sonare_free_mastering_result(&result);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_sine(440.0f, 22050, 1.0f);
    SonareMasteringConfig config{};
    config.target_lufs = -18.0f;
    config.ceiling_db = -1.0f;
    config.true_peak_oversample = 4;
    SonareMasteringResult result{};

    REQUIRE(sonare_mastering_process(nullptr, samples.size(), 22050, &config, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_process(samples.data(), samples.size(), 22050, &config, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("applies named mono and stereo processors") {
    auto samples = generate_sine(440.0f, 22050, 0.5f);
    for (auto& sample : samples) sample *= 0.2f;

    SonareMasteringParam params[] = {{"thresholdDb", -24.0}, {"ratio", 1.5}};
    SonareMasteringResult mono{};
    REQUIRE(sonare_mastering_apply_processor("dynamics.compressor", samples.data(), samples.size(),
                                             22050, params, 2, &mono) == SONARE_OK);
    REQUIRE(mono.samples != nullptr);
    REQUIRE(mono.length == samples.size());
    REQUIRE(std::isfinite(mono.output_lufs));
    sonare_free_mastering_result(&mono);

    SonareMasteringParam stereo_params[] = {{"width", 1.1}};
    SonareMasteringStereoResult stereo{};
    REQUIRE(sonare_mastering_apply_processor_stereo("stereo.imager", samples.data(), samples.data(),
                                                    samples.size(), 22050, stereo_params, 1,
                                                    &stereo) == SONARE_OK);
    REQUIRE(stereo.left != nullptr);
    REQUIRE(stereo.right != nullptr);
    REQUIRE(stereo.length == samples.size());
    REQUIRE(std::isfinite(stereo.output_lufs));
    sonare_free_mastering_stereo_result(&stereo);

    const char* names = sonare_mastering_processor_names();
    REQUIRE(names != nullptr);
    REQUIRE(std::strstr(names, "dynamics.compressor") != nullptr);
    REQUIRE(std::strstr(names, "eq.equalizer") != nullptr);
    REQUIRE(std::strstr(names, "stereo.imager") != nullptr);

    SonareMasteringParam eq_params[] = {{"band0.enabled", 1.0}, {"band0.frequencyHz", 440.0},
                                        {"band0.gainDb", 6.0},  {"band0.q", 1.0},
                                        {"autoGain", 1.0},      {"gainScale", 0.5},
                                        {"outputGainDb", 1.0},  {"outputPan", 0.0}};
    SonareMasteringResult eq_result{};
    REQUIRE(sonare_mastering_apply_processor("eq.equalizer", samples.data(), samples.size(), 22050,
                                             eq_params, 8, &eq_result) == SONARE_OK);
    REQUIRE(eq_result.samples != nullptr);
    REQUIRE(eq_result.length == samples.size());
    sonare_free_mastering_result(&eq_result);

    auto high_tone = generate_sine(8000.0f, 22050, 0.5f);
    SonareMasteringParam type_only_eq_params[] = {{"band0.type", 3.0}, {"band0.coeffMode", 1.0}};
    SonareMasteringResult type_only_eq{};
    REQUIRE(sonare_mastering_apply_processor("eq.equalizer", high_tone.data(), high_tone.size(),
                                             22050, type_only_eq_params, 2,
                                             &type_only_eq) == SONARE_OK);
    REQUIRE(type_only_eq.samples != nullptr);
    REQUIRE(max_abs(type_only_eq.samples, type_only_eq.length) <
            max_abs(high_tone.data(), high_tone.size()) * 0.6f);
    sonare_free_mastering_result(&type_only_eq);

    SonareMasteringParam left_eq_params[] = {{"band0.enabled", 1.0},
                                             {"band0.frequencyHz", 440.0},
                                             {"band0.gainDb", 12.0},
                                             {"band0.q", 1.0},
                                             {"band0.placement", 1.0}};
    SonareMasteringStereoResult left_eq{};
    REQUIRE(sonare_mastering_apply_processor_stereo("eq.equalizer", samples.data(), samples.data(),
                                                    samples.size(), 22050, left_eq_params, 5,
                                                    &left_eq) == SONARE_OK);
    REQUIRE(left_eq.left != nullptr);
    REQUIRE(left_eq.right != nullptr);
    REQUIRE(left_eq.length == samples.size());
    REQUIRE(max_abs(left_eq.left, left_eq.length) > max_abs(left_eq.right, left_eq.length) * 1.5f);
    sonare_free_mastering_stereo_result(&left_eq);

    SonareMasteringParam linear_eq_params[] = {{"phaseMode", 3.0},     {"resolution", 1.0},
                                               {"band0.enabled", 1.0}, {"band0.frequencyHz", 440.0},
                                               {"band0.gainDb", 3.0},  {"band0.q", 1.0}};
    SonareMasteringStereoResult linear_eq{};
    REQUIRE(sonare_mastering_apply_processor_stereo("eq.equalizer", samples.data(), samples.data(),
                                                    samples.size(), 22050, linear_eq_params, 6,
                                                    &linear_eq) == SONARE_OK);
    REQUIRE(linear_eq.latency_samples == 512);
    sonare_free_mastering_stereo_result(&linear_eq);
  }

  SECTION("stereo chain rejects non-finite samples like the mono path") {
    auto samples = generate_sine(440.0f, 22050, 0.3f);
    SonareMasteringParam params[] = {{"thresholdDb", -24.0}};
    SonareMasteringChainStereoResult bad{};
    auto nan_samples = samples;
    nan_samples[10] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(sonare_mastering_chain_stereo(nan_samples.data(), samples.data(), samples.size(), 22050,
                                          params, 1, &bad) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_chain_stereo(samples.data(), nan_samples.data(), samples.size(), 22050,
                                          params, 1, &bad) == SONARE_ERROR_INVALID_PARAMETER);
    // Out-of-range sample rate is rejected too.
    REQUIRE(sonare_mastering_chain_stereo(samples.data(), samples.data(), samples.size(), 0, params,
                                          1, &bad) == SONARE_ERROR_INVALID_PARAMETER);

    SonareMasteringChainStereoResult ok{};
    REQUIRE(sonare_mastering_chain_stereo(samples.data(), samples.data(), samples.size(), 22050,
                                          nullptr, 0, &ok) == SONARE_OK);
    sonare_free_mastering_chain_stereo_result(&ok);
  }

  SECTION("named processor validation includes processor and parameter name") {
    auto samples = generate_sine(440.0f, 22050, 0.5f);
    SonareMasteringParam params[] = {{"width", 3.5}};
    SonareMasteringStereoResult stereo{};

    REQUIRE(sonare_mastering_apply_processor_stereo("stereo.imager", samples.data(), samples.data(),
                                                    samples.size(), 22050, params, 1,
                                                    &stereo) == SONARE_ERROR_INVALID_PARAMETER);
    const char* msg = sonare_last_error_message();
    REQUIRE(msg != nullptr);
    REQUIRE(std::string(msg).find("stereo.imager.width must be in [0, 2], got 3.5") !=
            std::string::npos);
    sonare_free_mastering_stereo_result(&stereo);
  }

  SECTION("applies pair processors and analyses") {
    auto source = generate_sine(440.0f, 44100, 0.25f);
    auto reference = generate_sine(880.0f, 44100, 0.25f);
    for (auto& sample : source) sample *= 0.18f;
    for (auto& sample : reference) sample *= 0.12f;

    SonareMasteringParam pair_params[] = {{"mix", 0.25}};
    SonareMasteringResult paired{};
    REQUIRE(sonare_mastering_apply_pair_processor("match.abCrossfade", source.data(),
                                                  reference.data(), source.size(), 44100,
                                                  pair_params, 1, &paired) == SONARE_OK);
    REQUIRE(paired.samples != nullptr);
    REQUIRE(paired.length == source.size());
    sonare_free_mastering_result(&paired);

    char* pair_json = nullptr;
    REQUIRE(sonare_mastering_analyze_pair("match.referenceLoudness", source.data(),
                                          reference.data(), source.size(), 44100, nullptr, 0,
                                          &pair_json) == SONARE_OK);
    REQUIRE(pair_json != nullptr);
    REQUIRE(std::strstr(pair_json, "sourceLufs") != nullptr);
    REQUIRE(std::strstr(pair_json, "referenceLufs") != nullptr);
    sonare_free_string(pair_json);

    char* stereo_json = nullptr;
    REQUIRE(sonare_mastering_analyze_stereo("stereo.monoCompatCheck", source.data(),
                                            reference.data(), source.size(), 44100, nullptr, 0,
                                            &stereo_json) == SONARE_OK);
    REQUIRE(stereo_json != nullptr);
    REQUIRE(std::strstr(stereo_json, "correlation") != nullptr);
    sonare_free_string(stereo_json);

    REQUIRE(std::strstr(sonare_mastering_pair_processor_names(), "match.abCrossfade") != nullptr);
    REQUIRE(std::strstr(sonare_mastering_pair_analysis_names(), "match.referenceLoudness") !=
            nullptr);
    REQUIRE(std::strstr(sonare_mastering_stereo_analysis_names(), "stereo.monoCompatCheck") !=
            nullptr);
  }

  SECTION("streaming preview is reachable through the C API") {
    auto samples = generate_sine(1000.0f, 48000, 1.0f);
    for (auto& sample : samples) sample *= 0.2f;
    SonareStreamingPlatform platforms[] = {{"Unit Test", 0.0f, -6.0f}};

    char* json = nullptr;
    REQUIRE(sonare_mastering_streaming_preview(samples.data(), samples.size(), 48000, platforms, 1,
                                               &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    REQUIRE(std::strstr(json, "\"platforms\"") != nullptr);
    REQUIRE(std::strstr(json, "\"name\":\"Unit Test\"") != nullptr);
    REQUIRE(std::strstr(json, "\"normalizationGainDb\"") != nullptr);
    REQUIRE(std::strstr(json, "\"ceilingRisk\":true") != nullptr);
    sonare_free_string(json);

    json = nullptr;
    REQUIRE(sonare_mastering_streaming_preview(samples.data(), samples.size(), 48000, nullptr, 0,
                                               &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    REQUIRE(std::strstr(json, "\"Spotify\"") != nullptr);
    sonare_free_string(json);
  }

  SECTION("assistant suggestion is reachable through the C API") {
    auto samples = generate_sine(220.0f, 48000, 3.0f);
    for (auto& sample : samples) sample *= 0.2f;
    SonareMasteringParam params[] = {{"targetLufs", -13.0}, {"ceilingDb", -0.8}};

    char* json = nullptr;
    REQUIRE(sonare_mastering_assistant_suggest(samples.data(), samples.size(), 48000, params, 2,
                                               &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    REQUIRE(std::strstr(json, "\"chainConfig\"") != nullptr);
    REQUIRE(std::strstr(json, "\"explanation\"") != nullptr);
    REQUIRE(std::strstr(json, "\"genreCandidates\"") != nullptr);
    REQUIRE(std::strstr(json, "\"loudness.targetLufs\":-13") != nullptr);
    REQUIRE(std::strstr(json, "\"loudness.ceilingDb\":-0.8") != nullptr);
    sonare_free_string(json);
  }

  SECTION("assistant audio profile is reachable through the C API") {
    auto samples = generate_sine(330.0f, 48000, 2.0f);
    for (auto& sample : samples) sample *= 0.2f;
    SonareMasteringParam params[] = {{"nFft", 1024.0}, {"hopLength", 256.0}};

    char* json = nullptr;
    REQUIRE(sonare_mastering_audio_profile(samples.data(), samples.size(), 48000, params, 2,
                                           &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    REQUIRE(std::strstr(json, "\"durationSec\"") != nullptr);
    REQUIRE(std::strstr(json, "\"loudness\"") != nullptr);
    REQUIRE(std::strstr(json, "\"integratedLufs\"") != nullptr);
    REQUIRE(std::strstr(json, "\"spectral\"") != nullptr);
    REQUIRE(std::strstr(json, "\"centroidHz\"") != nullptr);
    REQUIRE(std::strstr(json, "\"dynamics\"") != nullptr);
    REQUIRE(std::strstr(json, "\"genreCandidates\"") != nullptr);
    sonare_free_string(json);
  }

  SECTION("all listed processors execute through the shared stereo entrypoint") {
    auto left = generate_sine(440.0f, 44100, 0.25f);
    auto right = generate_sine(660.0f, 44100, 0.25f);
    for (auto& sample : left) sample *= 0.18f;
    for (auto& sample : right) sample *= 0.12f;

    const auto names = split_lines(sonare_mastering_processor_names());
    REQUIRE_FALSE(names.empty());
    for (const auto& name : names) {
      INFO("processor: " << name);
      SonareMasteringStereoResult result{};
      REQUIRE(sonare_mastering_apply_processor_stereo(name.c_str(), left.data(), right.data(),
                                                      left.size(), 44100, nullptr, 0,
                                                      &result) == SONARE_OK);
      REQUIRE(result.left != nullptr);
      REQUIRE(result.right != nullptr);
      if (name == "repair.trimSilence") {
        REQUIRE(result.length <= left.size());
        REQUIRE(result.length > 0);
      } else {
        REQUIRE(result.length == left.size());
      }
      REQUIRE(std::isfinite(result.output_lufs));
      sonare_free_mastering_stereo_result(&result);
    }
  }

  SECTION("all listed pair processors and analyses execute") {
    auto source = generate_sine(440.0f, 44100, 0.25f);
    auto reference = generate_sine(880.0f, 44100, 0.25f);
    for (auto& sample : source) sample *= 0.18f;
    for (auto& sample : reference) sample *= 0.12f;

    const auto processors = split_lines(sonare_mastering_pair_processor_names());
    REQUIRE_FALSE(processors.empty());
    for (const auto& name : processors) {
      INFO("pair processor: " << name);
      SonareMasteringResult result{};
      REQUIRE(sonare_mastering_apply_pair_processor(name.c_str(), source.data(), reference.data(),
                                                    source.size(), 44100, nullptr, 0,
                                                    &result) == SONARE_OK);
      REQUIRE(result.samples != nullptr);
      REQUIRE(result.length == source.size());
      sonare_free_mastering_result(&result);
    }

    const auto pair_analyses = split_lines(sonare_mastering_pair_analysis_names());
    REQUIRE_FALSE(pair_analyses.empty());
    for (const auto& name : pair_analyses) {
      INFO("pair analysis: " << name);
      SonareMasteringParam params[] = {{"highHz", 18000.0}};
      char* json = nullptr;
      REQUIRE(sonare_mastering_analyze_pair(name.c_str(), source.data(), reference.data(),
                                            source.size(), 44100, params, 1, &json) == SONARE_OK);
      REQUIRE(json != nullptr);
      REQUIRE(std::strlen(json) > 2);
      sonare_free_string(json);
    }

    const auto stereo_analyses = split_lines(sonare_mastering_stereo_analysis_names());
    REQUIRE_FALSE(stereo_analyses.empty());
    for (const auto& name : stereo_analyses) {
      INFO("stereo analysis: " << name);
      SonareMasteringParam params[] = {{"highHz", 18000.0}};
      char* json = nullptr;
      REQUIRE(sonare_mastering_analyze_stereo(name.c_str(), source.data(), reference.data(),
                                              source.size(), 44100, params, 1, &json) == SONARE_OK);
      REQUIRE(json != nullptr);
      REQUIRE(std::strlen(json) > 2);
      sonare_free_string(json);
    }
  }
}

TEST_CASE("sonare_mastering_repair_declick", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(440.0f, sr, 0.5f);
  for (auto& s : samples) s *= 0.3f;
  // Inject a few impulsive clicks (sample-wide spikes).
  for (size_t i = 0; i < 5; ++i) {
    samples[2000 + i * 1500] = 1.0f;
  }

  SECTION("returns same-length cleaned buffer with NULL config (defaults)") {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, nullptr, &out,
                                            &out_length) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == samples.size());
    // Output should be finite and not silent (the wrapper preserves the signal).
    REQUIRE(std::isfinite(max_abs(out, out_length)));
    REQUIRE(max_abs(out, out_length) > 0.1f);
    sonare_free_floats(out);
  }

  SECTION("accepts explicit config") {
    SonareDeclickConfig config = {};
    config.threshold = 0.8f;
    config.neighbor_ratio = 4.0f;
    config.max_click_samples = 8;
    config.lpc_order = 20;
    config.residual_ratio = 8.0f;

    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, &config, &out,
                                            &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("rejects null out / bad inputs") {
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, nullptr, nullptr,
                                            nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_declick(nullptr, 0, sr, nullptr, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareDeclickConfig config = {};
    config.threshold = 0.0f;
    config.neighbor_ratio = 4.0f;
    config.max_click_samples = 8;
    config.lpc_order = 20;
    config.residual_ratio = 8.0f;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, &config, &out,
                                            &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_denoise_classical", "[c_api][mastering]") {
  const int sr = 22050;
  auto signal = generate_sine(440.0f, sr, 1.0f);
  // Add white noise.
  std::vector<float> noisy(signal.size());
  uint32_t state = 1u;
  for (size_t i = 0; i < signal.size(); ++i) {
    state = state * 1664525u + 1013904223u;
    float u = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);  // [0,1)
    float n = (u - 0.5f) * 0.4f;
    noisy[i] = 0.5f * signal[i] + n;
  }

  SECTION("LogMMSE default config reduces noise floor") {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, nullptr, &out,
                                                      &out_length) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == noisy.size());
    sonare_free_floats(out);
  }

  SECTION("Berouti SpectralSubtraction config runs") {
    SonareDenoiseClassicalConfig config = {};
    config.mode = SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION;
    config.noise_estimator = SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE;
    config.n_fft = 1024;
    config.hop_length = 256;
    config.dd_alpha = 0.98f;
    config.gain_floor = 0.05f;
    config.over_subtraction = 2.0f;
    config.spectral_floor = 0.05f;
    config.noise_estimation_quantile = 0.1f;
    config.speech_presence_gain = 0;
    config.gain_smoothing = 1;

    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config, &out,
                                                      &out_length) == SONARE_OK);
    REQUIRE(out_length == noisy.size());
    sonare_free_floats(out);
  }

  SECTION("rejects non-power-of-two n_fft and bad hop") {
    SonareDenoiseClassicalConfig config = {};
    config.mode = SONARE_DENOISE_MODE_LOG_MMSE;
    config.noise_estimator = SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE;
    config.n_fft = 1500;  // not a power of two
    config.hop_length = 256;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config, &out,
                                                      &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);

    config.n_fft = 1024;
    config.hop_length = 0;
    out = non_null_sentinel_float_ptr();
    out_length = 123;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config, &out,
                                                      &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }

  SECTION("rejects unknown mode and noise estimator enums") {
    SonareDenoiseClassicalConfig config = {};
    config.mode = 999;
    config.noise_estimator = SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE;
    config.n_fft = 1024;
    config.hop_length = 256;
    config.dd_alpha = 0.98f;
    config.gain_floor = 0.05f;
    config.over_subtraction = 2.0f;
    config.spectral_floor = 0.05f;
    config.noise_estimation_quantile = 0.1f;
    config.speech_presence_gain = 1;
    config.gain_smoothing = 1;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config, &out,
                                                      &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);

    config.mode = SONARE_DENOISE_MODE_LOG_MMSE;
    config.noise_estimator = 999;
    out = non_null_sentinel_float_ptr();
    out_length = 123;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config, &out,
                                                      &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_declip", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(440.0f, sr, 0.5f);
  // Hard-clip the signal at +/- 0.9.
  for (auto& s : samples) {
    s = std::max(-0.9f, std::min(0.9f, s * 2.0f));
  }

  SECTION("default config restores a length-matching buffer") {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_declip(samples.data(), samples.size(), sr, nullptr, &out,
                                           &out_length) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == samples.size());
    REQUIRE(std::isfinite(max_abs(out, out_length)));
    sonare_free_floats(out);
  }

  SECTION("explicit config") {
    SonareDeclipConfig config = {};
    config.clip_threshold = 0.85f;
    config.lpc_order = 24;
    config.iterations = 1;
    config.lpc_blend = 0.5f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_declip(samples.data(), samples.size(), sr, &config, &out,
                                           &out_length) == SONARE_OK);
    sonare_free_floats(out);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareDeclipConfig config = {};
    config.clip_threshold = 2.0f;
    config.lpc_order = 24;
    config.iterations = 1;
    config.lpc_blend = 0.5f;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_declip(samples.data(), samples.size(), sr, &config, &out,
                                           &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_decrackle", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(440.0f, sr, 0.5f);
  for (auto& s : samples) s *= 0.4f;
  // Inject crackle impulses.
  for (size_t i = 500; i < samples.size(); i += 1700) {
    samples[i] = (i % 2 == 0) ? 0.95f : -0.95f;
  }

  SECTION("median mode (default)") {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, nullptr, &out,
                                              &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("wavelet shrinkage mode") {
    SonareDecrackleConfig config = {};
    config.threshold = 0.4f;
    config.mode = SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE;
    config.levels = 4;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, &config, &out,
                                              &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareDecrackleConfig config = {};
    config.threshold = 0.0f;
    config.mode = SONARE_DECRACKLE_MODE_MEDIAN;
    config.levels = 4;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, &config, &out,
                                              &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }

  SECTION("rejects unknown mode enum") {
    SonareDecrackleConfig config = {};
    config.threshold = 0.4f;
    config.mode = 999;
    config.levels = 4;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, &config, &out,
                                              &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_dehum", "[c_api][mastering]") {
  const int sr = 48000;
  auto signal = generate_sine(440.0f, sr, 1.0f);
  auto hum = generate_sine(50.0f, sr, 1.0f);
  std::vector<float> samples(signal.size());
  for (size_t i = 0; i < signal.size(); ++i) samples[i] = 0.5f * signal[i] + 0.2f * hum[i];

  SECTION("static notch (default)") {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_dehum(samples.data(), samples.size(), sr, nullptr, &out,
                                          &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("adaptive tracking with explicit config") {
    SonareDehumConfig config = {};
    config.fundamental_hz = 50.0f;
    config.harmonics = 4;
    config.q = 20.0f;
    config.adaptive = 1;
    config.search_range_hz = 2.0f;
    config.adaptation = 0.25f;
    config.frame_size = 2048;
    config.pll_bandwidth = 0.01f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_dehum(samples.data(), samples.size(), sr, &config, &out,
                                          &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareDehumConfig config = {};
    config.fundamental_hz = 0.0f;
    config.harmonics = 4;
    config.q = 20.0f;
    config.frame_size = 2048;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_dehum(samples.data(), samples.size(), sr, &config, &out,
                                          &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_dereverb_classical", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(440.0f, sr, 1.0f);
  for (auto& s : samples) s *= 0.5f;

  SECTION("default config") {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, nullptr,
                                                       &out, &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("WPE-enabled config") {
    SonareDereverbClassicalConfig config = {};
    config.threshold = 0.05f;
    config.attenuation = 0.5f;
    config.n_fft = 1024;
    config.hop_length = 256;
    config.t60_sec = 0.4f;
    config.late_delay_ms = 50.0f;
    config.over_subtraction = 1.0f;
    config.spectral_floor = 0.08f;
    config.wpe_enabled = 1;
    config.wpe_iterations = 2;
    config.wpe_taps = 3;
    config.wpe_strength = 0.7f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, &config,
                                                       &out, &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("rejects bad n_fft / hop_length") {
    SonareDereverbClassicalConfig config = {};
    config.threshold = 0.05f;
    config.attenuation = 0.5f;
    config.n_fft = 1500;  // not a power of two
    config.hop_length = 256;
    config.t60_sec = 0.4f;
    config.late_delay_ms = 50.0f;
    config.over_subtraction = 1.0f;
    config.spectral_floor = 0.08f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, &config,
                                                       &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);

    config.n_fft = 1024;
    config.hop_length = 2048;  // larger than n_fft
    out = non_null_sentinel_float_ptr();
    out_length = 123;
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, &config,
                                                       &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_trim_silence", "[c_api][mastering]") {
  const int sr = 48000;
  const size_t silent_pad = 2400;  // 50 ms
  std::vector<float> samples(silent_pad, 0.0f);
  auto sig = generate_sine(440.0f, sr, 0.2f);
  for (auto& s : sig) s *= 0.5f;
  samples.insert(samples.end(), sig.begin(), sig.end());
  samples.insert(samples.end(), silent_pad, 0.0f);

  SECTION("peak mode (default) shortens the buffer") {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, nullptr, &out,
                                                 &out_length) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length < samples.size());
    REQUIRE(out_length > 0);
    sonare_free_floats(out);
  }

  SECTION("LUFS-gated mode with padding") {
    SonareTrimSilenceConfig config = {};
    config.threshold = 0.001f;
    config.padding_samples = 1200;
    config.mode = SONARE_TRIM_SILENCE_MODE_LUFS_GATED;
    config.gate_lufs = -40.0f;
    config.window_ms = 400.0f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config, &out,
                                                 &out_length) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length > 0);
    sonare_free_floats(out);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareTrimSilenceConfig config = {};
    config.threshold = -1.0f;
    config.mode = SONARE_TRIM_SILENCE_MODE_PEAK;
    config.window_ms = 400.0f;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config, &out,
                                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }

  SECTION("rejects unknown mode enum") {
    SonareTrimSilenceConfig config = {};
    config.threshold = 0.001f;
    config.padding_samples = 0;
    config.mode = 999;
    config.gate_lufs = -60.0f;
    config.window_ms = 400.0f;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config, &out,
                                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }
}

namespace {
float max_abs_sample(const float* buf, size_t length) {
  float peak = 0.0f;
  for (size_t i = 0; i < length; ++i) {
    peak = std::max(peak, std::abs(buf[i]));
  }
  return peak;
}
}  // namespace

TEST_CASE("sonare_mastering_dynamics_compressor", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(440.0f, sr, 0.5f);
  for (auto& s : samples) s *= 0.9f;  // hot signal so compression engages

  SECTION("default config returns finite buffer of the same length") {
    float* out = nullptr;
    size_t out_length = 0;
    int latency = -1;
    REQUIRE(sonare_mastering_dynamics_compressor(samples.data(), samples.size(), sr, nullptr, &out,
                                                 &out_length, &latency) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == samples.size());
    REQUIRE(latency >= 0);
    REQUIRE(std::isfinite(max_abs_sample(out, out_length)));
    sonare_free_floats(out);
  }

  SECTION("strong threshold + 4:1 ratio reduces peak vs input") {
    SonareCompressorConfig config = {};
    config.threshold_db = -24.0f;
    config.ratio = 4.0f;
    config.attack_ms = 1.0f;
    config.release_ms = 50.0f;
    config.knee_db = 0.0f;
    config.makeup_gain_db = 0.0f;
    config.auto_makeup = 0;
    config.detector = SONARE_COMPRESSOR_DETECTOR_PEAK;
    config.sidechain_hpf_hz = 100.0f;
    config.pdr_release_scale = 1.0f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_dynamics_compressor(samples.data(), samples.size(), sr, &config, &out,
                                                 &out_length, nullptr) == SONARE_OK);
    const float in_peak = max_abs_sample(samples.data(), samples.size());
    const float out_peak = max_abs_sample(out, out_length);
    REQUIRE(out_peak < in_peak);
    sonare_free_floats(out);
  }

  SECTION("NULL output pointer returns invalid parameter") {
    REQUIRE(sonare_mastering_dynamics_compressor(samples.data(), samples.size(), sr, nullptr,
                                                 nullptr, nullptr,
                                                 nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_dynamics_gate", "[c_api][mastering]") {
  const int sr = 48000;
  // 200 ms of loud tone followed by 200 ms of near-silence.
  std::vector<float> samples;
  auto loud = generate_sine(440.0f, sr, 0.2f);
  samples.insert(samples.end(), loud.begin(), loud.end());
  for (auto& s : loud) s *= 0.0005f;  // -66 dBFS, well below default -50 threshold
  samples.insert(samples.end(), loud.begin(), loud.end());

  SECTION("default config returns finite buffer of the same length") {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_dynamics_gate(samples.data(), samples.size(), sr, nullptr, &out,
                                           &out_length, nullptr) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == samples.size());
    REQUIRE(std::isfinite(max_abs_sample(out, out_length)));
    sonare_free_floats(out);
  }

  SECTION("gate attenuates the silent tail vs input") {
    SonareGateConfig config = {};
    config.threshold_db = -40.0f;
    config.attack_ms = 1.0f;
    config.release_ms = 20.0f;
    config.range_db = -60.0f;
    config.close_threshold_db = -40.0f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_dynamics_gate(samples.data(), samples.size(), sr, &config, &out,
                                           &out_length, nullptr) == SONARE_OK);
    // Last 50 ms of the input should be quieter on output (gated down).
    const size_t tail = static_cast<size_t>(0.05 * sr);
    const float in_tail_peak = max_abs_sample(samples.data() + samples.size() - tail, tail);
    const float out_tail_peak = max_abs_sample(out + out_length - tail, tail);
    REQUIRE(out_tail_peak < in_tail_peak);
    sonare_free_floats(out);
  }
}

TEST_CASE("sonare_mastering_dynamics_transient_shaper", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_clicks(120.0f, sr, 2.0f);
  for (auto& s : samples) s *= 0.5f;

  SECTION("default config returns finite buffer of the same length") {
    float* out = nullptr;
    size_t out_length = 0;
    int latency = -1;
    REQUIRE(sonare_mastering_dynamics_transient_shaper(samples.data(), samples.size(), sr, nullptr,
                                                       &out, &out_length, &latency) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == samples.size());
    REQUIRE(latency >= 0);
    REQUIRE(std::isfinite(max_abs_sample(out, out_length)));
    sonare_free_floats(out);
  }

  SECTION("boosted attack lifts the click peaks") {
    SonareTransientShaperConfig config = {};
    config.attack_gain_db = 9.0f;
    config.sustain_gain_db = 0.0f;
    config.fast_attack_ms = 0.0f;
    config.fast_release_ms = 10.0f;
    config.slow_attack_ms = 30.0f;
    config.slow_release_ms = 200.0f;
    config.sensitivity = 1.0f;
    config.max_gain_db = 12.0f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_dynamics_transient_shaper(samples.data(), samples.size(), sr, &config,
                                                       &out, &out_length, nullptr) == SONARE_OK);
    const float in_peak = max_abs_sample(samples.data(), samples.size());
    const float out_peak = max_abs_sample(out, out_length);
    REQUIRE(out_peak > in_peak);
    sonare_free_floats(out);
  }
}
#endif

TEST_CASE("sonare_onset_strength", "[c_api]") {
  SECTION("returns a finite onset envelope") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    float* env = nullptr;
    size_t count = 0;

    SonareError err =
        sonare_onset_strength(samples.data(), samples.size(), 22050, 2048, 512, 128, &env, &count);

    REQUIRE(err == SONARE_OK);
    REQUIRE(count > 0);
    REQUIRE(env != nullptr);
    for (size_t i = 0; i < count; ++i) {
      REQUIRE(std::isfinite(env[i]));
    }
    sonare_free_floats(env);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_clicks(120.0f, 22050, 1.0f);
    float* env = nullptr;
    size_t count = 0;

    REQUIRE(sonare_onset_strength(nullptr, samples.size(), 22050, 2048, 512, 128, &env, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_onset_strength(samples.data(), samples.size(), 22050, 2048, 512, 128, nullptr,
                                  &count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_onset_strength(samples.data(), samples.size(), 22050, 2048, 512, 128, &env,
                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_fourier_tempogram", "[c_api]") {
  SECTION("returns an [n_bins x n_frames] magnitude matrix") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    float* env = nullptr;
    size_t env_count = 0;
    REQUIRE(sonare_onset_strength(samples.data(), samples.size(), 22050, 2048, 512, 128, &env,
                                  &env_count) == SONARE_OK);

    const int win_length = 384;
    float* data = nullptr;
    size_t out_length = 0;
    int n_frames = 0;
    SonareError err = sonare_fourier_tempogram(env, env_count, 22050, 512, win_length, 1, 1, &data,
                                               &out_length, &n_frames);

    REQUIRE(err == SONARE_OK);
    REQUIRE(data != nullptr);
    REQUIRE(n_frames == static_cast<int>(env_count));
    const size_t n_bins = static_cast<size_t>(win_length) / 2 + 1;
    REQUIRE(out_length == n_bins * static_cast<size_t>(n_frames));
    for (size_t i = 0; i < out_length; ++i) {
      REQUIRE(std::isfinite(data[i]));
    }

    sonare_free_floats(data);
    sonare_free_floats(env);
  }

  SECTION("rejects invalid parameters") {
    std::vector<float> env(256, 0.1f);
    float* data = nullptr;
    size_t out_length = 0;
    int n_frames = 0;

    REQUIRE(sonare_fourier_tempogram(nullptr, env.size(), 22050, 512, 384, 1, 1, &data, &out_length,
                                     &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_fourier_tempogram(env.data(), env.size(), 22050, 512, 384, 1, 1, &data,
                                     &out_length, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_tempogram_with_mode", "[c_api]") {
  const std::vector<float> env{0.2f, 1.0f, 0.4f, 0.0f, 0.8f, 0.1f, 0.5f, 0.3f,
                               0.6f, 0.0f, 0.9f, 0.2f, 0.4f, 0.7f, 0.1f, 0.5f};
  float* data = nullptr;
  size_t out_length = 0;
  int n_frames = 0;
  SonareError err =
      sonare_tempogram_with_mode(env.data(), env.size(), 8, 1, 8, 0, 0, SONARE_TEMPOGRAM_COSINE,
                                 &data, &out_length, &n_frames);

  REQUIRE(err == SONARE_OK);
  REQUIRE(data != nullptr);
  REQUIRE(n_frames == static_cast<int>(env.size()));
  REQUIRE(out_length == env.size() * 8);
  for (size_t i = 0; i < out_length; ++i) {
    REQUIRE(std::isfinite(data[i]));
    REQUIRE(data[i] >= -1.0f - 1.0e-6f);
    REQUIRE(data[i] <= 1.0f + 1.0e-6f);
  }
  sonare_free_floats(data);

  REQUIRE(sonare_tempogram_with_mode(env.data(), env.size(), 8, 1, 8, 0, 0, 99, &data, &out_length,
                                     &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_tempogram_ratio", "[c_api]") {
  SECTION("returns one finite value per factor") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    float* env = nullptr;
    size_t env_count = 0;
    REQUIRE(sonare_onset_strength(samples.data(), samples.size(), 22050, 2048, 512, 128, &env,
                                  &env_count) == SONARE_OK);

    const int win_length = 384;
    float* tg = nullptr;
    size_t tg_length = 0;
    int tg_frames = 0;
    REQUIRE(sonare_tempogram(env, env_count, 22050, 512, win_length, 1, 1, &tg, &tg_length,
                             &tg_frames) == SONARE_OK);

    SECTION("default factors") {
      float* ratio = nullptr;
      size_t ratio_count = 0;
      SonareError err = sonare_tempogram_ratio(tg, tg_length, win_length, 22050, 512, nullptr, 0,
                                               &ratio, &ratio_count);
      REQUIRE(err == SONARE_OK);
      REQUIRE(ratio != nullptr);
      REQUIRE(ratio_count == 5);  // {0.5, 1, 2, 3, 4}
      for (size_t i = 0; i < ratio_count; ++i) {
        REQUIRE(std::isfinite(ratio[i]));
      }
      sonare_free_floats(ratio);
    }

    SECTION("explicit factors") {
      const float factors[] = {1.0f, 2.0f, 3.0f};
      float* ratio = nullptr;
      size_t ratio_count = 0;
      SonareError err = sonare_tempogram_ratio(tg, tg_length, win_length, 22050, 512, factors, 3,
                                               &ratio, &ratio_count);
      REQUIRE(err == SONARE_OK);
      REQUIRE(ratio_count == 3);
      for (size_t i = 0; i < ratio_count; ++i) {
        REQUIRE(std::isfinite(ratio[i]));
      }
      sonare_free_floats(ratio);
    }

    sonare_free_floats(tg);
    sonare_free_floats(env);
  }

  SECTION("rejects invalid parameters") {
    std::vector<float> tg(384 * 4, 0.1f);
    const float factors[] = {1.0f};
    float* ratio = nullptr;
    size_t ratio_count = 0;

    REQUIRE(sonare_tempogram_ratio(nullptr, tg.size(), 384, 22050, 512, factors, 1, &ratio,
                                   &ratio_count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_tempogram_ratio(tg.data(), tg.size(), 384, 22050, 512, nullptr, 2, &ratio,
                                   &ratio_count) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_nnls_chroma", "[c_api]") {
  SECTION("returns a 12 x n_frames chroma matrix") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 2.0f);
    float* data = nullptr;
    size_t out_length = 0;
    int n_frames = 0;

    SonareError err =
        sonare_nnls_chroma(samples.data(), samples.size(), 22050, &data, &out_length, &n_frames);

    REQUIRE(err == SONARE_OK);
    REQUIRE(data != nullptr);
    REQUIRE(n_frames > 0);
    REQUIRE(out_length == 12u * static_cast<size_t>(n_frames));
    for (size_t i = 0; i < out_length; ++i) {
      REQUIRE(std::isfinite(data[i]));
      REQUIRE(data[i] >= 0.0f);  // NNLS output is non-negative
    }
    sonare_free_floats(data);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_sine(440.0f, 22050, 1.0f);
    float* data = nullptr;
    size_t out_length = 0;
    int n_frames = 0;

    REQUIRE(sonare_nnls_chroma(nullptr, samples.size(), 22050, &data, &out_length, &n_frames) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, nullptr, &out_length,
                               &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, &data, nullptr, &n_frames) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, &data, &out_length,
                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_lufs", "[c_api]") {
  SECTION("returns finite loudness measures") {
    auto samples = generate_sine(440.0f, 48000, 3.0f);
    SonareLufsResult result = {};

    SonareError err = sonare_lufs(samples.data(), samples.size(), 48000, &result);

    REQUIRE(err == SONARE_OK);
    REQUIRE(std::isfinite(result.integrated_lufs));
    REQUIRE(std::isfinite(result.momentary_lufs));
    REQUIRE(std::isfinite(result.short_term_lufs));
    REQUIRE(std::isfinite(result.loudness_range));
    REQUIRE(result.loudness_range >= 0.0f);
  }

  SECTION("louder signal measures higher integrated LUFS") {
    auto loud = generate_sine(440.0f, 48000, 3.0f);
    auto quiet = loud;
    for (auto& sample : quiet) sample *= 0.1f;

    SonareLufsResult loud_result = {};
    SonareLufsResult quiet_result = {};
    REQUIRE(sonare_lufs(loud.data(), loud.size(), 48000, &loud_result) == SONARE_OK);
    REQUIRE(sonare_lufs(quiet.data(), quiet.size(), 48000, &quiet_result) == SONARE_OK);

    REQUIRE(loud_result.integrated_lufs > quiet_result.integrated_lufs);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    SonareLufsResult result = {};

    REQUIRE(sonare_lufs(nullptr, samples.size(), 48000, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_lufs(samples.data(), samples.size(), 48000, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_momentary_lufs and sonare_short_term_lufs", "[c_api]") {
  SECTION("return finite time series") {
    auto samples = generate_sine(440.0f, 48000, 3.0f);

    float* momentary = nullptr;
    size_t momentary_count = 0;
    REQUIRE(sonare_momentary_lufs(samples.data(), samples.size(), 48000, &momentary,
                                  &momentary_count) == SONARE_OK);
    REQUIRE(momentary != nullptr);
    REQUIRE(momentary_count > 0);
    for (size_t i = 0; i < momentary_count; ++i) {
      REQUIRE(std::isfinite(momentary[i]));
    }
    sonare_free_floats(momentary);

    float* short_term = nullptr;
    size_t short_term_count = 0;
    REQUIRE(sonare_short_term_lufs(samples.data(), samples.size(), 48000, &short_term,
                                   &short_term_count) == SONARE_OK);
    REQUIRE(short_term != nullptr);
    REQUIRE(short_term_count > 0);
    for (size_t i = 0; i < short_term_count; ++i) {
      REQUIRE(std::isfinite(short_term[i]));
    }
    sonare_free_floats(short_term);
  }

  SECTION("reject invalid parameters") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    float* out = nullptr;
    size_t count = 0;

    REQUIRE(sonare_momentary_lufs(nullptr, samples.size(), 48000, &out, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_short_term_lufs(nullptr, samples.size(), 48000, &out, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_metering basic offline meters", "[c_api]") {
  SECTION("peak / rms / crest / dc agree with library") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    float peak = 0.0f;
    float rms = 0.0f;
    float crest = 0.0f;
    float dc = 0.0f;
    REQUIRE(sonare_metering_peak_db(samples.data(), samples.size(), 48000, &peak) == SONARE_OK);
    REQUIRE(sonare_metering_rms_db(samples.data(), samples.size(), 48000, &rms) == SONARE_OK);
    REQUIRE(sonare_metering_crest_factor_db(samples.data(), samples.size(), 48000, &crest) ==
            SONARE_OK);
    REQUIRE(sonare_metering_dc_offset(samples.data(), samples.size(), 48000, &dc) == SONARE_OK);
    REQUIRE(std::isfinite(peak));
    REQUIRE(std::isfinite(rms));
    REQUIRE(peak >= rms);
    REQUIRE(crest == Catch::Approx(peak - rms).margin(1e-3f));
    REQUIRE(std::abs(dc) < 1e-2f);
  }

  SECTION("rejects null output and bad inputs") {
    auto samples = generate_sine(440.0f, 48000, 0.1f);
    float out = 0.0f;
    REQUIRE(sonare_metering_peak_db(samples.data(), samples.size(), 48000, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_metering_true_peak_db(samples.data(), samples.size(), 48000, 3, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);  // 3 is not a power of two
  }
}

TEST_CASE("sonare_metering_true_peak_db", "[c_api]") {
  SECTION("matches sample peak within a small margin for low-frequency sine") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    float peak_db_value = 0.0f;
    float tp_db = 0.0f;
    REQUIRE(sonare_metering_peak_db(samples.data(), samples.size(), 48000, &peak_db_value) ==
            SONARE_OK);
    REQUIRE(sonare_metering_true_peak_db(samples.data(), samples.size(), 48000, 0, &tp_db) ==
            SONARE_OK);
    // True peak >= sample peak (inter-sample peaks can only be higher).
    REQUIRE(tp_db >= peak_db_value - 0.1f);
  }
}

TEST_CASE("sonare_metering_detect_clipping", "[c_api]") {
  SECTION("reports clipped regions and frees them") {
    // Hard-clipped signal: ones in a run.
    std::vector<float> samples(8000, 0.1f);
    for (size_t i = 1000; i < 1064; ++i) samples[i] = 1.0f;
    SonareClippingResult result = {};
    REQUIRE(sonare_metering_detect_clipping(samples.data(), samples.size(), 48000, 0.999f, 0,
                                            &result) == SONARE_OK);
    REQUIRE(result.region_count >= 1);
    REQUIRE(result.regions != nullptr);
    REQUIRE(result.clipped_samples >= 1);
    REQUIRE(result.max_clipped_peak >= 1.0f);
    sonare_free_clipping_result(&result);
    REQUIRE(result.regions == nullptr);
    REQUIRE(result.region_count == 0);
  }

  SECTION("clean signal reports zero regions") {
    auto samples = generate_sine(440.0f, 48000, 0.5f);
    for (auto& s : samples) s *= 0.3f;
    SonareClippingResult result = {};
    REQUIRE(sonare_metering_detect_clipping(samples.data(), samples.size(), 48000, 0.999f, 0,
                                            &result) == SONARE_OK);
    REQUIRE(result.region_count == 0);
    REQUIRE(result.regions == nullptr);
    sonare_free_clipping_result(&result);
  }
}

TEST_CASE("sonare_metering_dynamic_range", "[c_api]") {
  SECTION("returns a positive DR for varying-level signal") {
    // 5 seconds: alternate 0.5s loud / 0.5s quiet.
    int sr = 48000;
    int total = sr * 5;
    std::vector<float> samples(total, 0.0f);
    auto loud = generate_sine(440.0f, sr, 0.5f);
    for (int i = 0; i < 5; ++i) {
      int offset = i * sr;
      float amp = (i % 2 == 0) ? 0.8f : 0.05f;
      for (size_t j = 0; j < loud.size() && offset + j < samples.size(); ++j) {
        samples[offset + j] = loud[j] * amp;
      }
    }
    SonareDynamicRangeResult result = {};
    REQUIRE(sonare_metering_dynamic_range(samples.data(), samples.size(), sr, 0.0f, 0.0f, 0.0f,
                                          0.0f, &result) == SONARE_OK);
    REQUIRE(result.window_count > 0);
    REQUIRE(result.window_rms_db != nullptr);
    REQUIRE(result.dynamic_range_db > 0.0f);
    sonare_free_dynamic_range_result(&result);
    REQUIRE(result.window_rms_db == nullptr);
  }

  SECTION("rejects inverted percentiles") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    SonareDynamicRangeResult result = {};
    REQUIRE(sonare_metering_dynamic_range(samples.data(), samples.size(), 48000, 0.0f, 0.0f, 0.9f,
                                          0.1f, &result) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_metering stereo wrappers", "[c_api]") {
  const int sr = 48000;
  auto left = generate_sine(440.0f, sr, 0.5f);
  std::vector<float> right_in_phase = left;
  std::vector<float> right_inverted(left.size());
  for (size_t i = 0; i < left.size(); ++i) right_inverted[i] = -left[i];

  SECTION("correlation: in-phase ≈ +1, inverted ≈ -1") {
    float c = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(left.data(), right_in_phase.data(), left.size(), sr,
                                               &c) == SONARE_OK);
    REQUIRE(c == Catch::Approx(1.0f).margin(1e-3f));
    REQUIRE(sonare_metering_stereo_correlation(left.data(), right_inverted.data(), left.size(), sr,
                                               &c) == SONARE_OK);
    REQUIRE(c == Catch::Approx(-1.0f).margin(1e-3f));
  }

  SECTION("stereo_width: mono ≈ 0, inverted > mono") {
    float mono_width = 0.0f;
    float inv_width = 0.0f;
    REQUIRE(sonare_metering_stereo_width(left.data(), right_in_phase.data(), left.size(), sr,
                                         &mono_width) == SONARE_OK);
    REQUIRE(sonare_metering_stereo_width(left.data(), right_inverted.data(), left.size(), sr,
                                         &inv_width) == SONARE_OK);
    REQUIRE(mono_width < 1e-3f);
    REQUIRE(inv_width > mono_width);
  }

  SECTION("vectorscope returns one point per sample and frees cleanly") {
    SonareVectorscopeResult result = {};
    REQUIRE(sonare_metering_vectorscope(left.data(), right_in_phase.data(), left.size(), sr,
                                        &result) == SONARE_OK);
    REQUIRE(result.point_count == left.size());
    REQUIRE(result.points != nullptr);
    // In-phase: side ≈ 0 for every sample.
    float max_side = 0.0f;
    for (size_t i = 0; i < result.point_count; ++i) {
      max_side = std::max(max_side, std::abs(result.points[i].side));
    }
    REQUIRE(max_side < 1e-3f);
    sonare_free_vectorscope_result(&result);
    REQUIRE(result.points == nullptr);
    REQUIRE(result.point_count == 0);
  }

  SECTION("phase_scope populates summary stats") {
    SonarePhaseScopeResult result = {};
    REQUIRE(sonare_metering_phase_scope(left.data(), right_in_phase.data(), left.size(), sr,
                                        &result) == SONARE_OK);
    REQUIRE(result.point_count == left.size());
    REQUIRE(result.points != nullptr);
    REQUIRE(result.correlation == Catch::Approx(1.0f).margin(1e-3f));
    REQUIRE(result.max_radius > 0.0f);
    sonare_free_phase_scope_result(&result);
    REQUIRE(result.points == nullptr);
    REQUIRE(result.point_count == 0);
  }

  SECTION("rejects null pair / null out") {
    float out_v = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(nullptr, right_in_phase.data(), left.size(), sr,
                                               &out_v) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_metering_stereo_correlation(left.data(), right_in_phase.data(), left.size(), sr,
                                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    SonareVectorscopeResult vs = {};
    REQUIRE(sonare_metering_vectorscope(left.data(), nullptr, left.size(), sr, &vs) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_metering_spectrum", "[c_api]") {
  SECTION("returns n_fft/2+1 bins and a peak near the tone frequency") {
    const int sr = 48000;
    const int n_fft = 2048;
    auto samples = generate_sine(1000.0f, sr, 0.5f);
    SonareSpectrumResult result = {};
    REQUIRE(sonare_metering_spectrum(samples.data(), samples.size(), sr, n_fft, 0, 0, 0.0f, 0.0f,
                                     &result) == SONARE_OK);
    REQUIRE(result.bin_count == static_cast<size_t>(n_fft / 2 + 1));
    REQUIRE(result.n_fft == n_fft);
    REQUIRE(result.sample_rate == sr);
    REQUIRE(result.frequencies != nullptr);
    REQUIRE(result.magnitude != nullptr);
    REQUIRE(result.power != nullptr);
    REQUIRE(result.db != nullptr);
    // Identify the peak bin.
    size_t peak_bin = 0;
    float peak_mag = -1.0f;
    for (size_t i = 0; i < result.bin_count; ++i) {
      if (result.magnitude[i] > peak_mag) {
        peak_mag = result.magnitude[i];
        peak_bin = i;
      }
    }
    const float peak_freq = result.frequencies[peak_bin];
    REQUIRE(peak_freq == Catch::Approx(1000.0f).margin(60.0f));
    // power[i] ≈ magnitude[i]^2.
    REQUIRE(result.power[peak_bin] ==
            Catch::Approx(result.magnitude[peak_bin] * result.magnitude[peak_bin]).margin(1e-2f));
    sonare_free_spectrum_result(&result);
    REQUIRE(result.frequencies == nullptr);
    REQUIRE(result.bin_count == 0);
  }

  SECTION("rejects non-power-of-two n_fft") {
    auto samples = generate_sine(440.0f, 48000, 0.1f);
    SonareSpectrumResult result = {};
    REQUIRE(sonare_metering_spectrum(samples.data(), samples.size(), 48000, 1500, 0, 0, 0.0f, 0.0f,
                                     &result) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_scale_quantize_midi", "[c_api]") {
  // C major scale mask: C D E F G A B (bits 0,2,4,5,7,9,11).
  static constexpr uint16_t kCMajorMask = 0b101010110101;

  SECTION("snaps an off-scale note to the nearest in-scale neighbour") {
    // C#4 (61) -> nearest in C major is C4 (60) or D4 (62); either is acceptable.
    float out = 0.0f;
    REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, 0.0f, 61.0f, &out) == SONARE_OK);
    REQUIRE(
        (out == Catch::Approx(60.0f).margin(0.01f) || out == Catch::Approx(62.0f).margin(0.01f)));
  }

  SECTION("in-scale notes pass through") {
    float out = 0.0f;
    REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, 0.0f, 60.0f, &out) == SONARE_OK);
    REQUIRE(out == Catch::Approx(60.0f).margin(0.01f));
  }

  SECTION("correction_semitones agrees with quantize_midi") {
    float q = 0.0f;
    float c = 0.0f;
    REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, 0.0f, 61.4f, &q) == SONARE_OK);
    REQUIRE(sonare_scale_correction_semitones(0, kCMajorMask, 0.0f, 61.4f, &c) == SONARE_OK);
    REQUIRE(c == Catch::Approx(q - 61.4f).margin(0.01f));
  }

  SECTION("pitch_class_enabled reflects the mode mask") {
    int enabled = -1;
    REQUIRE(sonare_scale_pitch_class_enabled(0, kCMajorMask, 0, &enabled) == SONARE_OK);
    REQUIRE(enabled == 1);  // C is in C major
    REQUIRE(sonare_scale_pitch_class_enabled(0, kCMajorMask, 1, &enabled) == SONARE_OK);
    REQUIRE(enabled == 0);  // C# is not
  }

  SECTION("rejects bad roots / pitch classes / empty mask") {
    float out = 0.0f;
    REQUIRE(sonare_scale_quantize_midi(-1, kCMajorMask, 0.0f, 60.0f, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_scale_quantize_midi(0, 0, 0.0f, 60.0f, &out) == SONARE_ERROR_INVALID_PARAMETER);
    int enabled = -1;
    REQUIRE(sonare_scale_pitch_class_enabled(0, kCMajorMask, 12, &enabled) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_error_message", "[c_api]") {
  SECTION("returns messages for all error codes") {
    REQUIRE(std::strcmp(sonare_error_message(SONARE_OK), "OK") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_FILE_NOT_FOUND), "File not found") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_INVALID_FORMAT), "Invalid format") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_DECODE_FAILED), "Decode failed") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_INVALID_PARAMETER),
                        "Invalid parameter") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_OUT_OF_MEMORY), "Out of memory") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_UNKNOWN), "Unknown error") == 0);
  }
}

TEST_CASE("sonare_version", "[c_api]") {
  SECTION("returns version string") {
    const char* ver = sonare_version();
    REQUIRE(ver != nullptr);
    REQUIRE(std::strlen(ver) > 0);
  }

  SECTION("returns engine ABI version") { REQUIRE(sonare_engine_abi_version() > 0); }
}

TEST_CASE("sonare_engine MIDI scalar commands respect arrangement feature flag", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(sonare_engine_push_midi_cc(engine, 0, 0, 0, 74, 100, -1) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_panic(engine, -1) == SONARE_OK);
#else
  REQUIRE(sonare_engine_push_midi_cc(engine, 0, 0, 0, 74, 100, -1) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_push_midi_panic(engine, -1) == SONARE_ERROR_NOT_SUPPORTED);
#endif
  REQUIRE(sonare_engine_push_midi_cc(engine, 0, 16, 0, 74, 100, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

#ifdef SONARE_WITH_VOICE_CHANGER
TEST_CASE("sonare_daw_editing_c_api_smoke", "[c_api]") {
  auto samples = generate_sine(440.0f, 22050, 0.25f);
  float* out = nullptr;
  size_t out_length = 0;

  REQUIRE(sonare_pitch_correct_to_midi(samples.data(), samples.size(), 22050, 69.0f, 70.0f, &out,
                                       &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == samples.size());
  sonare_free_floats(out);

  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_note_stretch(samples.data(), samples.size(), 22050, 100, 1000, 1.25f, &out,
                              &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length > samples.size());
  sonare_free_floats(out);

  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_voice_change(samples.data(), samples.size(), 22050, 5.0f, 1.1f, &out,
                              &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == samples.size());
  sonare_free_floats(out);
}

TEST_CASE("sonare_voice_change_realtime processes mono and interleaved stereo buffers",
          "[c_api][voice_changer]") {
  std::vector<float> mono(384);
  for (size_t i = 0; i < mono.size(); ++i) {
    mono[i] =
        0.05f * std::sin(sonare::constants::kTwoPi * 220.0f * static_cast<float>(i) / 48000.0f);
  }

  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_voice_change_realtime(mono.data(), mono.size(), 48000, "neutral-monitor", 1, &out,
                                       &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == mono.size());
  for (size_t i = 0; i < out_length; ++i) {
    REQUIRE(std::isfinite(out[i]));
  }
  sonare_free_floats(out);

  std::vector<float> stereo(mono.size() * 2);
  for (size_t i = 0; i < mono.size(); ++i) {
    stereo[i * 2] = mono[i];
    stereo[i * 2 + 1] = -mono[i];
  }
  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_voice_change_realtime(stereo.data(), stereo.size(), 48000, "soft-whisper", 2, &out,
                                       &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == stereo.size());
  for (size_t i = 0; i < out_length; ++i) {
    REQUIRE(std::isfinite(out[i]));
  }
  sonare_free_floats(out);

  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_voice_change_realtime(stereo.data(), stereo.size() - 1, 48000, "soft-whisper", 2,
                                       &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_length == 0);

  REQUIRE(sonare_voice_change_realtime(mono.data(), mono.size(), 48000, "neutral-monitor", 3, &out,
                                       &out_length) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_realtime_voice_changer ISP limiter fields round-trip through the POD config",
          "[c_api]") {
  SonareRealtimeVoiceChangerConfig config{};
  REQUIRE(sonare_realtime_voice_changer_preset_config(SONARE_VC_PRESET_NEUTRAL_MONITOR, &config) ==
          SONARE_OK);
  config.limiter_enable_isp_limiter = 0;
  config.limiter_isp_ceiling_dbtp = -2.5f;

  SonareRealtimeVoiceChanger* handle = nullptr;
  REQUIRE(sonare_realtime_voice_changer_create(&config, 48000, 128, 1, &handle) == SONARE_OK);
  REQUIRE(handle != nullptr);

  SonareRealtimeVoiceChangerConfig read_back{};
  REQUIRE(sonare_realtime_voice_changer_get_config(handle, &read_back) == SONARE_OK);
  REQUIRE(read_back.limiter_enable_isp_limiter == 0);
  REQUIRE(read_back.limiter_isp_ceiling_dbtp == Catch::Approx(-2.5f).margin(0.001f));

  sonare_realtime_voice_changer_destroy(handle);
}

TEST_CASE("sonare_engine_get_transport_state surfaces bar position and time signature", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  REQUIRE(sonare_engine_set_tempo(engine, 120.0) == SONARE_OK);
  REQUIRE(sonare_engine_set_time_signature(engine, 3, 4) == SONARE_OK);

  SonareTransportState state{};
  REQUIRE(sonare_engine_get_transport_state(engine, &state) == SONARE_OK);
  REQUIRE(state.time_signature.numerator == 3);
  REQUIRE(state.time_signature.denominator == 4);
  REQUIRE(state.bar_count >= 0);
  REQUIRE(std::isfinite(state.bar_start_ppq));

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_bounce_offline NULLs the owned result on validation failure", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 16, 16, 16) == SONARE_OK);

  SonareEngineBounceOptions bad_options{};
  bad_options.total_frames = 16;
  bad_options.block_size = 16;
  bad_options.num_channels = 0;  // invalid -> early validation failure
  bad_options.source_sample_rate = 48000;
  bad_options.target_sample_rate = 48000;

  // Pre-dirty the result so we can prove the failure path overwrites it with NULL
  // rather than leaving a dangling owned pointer that the free idiom would delete.
  SonareEngineBounceResult result{};
  result.interleaved = reinterpret_cast<float*>(0xDEADBEEF);
  result.frames = 123;

  REQUIRE(sonare_engine_bounce_offline(engine, &bad_options, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.interleaved == nullptr);
  REQUIRE(result.frames == 0);
  sonare_free_floats(result.interleaved);  // must be a safe no-op on NULL

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_realtime_engine_c_api_smoke", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  REQUIRE(sonare_engine_set_tempo(engine, 60.0) == SONARE_OK);
  REQUIRE(sonare_engine_set_time_signature(engine, 3, 4) == SONARE_OK);
  SonareEngineMarker markers[2]{};
  markers[0].id = 11;
  markers[0].ppq = 1.0;
  std::strncpy(markers[0].name, "intro", sizeof(markers[0].name) - 1);
  markers[1].id = 12;
  markers[1].ppq = 2.0;
  std::strncpy(markers[1].name, "out", sizeof(markers[1].name) - 1);
  REQUIRE(sonare_engine_set_markers(engine, markers, 2) == SONARE_OK);
  size_t marker_count = 0;
  REQUIRE(sonare_engine_marker_count(engine, &marker_count) == SONARE_OK);
  REQUIRE(marker_count == 2);
  SonareEngineMarker marker_out{};
  REQUIRE(sonare_engine_marker_by_index(engine, 0, &marker_out) == SONARE_OK);
  REQUIRE(marker_out.id == 11);
  REQUIRE(std::strcmp(marker_out.name, "intro") == 0);
  REQUIRE(sonare_engine_marker(engine, 12, &marker_out) == SONARE_OK);
  REQUIRE(marker_out.ppq == Catch::Approx(2.0));
  REQUIRE(sonare_engine_set_loop_from_markers(engine, 11, 12) == SONARE_OK);
  REQUIRE(sonare_engine_seek_marker(engine, 11, -1) == SONARE_OK);
  SonareEngineMetronomeConfig metronome{};
  metronome.enabled = 1;
  metronome.beat_gain = 0.25f;
  metronome.accent_gain = 0.75f;
  metronome.click_samples = 16;
  REQUIRE(sonare_engine_set_metronome(engine, &metronome) == SONARE_OK);
  SonareEngineMetronomeConfig metronome_out{};
  REQUIRE(sonare_engine_metronome(engine, &metronome_out) == SONARE_OK);
  REQUIRE(metronome_out.enabled == 1);
  REQUIRE(metronome_out.click_samples == 16);
  int64_t count_in_end = 0;
  REQUIRE(sonare_engine_count_in_end_sample(engine, 0, 2, &count_in_end) == SONARE_OK);
  REQUIRE(count_in_end == 288000);
  metronome.enabled = 0;
  REQUIRE(sonare_engine_set_metronome(engine, &metronome) == SONARE_OK);

  SonareParameterInfo parameter{};
  parameter.id = 7;
  std::strncpy(parameter.name, "gain", sizeof(parameter.name) - 1);
  std::strncpy(parameter.unit, "dB", sizeof(parameter.unit) - 1);
  parameter.min_value = -60.0f;
  parameter.max_value = 12.0f;
  parameter.default_value = 0.0f;
  parameter.rt_safe = 1;
  parameter.default_curve = 0;  // canonical AutomationCurve::Linear
  REQUIRE(sonare_engine_add_parameter(engine, &parameter) == SONARE_OK);
  size_t parameter_count = 0;
  REQUIRE(sonare_engine_parameter_count(engine, &parameter_count) == SONARE_OK);
  REQUIRE(parameter_count == 1);
  SonareParameterInfo parameter_out{};
  REQUIRE(sonare_engine_parameter_info_by_index(engine, 0, &parameter_out) == SONARE_OK);
  REQUIRE(parameter_out.id == 7);
  REQUIRE(std::strcmp(parameter_out.name, "gain") == 0);

  const SonareAutomationPoint points[] = {{0.0, 0.0f, 0}, {1.0, 6.0205999f, 0}};
  REQUIRE(sonare_engine_set_automation_lane(engine, 7, points, 2) == SONARE_OK);
  size_t lane_count = 0;
  REQUIRE(sonare_engine_automation_lane_count(engine, &lane_count) == SONARE_OK);
  REQUIRE(lane_count == 1);

  SonareEngineGraphNode graph_nodes[3]{};
  std::strncpy(graph_nodes[0].id, "in", sizeof(graph_nodes[0].id) - 1);
  graph_nodes[0].type = 0;
  graph_nodes[0].num_ports = 2;
  std::strncpy(graph_nodes[1].id, "gain", sizeof(graph_nodes[1].id) - 1);
  graph_nodes[1].type = 1;
  graph_nodes[1].gain_db = 0.0f;
  graph_nodes[1].num_ports = 2;
  std::strncpy(graph_nodes[2].id, "out", sizeof(graph_nodes[2].id) - 1);
  graph_nodes[2].type = 0;
  graph_nodes[2].num_ports = 2;
  SonareEngineGraphConnection graph_connections[4]{};
  std::strncpy(graph_connections[0].source_node, "in",
               sizeof(graph_connections[0].source_node) - 1);
  std::strncpy(graph_connections[0].dest_node, "gain", sizeof(graph_connections[0].dest_node) - 1);
  graph_connections[0].source_port = 0;
  graph_connections[0].dest_port = 0;
  graph_connections[0].mix = 1;
  std::strncpy(graph_connections[1].source_node, "in",
               sizeof(graph_connections[1].source_node) - 1);
  std::strncpy(graph_connections[1].dest_node, "gain", sizeof(graph_connections[1].dest_node) - 1);
  graph_connections[1].source_port = 1;
  graph_connections[1].dest_port = 1;
  graph_connections[1].mix = 1;
  std::strncpy(graph_connections[2].source_node, "gain",
               sizeof(graph_connections[2].source_node) - 1);
  std::strncpy(graph_connections[2].dest_node, "out", sizeof(graph_connections[2].dest_node) - 1);
  graph_connections[2].source_port = 0;
  graph_connections[2].dest_port = 0;
  graph_connections[2].mix = 1;
  std::strncpy(graph_connections[3].source_node, "gain",
               sizeof(graph_connections[3].source_node) - 1);
  std::strncpy(graph_connections[3].dest_node, "out", sizeof(graph_connections[3].dest_node) - 1);
  graph_connections[3].source_port = 1;
  graph_connections[3].dest_port = 1;
  graph_connections[3].mix = 1;
  SonareEngineGraphSpec graph_spec{};
  graph_spec.nodes = graph_nodes;
  graph_spec.node_count = 3;
  graph_spec.connections = graph_connections;
  graph_spec.connection_count = 4;
  SonareEngineGraphParameterBinding graph_bindings[1]{};
  graph_bindings[0].param_id = 7;
  std::strncpy(graph_bindings[0].node_id, "gain", sizeof(graph_bindings[0].node_id) - 1);
  graph_spec.parameter_bindings = graph_bindings;
  graph_spec.parameter_binding_count = 1;
  std::strncpy(graph_spec.input_node, "in", sizeof(graph_spec.input_node) - 1);
  std::strncpy(graph_spec.output_node, "out", sizeof(graph_spec.output_node) - 1);
  graph_spec.num_channels = 2;
  REQUIRE(sonare_engine_set_graph(engine, &graph_spec) == SONARE_OK);
  size_t graph_node_count = 0;
  size_t graph_connection_count = 0;
  REQUIRE(sonare_engine_graph_node_count(engine, &graph_node_count) == SONARE_OK);
  REQUIRE(sonare_engine_graph_connection_count(engine, &graph_connection_count) == SONARE_OK);
  REQUIRE(graph_node_count == 3);
  REQUIRE(graph_connection_count == 4);

  std::array<float, 128> clip_left{};
  std::array<float, 128> clip_right{};
  clip_left.fill(0.125f);
  clip_right.fill(-0.125f);
  const float* clip_channels[] = {clip_left.data(), clip_right.data()};
  SonareEngineClip clip{};
  clip.id = 101;
  clip.channels = clip_channels;
  clip.num_channels = 2;
  clip.num_samples = 128;
  clip.start_ppq = 1.0;
  clip.length_samples = 128;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  size_t clip_count = 0;
  REQUIRE(sonare_engine_clip_count(engine, &clip_count) == SONARE_OK);
  REQUIRE(clip_count == 1);

  std::array<float, 128> capture_left{};
  std::array<float, 128> capture_right{};
  float* capture_channels[] = {capture_left.data(), capture_right.data()};
  SonareEngineCaptureBuffer capture_buffer{};
  capture_buffer.channels = capture_channels;
  capture_buffer.num_channels = 2;
  capture_buffer.capacity_frames = 128;
  REQUIRE(sonare_engine_set_capture_buffer(engine, &capture_buffer) == SONARE_OK);
  REQUIRE(sonare_engine_set_capture_punch(engine, 48000, 48128, 1) == SONARE_OK);
  REQUIRE(sonare_engine_arm_capture(engine, 1) == SONARE_OK);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::array<float, 128> left{};
  std::array<float, 128> right{};
  left.fill(0.25f);
  right.fill(-0.25f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  REQUIRE(left[0] == Catch::Approx(0.75f).margin(0.0001f));
  REQUIRE(right[0] == Catch::Approx(-0.75f).margin(0.0001f));

  SonareEngineCaptureStatus capture_status{};
  REQUIRE(sonare_engine_capture_status(engine, &capture_status) == SONARE_OK);
  REQUIRE(capture_status.captured_frames == 128);
  REQUIRE(capture_status.overflow_count == 0);
  REQUIRE(capture_status.armed == 1);
  REQUIRE(capture_left[0] == Catch::Approx(0.75f).margin(0.0001f));
  REQUIRE(capture_right[0] == Catch::Approx(-0.75f).margin(0.0001f));
  REQUIRE(sonare_engine_reset_capture(engine) == SONARE_OK);
  REQUIRE(sonare_engine_capture_status(engine, &capture_status) == SONARE_OK);
  REQUIRE(capture_status.captured_frames == 0);

  std::array<SonareEngineTelemetry, 4> telemetry{};
  size_t written = 0;
  REQUIRE(sonare_engine_drain_telemetry(engine, telemetry.data(), telemetry.size(), &written) ==
          SONARE_OK);
  REQUIRE(written > 0);
  REQUIRE(telemetry[written - 1].render_frame == 0);
  REQUIRE(telemetry[written - 1].timeline_sample == 48000 + 128);

  REQUIRE(sonare_engine_render_offline(engine, channels, 2, 128, 128) == SONARE_OK);
  SonareEngineBounceOptions bounce_options{};
  bounce_options.total_frames = 128;
  bounce_options.block_size = 128;
  bounce_options.num_channels = 2;
  bounce_options.source_sample_rate = 48000;
  bounce_options.target_sample_rate = 24000;
  bounce_options.normalize_lufs = 0;
  bounce_options.dither = 0;
  SonareEngineBounceResult bounce{};
  REQUIRE(sonare_engine_bounce_offline(engine, &bounce_options, &bounce) == SONARE_OK);
  REQUIRE(bounce.interleaved != nullptr);
  REQUIRE(bounce.frames == 64);
  REQUIRE(bounce.num_channels == 2);
  REQUIRE(bounce.sample_rate == 24000);
  REQUIRE(bounce.sample_count == 128);
  REQUIRE((std::isfinite(bounce.integrated_lufs) || std::isinf(bounce.integrated_lufs)));
  sonare_free_floats(bounce.interleaved);
  sonare_engine_destroy(engine);
}
#endif

TEST_CASE("sonare_engine_process_with_monitor returns a separate monitor bus", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 16, 16, 16) == SONARE_OK);

  std::array<float, 16> left{};
  std::array<float, 16> right{};
  left.fill(0.25f);
  right.fill(-0.25f);
  float* channels[] = {left.data(), right.data()};

  std::array<float, 16> monitor_left{};
  std::array<float, 16> monitor_right{};
  monitor_left.fill(99.0f);
  monitor_right.fill(99.0f);
  float* monitor_channels[] = {monitor_left.data(), monitor_right.data()};

  REQUIRE(sonare_engine_process_with_monitor(engine, channels, monitor_channels, 2, 16) ==
          SONARE_OK);
  REQUIRE(left[0] == Catch::Approx(0.25f).margin(0.0001f));
  REQUIRE(right[0] == Catch::Approx(-0.25f).margin(0.0001f));
  REQUIRE(monitor_left[0] == Catch::Approx(0.0f).margin(0.0001f));
  REQUIRE(monitor_right[0] == Catch::Approx(0.0f).margin(0.0001f));

  sonare_engine_destroy(engine);
}

#ifdef SONARE_WITH_VOICE_CHANGER
TEST_CASE("sonare_realtime_engine_freeze_c_api_matches_clip_playback", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 64, 64) == SONARE_OK);

  std::array<float, 128> clip_left{};
  std::array<float, 128> clip_right{};
  clip_left.fill(0.125f);
  clip_right.fill(-0.25f);
  const float* clip_channels[] = {clip_left.data(), clip_right.data()};
  SonareEngineClip clip{};
  clip.id = 7;
  clip.channels = clip_channels;
  clip.num_channels = 2;
  clip.num_samples = 128;
  clip.start_ppq = 0.0;
  clip.length_samples = 128;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  SonareEngineFreezeOptions freeze_options{};
  freeze_options.total_frames = 128;
  freeze_options.block_size = 128;
  freeze_options.num_channels = 2;
  freeze_options.clip_id = 77;
  freeze_options.start_ppq = 0.0;
  freeze_options.gain = 1.0f;
  SonareEngineFreezeResult freeze{};
  REQUIRE(sonare_engine_freeze_offline(engine, &freeze_options, &freeze) == SONARE_OK);
  REQUIRE(freeze.clip_id == 77);
  REQUIRE(freeze.frames == 128);
  REQUIRE(freeze.num_channels == 2);
  size_t clip_count = 0;
  REQUIRE(sonare_engine_clip_count(engine, &clip_count) == SONARE_OK);
  REQUIRE(clip_count == 1);

  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  std::array<float, 128> left{};
  std::array<float, 128> right{};
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_render_offline(engine, channels, 2, 128, 128) == SONARE_OK);
  REQUIRE(left[0] == Catch::Approx(0.125f).margin(0.0001f));
  REQUIRE(right[0] == Catch::Approx(-0.25f).margin(0.0001f));
  REQUIRE(left[127] == Catch::Approx(0.125f).margin(0.0001f));
  REQUIRE(right[127] == Catch::Approx(-0.25f).margin(0.0001f));

  sonare_engine_destroy(engine);
}
#endif

TEST_CASE("sonare_last_error_message", "[c_api]") {
  SECTION("never returns null pointer") {
    const char* msg = sonare_last_error_message();
    REQUIRE(msg != nullptr);
  }

  SECTION("captures detailed message when a C API call fails") {
    // 12 bytes of random non-audio data so format detection returns Unknown.
    std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
    SonareAudio* audio = nullptr;

    SonareError err = sonare_audio_from_memory(garbage.data(), garbage.size(), &audio);
#ifdef SONARE_WITH_FFMPEG
    // With FFmpeg the buffer still fails to decode but the message comes from
    // FFmpeg rather than the static "Unsupported audio format" path.
    REQUIRE(err != SONARE_OK);
    const char* msg = sonare_last_error_message();
    REQUIRE(msg != nullptr);
    REQUIRE(std::strlen(msg) > 0);
#else
    REQUIRE(err == SONARE_ERROR_INVALID_FORMAT);
    const char* msg = sonare_last_error_message();
    REQUIRE(msg != nullptr);
    // The detailed message must be more informative than the generic code label.
    REQUIRE(std::string(msg).find("Unsupported audio format") != std::string::npos);
    REQUIRE(std::string(msg).find("WAV, MP3") != std::string::npos);
    REQUIRE(std::string(msg).find("ffmpeg") != std::string::npos);
#endif
    REQUIRE(audio == nullptr);
  }
}

TEST_CASE("sonare_strip_schedule_send_automation error mapping", "[c_api][mixing]") {
  SonareMixer* mixer = sonare_mixer_create(48000, 512);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "src");
  REQUIRE(strip != nullptr);

  size_t send_index = 0;
  REQUIRE(sonare_strip_add_send(strip, "send0", "bus0", -6.0f, 0, &send_index) == SONARE_OK);

  SECTION("out-of-range send_index -> INVALID_PARAMETER") {
    // A bad argument must be reported distinctly from a capacity condition.
    REQUIRE(sonare_strip_schedule_send_automation(strip, send_index + 1, 0, -3.0f, 0) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("full send lane -> OUT_OF_MEMORY") {
    // Fill the lane to its ring-buffer capacity (default 1024 usable slots).
    // Use a non-decreasing sample_pos so push() is not rejected for ordering.
    SonareError err = SONARE_OK;
    int pushed = 0;
    for (int i = 0; i < 100000; ++i) {
      err = sonare_strip_schedule_send_automation(strip, send_index, i, -3.0f, 0);
      if (err != SONARE_OK) {
        break;
      }
      ++pushed;
    }
    // The lane should accept many events, then fail with OUT_OF_MEMORY (capacity)
    // rather than INVALID_PARAMETER, mirroring the fader/pan/width schedulers.
    REQUIRE(pushed > 0);
    REQUIRE(err == SONARE_ERROR_OUT_OF_MEMORY);
  }

  sonare_mixer_destroy(mixer);
}

TEST_CASE("sonare_metering stereo pair validates both channels", "[c_api][mixing]") {
  const int sr = 48000;
  auto left = generate_sine(440.0f, sr, 0.25f);
  std::vector<float> right = left;

  SECTION("right channel NaN is rejected") {
    std::vector<float> bad_right = right;
    bad_right[bad_right.size() / 2] = std::numeric_limits<float>::quiet_NaN();
    float c = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(left.data(), bad_right.data(), left.size(), sr,
                                               &c) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("right channel Inf is rejected") {
    std::vector<float> bad_right = right;
    bad_right[0] = std::numeric_limits<float>::infinity();
    float c = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(left.data(), bad_right.data(), left.size(), sr,
                                               &c) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("left channel NaN is rejected (parity)") {
    std::vector<float> bad_left = left;
    bad_left[bad_left.size() / 2] = std::numeric_limits<float>::quiet_NaN();
    float c = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(bad_left.data(), right.data(), left.size(), sr,
                                               &c) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mel_spectrogram_ex exposes a custom Mel range from pure C", "[c_api][features]") {
  const int sr = 22050;
  const int n_fft = 1024;
  const int hop = 256;
  const int n_mels = 40;
  auto samples = generate_sine(440.0f, sr, 1.0f);

  SECTION("custom fmin/fmax forward transform round-trips with the inverse API") {
    // The forward _ex transform and the inverse sonare_mel_to_stft now share the
    // same fmin/fmax, so a custom-range round-trip is possible from pure C.
    const float fmin = 100.0f;
    const float fmax = 8000.0f;
    SonareMelResult mel = {};
    REQUIRE(sonare_mel_spectrogram_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels, fmin,
                                      fmax, 0, &mel) == SONARE_OK);
    REQUIRE(mel.power != nullptr);
    REQUIRE(mel.n_mels == n_mels);
    REQUIRE(mel.n_frames > 0);

    SonareInverseResult stft = {};
    REQUIRE(sonare_mel_to_stft(mel.power, mel.n_mels, mel.n_frames, sr, n_fft, fmin, fmax, &stft) ==
            SONARE_OK);
    REQUIRE(stft.data != nullptr);
    REQUIRE(stft.rows == n_fft / 2 + 1);
    REQUIRE(stft.n_frames == mel.n_frames);

    sonare_free_inverse_result(&stft);
    sonare_free_mel_result(&mel);
  }

  SECTION("a non-default range yields a different forward result than the default") {
    SonareMelResult def = {};
    SonareMelResult ranged = {};
    REQUIRE(sonare_mel_spectrogram(samples.data(), samples.size(), sr, n_fft, hop, n_mels, &def) ==
            SONARE_OK);
    REQUIRE(sonare_mel_spectrogram_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels,
                                      500.0f, 4000.0f, 0, &ranged) == SONARE_OK);
    const size_t total = static_cast<size_t>(n_mels) * def.n_frames;
    bool differs = false;
    for (size_t i = 0; i < total && !differs; ++i) {
      differs = std::abs(def.power[i] - ranged.power[i]) > 1e-6f;
    }
    REQUIRE(differs);
    sonare_free_mel_result(&def);
    sonare_free_mel_result(&ranged);
  }

  SECTION("sonare_mfcc_ex accepts the range and a null out is rejected") {
    SonareMfccResult mfcc = {};
    REQUIRE(sonare_mfcc_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels, 13, 100.0f,
                           8000.0f, 0, &mfcc) == SONARE_OK);
    REQUIRE(mfcc.coefficients != nullptr);
    REQUIRE(mfcc.n_mfcc == 13);
    sonare_free_mfcc_result(&mfcc);

    REQUIRE(sonare_mfcc_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels, 13, 0.0f, 0.0f,
                           0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}
