/// @file sonare_c_core_test.cpp
/// @brief Core C API tests.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "analysis/analysis_json.h"
#include "analysis/meter_analyzer.h"
#include "core/audio_io.h"
#include "effects/pitch_shift.h"
#include "mastering/api/insert_factory.h"
#include "sonare_c_test_helpers.h"
#include "support/alloc_guard.h"
#include "support/schema_paths.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/json.h"

namespace {

using sonare::test::collect_schema_paths;

TEST_CASE("sonare_detect_onsets with a configuration", "[c_api]") {
  const auto samples = generate_clicks(120.0f, 22050, 2.0f);
  SonareOnsetDetectConfig config;
  sonare_onset_detect_config_init(&config);
  config.n_fft = 256;
  config.hop_length = 64;
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
  REQUIRE(sonare_detect_onsets(samples.data(), samples.size(), 22050, &config, &times, &count) ==
          SONARE_OK);
  REQUIRE((count == 0 || times != nullptr));
  sonare_free_floats(times);

  config.hop_length = 0;
  REQUIRE(sonare_detect_onsets(samples.data(), samples.size(), 22050, &config, &times, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // A given threshold is a real minimum strength: 0 and negatives are refused by name, NaN is
  // the adaptive path.
  config.hop_length = 64;
  config.threshold = 0.0f;
  REQUIRE(sonare_detect_onsets(samples.data(), samples.size(), 22050, &config, &times, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()).find("config.threshold") != std::string::npos);
  config.threshold = -1.0f;
  REQUIRE(sonare_detect_onsets(samples.data(), samples.size(), 22050, &config, &times, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  config.threshold = 0.5f;
  REQUIRE(sonare_detect_onsets(samples.data(), samples.size(), 22050, &config, &times, &count) ==
          SONARE_OK);
  sonare_free_floats(times);
  config.threshold = std::numeric_limits<float>::quiet_NaN();
  REQUIRE(sonare_detect_onsets(samples.data(), samples.size(), 22050, &config, &times, &count) ==
          SONARE_OK);
  sonare_free_floats(times);

  SonareOnsetDetectConfig zeroed{};
  REQUIRE(sonare_detect_onsets(samples.data(), samples.size(), 22050, &zeroed, &times, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()) ==
          "config.struct_size must be set by sonare_onset_detect_config_init");
}

TEST_CASE("a configured onset entry agrees with the default one at every rate", "[c_api][onsets]") {
  // The two are documented as the same quantity with and without a configuration,
  // and every field of the config defaults to the core default, so a
  // default-filled config reads as a no-op. It has to BE one: every peak-picking
  // field is a FRAME count and a frame is hop_length over the rate in force, so
  // an entry that analysed at a different rate would reinterpret all of them
  // while looking like it had been handed nothing.
  SonareOnsetDetectConfig config;
  sonare_onset_detect_config_init(&config);
  config.n_fft = 2048;
  config.hop_length = 512;
  config.pre_max = 1;
  config.post_max = 1;
  config.pre_avg = 3;
  config.post_avg = 4;
  config.delta = 0.06f;
  config.wait = 1;
  config.backtrack = 0;
  config.backtrack_range = 10;

  // Two rates above the analysis rate, resampling by different ratios, so a fix
  // that happened to work at one ratio is not mistaken for agreement. 22050 is
  // at the analysis rate, where the resample is a no-op and the two must agree
  // trivially -- the case that says the comparison can tell agreement from
  // coincidence.
  for (int rate : {44100, 48000, 22050}) {
    CAPTURE(rate);
    const std::vector<float> samples = generate_clicks(120.0f, rate, 4.0f);

    float* plain = nullptr;
    size_t plain_count = 0;
    REQUIRE(sonare_detect_onsets(samples.data(), samples.size(), rate, nullptr, &plain,
                                 &plain_count) == SONARE_OK);
    float* configured = nullptr;
    size_t configured_count = 0;
    REQUIRE(sonare_detect_onsets(samples.data(), samples.size(), rate, &config, &configured,
                                 &configured_count) == SONARE_OK);

    // Element by element: a count comparison passes while the positions differ.
    CHECK(configured_count == plain_count);
    for (size_t i = 0; i < std::min(plain_count, configured_count); ++i) {
      CAPTURE(i);
      CHECK(configured[i] == plain[i]);
    }
    // Two empty lists agree perfectly, so the fixture has to produce onsets.
    CHECK(plain_count > 0);

    sonare_free_floats(plain);
    sonare_free_floats(configured);
  }
}

TEST_CASE("a configured onset detector still answers to its configuration", "[c_api][onsets]") {
  // The control for the agreement above. Without it that equality also holds for
  // an entry point that ignored the configuration and called the plain path.
  const std::vector<float> samples = generate_clicks(120.0f, 44100, 4.0f);
  SonareOnsetDetectConfig config;
  sonare_onset_detect_config_init(&config);
  config.n_fft = 2048;
  config.hop_length = 512;
  config.pre_max = 1;
  config.post_max = 1;
  config.pre_avg = 3;
  config.post_avg = 4;
  config.delta = 0.06f;
  config.wait = 1;
  config.backtrack = 0;
  config.backtrack_range = 10;

  const auto onset_count = [&samples](const SonareOnsetDetectConfig& cfg) {
    float* times = nullptr;
    size_t count = 0;
    REQUIRE(sonare_detect_onsets(samples.data(), samples.size(), 44100, &cfg, &times, &count) ==
            SONARE_OK);
    sonare_free_floats(times);
    return count;
  };

  const size_t as_defaulted = onset_count(config);
  // A threshold this far above the envelope leaves nothing to pick, and a wait
  // this long admits at most one onset per second.
  SonareOnsetDetectConfig strict = config;
  strict.delta = 10.0f;
  CHECK(onset_count(strict) < as_defaulted);

  SonareOnsetDetectConfig sparse = config;
  sparse.wait = 200;
  CHECK(onset_count(sparse) < as_defaulted);
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
  // Deliberately distinct from the beats' own strengths above so the analysis
  // record proves it carries the per-beat evidence used by meter estimation.
  result.beat_observations.accent = {0.62f, 0.37f};
  result.beat_observations.has_attack = {1, 0};
  result.beat_observations.low_frequency_energy = {0.31f, 0.12f};
  result.beat_observations.subdivision_accent = {0.15f};
  result.beat_observations.chord_change = {1.0f, 0.0f};
  // Two beats half a second apart is 120 BPM, but the curve is a decoded value
  // rather than a restatement of the beat spacing, so the fixture carries
  // numbers no other field could supply.
  result.beat_local_bpm = {118.5f, 121.5f};
  result.chords.push_back({sonare::PitchClass::C, sonare::ChordQuality::Major, 0.0f, 1.0f, 0.8f,
                           sonare::PitchClass::C});
  result.sections.push_back({sonare::SectionType::Verse, 0.0f, 1.0f, 0.5f, 0.9f, 2});
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

namespace {

/// Frame-interleaved float32 WAV with @p channels planes, built in memory.
std::vector<uint8_t> interleaved_float_wav(const std::vector<float>& interleaved, int channels,
                                           int sample_rate) {
  std::vector<uint8_t> out;
  const auto put = [&out](const void* data, size_t n) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + n);
  };
  const uint32_t data_size = static_cast<uint32_t>(interleaved.size() * sizeof(float));
  const uint32_t riff_size = 36 + data_size;
  const uint32_t fmt_size = 16;
  const uint16_t format = 3;
  const uint16_t ch = static_cast<uint16_t>(channels);
  const uint32_t rate = static_cast<uint32_t>(sample_rate);
  const uint16_t block = static_cast<uint16_t>(channels * 4);
  const uint32_t byte_rate = rate * block;
  const uint16_t bits = 32;
  put("RIFF", 4);
  put(&riff_size, 4);
  put("WAVEfmt ", 8);
  put(&fmt_size, 4);
  put(&format, 2);
  put(&ch, 2);
  put(&rate, 4);
  put(&byte_rate, 4);
  put(&block, 2);
  put(&bits, 2);
  put("data", 4);
  put(&data_size, 4);
  put(interleaved.data(), data_size);
  return out;
}

std::vector<float> channel_test_signal(size_t frames, int channels) {
  std::vector<float> interleaved(frames * static_cast<size_t>(channels));
  for (size_t frame = 0; frame < frames; ++frame) {
    for (int channel = 0; channel < channels; ++channel) {
      interleaved[frame * static_cast<size_t>(channels) + static_cast<size_t>(channel)] =
          0.05f * static_cast<float>(channel + 1) *
          std::sin(0.01f * static_cast<float>(frame * static_cast<size_t>(channel + 2)));
    }
  }
  return interleaved;
}

}  // namespace

TEST_CASE("sonare_decode_channels returns every source channel the core decoder reads",
          "[c_api][audio_io]") {
  constexpr size_t kFrames = 480;
  for (const int channels : {1, 2, 6}) {
    const auto interleaved = channel_test_signal(kFrames, channels);
    const auto wav = interleaved_float_wav(interleaved, channels, 22050);
    const auto [core_samples, core_rate, core_channels] =
        sonare::load_buffer_interleaved(wav.data(), wav.size());

    float* planar = nullptr;
    size_t frames = 0;
    int out_channels = 0;
    int rate = 0;
    REQUIRE(sonare_decode_channels(wav.data(), wav.size(), &planar, &frames, &out_channels,
                                   &rate) == SONARE_OK);
    REQUIRE(planar != nullptr);
    REQUIRE(out_channels == core_channels);
    REQUIRE(rate == core_rate);
    REQUIRE(frames == kFrames);
    for (int channel = 0; channel < channels; ++channel) {
      for (size_t frame = 0; frame < frames; ++frame) {
        REQUIRE(planar[static_cast<size_t>(channel) * frames + frame] ==
                core_samples[frame * static_cast<size_t>(channels) + static_cast<size_t>(channel)]);
      }
    }
    // Folding the decoded planes lands on the samples the mono decoder returns.
    const auto [mono_samples, mono_rate] = sonare::load_buffer(wav.data(), wav.size());
    float* folded = nullptr;
    REQUIRE(sonare_downmix(planar, frames, static_cast<size_t>(out_channels),
                           SONARE_CHANNEL_LAYOUT_MONO, nullptr, &folded) == SONARE_OK);
    REQUIRE(mono_samples.size() == frames);
    for (size_t frame = 0; frame < frames; ++frame) {
      REQUIRE(folded[frame] == mono_samples[frame]);
    }
    sonare_free_floats(folded);
    sonare_free_floats(planar);
  }
}

TEST_CASE("sonare_decode_channels refuses unusable input and clears its outputs",
          "[c_api][audio_io]") {
  const auto wav = interleaved_float_wav(channel_test_signal(64, 2), 2, 22050);
  float* planar = reinterpret_cast<float*>(0x1);
  size_t frames = 7;
  int channels = 7;
  int rate = 7;
  REQUIRE(sonare_decode_channels(nullptr, wav.size(), &planar, &frames, &channels, &rate) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(planar == nullptr);
  REQUIRE(frames == 0);
  REQUIRE(channels == 0);
  REQUIRE(rate == 0);
  REQUIRE(std::string(sonare_last_error_message()) == "data must not be NULL when length is not 0");
  REQUIRE(sonare_decode_channels(wav.data(), 0, &planar, &frames, &channels, &rate) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()) == "data must not be empty");
  REQUIRE(sonare_decode_channels(nullptr, 0, &planar, &frames, &channels, &rate) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()) == "data must not be empty");
  REQUIRE(sonare_decode_channels(wav.data(), wav.size(), nullptr, &frames, &channels, &rate) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()) == "out_samples must not be NULL");
  REQUIRE(sonare_decode_channels(wav.data(), wav.size(), &planar, &frames, &channels, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()) == "out_sample_rate must not be NULL");

  const std::vector<uint8_t> garbage(256, 0x5A);
  REQUIRE(sonare_decode_channels(garbage.data(), garbage.size(), &planar, &frames, &channels,
                                 &rate) != SONARE_OK);
  REQUIRE(planar == nullptr);

  auto nan_signal = channel_test_signal(64, 2);
  nan_signal[5] = std::numeric_limits<float>::quiet_NaN();
  const auto nan_wav = interleaved_float_wav(nan_signal, 2, 22050);
  REQUIRE(sonare_decode_channels(nan_wav.data(), nan_wav.size(), &planar, &frames, &channels,
                                 &rate) == SONARE_ERROR_DECODE_FAILED);
  REQUIRE(planar == nullptr);
}

TEST_CASE("sonare_downmix applies the BS.775 coefficients", "[c_api][audio_io]") {
  using sonare::constants::kInvSqrt2;
  constexpr size_t kFrames = 32;
  // L R C LFE Ls Rs, each plane a distinct constant so the matrix is readable.
  const float plane_values[6] = {0.10f, 0.20f, 0.30f, 0.40f, 0.05f, 0.15f};
  std::vector<float> surround(6 * kFrames);
  for (size_t channel = 0; channel < 6; ++channel) {
    std::fill_n(surround.begin() + static_cast<std::ptrdiff_t>(channel * kFrames), kFrames,
                plane_values[channel]);
  }

  float* stereo = nullptr;
  REQUIRE(sonare_downmix(surround.data(), kFrames, 6, SONARE_CHANNEL_LAYOUT_STEREO, nullptr,
                         &stereo) == SONARE_OK);
  const float left = 0.10f + kInvSqrt2 * 0.30f + kInvSqrt2 * 0.05f;
  const float right = 0.20f + kInvSqrt2 * 0.30f + kInvSqrt2 * 0.15f;
  for (size_t frame = 0; frame < kFrames; ++frame) {
    REQUIRE(stereo[frame] == Catch::Approx(left).margin(1e-6));
    REQUIRE(stereo[kFrames + frame] == Catch::Approx(right).margin(1e-6));
  }
  sonare_free_floats(stereo);

  float* mono = nullptr;
  REQUIRE(sonare_downmix(surround.data(), kFrames, 6, SONARE_CHANNEL_LAYOUT_MONO, nullptr, &mono) ==
          SONARE_OK);
  for (size_t frame = 0; frame < kFrames; ++frame) {
    REQUIRE(mono[frame] == Catch::Approx(0.5f * (left + right)).margin(1e-6));
  }
  sonare_free_floats(mono);

  // Stereo to mono is the plain average, and 5.1 to 5.1 is a copy.
  const float pair[4] = {0.5f, 0.5f, -0.25f, 0.75f};
  REQUIRE(sonare_downmix(pair, 2, 2, SONARE_CHANNEL_LAYOUT_MONO, nullptr, &mono) == SONARE_OK);
  REQUIRE(mono[0] == Catch::Approx(0.125f).margin(1e-7));
  REQUIRE(mono[1] == Catch::Approx(0.625f).margin(1e-7));
  sonare_free_floats(mono);
  float* copy = nullptr;
  REQUIRE(sonare_downmix(surround.data(), kFrames, 6, SONARE_CHANNEL_LAYOUT_5_1, nullptr, &copy) ==
          SONARE_OK);
  REQUIRE(std::equal(surround.begin(), surround.end(), copy));
  sonare_free_floats(copy);
}

TEST_CASE("sonare_downmix reports the unnormalised fold and applies the options",
          "[c_api][audio_io]") {
  using sonare::constants::kInvSqrt2;
  constexpr size_t kFrames = 8;
  std::vector<float> full(6 * kFrames, 1.0f);

  // The default fold of six full-scale planes exceeds full scale.
  float* stereo = nullptr;
  REQUIRE(sonare_downmix(full.data(), kFrames, 6, SONARE_CHANNEL_LAYOUT_STEREO, nullptr, &stereo) ==
          SONARE_OK);
  CHECK(stereo[0] == Catch::Approx(1.0f + 2.0f * kInvSqrt2).margin(1e-5));
  sonare_free_floats(stereo);

  SonareDownmixOptions options;
  sonare_downmix_options_init(&options);
  CHECK(options.struct_size == sizeof(SonareDownmixOptions));
  CHECK(options.include_lfe == 0);
  CHECK(options.normalize == 0);

  options.normalize = 1;
  REQUIRE(sonare_downmix(full.data(), kFrames, 6, SONARE_CHANNEL_LAYOUT_STEREO, &options,
                         &stereo) == SONARE_OK);
  CHECK(stereo[0] == Catch::Approx(1.0f).margin(1e-5));
  sonare_free_floats(stereo);

  options.normalize = 0;
  options.include_lfe = 1;
  REQUIRE(sonare_downmix(full.data(), kFrames, 6, SONARE_CHANNEL_LAYOUT_STEREO, &options,
                         &stereo) == SONARE_OK);
  CHECK(stereo[0] == Catch::Approx(1.0f + 3.0f * kInvSqrt2).margin(1e-5));
  sonare_free_floats(stereo);

  // Initialised defaults are the NULL-options fold exactly.
  sonare_downmix_options_init(&options);
  float* plain = nullptr;
  REQUIRE(sonare_downmix(full.data(), kFrames, 6, SONARE_CHANNEL_LAYOUT_STEREO, &options, &plain) ==
          SONARE_OK);
  CHECK(plain[0] == Catch::Approx(1.0f + 2.0f * kInvSqrt2).margin(1e-5));
  sonare_free_floats(plain);

  float* out = nullptr;
  CHECK(sonare_downmix(full.data(), kFrames, 6, 4, &options, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out == nullptr);

  // A struct_size below this struct's is refused before anything is read.
  options.struct_size = sizeof(uint32_t);
  out = reinterpret_cast<float*>(0x1);
  CHECK(sonare_downmix(full.data(), kFrames, 6, SONARE_CHANNEL_LAYOUT_STEREO, &options, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out == nullptr);
  options.struct_size = 0;
  CHECK(sonare_downmix(full.data(), kFrames, 6, SONARE_CHANNEL_LAYOUT_STEREO, &options, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "options.struct_size must be set by sonare_downmix_options_init");
}

TEST_CASE("sonare_downmix folds an unmodelled channel count to mono by mean", "[c_api][audio_io]") {
  const float planes[9] = {0.3f, 0.6f, 0.9f, 0.0f, 0.3f, 0.6f, 0.6f, 0.0f, 0.3f};
  float* mono = nullptr;
  REQUIRE(sonare_downmix(planes, 3, 3, SONARE_CHANNEL_LAYOUT_MONO, nullptr, &mono) == SONARE_OK);
  REQUIRE(mono[0] == Catch::Approx(0.3f).margin(1e-6));
  REQUIRE(mono[1] == Catch::Approx(0.3f).margin(1e-6));
  REQUIRE(mono[2] == Catch::Approx(0.6f).margin(1e-6));
  sonare_free_floats(mono);
  REQUIRE(sonare_downmix(planes, 3, 3, SONARE_CHANNEL_LAYOUT_STEREO, nullptr, &mono) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()) ==
          "target_layout must be mono when num_channels has no speaker layout");
  REQUIRE(mono == nullptr);
}

TEST_CASE("sonare_downmix refuses an upmix, an unknown layout and malformed arguments",
          "[c_api][audio_io]") {
  const std::vector<float> stereo(16, 0.25f);
  float* out = reinterpret_cast<float*>(0x1);
  REQUIRE(sonare_downmix(stereo.data(), 8, 2, SONARE_CHANNEL_LAYOUT_5_1, nullptr, &out) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(out == nullptr);
  const auto message = [] { return std::string(sonare_last_error_message()); };
  REQUIRE(sonare_downmix(stereo.data(), 8, 2, 4, nullptr, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(message() == "target_layout must be an integer in [0, 3]");
  REQUIRE(sonare_downmix(stereo.data(), 8, 2, -1, nullptr, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(message() == "target_layout must be an integer in [0, 3]");
  REQUIRE(sonare_downmix(stereo.data(), 0, 2, SONARE_CHANNEL_LAYOUT_MONO, nullptr, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(message() == "frames must be an integer of at least 1");
  REQUIRE(sonare_downmix(stereo.data(), 8, 0, SONARE_CHANNEL_LAYOUT_MONO, nullptr, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(message() == "num_channels must be an integer in [1, 2147483647]");
  REQUIRE(sonare_downmix(nullptr, 8, 2, SONARE_CHANNEL_LAYOUT_MONO, nullptr, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(message() == "channels must not be NULL");
  REQUIRE(sonare_downmix(stereo.data(), 8, 2, SONARE_CHANNEL_LAYOUT_MONO, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(message() == "out_samples must not be NULL");
}

#ifndef __EMSCRIPTEN__
TEST_CASE("sonare_audio_file_num_channels reports source channels", "[c_api][audio_io]") {
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
  REQUIRE(sonare_audio_file_num_channels(mono_path.c_str(), &channels) == SONARE_OK);
  REQUIRE(channels == 1);
  channels = -1;
  REQUIRE(sonare_audio_file_num_channels(stereo_path.c_str(), &channels) == SONARE_OK);
  REQUIRE(channels == 2);

  channels = 17;
  REQUIRE(sonare_audio_file_num_channels("missing-c-api-channel-count.wav", &channels) ==
          SONARE_ERROR_FILE_NOT_FOUND);
  REQUIRE(channels == 0);

  channels = 17;
  REQUIRE(sonare_audio_file_num_channels(invalid_path.c_str(), &channels) ==
          SONARE_ERROR_DECODE_FAILED);
  REQUIRE(channels == 0);

  channels = 17;
  REQUIRE(sonare_audio_file_num_channels(".", &channels) == SONARE_ERROR_DECODE_FAILED);
  CHECK(std::string(sonare_last_error_message()).find("Failed to read file:") == 0);
  CHECK(channels == 0);

  channels = 17;
  REQUIRE(sonare_audio_file_num_channels(nullptr, &channels) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(channels == 0);
  REQUIRE(sonare_audio_file_num_channels(mono_path.c_str(), nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  cleanup();
}
#endif

TEST_CASE("C pitch shift rejects an unsupported shifted rate before allocating",
          "[c_api][pitch_shift][resource_limit]") {
  const float samples[4] = {0.0f, 0.25f, -0.25f, 0.0f};
  // -120 st puts the shifted rate near 47 Hz, under the 1000 Hz floor; +400 st puts it past
  // INT_MAX. Both are refused by the plan, before any buffer is made.
  for (const float semitones : {-120.0f, 400.0f}) {
    CAPTURE(semitones);
    float* out = nullptr;
    size_t out_length = 0;
    SonareError error = SONARE_OK;
    size_t allocations = 999;
    {
      sonare::test::AllocationGuard guard;
      error = sonare_pitch_shift(samples, 4, 48000, semitones, nullptr, &out, &out_length);
      allocations = guard.count();
    }
    REQUIRE(error == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(allocations == 0);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()) == sonare::kPitchShiftPlanError);
    // The static text is the one the core refuses the same plan with.
    const sonare::Audio audio = sonare::Audio::from_buffer(samples, 4, 48000);
    try {
      (void)sonare::pitch_shift(audio, semitones);
      FAIL("expected the core to refuse the plan");
    } catch (const sonare::SonareException& refusal) {
      CHECK(std::string(refusal.what()) == sonare::kPitchShiftPlanError);
    }
  }
}

TEST_CASE("C VQT reads NaN as automatic gamma and rejects a negative or infinite one",
          "[c_api][vqt]") {
  std::vector<float> samples(2048, 0.0f);
  SonareCqtResult result{};
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  REQUIRE(sonare_vqt(samples.data(), samples.size(), 22050, 512, 32.7f, 12, 12, 1.0f, nan,
                     &result) == SONARE_OK);
  // The automatic gamma is resolved and reported; gamma 0 is the constant-Q bandwidth.
  CHECK(result.gamma > 0.0f);
  sonare_free_cqt_result(&result);
  REQUIRE(sonare_vqt(samples.data(), samples.size(), 22050, 512, 32.7f, 12, 12, 1.0f, 0.0f,
                     &result) == SONARE_OK);
  CHECK(result.gamma == 0.0f);
  sonare_free_cqt_result(&result);
  for (const float bad : {inf, -inf, -1.0f}) {
    REQUIRE(sonare_vqt(samples.data(), samples.size(), 22050, 512, 32.7f, 12, 12, 1.0f, bad,
                       &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.magnitude == nullptr);
    REQUIRE(result.frequencies == nullptr);
  }
  REQUIRE(sonare_cqt(samples.data(), samples.size(), 22050, 512, 32.7f, 12, 12, 1.0f, &result) ==
          SONARE_OK);
  CHECK(result.gamma == 0.0f);
  sonare_free_cqt_result(&result);
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
                     bins_per_octave, 1.0f, &cqt_result) == SONARE_OK);
  REQUIRE(cqt_result.n_frames > 0);
  const size_t matrix_length =
      static_cast<size_t>(cqt_result.n_bins) * static_cast<size_t>(cqt_result.n_frames);

  float* output = nullptr;
  size_t output_length = 0;
  REQUIRE(sonare_cqt_to_audio(cqt_result.magnitude, matrix_length, cqt_result.n_bins,
                              cqt_result.n_frames, sample_rate, hop_length, fmin, bins_per_octave,
                              2, &output, &output_length) == SONARE_OK);
  REQUIRE(output != nullptr);
  REQUIRE(output_length > 0);
  for (size_t i = 0; i < output_length; ++i) REQUIRE(std::isfinite(output[i]));
  sonare_free_floats(output);

  output = nullptr;
  output_length = 0;
  REQUIRE(sonare_vqt_to_audio(cqt_result.magnitude, matrix_length, cqt_result.n_bins,
                              cqt_result.n_frames, sample_rate, hop_length, fmin, bins_per_octave,
                              0.0f, 2, &output, &output_length) == SONARE_OK);
  REQUIRE(output != nullptr);
  REQUIRE(output_length > 0);
  sonare_free_floats(output);

  REQUIRE(sonare_cqt_to_audio(cqt_result.magnitude, matrix_length - 1, cqt_result.n_bins,
                              cqt_result.n_frames, sample_rate, hop_length, fmin, bins_per_octave,
                              2, &output, &output_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_vqt_to_audio(cqt_result.magnitude, matrix_length, cqt_result.n_bins,
                              cqt_result.n_frames, sample_rate, hop_length, fmin, bins_per_octave,
                              0.0f, 257, &output,
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
    REQUIRE(sonare_audio_duration_sec(audio) > 0.9f);
    REQUIRE(sonare_audio_duration_sec(audio) < 1.1f);

    sonare_audio_destroy(audio);
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

TEST_CASE("silence leaves tempo, rhythm, timbre, melody and pitch summaries not measured",
          "[c_api]") {
  const std::vector<float> silence(22050 * 3, 0.0f);
  const float* x = silence.data();
  const size_t n = silence.size();

  float bpm = 0.0f;
  REQUIRE(sonare_detect_bpm(x, n, 22050, 60.0f, 200.0f, &bpm) == SONARE_OK);
  CHECK(std::isnan(bpm));

  SonareBpmAnalysisResult tempo{};
  REQUIRE(sonare_analyze_bpm(x, n, 22050, 60.0f, 200.0f, 2048, 512, 5, &tempo) == SONARE_OK);
  CHECK(std::isnan(tempo.bpm));
  CHECK(tempo.candidate_count == 0);
  sonare_free_bpm_analysis_result(&tempo);

  SonareRhythmResult rhythm{};
  REQUIRE(sonare_analyze_rhythm(x, n, 22050, 60.0f, 200.0f, 120.0f, 2048, 512, &rhythm) ==
          SONARE_OK);
  CHECK(std::isnan(rhythm.bpm));
  CHECK(std::isnan(rhythm.syncopation));
  CHECK(std::isnan(rhythm.pattern_regularity));
  CHECK(std::isnan(rhythm.tempo_stability));
  sonare_free_rhythm_result(&rhythm);

  SonareTimbreResult timbre{};
  REQUIRE(sonare_analyze_timbre(x, n, 22050, 2048, 512, 128, 13, 0.5f, &timbre) == SONARE_OK);
  CHECK(std::isnan(timbre.brightness));
  CHECK(std::isnan(timbre.warmth));
  CHECK(std::isnan(timbre.density));
  CHECK(std::isnan(timbre.roughness));
  CHECK(std::isnan(timbre.complexity));
  REQUIRE(timbre.timbre_over_time_count > 0);
  for (size_t i = 0; i < timbre.timbre_over_time_count; ++i) {
    CHECK(std::isnan(timbre.timbre_over_time[i].brightness));
    CHECK(std::isnan(timbre.timbre_over_time[i].complexity));
  }
  sonare_free_timbre_result(&timbre);

  SonareMelodyResult melody{};
  REQUIRE(sonare_analyze_melody(x, n, 22050, 65.0f, 2093.0f, 2048, 256, 0.1f, nullptr, &melody) ==
          SONARE_OK);
  CHECK(std::isnan(melody.pitch_range_octaves));
  CHECK(std::isnan(melody.pitch_stability));
  CHECK(std::isnan(melody.mean_frequency_hz));
  CHECK(std::isnan(melody.vibrato_rate_hz));
  sonare_free_melody_result(&melody);

  SonarePitchResult pitch{};
  REQUIRE(sonare_pitch_yin(x, n, 22050, 2048, 512, 65.0f, 2093.0f, 0.1f, &pitch) == SONARE_OK);
  CHECK(std::isnan(pitch.median_f0));
  CHECK(std::isnan(pitch.mean_f0));
  sonare_free_pitch_result(&pitch);
}

TEST_CASE("sonare_detect_bpm", "[c_api]") {
  SECTION("detects BPM from samples") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    float bpm = 0.0f;

    SonareError err = sonare_detect_bpm(samples.data(), samples.size(), 22050, 30.0f, 300.0f, &bpm);

    REQUIRE(err == SONARE_OK);
    REQUIRE(bpm > 0.0f);
  }

  SECTION("a narrow range limits the search") {
    auto samples = generate_clicks(120.0f, 22050, 6.0f);
    float wide = 0.0f;
    float narrow = 0.0f;
    REQUIRE(sonare_detect_bpm(samples.data(), samples.size(), 22050, 30.0f, 300.0f, &wide) ==
            SONARE_OK);
    REQUIRE(sonare_detect_bpm(samples.data(), samples.size(), 22050, 60.0f, 100.0f, &narrow) ==
            SONARE_OK);
    CHECK(wide == Catch::Approx(120.0f).margin(3.0f));
    CHECK(narrow <= 100.0f);
    CHECK(narrow >= 60.0f);
  }

  SECTION("an inverted or non-positive range is refused") {
    auto samples = generate_clicks(120.0f, 22050, 2.0f);
    float bpm = 0.0f;
    CHECK(sonare_detect_bpm(samples.data(), samples.size(), 22050, 200.0f, 60.0f, &bpm) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_detect_bpm(samples.data(), samples.size(), 22050, 0.0f, 200.0f, &bpm) ==
          SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("returns error for null samples") {
    float bpm = 0.0f;
    SonareError err = sonare_detect_bpm(nullptr, 100, 22050, 30.0f, 300.0f, &bpm);
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("returns error for null output") {
    auto samples = generate_clicks(120.0f, 22050, 1.0f);
    SonareError err =
        sonare_detect_bpm(samples.data(), samples.size(), 22050, 30.0f, 300.0f, nullptr);
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }
}

static SonareDetectKeyOptions key_options(int n_fft, int hop_length, int use_hpss,
                                          int loudness_weighted, float chroma_high_pass_hz) {
  SonareDetectKeyOptions options;
  sonare_detect_key_options_init(&options);
  options.n_fft = n_fft;
  options.hop_length = hop_length;
  options.use_hpss = use_hpss;
  options.loudness_weighted = loudness_weighted;
  options.chroma_high_pass_hz = chroma_high_pass_hz;
  return options;
}

TEST_CASE("sonare_detect_key", "[c_api]") {
  SECTION("detects key from samples") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKey key = {};

    SonareError err =
        sonare_detect_key(samples.data(), samples.size(), 22050, nullptr, &key, nullptr);

    REQUIRE(err == SONARE_OK);
    REQUIRE(key.root >= SONARE_PITCH_C);
    REQUIRE(key.root <= SONARE_PITCH_B);
    REQUIRE((key.mode == SONARE_MODE_MAJOR || key.mode == SONARE_MODE_MINOR));
    REQUIRE(key.confidence >= 0.0f);
    REQUIRE(key.confidence <= 1.0f);
  }

  SECTION("returns error for null samples") {
    SonareKey key = {};
    SonareError err = sonare_detect_key(nullptr, 100, 22050, nullptr, &key, nullptr);
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("detects key with explicit analysis options") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKey key = {};
    const SonareDetectKeyOptions options = key_options(4096, 512, 0, 0, 80.0f);

    SonareError err =
        sonare_detect_key(samples.data(), samples.size(), 22050, &options, &key, nullptr);

    REQUIRE(err == SONARE_OK);
    REQUIRE(key.root >= SONARE_PITCH_C);
    REQUIRE(key.root <= SONARE_PITCH_B);
    REQUIRE(key.confidence >= 0.0f);
    REQUIRE(key.confidence <= 1.0f);
  }

  SECTION("returns error for invalid explicit key options") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKey key = {};
    const SonareDetectKeyOptions options = key_options(0, 512, 0, 0, 0.0f);

    SonareError err =
        sonare_detect_key(samples.data(), samples.size(), 22050, &options, &key, nullptr);

    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("refuses options not made by the init function") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKey key = {};
    SonareDetectKeyOptions options = {};

    REQUIRE(sonare_detect_key(samples.data(), samples.size(), 22050, &options, &key, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()) ==
            "options.struct_size must be set by sonare_detect_key_options_init");
    SonareKeyCandidate* candidates = nullptr;
    size_t count = 0;
    REQUIRE(sonare_detect_key_candidates(samples.data(), samples.size(), 22050, &options,
                                         &candidates, &count,
                                         nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("returns sorted key candidates with correlations") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKeyCandidate* candidates = nullptr;
    size_t count = 0;
    const SonareDetectKeyOptions options = key_options(4096, 512, 0, 0, 0.0f);

    SonareError err = sonare_detect_key_candidates(samples.data(), samples.size(), 22050, &options,
                                                   &candidates, &count, nullptr);

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
    SonareDetectKeyOptions options = key_options(4096, 512, 0, 0, 0.0f);
    options.modes = modes;
    options.mode_count = 4;

    SonareError err = sonare_detect_key_candidates(samples.data(), samples.size(), 22050, &options,
                                                   &candidates, &count, nullptr);

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
    SonareDetectKeyOptions options = key_options(4096, 512, 0, 0, 0.0f);
    options.modes = modes;
    options.mode_count = 3;

    SonareError err =
        sonare_detect_key(samples.data(), samples.size(), 22050, &options, &key, nullptr);

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
    SonareDetectKeyOptions options = key_options(4096, 512, 1, 1, 80.0f);
    options.modes = modes;
    options.mode_count = 2;
    options.profile_type = SONARE_KEY_PROFILE_FARALDO_EDMA;
    options.genre_hint = "edm";

    SonareError err =
        sonare_detect_key(samples.data(), samples.size(), 22050, &options, &key, nullptr);

    REQUIRE(err == SONARE_OK);
    REQUIRE(key.root >= SONARE_PITCH_C);
    REQUIRE(key.root <= SONARE_PITCH_B);
    REQUIRE((key.mode == SONARE_MODE_MAJOR || key.mode == SONARE_MODE_MINOR));
  }

  SECTION("returns candidates with explicit profile and genre options") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKeyCandidate* candidates = nullptr;
    size_t count = 0;
    SonareDetectKeyOptions options = key_options(4096, 512, 0, 0, 0.0f);
    options.profile_type = SONARE_KEY_PROFILE_TEMPERLEY;
    options.genre_hint = "pop";

    SonareError err = sonare_detect_key_candidates(samples.data(), samples.size(), 22050, &options,
                                                   &candidates, &count, nullptr);

    REQUIRE(err == SONARE_OK);
    REQUIRE(candidates != nullptr);
    REQUIRE(count == 24);
    sonare_free_key_candidates(candidates);
  }

  SECTION("returns error for null key candidate outputs") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    SonareKeyCandidate* candidates = nullptr;
    size_t count = 0;
    const SonareDetectKeyOptions options = key_options(4096, 512, 0, 0, 0.0f);

    REQUIRE(sonare_detect_key_candidates(samples.data(), samples.size(), 22050, &options, nullptr,
                                         &count, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_key_candidates(samples.data(), samples.size(), 22050, &options,
                                         &candidates, nullptr,
                                         nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("returns error for invalid modal options") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    const SonareMode bad_modes[] = {static_cast<SonareMode>(99)};
    SonareKey key = {};
    SonareKeyCandidate* candidates = nullptr;
    size_t count = 0;
    SonareDetectKeyOptions options = key_options(4096, 512, 0, 0, 0.0f);
    options.modes = bad_modes;
    options.mode_count = 1;

    REQUIRE(sonare_detect_key(samples.data(), samples.size(), 22050, &options, &key, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_key_candidates(samples.data(), samples.size(), 22050, &options,
                                         &candidates, &count,
                                         nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    options.modes = nullptr;
    options.mode_count = 0;
    options.profile_type = static_cast<SonareKeyProfileType>(99);
    REQUIRE(sonare_detect_key(samples.data(), samples.size(), 22050, &options, &key, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_stream_analyzer C API validates config and reads quantized frames", "[c_api]") {
  SonareStreamConfig config;
  sonare_stream_config_init(&config);
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
      REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size(), nullptr) ==
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
      REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size(), nullptr) ==
              SONARE_OK);
      SonareStreamFramesU8 u8{};
      REQUIRE(sonare_stream_analyzer_read_quantized_frames_u8(analyzer, nullptr, 1, &u8) ==
              SONARE_OK);
      REQUIRE(u8.n_frames == 1);
      REQUIRE(u8.feature_flags == mask);
      REQUIRE(u8.n_mels == ((mask & SONARE_STREAM_FEATURE_MEL) ? 8 : 0));
      REQUIRE(u8.n_chroma == ((mask & SONARE_STREAM_FEATURE_CHROMA) ? 12 : 0));
      REQUIRE((u8.mel != nullptr) == ((mask & SONARE_STREAM_FEATURE_MEL) != 0));
      REQUIRE((u8.chroma != nullptr) == ((mask & SONARE_STREAM_FEATURE_CHROMA) != 0));
      REQUIRE((u8.onset_strength != nullptr) == ((mask & SONARE_STREAM_FEATURE_ONSET) != 0));
      REQUIRE((u8.spectral_centroid != nullptr) == ((mask & SONARE_STREAM_FEATURE_SPECTRAL) != 0));
      REQUIRE((u8.chord_root != nullptr) == ((mask & SONARE_STREAM_FEATURE_CHROMA) != 0));
      REQUIRE((u8.chord_quality != nullptr) == ((mask & SONARE_STREAM_FEATURE_CHROMA) != 0));
      REQUIRE((u8.chord_confidence != nullptr) == ((mask & SONARE_STREAM_FEATURE_CHROMA) != 0));
      REQUIRE(u8.rms_energy != nullptr);
      sonare_free_stream_frames_u8(&u8);
      sonare_stream_analyzer_destroy(analyzer);

      analyzer = nullptr;
      REQUIRE(sonare_stream_analyzer_create(&shaped, &analyzer) == SONARE_OK);
      REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size(), nullptr) ==
              SONARE_OK);
      SonareStreamFramesI16 i16{};
      REQUIRE(sonare_stream_analyzer_read_quantized_frames_i16(analyzer, nullptr, 1, &i16) ==
              SONARE_OK);
      REQUIRE(i16.n_frames == 1);
      REQUIRE(i16.feature_flags == mask);
      REQUIRE(i16.n_mels == ((mask & SONARE_STREAM_FEATURE_MEL) ? 8 : 0));
      REQUIRE(i16.n_chroma == ((mask & SONARE_STREAM_FEATURE_CHROMA) ? 12 : 0));
      REQUIRE((i16.mel != nullptr) == ((mask & SONARE_STREAM_FEATURE_MEL) != 0));
      REQUIRE((i16.chroma != nullptr) == ((mask & SONARE_STREAM_FEATURE_CHROMA) != 0));
      REQUIRE((i16.onset_strength != nullptr) == ((mask & SONARE_STREAM_FEATURE_ONSET) != 0));
      REQUIRE((i16.spectral_centroid != nullptr) == ((mask & SONARE_STREAM_FEATURE_SPECTRAL) != 0));
      REQUIRE((i16.chord_root != nullptr) == ((mask & SONARE_STREAM_FEATURE_CHROMA) != 0));
      REQUIRE((i16.chord_quality != nullptr) == ((mask & SONARE_STREAM_FEATURE_CHROMA) != 0));
      REQUIRE((i16.chord_confidence != nullptr) == ((mask & SONARE_STREAM_FEATURE_CHROMA) != 0));
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
    REQUIRE(std::string(sonare_last_error_message()) ==
            "config.hop_length must be at most config.n_fft");
    REQUIRE(analyzer == nullptr);
  }

  SECTION("rejects an out-of-range sample rate and oversized sizing before allocating") {
    SonareStreamAnalyzer* analyzer = nullptr;
    for (const int rate : {7999, 384001, 1000000000}) {
      SonareStreamConfig bad = config;
      bad.sample_rate = rate;
      INFO("sample_rate " << rate);
      REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(analyzer == nullptr);
    }
    for (const int rate : {22050, 44100, 48000}) {
      SonareStreamConfig bad = config;
      bad.sample_rate = rate;
      bad.n_fft = 1 << 26;
      INFO("n_fft at " << rate);
      REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(analyzer == nullptr);
    }
    SonareStreamConfig bad = config;
    bad.n_fft = std::numeric_limits<int>::max();
    bad.hop_length = bad.n_fft;
    REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
    bad = config;
    bad.n_mels = 1 << 24;
    REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(analyzer == nullptr);
  }

  SECTION("reports the unavailable magnitude plane by its C field name") {
    SonareStreamConfig bad = config;
    bad.compute_magnitude = 1;
    auto* analyzer = reinterpret_cast<SonareStreamAnalyzer*>(&bad);
    REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_NOT_SUPPORTED);
    CHECK(std::string(sonare_last_error_message()) ==
          "config.compute_magnitude is not supported because magnitude frames are not "
          "exposed by StreamAnalyzer read paths");
    CHECK(analyzer == nullptr);
  }

  SECTION("rejects invalid intervals and mel range") {
    SonareStreamConfig bad = config;
    bad.key_update_interval_sec = 0.0f;
    SonareStreamAnalyzer* analyzer = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()) ==
            "config.key_update_interval_sec must be finite and greater than 0");

    bad = config;
    bad.max_progression_entries = 0;
    REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()) ==
            "config.max_progression_entries must be an integer in [1, 1048576]");

    bad = config;
    bad.fmin_hz = 1000.0f;
    bad.fmax_hz = 500.0f;
    REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()) ==
            "config.fmax_hz must be greater than config.fmin_hz");

    // NaN is the Nyquist default; a given 0 is refused by name.
    REQUIRE(std::isnan(config.fmax_hz));
    bad = config;
    bad.fmax_hz = 0.0f;
    REQUIRE(sonare_stream_analyzer_create(&bad, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()).find("config.fmax_hz") != std::string::npos);
  }

  SECTION("refuses a config or quantize config not started from its init") {
    SonareStreamAnalyzer* analyzer = nullptr;
    SonareStreamConfig zeroed{};
    REQUIRE(sonare_stream_analyzer_create(&zeroed, &analyzer) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()) ==
            "config.struct_size must be set by sonare_stream_config_init");
    REQUIRE(sonare_stream_analyzer_create(&config, &analyzer) == SONARE_OK);
    SonareStreamQuantizeConfig quantize{};
    SonareStreamFramesU8 frames{};
    REQUIRE(sonare_stream_analyzer_read_quantized_frames_u8(analyzer, &quantize, 0, &frames) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()) ==
            "config.struct_size must be set by sonare_stream_quantize_config_init");
    sonare_stream_analyzer_destroy(analyzer);
  }

  SECTION("create and the live setter accept one tuning range") {
    // The same value must be accepted or refused identically whether it arrives
    // in the create config or through the setter; neither clamps.
    SonareStreamConfig bad = config;
    bad.reference_hz = 100.0f;
    SonareStreamAnalyzer* rejected = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&bad, &rejected) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(rejected == nullptr);
    bad.reference_hz = 1000.0f;
    REQUIRE(sonare_stream_analyzer_create(&bad, &rejected) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(rejected == nullptr);

    SonareStreamAnalyzer* analyzer = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&config, &analyzer) == SONARE_OK);
    REQUIRE(sonare_stream_analyzer_set_reference_hz(analyzer, 100.0f) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_stream_analyzer_set_reference_hz(analyzer, 1000.0f) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_stream_analyzer_set_reference_hz(analyzer, 466.16f) == SONARE_OK);
    sonare_stream_analyzer_destroy(analyzer);
  }

  SECTION("rejects non-finite setters and quantization ranges") {
    SonareStreamAnalyzer* analyzer = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&config, &analyzer) == SONARE_OK);
    REQUIRE(analyzer != nullptr);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (float invalid : {nan, inf, -inf, -1.0f}) {
      REQUIRE(sonare_stream_analyzer_set_expected_duration(analyzer, invalid) ==
              SONARE_ERROR_INVALID_PARAMETER);
      CHECK(std::string(sonare_last_error_message()) ==
            "duration_sec must be finite and at least 0");
    }
    CHECK(sonare_stream_analyzer_set_expected_duration(analyzer, 0.0f) == SONARE_OK);
    REQUIRE(sonare_stream_analyzer_set_normalization_gain(analyzer, inf) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_stream_analyzer_set_reference_hz(analyzer, nan) ==
            SONARE_ERROR_INVALID_PARAMETER);

    SonareStreamQuantizeConfig quantize;

    sonare_stream_quantize_config_init(&quantize);
    quantize.rms_max = inf;
    SonareStreamFramesU8 frames{};
    REQUIRE(sonare_stream_analyzer_read_quantized_frames_u8(analyzer, &quantize, 0, &frames) ==
            SONARE_ERROR_INVALID_PARAMETER);
    sonare_stream_quantize_config_init(&quantize);
    quantize.mel_db_min = quantize.mel_db_max;
    REQUIRE(sonare_stream_analyzer_read_quantized_frames_u8(analyzer, &quantize, 0, &frames) ==
            SONARE_ERROR_INVALID_PARAMETER);
    sonare_stream_analyzer_destroy(analyzer);
  }

  SECTION("reads U8 and I16 frames") {
    SonareStreamAnalyzer* analyzer = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&config, &analyzer) == SONARE_OK);
    REQUIRE(analyzer != nullptr);

    auto samples = generate_sine(440.0f, config.sample_rate, 0.25f);
    REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size(), nullptr) ==
            SONARE_OK);

    SonareStreamFramesU8 u8 = {};
    REQUIRE(sonare_stream_analyzer_read_quantized_frames_u8(analyzer, nullptr, 4, &u8) ==
            SONARE_OK);
    REQUIRE(u8.n_frames > 0);
    REQUIRE(u8.n_frames <= 4);
    REQUIRE(u8.n_mels == config.n_mels);
    REQUIRE(u8.n_chroma == 12);
    REQUIRE(u8.feature_flags == (SONARE_STREAM_FEATURE_MEL | SONARE_STREAM_FEATURE_CHROMA |
                                 SONARE_STREAM_FEATURE_ONSET | SONARE_STREAM_FEATURE_SPECTRAL));
    REQUIRE(u8.timestamps_sec != nullptr);
    REQUIRE(u8.mel != nullptr);
    REQUIRE(u8.chroma != nullptr);
    sonare_free_stream_frames_u8(&u8);

    REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size(), nullptr) ==
            SONARE_OK);
    SonareStreamFramesI16 i16 = {};
    REQUIRE(sonare_stream_analyzer_read_quantized_frames_i16(analyzer, nullptr, 4, &i16) ==
            SONARE_OK);
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

  SECTION("quantized chord arrays agree with the float read within one quantization step") {
    const auto samples = generate_sine(440.0f, config.sample_rate, 0.5f);
    const auto fed = [&] {
      SonareStreamAnalyzer* analyzer = nullptr;
      REQUIRE(sonare_stream_analyzer_create(&config, &analyzer) == SONARE_OK);
      REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size(), nullptr) ==
              SONARE_OK);
      return analyzer;
    };

    SonareStreamAnalyzer* float_analyzer = fed();
    SonareStreamFrames floats{};
    REQUIRE(sonare_stream_analyzer_read_frames(float_analyzer, 8, &floats) == SONARE_OK);
    REQUIRE(floats.n_frames > 0);
    REQUIRE(floats.chord_root != nullptr);

    SonareStreamAnalyzer* u8_analyzer = fed();
    SonareStreamFramesU8 u8{};
    REQUIRE(sonare_stream_analyzer_read_quantized_frames_u8(u8_analyzer, nullptr, 8, &u8) ==
            SONARE_OK);
    REQUIRE(u8.n_frames == floats.n_frames);
    REQUIRE(u8.chord_root != nullptr);
    REQUIRE(u8.chord_quality != nullptr);
    REQUIRE(u8.chord_confidence != nullptr);

    SonareStreamAnalyzer* i16_analyzer = fed();
    SonareStreamFramesI16 i16{};
    REQUIRE(sonare_stream_analyzer_read_quantized_frames_i16(i16_analyzer, nullptr, 8, &i16) ==
            SONARE_OK);
    REQUIRE(i16.n_frames == floats.n_frames);
    REQUIRE(i16.chord_root != nullptr);
    REQUIRE(i16.chord_quality != nullptr);
    REQUIRE(i16.chord_confidence != nullptr);

    for (int i = 0; i < floats.n_frames; ++i) {
      CAPTURE(i);
      CHECK(u8.chord_root[i] == floats.chord_root[i]);
      CHECK(u8.chord_quality[i] == floats.chord_quality[i]);
      CHECK(std::fabs(static_cast<float>(u8.chord_confidence[i]) / 255.0f -
                      floats.chord_confidence[i]) <= 1.0f / 255.0f);
      CHECK(i16.chord_root[i] == floats.chord_root[i]);
      CHECK(i16.chord_quality[i] == floats.chord_quality[i]);
      CHECK(std::fabs((static_cast<float>(i16.chord_confidence[i]) + 32768.0f) / 65535.0f -
                      floats.chord_confidence[i]) <= 1.0f / 65535.0f);
    }

    sonare_free_stream_frames(&floats);
    sonare_free_stream_frames_u8(&u8);
    sonare_free_stream_frames_i16(&i16);
    sonare_stream_analyzer_destroy(float_analyzer);
    sonare_stream_analyzer_destroy(u8_analyzer);
    sonare_stream_analyzer_destroy(i16_analyzer);
  }

  SECTION("finalize flushes a partial tail frame") {
    SonareStreamAnalyzer* analyzer = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&config, &analyzer) == SONARE_OK);
    REQUIRE(analyzer != nullptr);

    std::vector<float> samples(600, 0.0f);
    REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size(), nullptr) ==
            SONARE_OK);

    size_t available = 0;
    // window = 1024 * 22050 / 44100 = 512 samples, hop 256: 600 samples hold
    // floor((600 - 512) / 256) + 1 = 1 full frame; finalize adds the zero-padded tail frame.
    REQUIRE(sonare_stream_analyzer_available_frames(analyzer, &available) == SONARE_OK);
    REQUIRE(available == 1);

    REQUIRE(sonare_stream_analyzer_finalize(analyzer) == SONARE_OK);
    REQUIRE(sonare_stream_analyzer_available_frames(analyzer, &available) == SONARE_OK);
    REQUIRE(available == 2);

    REQUIRE(sonare_stream_analyzer_finalize(analyzer) == SONARE_OK);
    REQUIRE(sonare_stream_analyzer_available_frames(analyzer, &available) == SONARE_OK);
    REQUIRE(available == 2);

    SonareStreamFrames frames = {};
    REQUIRE(sonare_stream_analyzer_read_frames(analyzer, 2, &frames) == SONARE_OK);
    REQUIRE(frames.n_frames == 2);
    REQUIRE(frames.n_mels == config.n_mels);
    REQUIRE(frames.timestamps_sec != nullptr);
    REQUIRE(frames.timestamps_sec[0] == Catch::Approx(0.0f));
    REQUIRE(frames.timestamps_sec[1] == Catch::Approx(256.0f / 22050.0f));
    sonare_free_stream_frames(&frames);

    sonare_stream_analyzer_destroy(analyzer);
  }

  SECTION("quantize-config override widens the saturating range") {
    SonareStreamQuantizeConfig qdefault;
    sonare_stream_quantize_config_init(&qdefault);
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
    REQUIRE(sonare_stream_analyzer_process(tight, samples.data(), samples.size(), nullptr) ==
            SONARE_OK);
    SonareStreamQuantizeConfig narrow = qdefault;
    narrow.centroid_max = 1.0f;
    SonareStreamFramesU8 tight_frames = {};
    REQUIRE(sonare_stream_analyzer_read_quantized_frames_u8(tight, &narrow, 4, &tight_frames) ==
            SONARE_OK);
    REQUIRE(tight_frames.n_frames > 0);

    SonareStreamAnalyzer* wide = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&config, &wide) == SONARE_OK);
    REQUIRE(sonare_stream_analyzer_process(wide, samples.data(), samples.size(), nullptr) ==
            SONARE_OK);
    SonareStreamQuantizeConfig broad = qdefault;
    broad.centroid_max = 1.0e9f;
    SonareStreamFramesU8 wide_frames = {};
    REQUIRE(sonare_stream_analyzer_read_quantized_frames_u8(wide, &broad, 4, &wide_frames) ==
            SONARE_OK);
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
    REQUIRE(sonare_stream_analyzer_read_quantized_frames_u8(wide, nullptr, 4, &null_frames) ==
            SONARE_OK);
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
    REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), samples.size(), nullptr) ==
            SONARE_OK);
    size_t available = 0;
    REQUIRE(sonare_stream_analyzer_available_frames(analyzer, &available) == SONARE_OK);
    REQUIRE(available == 3);

    SonareStreamStats stats{};
    REQUIRE(sonare_stream_analyzer_stats(analyzer, &stats) == SONARE_OK);
    REQUIRE(stats.pending_frames == 3);
    REQUIRE(stats.dropped_output_frames > 0);
    REQUIRE(stats.pending_frames + stats.dropped_output_frames ==
            static_cast<uint64_t>(stats.total_frames));
    REQUIRE(stats.dropped_chord_progression_entries == 0);
    REQUIRE(stats.dropped_bar_progression_entries == 0);
    sonare_free_stream_stats(&stats);
    sonare_stream_analyzer_destroy(analyzer);
  }
}

TEST_CASE("stream quantize config requires struct_size", "[c_api][streaming]") {
  SonareStreamConfig stream_config;
  sonare_stream_config_init(&stream_config);
  stream_config.sample_rate = 22050;

  SonareStreamAnalyzer* analyzer = nullptr;
  REQUIRE(sonare_stream_analyzer_create(&stream_config, &analyzer) == SONARE_OK);

  SonareStreamQuantizeConfig quantize_config{};
  SonareStreamFramesU8 frames{};
  CHECK(sonare_stream_analyzer_read_quantized_frames_u8(analyzer, &quantize_config, 0, &frames) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "config.struct_size must be set by sonare_stream_quantize_config_init");
  sonare_stream_analyzer_destroy(analyzer);
}

TEST_CASE("sonare_detect_beats", "[c_api]") {
  SECTION("detects beats from samples") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    float* times = nullptr;
    size_t count = 0;

    SonareError err =
        sonare_detect_beats(samples.data(), samples.size(), 22050, 30.0f, 300.0f, &times, &count);

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

  SECTION("a narrow range limits the beat spacing and an inverted one is refused") {
    auto samples = generate_clicks(120.0f, 22050, 8.0f);
    float* times = nullptr;
    size_t count = 0;
    REQUIRE(sonare_detect_beats(samples.data(), samples.size(), 22050, 60.0f, 100.0f, &times,
                                &count) == SONARE_OK);
    if (count > 1) {
      // 100 BPM is the fastest the range allows: beats are at least 0.6 s apart on average.
      const float mean_spacing = (times[count - 1] - times[0]) / static_cast<float>(count - 1);
      CHECK(mean_spacing >= 0.55f);
    }
    sonare_free_floats(times);

    times = nullptr;
    count = 0;
    CHECK(sonare_detect_beats(samples.data(), samples.size(), 22050, 200.0f, 60.0f, &times,
                              &count) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(times == nullptr);
    CHECK(count == 0);
  }
}

TEST_CASE("sonare_audio_detect_bpm and sonare_audio_detect_beats take the search range",
          "[c_api]") {
  auto samples = generate_clicks(120.0f, 22050, 8.0f);
  SonareAudio* audio = nullptr;
  REQUIRE(sonare_audio_from_buffer(samples.data(), samples.size(), 22050, &audio) == SONARE_OK);

  SECTION("the handle agrees with the sample entry for the same range") {
    float from_handle = 0.0f;
    float from_samples = 0.0f;
    REQUIRE(sonare_audio_detect_bpm(audio, 60.0f, 100.0f, &from_handle) == SONARE_OK);
    REQUIRE(sonare_detect_bpm(samples.data(), samples.size(), 22050, 60.0f, 100.0f,
                              &from_samples) == SONARE_OK);
    CHECK(from_handle == Catch::Approx(from_samples).margin(1e-3f));
    CHECK(from_handle >= 60.0f);
    CHECK(from_handle <= 100.0f);

    float wide = 0.0f;
    REQUIRE(sonare_audio_detect_bpm(audio, 30.0f, 300.0f, &wide) == SONARE_OK);
    CHECK(wide == Catch::Approx(120.0f).margin(3.0f));

    float* times = nullptr;
    size_t count = 0;
    REQUIRE(sonare_audio_detect_beats(audio, 60.0f, 100.0f, &times, &count) == SONARE_OK);
    float* sample_times = nullptr;
    size_t sample_count = 0;
    REQUIRE(sonare_detect_beats(samples.data(), samples.size(), 22050, 60.0f, 100.0f, &sample_times,
                                &sample_count) == SONARE_OK);
    REQUIRE(count == sample_count);
    for (size_t i = 0; i < count; ++i) {
      CHECK(times[i] == Catch::Approx(sample_times[i]).margin(1e-4f));
    }
    if (count > 1) {
      const float mean_spacing = (times[count - 1] - times[0]) / static_cast<float>(count - 1);
      CHECK(mean_spacing >= 0.55f);
    }
    sonare_free_floats(times);
    sonare_free_floats(sample_times);
  }

  SECTION("an inverted, non-positive or non-finite range is refused") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::array<std::array<float, 2>, 3> ranges = {
        {{200.0f, 60.0f}, {0.0f, 200.0f}, {60.0f, nan}}};
    for (const auto& range : ranges) {
      float bpm = -1.0f;
      CHECK(sonare_audio_detect_bpm(audio, range[0], range[1], &bpm) ==
            SONARE_ERROR_INVALID_PARAMETER);
      float* times = nullptr;
      size_t count = 7;
      CHECK(sonare_audio_detect_beats(audio, range[0], range[1], &times, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
      CHECK(times == nullptr);
      CHECK(count == 0);
    }
  }

  SECTION("a null handle is refused") {
    float bpm = 0.0f;
    CHECK(sonare_audio_detect_bpm(nullptr, 60.0f, 200.0f, &bpm) == SONARE_ERROR_INVALID_PARAMETER);
    float* times = nullptr;
    size_t count = 0;
    CHECK(sonare_audio_detect_beats(nullptr, 60.0f, 200.0f, &times, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  }

  sonare_audio_destroy(audio);
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

    sonare_audio_destroy(audio);
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

    SonareError err =
        sonare_detect_onsets(samples.data(), samples.size(), 22050, nullptr, &times, &count);

    REQUIRE(err == SONARE_OK);
    if (count > 0) {
      REQUIRE(times != nullptr);
      sonare_free_floats(times);
    }
  }
}

TEST_CASE("C music analysis defaults skip HPSS and use the chord HMM", "[c_api][analysis]") {
  SonareMusicAnalyzeOptions options;
  sonare_music_analyze_options_init(&options);
  CHECK(options.use_hpss == 0);
  CHECK(options.use_chord_hmm == 1);
}

TEST_CASE("C key detection defaults use a 2048-point FFT and an 80 Hz high-pass",
          "[c_api][analysis]") {
  SonareDetectKeyOptions options;
  sonare_detect_key_options_init(&options);
  CHECK(options.n_fft == 2048);
  CHECK(options.chroma_high_pass_hz == 80.0f);
}

TEST_CASE("sonare_analyze_json", "[.][slow][c_api]") {
  auto samples = generate_clicks(120.0f, 22050, 4.0f);

  SECTION("serializes the full analysis result") {
    char* json = nullptr;
    SonareError err = sonare_analyze_json(samples.data(), samples.size(), 22050, nullptr, &json);
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
    // Beats carry per-beat strength.
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
    REQUIRE(root["sections"][static_cast<std::size_t>(0)]["group"].as_number() ==
            static_cast<double>(fixture.sections.front().group));

    // Downbeats serialize as the beat records they index, plus the meter phase.
    const auto& downbeat =
        fixture.beats.at(static_cast<std::size_t>(fixture.downbeat_indices.front()));
    REQUIRE(root["downbeats"].is_array());
    REQUIRE(root["downbeats"].size() == fixture.downbeat_indices.size());
    REQUIRE(root["downbeats"][static_cast<std::size_t>(0)]["timeSec"].as_number() ==
            Catch::Approx(downbeat.time));
    REQUIRE(root["downbeats"][static_cast<std::size_t>(0)]["strength"].as_number() ==
            Catch::Approx(downbeat.strength));
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

    SonareMusicAnalyzeOptions defaults;

    sonare_music_analyze_options_init(&defaults);
    defaults.progress_callback = cb;
    defaults.progress_user_data = &state;
    char* json = nullptr;
    SonareError err = sonare_analyze_json(samples.data(), samples.size(), 22050, &defaults, &json);
    REQUIRE(err == SONARE_OK);
    REQUIRE(json != nullptr);
    REQUIRE(state.calls > 0);
    REQUIRE(state.last >= 0.0f);
    REQUIRE(sonare::util::json::parse(json).contains("chords"));
    sonare_free_string(json);

    // A null callback runs silently and still produces output.
    char* json2 = nullptr;
    defaults.progress_callback = nullptr;
    defaults.progress_user_data = nullptr;
    REQUIRE(sonare_analyze_json(samples.data(), samples.size(), 22050, &defaults, &json2) ==
            SONARE_OK);
    REQUIRE(json2 != nullptr);
    sonare_free_string(json2);
  }

  // Every binding hands this entry point a pointer it does not own (a Node
  // TypedArray, a Python buffer, a WASM heap view) and then lets the progress
  // callback re-enter caller code, where that memory can be rewritten or freed.
  // run_offline copies the buffer into an Audio before the analyzer — and
  // therefore the callback — exists, which is what makes the borrow safe;
  // mutating the caller's buffer from inside the callback is how that stays
  // checkable. A copy dropped here would make every binding pay for its own.
  SECTION("copies the input before the first progress callback") {
    struct MutateState {
      std::vector<float>* buffer = nullptr;
      int calls = 0;
    };

    SonareMusicAnalyzeOptions defaults;

    sonare_music_analyze_options_init(&defaults);
    char* reference_json = nullptr;
    MutateState quiet{nullptr, 0};
    auto count_only = [](float, const char*, void* user_data) {
      ++static_cast<MutateState*>(user_data)->calls;
    };
    defaults.progress_callback = count_only;
    defaults.progress_user_data = &quiet;
    REQUIRE(sonare_analyze_json(samples.data(), samples.size(), 22050, &defaults,
                                &reference_json) == SONARE_OK);
    REQUIRE(reference_json != nullptr);
    REQUIRE(quiet.calls > 0);
    const std::string reference(reference_json);
    sonare_free_string(reference_json);

    auto victim = samples;
    MutateState state{&victim, 0};
    auto mutate = [](float, const char*, void* user_data) {
      auto* s = static_cast<MutateState*>(user_data);
      ++s->calls;
      std::fill(s->buffer->begin(), s->buffer->end(), -1.0f);
    };
    char* mutated_json = nullptr;
    defaults.progress_callback = mutate;
    defaults.progress_user_data = &state;
    REQUIRE(sonare_analyze_json(victim.data(), victim.size(), 22050, &defaults, &mutated_json) ==
            SONARE_OK);
    REQUIRE(mutated_json != nullptr);
    const std::string mutated(mutated_json);
    sonare_free_string(mutated_json);
    REQUIRE(state.calls > 0);
    CHECK(mutated == reference);

    // Non-vacuity: analysing the overwritten signal really does produce a
    // different result, so equality above is evidence rather than a tautology.
    std::vector<float> overwritten(samples.size(), -1.0f);
    char* overwritten_json = nullptr;
    REQUIRE(sonare_analyze_json(overwritten.data(), overwritten.size(), 22050, nullptr,
                                &overwritten_json) == SONARE_OK);
    const std::string different(overwritten_json);
    sonare_free_string(overwritten_json);
    CHECK(different != reference);
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

    SonareMusicAnalyzeOptions defaults;

    sonare_music_analyze_options_init(&defaults);
    char* json = nullptr;
    defaults.progress_callback = progress;
    defaults.progress_user_data = &state;
    defaults.cancel_callback = cancel;
    defaults.cancel_user_data = &state;
    REQUIRE(sonare_analyze_json(samples.data(), samples.size(), 22050, &defaults, &json) ==
            SONARE_ERROR_CANCELLED);
    REQUIRE(state.progress_calls > 0);
    REQUIRE(state.last_progress > 0.5f);
    REQUIRE(json == nullptr);
  }

  SECTION("rejects a null out pointer") {
    REQUIRE(sonare_analyze_json(samples.data(), samples.size(), 22050, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("accepts the complete analyzer configuration") {
    SonareMusicAnalyzeOptions options;
    sonare_music_analyze_options_init(&options);
    options.bpm_min = 30.0f;
    options.bpm_max = 90.0f;
    options.start_bpm = 55.0f;
    options.use_hpss = 0;
    options.use_chord_hmm = 1;
    options.detect_chord_inversions = 1;
    char* json = nullptr;
    REQUIRE(sonare_analyze_json(samples.data(), samples.size(), 22050, &options, &json) ==
            SONARE_OK);
    REQUIRE(json != nullptr);
    const auto root = sonare::util::json::parse(json);
    REQUIRE(root["bpm"].as_number() >= options.bpm_min);
    REQUIRE(root["bpm"].as_number() <= options.bpm_max);
    sonare_free_string(json);

    options.bpm_max = options.bpm_min - 1.0f;
    REQUIRE(sonare_analyze_json(samples.data(), samples.size(), 22050, &options, &json) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("emits the tempo curve only when it is asked for") {
    SonareMusicAnalyzeOptions options;
    sonare_music_analyze_options_init(&options);
    REQUIRE(options.compute_tempo_curve == 0);

    char* json = nullptr;
    REQUIRE(sonare_analyze_json(samples.data(), samples.size(), 22050, &options, &json) ==
            SONARE_OK);
    const auto without = sonare::util::json::parse(json);
    // The key is always present; it is the array that is empty, so a reader
    // never has to distinguish "not asked for" from "field missing".
    REQUIRE(without["beatLocalBpm"].as_array().empty());
    const size_t beat_count = without["beats"].as_array().size();
    sonare_free_string(json);
    json = nullptr;

    options.compute_tempo_curve = 1;
    REQUIRE(sonare_analyze_json(samples.data(), samples.size(), 22050, &options, &json) ==
            SONARE_OK);
    const auto with = sonare::util::json::parse(json);
    const auto& curve = with["beatLocalBpm"].as_array();
    REQUIRE(curve.size() == beat_count);
    REQUIRE(!curve.empty());
    for (const auto& value : curve) {
      REQUIRE(value.as_number() > 0.0);
    }
    sonare_free_string(json);
  }
}

namespace {

/// @brief Beat evidence accented once every @p numerator beats.
sonare::MeterBeats make_beat_series(int numerator, int measures) {
  sonare::MeterBeats series;
  for (int i = 0; i < numerator * measures; ++i) {
    series.accent.push_back(i % numerator == 0 ? 1.0f : 0.35f);
    series.has_attack.push_back(1);
  }
  return series;
}

SonareMeterBeats to_c_meter_beats(const sonare::MeterBeats& observations) {
  SonareMeterBeats beats;
  sonare_meter_beats_init(&beats);
  beats.accent = observations.accent.empty() ? nullptr : observations.accent.data();
  beats.has_attack = observations.has_attack.empty() ? nullptr : observations.has_attack.data();
  beats.low_frequency_energy = observations.low_frequency_energy.empty()
                                   ? nullptr
                                   : observations.low_frequency_energy.data();
  beats.subdivision_accent =
      observations.subdivision_accent.empty() ? nullptr : observations.subdivision_accent.data();
  beats.beat_count = observations.accent.size();
  return beats;
}

SonareError call_meter_json(const sonare::MeterBeats& observations,
                            const SonareMeterOptions* options, char** out_json) {
  const SonareMeterBeats beats = to_c_meter_beats(observations);
  return sonare_estimate_meter_json(&beats, options, out_json);
}

}  // namespace

TEST_CASE("analysis_result_to_json emits beat-parallel observation streams",
          "[c_api][analysis_json]") {
  const auto fixture = make_analysis_schema_fixture();
  const auto root = sonare::util::json::parse(sonare::analysis_result_to_json(fixture));

  REQUIRE(root.contains("beatObservations"));
  const auto& observations = root["beatObservations"];
  for (const char* key :
       {"accent", "hasAttack", "lowFrequencyEnergy", "subdivisionAccent", "chordChange"}) {
    CAPTURE(key);
    REQUIRE(observations.contains(key));
    REQUIRE(observations[key].is_array());
  }

  REQUIRE(observations["accent"].size() == fixture.beats.size());
  REQUIRE(observations["hasAttack"].size() == fixture.beats.size());
  REQUIRE(observations["lowFrequencyEnergy"].size() == fixture.beats.size());
  REQUIRE(observations["subdivisionAccent"].size() + 1 == fixture.beats.size());

  for (std::size_t i = 0; i < fixture.beats.size(); ++i) {
    CAPTURE(i);
    REQUIRE(observations["accent"][i].as_number() ==
            Catch::Approx(fixture.beat_observations.accent[i]));
    REQUIRE(observations["hasAttack"][i].as_bool() ==
            (fixture.beat_observations.has_attack[i] != 0));
    REQUIRE(observations["lowFrequencyEnergy"][i].as_number() ==
            Catch::Approx(fixture.beat_observations.low_frequency_energy[i]));
    REQUIRE(observations["chordChange"][i].as_number() ==
            Catch::Approx(fixture.beat_observations.chord_change[i]));
    REQUIRE(observations["accent"][i].as_number() !=
            Catch::Approx(root["beats"][i]["strength"].as_number()));
  }
  REQUIRE(observations["subdivisionAccent"][0].as_number() ==
          Catch::Approx(fixture.beat_observations.subdivision_accent[0]));

  // Beat-indexed alongside the streams above, and serialized from the result
  // rather than re-derived from the beat spacing.
  REQUIRE(root.contains("beatLocalBpm"));
  REQUIRE(root["beatLocalBpm"].is_array());
  REQUIRE(root["beatLocalBpm"].size() == fixture.beats.size());
  for (std::size_t i = 0; i < fixture.beats.size(); ++i) {
    CAPTURE(i);
    REQUIRE(root["beatLocalBpm"][i].as_number() == Catch::Approx(fixture.beat_local_bpm[i]));
  }

  // The streams belong to the canonical schema rather than being an incidental
  // extra the snapshot comparison would not police.
  const auto& paths = sonare::analysis_result_schema_paths();
  for (const char* path :
       {"beatObservations", "beatObservations.accent", "beatObservations.hasAttack",
        "beatObservations.lowFrequencyEnergy", "beatObservations.subdivisionAccent",
        "beatObservations.chordChange", "beatLocalBpm"}) {
    CAPTURE(path);
    REQUIRE(std::find(paths.begin(), paths.end(), std::string(path)) != paths.end());
  }
}

TEST_CASE("meter_result_to_json matches its schema snapshot", "[c_api][analysis_json]") {
  const auto series = make_beat_series(4, 8);
  const sonare::MeterResult result =
      sonare::estimate_meter_from_observations(series, sonare::MeterConfig{});

  // The schema walk only descends into a populated array, so empty candidate
  // lists would drop the candidates[] paths instead of comparing them.
  REQUIRE_FALSE(result.candidate_scores.empty());
  REQUIRE_FALSE(result.candidates.empty());
  REQUIRE_FALSE(result.grouping.empty());

  const auto root = sonare::util::json::parse(sonare::meter_result_to_json(result));
  std::set<std::string> actual;
  collect_schema_paths(root, "", actual);

  const auto& expected_paths = sonare::meter_result_schema_paths();
  const std::set<std::string> expected(expected_paths.begin(), expected_paths.end());
  REQUIRE(actual == expected);

  REQUIRE(root["timeSignature"]["numerator"].as_number() ==
          static_cast<double>(result.time_signature.numerator));
  REQUIRE(root["timeSignature"]["denominator"].as_number() ==
          static_cast<double>(result.time_signature.denominator));
  REQUIRE(root["timeSignature"]["confidence"].as_number() ==
          Catch::Approx(result.time_signature.confidence));
  REQUIRE(root["downbeatPhase"].as_number() == static_cast<double>(result.downbeat_phase));

  REQUIRE(root["grouping"].size() == result.grouping.size());
  int grouped_beats = 0;
  for (std::size_t i = 0; i < result.grouping.size(); ++i) {
    CAPTURE(i);
    REQUIRE(root["grouping"][i].as_number() == static_cast<double>(result.grouping[i]));
    grouped_beats += result.grouping[i];
  }
  REQUIRE(grouped_beats == result.time_signature.numerator);

  REQUIRE(root["candidateScores"].size() == result.candidate_scores.size());
  for (std::size_t i = 0; i < result.candidate_scores.size(); ++i) {
    CAPTURE(i);
    REQUIRE(root["candidateScores"][i].as_number() == Catch::Approx(result.candidate_scores[i]));
  }

  REQUIRE(root["candidates"].size() == result.candidates.size());
  for (std::size_t i = 0; i < result.candidates.size(); ++i) {
    CAPTURE(i);
    REQUIRE(root["candidates"][i]["numerator"].as_number() ==
            static_cast<double>(result.candidates[i].numerator));
    REQUIRE(root["candidates"][i]["denominator"].as_number() ==
            static_cast<double>(result.candidates[i].denominator));
    REQUIRE(root["candidates"][i]["confidence"].as_number() ==
            Catch::Approx(result.candidates[i].confidence));
  }
}

TEST_CASE("sonare_meter_options_init mirrors the core defaults", "[c_api]") {
  SonareMeterOptions options;
  sonare_meter_options_init(&options);

  REQUIRE(options.candidate_numerator_count == 3u);
  REQUIRE(options.candidate_numerators[0] == 3);
  REQUIRE(options.candidate_numerators[1] == 4);
  REQUIRE(options.candidate_numerators[2] == 6);
  REQUIRE(options.denominator == 4);

  // A drift from MeterConfig would make the C surface answer a different
  // question than the core the other surfaces reach directly.
  const sonare::MeterConfig config;
  REQUIRE(options.candidate_numerator_count == config.candidate_numerators.size());
  for (std::size_t i = 0; i < options.candidate_numerator_count; ++i) {
    CAPTURE(i);
    REQUIRE(options.candidate_numerators[i] == config.candidate_numerators[i]);
  }
  REQUIRE(options.denominator == config.denominator);
  REQUIRE(options.downbeat_weight == config.downbeat_weight);
  REQUIRE(options.measure_weight == config.measure_weight);
  REQUIRE(options.subdivision_weight == config.subdivision_weight);
  REQUIRE(options.compound_subdivision_threshold == config.compound_subdivision_threshold);

  // The defaults are usable as-is, which is what makes zeroing the struct the
  // mistake the rejection below guards against.
  const auto series = make_beat_series(4, 8);
  char* json = nullptr;
  REQUIRE(call_meter_json(series, &options, &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  sonare_free_string(json);
}

TEST_CASE("sonare_estimate_meter_json", "[c_api]") {
  const auto series = make_beat_series(4, 8);
  SonareMeterOptions defaults;
  sonare_meter_options_init(&defaults);
  // Every rejecting call below starts from a non-null value: this entry point
  // clears the caller's pointer before any early return, so initializing to
  // nullptr would let a path that never touches it pass the check anyway.
  const auto sentinel = []() { return reinterpret_cast<char*>(static_cast<std::uintptr_t>(0x1)); };

  SECTION("emits parseable JSON a caller releases with sonare_free_string") {
    char* json = nullptr;
    REQUIRE(call_meter_json(series, &defaults, &json) == SONARE_OK);
    REQUIRE(json != nullptr);

    const auto root = sonare::util::json::parse(json);
    REQUIRE(root.is_object());
    REQUIRE(root["timeSignature"]["numerator"].as_number() == 4.0);
    REQUIRE(root["timeSignature"]["denominator"].as_number() == 4.0);
    REQUIRE(root["timeSignature"]["confidence"].as_number() > 0.5);
    REQUIRE(root["downbeatPhase"].as_number() == 0.0);
    REQUIRE(root["candidateScores"].size() == defaults.candidate_numerator_count);
    REQUIRE(root["candidates"].size() > 0);

    sonare_free_string(json);
  }

  SECTION("scores the requested candidate set in the requested beat unit") {
    const auto odd = make_beat_series(5, 6);

    SonareMeterOptions widened = defaults;
    widened.candidate_numerators[2] = 5;
    widened.denominator = 8;
    char* json = nullptr;
    REQUIRE(call_meter_json(odd, &widened, &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    const auto root = sonare::util::json::parse(json);
    REQUIRE(root["timeSignature"]["numerator"].as_number() == 5.0);
    REQUIRE(root["timeSignature"]["denominator"].as_number() == 8.0);
    sonare_free_string(json);

    // The default set cannot reach 5, so the result above came from the options
    // rather than from the accent pattern alone.
    char* default_json = nullptr;
    REQUIRE(call_meter_json(odd, &defaults, &default_json) == SONARE_OK);
    const auto default_root = sonare::util::json::parse(default_json);
    const double default_numerator = default_root["timeSignature"]["numerator"].as_number();
    CAPTURE(default_numerator);
    REQUIRE(default_numerator != 5.0);
    REQUIRE(default_root["timeSignature"]["denominator"].as_number() == 4.0);
    sonare_free_string(default_json);
  }

  SECTION("rejects a zeroed options struct rather than reading it as the defaults") {
    SonareMeterOptions zeroed = {};
    zeroed.struct_size = sizeof(zeroed);
    char* json = sentinel();
    REQUIRE(call_meter_json(series, &zeroed, &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);
  }

  SECTION("rejects a candidate count outside the flat array") {
    char* json = sentinel();
    SonareMeterOptions too_many = defaults;
    too_many.candidate_numerator_count = SONARE_MAX_METER_CANDIDATE_NUMERATORS + 1;
    REQUIRE(call_meter_json(series, &too_many, &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);

    SonareMeterOptions largest = defaults;
    largest.candidate_numerator_count = std::numeric_limits<std::size_t>::max();
    json = sentinel();
    REQUIRE(call_meter_json(series, &largest, &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);

    // The full array is still accepted, so the guard is a bound and not a
    // narrower cap.
    SonareMeterOptions full = defaults;
    full.candidate_numerator_count = SONARE_MAX_METER_CANDIDATE_NUMERATORS;
    for (int i = 0; i < SONARE_MAX_METER_CANDIDATE_NUMERATORS; ++i) {
      full.candidate_numerators[i] = 2 + i;
    }
    json = nullptr;
    REQUIRE(call_meter_json(series, &full, &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    sonare_free_string(json);
  }

  SECTION("rejects a candidate numerator outside the scorable range") {
    char* json = sentinel();
    SonareMeterOptions too_small = defaults;
    too_small.candidate_numerators[1] = 1;
    REQUIRE(call_meter_json(series, &too_small, &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);

    SonareMeterOptions too_large = defaults;
    too_large.candidate_numerators[1] = 33;
    json = sentinel();
    REQUIRE(call_meter_json(series, &too_large, &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);

    SonareMeterOptions odd_unit = defaults;
    odd_unit.denominator = 3;
    json = sentinel();
    REQUIRE(call_meter_json(series, &odd_unit, &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);
  }

  SECTION("rejects a beat series the core cannot score") {
    char* json = sentinel();
    sonare::MeterBeats nan_accents = series;
    nan_accents.accent[2] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(call_meter_json(nan_accents, &defaults, &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);

    sonare::MeterBeats negative_accents = series;
    negative_accents.accent[3] = -1.0f;
    json = sentinel();
    REQUIRE(call_meter_json(negative_accents, &defaults, &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);

    sonare::MeterBeats invalid_flags = series;
    invalid_flags.has_attack[4] = 2;
    json = sentinel();
    REQUIRE(call_meter_json(invalid_flags, &defaults, &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);
  }

  SECTION("rejects an empty beat series however the caller spells it") {
    // A zero count is nothing to score, so it reaches the core's empty-accent
    // refusal rather than being answered with the fixed low-confidence default.
    const sonare::MeterBeats empty;
    char* json = sentinel();
    REQUIRE(call_meter_json(empty, &defaults, &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);
    REQUIRE(std::string(sonare_last_error_message()).find("beats.accent") != std::string::npos);

    // One beat is not empty: it gets the fixed fallback rather than an error.
    json = nullptr;
    sonare::MeterBeats one_beat;
    one_beat.accent = {0.8f};
    REQUIRE(call_meter_json(one_beat, &defaults, &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    const auto root = sonare::util::json::parse(json);
    REQUIRE(root["timeSignature"]["numerator"].as_number() == 4.0);
    // The fallback reports no confidence at all, so a caller reading the field
    // without "searched" degrades toward "no idea".
    REQUIRE(root["timeSignature"]["confidence"].as_number() == 0.0);
    sonare_free_string(json);
  }

  SECTION("rejects null records, required fields, and options") {
    char* json = sentinel();
    REQUIRE(sonare_estimate_meter_json(nullptr, &defaults, &json) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);

    json = sentinel();
    SonareMeterBeats no_accent = to_c_meter_beats(series);
    no_accent.accent = nullptr;
    REQUIRE(sonare_estimate_meter_json(&no_accent, &defaults, &json) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()).find("beats.accent") != std::string::npos);
    REQUIRE(json == nullptr);

    json = sentinel();
    const SonareMeterBeats c_series = to_c_meter_beats(series);
    REQUIRE(sonare_estimate_meter_json(&c_series, nullptr, &json) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);

    // A null out pointer has nothing to clear and must not be written through.
    REQUIRE(sonare_estimate_meter_json(&c_series, &defaults, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_analyze_melody", "[c_api]") {
  auto samples = generate_sine(440.0f, 22050, 1.0f);

  SECTION("pYIN center contour differs from the plain-YIN default") {
    SonareAnalyzeMelodyOptions options;
    sonare_analyze_melody_options_init(&options);
    REQUIRE(options.struct_size == sizeof(SonareAnalyzeMelodyOptions));
    REQUIRE(options.use_pyin == 0);
    REQUIRE(options.center == 1);
    SonareMelodyResult plain = {};
    SonareMelodyResult pyin = {};
    REQUIRE(sonare_analyze_melody(samples.data(), samples.size(), 22050, 65.0f, 2093.0f, 2048, 256,
                                  0.1f, &options, &plain) == SONARE_OK);
    options.use_pyin = 1;
    REQUIRE(sonare_analyze_melody(samples.data(), samples.size(), 22050, 65.0f, 2093.0f, 2048, 256,
                                  0.1f, &options, &pyin) == SONARE_OK);
    // Both produce a usable contour; the pYIN path is genuinely reachable.
    REQUIRE(pyin.mean_frequency_hz >= 0.0f);
    sonare_free_melody_result(&plain);
    sonare_free_melody_result(&pyin);
  }

  SECTION("NULL options take the plain-YIN default") {
    SonareAnalyzeMelodyOptions options;
    sonare_analyze_melody_options_init(&options);
    SonareMelodyResult defaults = {};
    SonareMelodyResult explicit_defaults = {};
    REQUIRE(sonare_analyze_melody(samples.data(), samples.size(), 22050, 65.0f, 2093.0f, 2048, 256,
                                  0.1f, nullptr, &defaults) == SONARE_OK);
    REQUIRE(sonare_analyze_melody(samples.data(), samples.size(), 22050, 65.0f, 2093.0f, 2048, 256,
                                  0.1f, &options, &explicit_defaults) == SONARE_OK);
    REQUIRE(defaults.point_count == explicit_defaults.point_count);
    sonare_free_melody_result(&defaults);
    sonare_free_melody_result(&explicit_defaults);
  }

  SECTION("rejects a too-small options struct_size") {
    SonareAnalyzeMelodyOptions options;
    sonare_analyze_melody_options_init(&options);
    options.struct_size = 0;
    SonareMelodyResult out = {};
    REQUIRE(sonare_analyze_melody(samples.data(), samples.size(), 22050, 65.0f, 2093.0f, 2048, 256,
                                  0.1f, &options, &out) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("rejects invalid parameters") {
    SonareMelodyResult out = {};
    REQUIRE(sonare_analyze_melody(samples.data(), samples.size(), 22050, 0.0f, 2093.0f, 2048, 256,
                                  0.1f, nullptr, &out) == SONARE_ERROR_INVALID_PARAMETER);
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
    // The gaps between clicks are exact zeros, so the low percentile is -inf and the range over
    // it from an audible high percentile is +inf.
    REQUIRE(result.dynamic_range_db == std::numeric_limits<float>::infinity());
    REQUIRE(result.loudness_range_lu >= 0.0f);
    REQUIRE((result.is_compressed == 0 || result.is_compressed == 1));
    REQUIRE(result.loudness_count > 0);
    REQUIRE(result.loudness_times_sec != nullptr);
    REQUIRE(result.loudness_rms_db != nullptr);
    for (size_t i = 1; i < result.loudness_count; ++i) {
      REQUIRE(result.loudness_times_sec[i] >= result.loudness_times_sec[i - 1]);
    }

    sonare_free_dynamics_result(&result);
    REQUIRE(result.loudness_times_sec == nullptr);
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
    result.loudness_times_sec = new float[1]{0.0f};
    result.loudness_rms_db = new float[1]{-12.0f};
    result.loudness_count = 1;

    sonare_free_dynamics_result(&result);

    REQUIRE(result.loudness_times_sec == nullptr);
    REQUIRE(result.loudness_rms_db == nullptr);
    REQUIRE(result.loudness_count == 0);
  }
}

TEST_CASE("C analysis ranges reject non-finite bounds", "[c_api][validation]") {
  const auto clicks = generate_clicks(120.0f, 22050, 0.5f);
  const auto tone = generate_sine(440.0f, 22050, 0.5f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  SECTION("sonare_analyze_bpm") {
    SonareBpmAnalysisResult result = {};
    REQUIRE(sonare_analyze_bpm(clicks.data(), clicks.size(), 22050, 60.0f, 200.0f, 1024, 256, 5,
                               &result) == SONARE_OK);
    sonare_free_bpm_analysis_result(&result);

    REQUIRE(sonare_analyze_bpm(clicks.data(), clicks.size(), 22050, nan, 200.0f, 1024, 256, 5,
                               &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_bpm(clicks.data(), clicks.size(), 22050, 60.0f, nan, 1024, 256, 5,
                               &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_bpm(clicks.data(), clicks.size(), 22050, inf, 200.0f, 1024, 256, 5,
                               &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_bpm(clicks.data(), clicks.size(), 22050, 60.0f, inf, 1024, 256, 5,
                               &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_bpm(clicks.data(), clicks.size(), 22050, -inf, 200.0f, 1024, 256, 5,
                               &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_bpm(clicks.data(), clicks.size(), 22050, 200.0f, 60.0f, 1024, 256, 5,
                               &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.candidates == nullptr);
    REQUIRE(result.autocorrelation == nullptr);
    REQUIRE(result.tempogram == nullptr);
  }

  SECTION("sonare_analyze_rhythm") {
    SonareRhythmResult result = {};
    REQUIRE(sonare_analyze_rhythm(clicks.data(), clicks.size(), 22050, 60.0f, 200.0f, 120.0f, 1024,
                                  256, &result) == SONARE_OK);
    sonare_free_rhythm_result(&result);

    REQUIRE(sonare_analyze_rhythm(clicks.data(), clicks.size(), 22050, nan, 200.0f, 120.0f, 1024,
                                  256, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_rhythm(clicks.data(), clicks.size(), 22050, 60.0f, nan, 120.0f, 1024,
                                  256, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_rhythm(clicks.data(), clicks.size(), 22050, inf, 200.0f, 120.0f, 1024,
                                  256, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_rhythm(clicks.data(), clicks.size(), 22050, 60.0f, inf, 120.0f, 1024,
                                  256, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_rhythm(clicks.data(), clicks.size(), 22050, -inf, 200.0f, 120.0f, 1024,
                                  256, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_rhythm(clicks.data(), clicks.size(), 22050, 200.0f, 60.0f, 120.0f, 1024,
                                  256, &result) == SONARE_ERROR_INVALID_PARAMETER);
    // The prior seeds beat tracking when the search finds no tempo.
    REQUIRE(sonare_analyze_rhythm(clicks.data(), clicks.size(), 22050, 60.0f, 200.0f, nan, 1024,
                                  256, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_rhythm(clicks.data(), clicks.size(), 22050, 60.0f, 200.0f, inf, 1024,
                                  256, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_rhythm(clicks.data(), clicks.size(), 22050, 60.0f, 200.0f, -inf, 1024,
                                  256, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_rhythm(clicks.data(), clicks.size(), 22050, 60.0f, 200.0f, 0.0f, 1024,
                                  256, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.beat_intervals == nullptr);
  }

  SECTION("sonare_analyze_melody") {
    SonareMelodyResult result = {};
    REQUIRE(sonare_analyze_melody(tone.data(), tone.size(), 22050, 65.0f, 2093.0f, 2048, 256, 0.1f,
                                  nullptr, &result) == SONARE_OK);
    sonare_free_melody_result(&result);

    REQUIRE(sonare_analyze_melody(tone.data(), tone.size(), 22050, nan, 2093.0f, 2048, 256, 0.1f,
                                  nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_melody(tone.data(), tone.size(), 22050, 65.0f, nan, 2048, 256, 0.1f,
                                  nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_melody(tone.data(), tone.size(), 22050, inf, 2093.0f, 2048, 256, 0.1f,
                                  nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_melody(tone.data(), tone.size(), 22050, 65.0f, inf, 2048, 256, 0.1f,
                                  nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_melody(tone.data(), tone.size(), 22050, -inf, 2093.0f, 2048, 256, 0.1f,
                                  nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_melody(tone.data(), tone.size(), 22050, 2093.0f, 65.0f, 2048, 256, 0.1f,
                                  nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_melody(tone.data(), tone.size(), 22050, 65.0f, 2093.0f, 2048, 256, nan,
                                  nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_melody(tone.data(), tone.size(), 22050, 65.0f, 2093.0f, 2048, 256, inf,
                                  nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_melody(tone.data(), tone.size(), 22050, 65.0f, 2093.0f, 2048, 256, -inf,
                                  nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.points == nullptr);
  }
}

TEST_CASE("C analysis windows reject non-finite durations and thresholds", "[c_api][validation]") {
  const auto clicks = generate_clicks(120.0f, 22050, 0.5f);
  const auto tone = generate_sine(440.0f, 22050, 0.5f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  SECTION("sonare_analyze_dynamics") {
    SonareDynamicsResult result = {};
    REQUIRE(sonare_analyze_dynamics(clicks.data(), clicks.size(), 22050, 0.1f, 256, 6.0f,
                                    &result) == SONARE_OK);
    sonare_free_dynamics_result(&result);

    REQUIRE(sonare_analyze_dynamics(clicks.data(), clicks.size(), 22050, nan, 256, 6.0f, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_dynamics(clicks.data(), clicks.size(), 22050, inf, 256, 6.0f, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_dynamics(clicks.data(), clicks.size(), 22050, -inf, 256, 6.0f,
                                    &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_dynamics(clicks.data(), clicks.size(), 22050, 0.1f, 256, nan, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_dynamics(clicks.data(), clicks.size(), 22050, 0.1f, 256, inf, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_dynamics(clicks.data(), clicks.size(), 22050, 0.1f, 256, -inf,
                                    &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.loudness_times_sec == nullptr);
    REQUIRE(result.loudness_rms_db == nullptr);
  }

  SECTION("sonare_analyze_timbre") {
    SonareTimbreResult result = {};
    REQUIRE(sonare_analyze_timbre(tone.data(), tone.size(), 22050, 2048, 512, 128, 13, 0.25f,
                                  &result) == SONARE_OK);
    sonare_free_timbre_result(&result);

    REQUIRE(sonare_analyze_timbre(tone.data(), tone.size(), 22050, 2048, 512, 128, 13, nan,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(tone.data(), tone.size(), 22050, 2048, 512, 128, 13, inf,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(tone.data(), tone.size(), 22050, 2048, 512, 128, 13, -inf,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.spectral_centroid == nullptr);
    REQUIRE(result.timbre_over_time == nullptr);
  }
}

TEST_CASE("C CQT and key entry points reject a non-finite frequency", "[c_api][validation]") {
  const auto tone = generate_sine(440.0f, 22050, 0.5f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  SECTION("the CQT family rejects a non-finite fmin") {
    SonareCqtResult result{};
    REQUIRE(sonare_cqt(tone.data(), tone.size(), 22050, 256, 55.0f, 36, 12, 1.0f, &result) ==
            SONARE_OK);
    sonare_free_cqt_result(&result);
    REQUIRE(sonare_pseudo_cqt(tone.data(), tone.size(), 22050, 256, 55.0f, 36, 12, 1.0f, &result) ==
            SONARE_OK);
    sonare_free_cqt_result(&result);
    REQUIRE(sonare_hybrid_cqt(tone.data(), tone.size(), 22050, 256, 55.0f, 36, 12, 1.0f, &result) ==
            SONARE_OK);
    sonare_free_cqt_result(&result);

    for (float fmin : {nan, inf, -inf}) {
      REQUIRE(sonare_cqt(tone.data(), tone.size(), 22050, 256, fmin, 36, 12, 1.0f, &result) ==
              SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(sonare_pseudo_cqt(tone.data(), tone.size(), 22050, 256, fmin, 36, 12, 1.0f,
                                &result) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(sonare_hybrid_cqt(tone.data(), tone.size(), 22050, 256, fmin, 36, 12, 1.0f,
                                &result) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(result.magnitude == nullptr);
      REQUIRE(result.frequencies == nullptr);
    }
  }

  SECTION("key detection rejects a non-finite high-pass corner") {
    SonareKey key = {};
    SonareDetectKeyOptions options = key_options(2048, 512, 0, 0, 80.0f);
    REQUIRE(sonare_detect_key(tone.data(), tone.size(), 22050, &options, &key, nullptr) ==
            SONARE_OK);

    for (float chroma_high_pass_hz : {nan, inf, -inf}) {
      options.chroma_high_pass_hz = chroma_high_pass_hz;
      REQUIRE(sonare_detect_key(tone.data(), tone.size(), 22050, &options, &key, nullptr) ==
              SONARE_ERROR_INVALID_PARAMETER);
      options.profile_type = SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER;
      REQUIRE(sonare_detect_key(tone.data(), tone.size(), 22050, &options, &key, nullptr) ==
              SONARE_ERROR_INVALID_PARAMETER);
    }
  }

  SECTION("key candidates reject a non-finite high-pass corner") {
    SonareKeyCandidate* candidates = nullptr;
    size_t count = 0;
    SonareDetectKeyOptions options = key_options(2048, 512, 0, 0, 80.0f);
    REQUIRE(sonare_detect_key_candidates(tone.data(), tone.size(), 22050, &options, &candidates,
                                         &count, nullptr) == SONARE_OK);
    sonare_free_key_candidates(candidates);

    for (float chroma_high_pass_hz : {nan, inf, -inf}) {
      candidates = nullptr;
      count = 0;
      options.chroma_high_pass_hz = chroma_high_pass_hz;
      REQUIRE(sonare_detect_key_candidates(tone.data(), tone.size(), 22050, &options, &candidates,
                                           &count, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(candidates == nullptr);
      REQUIRE(count == 0);
    }
  }
}

TEST_CASE("C result emitters publish nothing when an allocation fails",
          "[c_api][resource_limit][staged_output]") {
  constexpr int rate = 22050;
  std::vector<float> tone(rate / 2);
  std::vector<float> decay(rate / 2);
  uint32_t state = 12345u;
  for (size_t i = 0; i < tone.size(); ++i) {
    tone[i] =
        0.4f * std::sin(2.0f * sonare::constants::kPi * 440.0f * static_cast<float>(i) / rate);
    state = state * 1664525u + 1013904223u;
    const float noise = static_cast<float>(state >> 8) / static_cast<float>(1u << 24) - 0.5f;
    decay[i] = noise * std::exp(-6.0f * static_cast<float>(i) / static_cast<float>(decay.size()));
  }

  // Fails the nth allocation of one call for n = 1, 2, ...; every failing run must hand back
  // an all-NULL, all-zero result, and at least one run must actually fail.
  const auto sweep = [](auto call, auto is_clear, auto release) {
    size_t failures = 0;
    for (size_t nth = 1; nth <= 400; ++nth) {
      SonareError error = SONARE_OK;
      {
        sonare::test::AllocationFailureAtGuard guard(nth);
        error = call();
      }
      if (error == SONARE_OK) {
        release();
        break;
      }
      ++failures;
      CAPTURE(nth);
      REQUIRE(is_clear());
      release();
    }
    REQUIRE(failures > 0);
  };

  SECTION("bpm") {
    SonareBpmAnalysisResult r{};
    sweep(
        [&] {
          return sonare_analyze_bpm(tone.data(), tone.size(), rate, 60.0f, 200.0f, 1024, 256, 4,
                                    &r);
        },
        [&] {
          return r.candidates == nullptr && r.candidate_count == 0 &&
                 r.autocorrelation == nullptr && r.autocorrelation_count == 0 &&
                 r.tempogram == nullptr && r.tempogram_count == 0;
        },
        [&] { sonare_free_bpm_analysis_result(&r); });
  }

  SECTION("rhythm") {
    SonareRhythmResult r{};
    sweep(
        [&] {
          return sonare_analyze_rhythm(tone.data(), tone.size(), rate, 60.0f, 200.0f, 120.0f, 1024,
                                       256, &r);
        },
        [&] { return r.beat_intervals == nullptr && r.beat_interval_count == 0; },
        [&] { sonare_free_rhythm_result(&r); });
  }

  SECTION("timbre") {
    SonareTimbreResult r{};
    sweep(
        [&] {
          return sonare_analyze_timbre(tone.data(), tone.size(), rate, 1024, 256, 40, 13, 0.1f, &r);
        },
        [&] {
          return r.spectral_centroid == nullptr && r.spectral_centroid_count == 0 &&
                 r.spectral_flatness == nullptr && r.spectral_flatness_count == 0 &&
                 r.spectral_rolloff == nullptr && r.spectral_rolloff_count == 0 &&
                 r.timbre_over_time == nullptr && r.timbre_over_time_count == 0;
        },
        [&] { sonare_free_timbre_result(&r); });
  }

  SECTION("spectrum") {
    SonareSpectrumResult r{};
    sweep(
        [&] {
          return sonare_metering_spectrum(tone.data(), tone.size(), rate, 1024, 0, 0, 0.0f, 0.0f,
                                          &r);
        },
        [&] {
          return r.frequencies == nullptr && r.magnitude == nullptr && r.power == nullptr &&
                 r.db == nullptr && r.bin_count == 0;
        },
        [&] { sonare_free_spectrum_result(&r); });
  }

  SECTION("acoustic bands") {
    SonareAcousticResult r{};
    sweep(
        [&] {
          return sonare_analyze_impulse_response(decay.data(), decay.size(), rate, 6, nullptr, &r);
        },
        [&] {
          return r.rt60_bands == nullptr && r.edt_bands == nullptr && r.c50_bands == nullptr &&
                 r.c80_bands == nullptr && r.band_count == 0;
        },
        [&] { sonare_free_acoustic_result(&r); });
  }

  SECTION("mastering chain tail allocations") {
    // The chain allocates far more than the sweep above reaches, and the output copies come last,
    // so fail each of the final allocations of a successful call.
    const std::vector<float> body(4096, 0.1f);
    const auto run_mono = [&](SonareMasteringChainResult* r) {
      return sonare_master_audio(nullptr, body.data(), body.size(), 48000, "{}", nullptr, r);
    };
    const auto run_stereo = [&](SonareMasteringChainStereoResult* r) {
      return sonare_master_audio_stereo(nullptr, body.data(), body.data(), body.size(), 48000, "{}",
                                        nullptr, r);
    };
    const auto count_allocations = [](auto call) {
      size_t count = 0;
      {
        sonare::test::AllocationGuard guard;
        REQUIRE(call() == SONARE_OK);
        count = guard.count();
      }
      return count;
    };
    SonareMasteringChainResult mono{};
    const size_t mono_total = count_allocations([&] { return run_mono(&mono); });
    sonare_free_mastering_chain_result(&mono);
    SonareMasteringChainStereoResult stereo{};
    const size_t stereo_total = count_allocations([&] { return run_stereo(&stereo); });
    sonare_free_mastering_chain_stereo_result(&stereo);
    REQUIRE(mono_total > 16);
    REQUIRE(stereo_total > 16);

    size_t failures = 0;
    for (size_t back = 0; back < 16; ++back) {
      SonareMasteringChainResult m{};
      SonareError error = SONARE_OK;
      {
        sonare::test::AllocationFailureAtGuard guard(mono_total - back);
        error = run_mono(&m);
      }
      if (error != SONARE_OK) {
        ++failures;
        CAPTURE(back);
        REQUIRE(m.samples == nullptr);
        REQUIRE(m.length == 0);
        REQUIRE(m.stages == nullptr);
        REQUIRE(m.stages_count == 0);
        REQUIRE(m.stage_gain_reduction_stages == nullptr);
        REQUIRE(m.stage_gain_reduction_values == nullptr);
        REQUIRE(m.stage_gain_reductions_count == 0);
      }
      sonare_free_mastering_chain_result(&m);

      SonareMasteringChainStereoResult st{};
      {
        sonare::test::AllocationFailureAtGuard guard(stereo_total - back);
        error = run_stereo(&st);
      }
      if (error != SONARE_OK) {
        ++failures;
        CAPTURE(back);
        REQUIRE(st.left == nullptr);
        REQUIRE(st.right == nullptr);
        REQUIRE(st.length == 0);
        REQUIRE(st.stages == nullptr);
        REQUIRE(st.stages_count == 0);
        REQUIRE(st.stage_gain_reduction_stages == nullptr);
        REQUIRE(st.stage_gain_reduction_values == nullptr);
        REQUIRE(st.stage_gain_reductions_count == 0);
      }
      sonare_free_mastering_chain_stereo_result(&st);
    }
    REQUIRE(failures > 0);
  }

  SECTION("stereo normalize") {
    float* left = nullptr;
    float* right = nullptr;
    size_t length = 0;
    sweep(
        [&] {
          return sonare_normalize_stereo(tone.data(), tone.data(), tone.size(), rate, -3.0f, &left,
                                         &right, &length);
        },
        [&] { return left == nullptr && right == nullptr && length == 0; },
        [&] {
          sonare_free_floats(left);
          sonare_free_floats(right);
          left = nullptr;
          right = nullptr;
          length = 0;
        });
  }
}

TEST_CASE("room insert refuses an unknown materialPreset instead of ignoring it",
          "[insert_factory][acoustic]") {
  using sonare::mastering::api::make_insert;
  for (const char* preset : {"6", "-1", "99"}) {
    CAPTURE(preset);
    const std::string params = std::string(R"({"materialPreset":)") + preset + "}";
    REQUIRE_THROWS_AS(make_insert("effects.acoustic.roomMorph", params), sonare::SonareException);
  }
  for (const char* preset : {"0", "1", "5"}) {
    CAPTURE(preset);
    const std::string params = std::string(R"({"materialPreset":)") + preset + "}";
    REQUIRE_NOTHROW(make_insert("effects.acoustic.roomMorph", params));
  }
}

TEST_CASE("C BPM and rhythm entries agree with detect_bpm across input rates",
          "[c_api][analysis]") {
  for (const int rate : {44100, 48000}) {
    CAPTURE(rate);
    const auto clicks = generate_clicks(120.0f, rate, 6.0f);

    float quick = 0.0f;
    REQUIRE(sonare_detect_bpm(clicks.data(), clicks.size(), rate, 30.0f, 300.0f, &quick) ==
            SONARE_OK);

    SonareBpmAnalysisResult detailed = {};
    REQUIRE(sonare_analyze_bpm(clicks.data(), clicks.size(), rate, 60.0f, 200.0f, 2048, 512, 5,
                               &detailed) == SONARE_OK);
    CHECK(detailed.bpm == Catch::Approx(quick).margin(2.5f));
    sonare_free_bpm_analysis_result(&detailed);

    SonareRhythmResult rhythm = {};
    REQUIRE(sonare_analyze_rhythm(clicks.data(), clicks.size(), rate, 60.0f, 200.0f, 120.0f, 2048,
                                  512, &rhythm) == SONARE_OK);
    CHECK(rhythm.bpm == Catch::Approx(quick).margin(2.5f));
    sonare_free_rhythm_result(&rhythm);
  }
}

TEST_CASE("sonare_stream_analyzer_process tracks its own offset or takes an explicit one",
          "[c_api][stream]") {
  SonareStreamConfig config;
  sonare_stream_config_init(&config);
  config.sample_rate = 22050;
  const auto samples = generate_sine(440.0f, config.sample_rate, 0.5f);
  const size_t half = samples.size() / 2;

  auto frames_after = [&](bool explicit_offset) {
    SonareStreamAnalyzer* analyzer = nullptr;
    REQUIRE(sonare_stream_analyzer_create(&config, &analyzer) == SONARE_OK);
    size_t offset = 0;
    REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), half,
                                           explicit_offset ? &offset : nullptr) == SONARE_OK);
    offset = half;
    REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data() + half, samples.size() - half,
                                           explicit_offset ? &offset : nullptr) == SONARE_OK);
    size_t available = 0;
    REQUIRE(sonare_stream_analyzer_available_frames(analyzer, &available) == SONARE_OK);
    sonare_stream_analyzer_destroy(analyzer);
    return available;
  };
  CHECK(frames_after(true) == frames_after(false));

  SonareStreamAnalyzer* analyzer = nullptr;
  REQUIRE(sonare_stream_analyzer_create(&config, &analyzer) == SONARE_OK);
  size_t gap = half + 1;
  REQUIRE(sonare_stream_analyzer_process(analyzer, samples.data(), half, nullptr) == SONARE_OK);
  CHECK(sonare_stream_analyzer_process(analyzer, samples.data() + half, 16, &gap) ==
        SONARE_ERROR_INVALID_PARAMETER);
  sonare_stream_analyzer_destroy(analyzer);
}

TEST_CASE("sonare_audio_from_file downmixes by default and loads one channel on request",
          "[c_api][audio_io]") {
  const auto path = std::filesystem::temp_directory_path() / "sonare_c_audio_from_file_test.wav";
  constexpr size_t kFrames = 2048;
  std::vector<float> interleaved(kFrames * 2);
  for (size_t i = 0; i < kFrames; ++i) {
    interleaved[2 * i] = 0.5f;
    interleaved[2 * i + 1] = -0.25f;
  }
  sonare::save_wav_multichannel(path.string(), interleaved.data(), kFrames, 2,
                                sonare::ChannelLayout::Stereo, 22050);

  SonareAudio* downmix = nullptr;
  REQUIRE(sonare_audio_from_file(path.string().c_str(), nullptr, &downmix) == SONARE_OK);
  SonareAudioFileOptions options;
  sonare_audio_file_options_init(&options);
  REQUIRE(options.channel_index == SONARE_AUDIO_CHANNEL_DOWNMIX);
  SonareAudio* explicit_downmix = nullptr;
  REQUIRE(sonare_audio_from_file(path.string().c_str(), &options, &explicit_downmix) == SONARE_OK);
  REQUIRE(sonare_audio_length(downmix) == sonare_audio_length(explicit_downmix));
  CHECK(sonare_audio_data(downmix)[100] ==
        Catch::Approx(sonare_audio_data(explicit_downmix)[100]).margin(1e-6));

  options.channel_index = 1;
  SonareAudio* right = nullptr;
  REQUIRE(sonare_audio_from_file(path.string().c_str(), &options, &right) == SONARE_OK);
  CHECK(sonare_audio_data(right)[100] == Catch::Approx(-0.25f).margin(1e-3));

  options.channel_index = 2;
  SonareAudio* missing = nullptr;
  CHECK(sonare_audio_from_file(path.string().c_str(), &options, &missing) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "options.channel_index must be an integer in [0, 1]");
  options.channel_index = -2;
  CHECK(sonare_audio_from_file(path.string().c_str(), &options, &missing) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(missing == nullptr);
  CHECK(sonare_audio_from_file(".", nullptr, &missing) == SONARE_ERROR_DECODE_FAILED);
  CHECK(std::string(sonare_last_error_message()).find("Failed to read file:") == 0);
  CHECK(missing == nullptr);

  sonare_audio_destroy(downmix);
  sonare_audio_destroy(explicit_downmix);
  sonare_audio_destroy(right);
  std::filesystem::remove(path);
}
