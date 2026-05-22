/// @file sonare_c_test.cpp
/// @brief Tests for C API functions.

#include "sonare_c.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Generate sine wave
std::vector<float> generate_sine(float freq, int sample_rate, float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    samples[i] = std::sin(2.0f * static_cast<float>(M_PI) * freq * i / sample_rate);
  }
  return samples;
}

#ifdef SONARE_WITH_MASTERING
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
      samples[start + i] = std::sin(static_cast<float>(M_PI) * i / click_length);
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
      samples[i] += gain * std::sin(2.0f * static_cast<float>(M_PI) * freq * i / sample_rate);
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
    REQUIRE(result.chords[0].quality <= SONARE_CHORD_UNKNOWN);
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

  SECTION("free is safe on partially initialized struct") {
    SonareChordAnalysisResult result = {};
    result.chords = new SonareChord[1]{{SONARE_PITCH_C, SONARE_CHORD_MAJOR, 0.0f, 1.0f, 1.0f}};
    result.chord_count = 1;

    sonare_free_chord_analysis_result(&result);

    REQUIRE(result.chords == nullptr);
    REQUIRE(result.chord_count == 0);
  }
}

#ifdef SONARE_WITH_MASTERING
TEST_CASE("sonare_mastering_process", "[c_api][mastering]") {
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
    REQUIRE(std::strstr(names, "stereo.imager") != nullptr);
  }

  SECTION("applies pair processors and analyses") {
    auto source = generate_sine(440.0f, 44100, 0.25f);
    auto reference = generate_sine(880.0f, 44100, 0.25f);
    for (auto& sample : source) sample *= 0.18f;
    for (auto& sample : reference) sample *= 0.12f;

    SonareMasteringParam pair_params[] = {{"mix", 0.25}};
    SonareMasteringResult paired{};
    REQUIRE(sonare_mastering_apply_pair_processor(
                "match.abCrossfade", source.data(), reference.data(), source.size(), 44100,
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
      REQUIRE(result.length == left.size());
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
                                              source.size(), 44100, params, 1,
                                              &json) == SONARE_OK);
      REQUIRE(json != nullptr);
      REQUIRE(std::strlen(json) > 2);
      sonare_free_string(json);
    }
  }
}
#endif

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
}

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
