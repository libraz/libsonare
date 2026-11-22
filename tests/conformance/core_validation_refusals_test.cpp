#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if defined(SONARE_WITH_VOICE_CHANGER)
#include "editing/voice_changer/streaming_retune.h"
#endif
#include "core/resample.h"
#include "effects/native_spectral_stretch.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "feature/inverse.h"
#include "feature/spectral.h"
#if defined(SONARE_WITH_MASTERING)
#include "mastering/api/chain.h"
#include "mastering/eq/equalizer.h"
#endif
#include "util/exception.h"
#include "util/json.h"

// The core function owns each check below; the C entry, the Node addon and the
// WASM embind reach it directly, so a refusal is asserted at the core and at the C
// entry here and on each binding by the matching conformance test.

namespace {

using sonare::util::json::Value;

Value load_refusal_corpus() {
  std::ifstream input("tests/conformance/core_validation_refusals.json");
  REQUIRE(input.good());
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return sonare::util::json::parse_strict(text);
}

std::vector<float> filled_matrix(const Value& input) {
  const int rows = static_cast<int>(input["n_mfcc"].as_number());
  const int cols = static_cast<int>(input["n_frames"].as_number());
  return std::vector<float>(static_cast<size_t>(rows) * cols,
                            static_cast<float>(input["fill"].as_number()));
}

std::string last_error_message() {
  const char* message = sonare_last_error_message();
  REQUIRE(message != nullptr);
  return message;
}

void require_argument_refusal(const std::function<void()>& call, const std::string& message) {
  try {
    call();
    FAIL("expected a refusal naming: " << message);
  } catch (const sonare::SonareException& e) {
    CHECK(e.code() == sonare::ErrorCode::InvalidParameter);
    CHECK_THAT(std::string(e.what()), Catch::Matchers::ContainsSubstring(message));
  }
}

}  // namespace

TEST_CASE("mfcc_to_mel refusals are owned by the core function", "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["mfcc_to_mel"];
  const Value& input = route["input"];
  const int n_mfcc = static_cast<int>(input["n_mfcc"].as_number());
  const int n_frames = static_cast<int>(input["n_frames"].as_number());
  const std::vector<float> mfcc = filled_matrix(input);

  for (const auto& item : route["accepted"].as_array()) {
    const int n_mels = static_cast<int>(item["n_mels"].as_number());
    INFO(item["id"].as_string());
    CHECK(sonare::mfcc_to_mel(mfcc.data(), n_mfcc, n_frames, n_mels).size() ==
          static_cast<size_t>(n_mels) * n_frames);
    SonareInverseResult out{};
    REQUIRE(sonare_mfcc_to_mel(mfcc.data(), mfcc.size(), n_mfcc, n_frames, n_mels, nullptr, &out) ==
            SONARE_OK);
    sonare_free_inverse_result(&out);
  }

  for (const auto& item : route["refused"].as_array()) {
    const int n_mels = static_cast<int>(item["n_mels"].as_number());
    const std::string message = item["message"].as_string();
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");

    require_argument_refusal([&] { sonare::mfcc_to_mel(mfcc.data(), n_mfcc, n_frames, n_mels); },
                             message);
    sonare::MelConfig config;
    config.n_mels = n_mels;
    require_argument_refusal(
        [&] { sonare::mfcc_to_audio(mfcc.data(), n_mfcc, n_frames, config, 1, 22050); }, message);

    SonareInverseResult out{};
    CHECK(sonare_mfcc_to_mel(mfcc.data(), mfcc.size(), n_mfcc, n_frames, n_mels, nullptr, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
    float* audio = nullptr;
    size_t audio_length = 0;
    CHECK(sonare_mfcc_to_audio(mfcc.data(), mfcc.size(), n_mfcc, n_frames, n_mels, 22050, 2048, 512,
                               0.0f, 0.0f, 1, nullptr, &audio,
                               &audio_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
  }
}

TEST_CASE("mel_to_audio refusals are owned by the core function",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["mel_to_audio"];
  const Value& input = route["input"];
  const int n_mels = static_cast<int>(input["n_mels"].as_number());
  const int n_frames = static_cast<int>(input["n_frames"].as_number());
  const std::vector<float> mel(static_cast<size_t>(n_mels) * n_frames,
                               static_cast<float>(input["fill"].as_number()));
  constexpr int kRate = 22050;
  constexpr int kFft = 2048;
  constexpr int kHop = 512;
  sonare::MelConfig config;
  config.n_mels = n_mels;

  for (const auto& item : route["accepted"].as_array()) {
    const int n_iter = static_cast<int>(item["n_iter"].as_number());
    INFO(item["id"].as_string());
    CHECK(sonare::mel_to_audio(mel.data(), n_mels, n_frames, config, n_iter, kRate).size() > 0);
    float* audio = nullptr;
    size_t audio_length = 0;
    REQUIRE(sonare_mel_to_audio(mel.data(), mel.size(), n_mels, n_frames, kRate, kFft, kHop, 0.0f,
                                0.0f, n_iter, nullptr, &audio, &audio_length) == SONARE_OK);
    sonare_free_floats(audio);
  }

  for (const auto& item : route["refused"].as_array()) {
    const int n_iter = static_cast<int>(item["n_iter"].as_number());
    const std::string message = item["message"].as_string();
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");

    require_argument_refusal(
        [&] { sonare::mel_to_audio(mel.data(), n_mels, n_frames, config, n_iter, kRate); },
        message);
    float* audio = nullptr;
    size_t audio_length = 0;
    CHECK(sonare_mel_to_audio(mel.data(), mel.size(), n_mels, n_frames, kRate, kFft, kHop, 0.0f,
                              0.0f, n_iter, nullptr, &audio,
                              &audio_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
  }

  const auto run = [&](int n_fft, size_t hop, SonareError expected) {
    float* audio = nullptr;
    size_t audio_length = 0;
    REQUIRE(sonare_mel_to_audio(mel.data(), mel.size(), n_mels, n_frames, kRate, n_fft, hop, 0.0f,
                                0.0f, 1, nullptr, &audio, &audio_length) == expected);
    std::vector<float> result(audio, audio + audio_length);
    sonare_free_floats(audio);
    return result;
  };
  for (const auto& item : route["absent_hop"].as_array()) {
    INFO(item["id"].as_string());
    const int n_fft = static_cast<int>(item["n_fft"].as_number());
    CHECK(run(n_fft, 0, SONARE_OK) ==
          run(n_fft, static_cast<size_t>(item["derived_hop_length"].as_number()), SONARE_OK));
  }
  for (const auto& item : route["negative_hop"].as_array()) {
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");
    // A negative spelling reaches the C entry as a value past the int range.
    run(static_cast<int>(item["n_fft"].as_number()),
        static_cast<size_t>(static_cast<long long>(item["hop_length"].as_number())),
        SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(),
               Catch::Matchers::ContainsSubstring(item["message"].as_string()));
  }
}

TEST_CASE("mfcc_to_audio refusals are owned by the core function",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["mfcc_to_audio"];
  const Value& input = route["input"];
  const int n_mfcc = static_cast<int>(input["n_mfcc"].as_number());
  const int n_frames = static_cast<int>(input["n_frames"].as_number());
  const int n_mels = static_cast<int>(input["n_mels"].as_number());
  const std::vector<float> mfcc(static_cast<size_t>(n_mfcc) * n_frames,
                                static_cast<float>(input["fill"].as_number()));
  constexpr int kRate = 22050;
  sonare::MelConfig config;
  config.n_mels = n_mels;

  for (const auto& item : route["accepted"].as_array()) {
    const int n_iter = static_cast<int>(item["n_iter"].as_number());
    INFO(item["id"].as_string());
    CHECK(sonare::mfcc_to_audio(mfcc.data(), n_mfcc, n_frames, config, n_iter, kRate).size() > 0);
    float* audio = nullptr;
    size_t audio_length = 0;
    REQUIRE(sonare_mfcc_to_audio(mfcc.data(), mfcc.size(), n_mfcc, n_frames, n_mels, kRate, 2048,
                                 512, 0.0f, 0.0f, n_iter, nullptr, &audio,
                                 &audio_length) == SONARE_OK);
    sonare_free_floats(audio);
  }

  for (const auto& item : route["refused"].as_array()) {
    const int n_iter = static_cast<int>(item["n_iter"].as_number());
    const std::string message = item["message"].as_string();
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");

    require_argument_refusal(
        [&] { sonare::mfcc_to_audio(mfcc.data(), n_mfcc, n_frames, config, n_iter, kRate); },
        message);
    float* audio = nullptr;
    size_t audio_length = 0;
    CHECK(sonare_mfcc_to_audio(mfcc.data(), mfcc.size(), n_mfcc, n_frames, n_mels, kRate, 2048, 512,
                               0.0f, 0.0f, n_iter, nullptr, &audio,
                               &audio_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
  }

  const auto run = [&](int n_fft, size_t hop, SonareError expected) {
    float* audio = nullptr;
    size_t audio_length = 0;
    REQUIRE(sonare_mfcc_to_audio(mfcc.data(), mfcc.size(), n_mfcc, n_frames, n_mels, kRate, n_fft,
                                 hop, 0.0f, 0.0f, 1, nullptr, &audio, &audio_length) == expected);
    std::vector<float> result(audio, audio + audio_length);
    sonare_free_floats(audio);
    return result;
  };
  for (const auto& item : route["absent_hop"].as_array()) {
    INFO(item["id"].as_string());
    const int n_fft = static_cast<int>(item["n_fft"].as_number());
    CHECK(run(n_fft, 0, SONARE_OK) ==
          run(n_fft, static_cast<size_t>(item["derived_hop_length"].as_number()), SONARE_OK));
  }
  for (const auto& item : route["negative_hop"].as_array()) {
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");
    // A negative spelling reaches the C entry as a value past the int range.
    run(static_cast<int>(item["n_fft"].as_number()),
        static_cast<size_t>(static_cast<long long>(item["hop_length"].as_number())),
        SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(),
               Catch::Matchers::ContainsSubstring(item["message"].as_string()));
  }
}

TEST_CASE("griffin_lim derives an absent hop and names a negative one",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["griffin_lim"];
  const Value& input = route["input"];
  const int rate = static_cast<int>(input["sample_rate"].as_number());
  const int n_frames = static_cast<int>(input["n_frames"].as_number());
  const float fill = static_cast<float>(input["fill"].as_number());

  const auto run = [&](int n_fft, size_t hop, SonareError expected) {
    const int n_bins = n_fft / 2 + 1;
    const std::vector<float> magnitude(static_cast<size_t>(n_bins) * n_frames, fill);
    float* audio = nullptr;
    size_t audio_length = 0;
    REQUIRE(sonare_griffin_lim(magnitude.data(), magnitude.size(), n_bins, n_frames, n_fft, hop,
                               rate, 1, 0.0f, &audio, &audio_length) == expected);
    std::vector<float> result(audio, audio + audio_length);
    sonare_free_floats(audio);
    return result;
  };
  for (const auto& item : route["absent_hop"].as_array()) {
    INFO(item["id"].as_string());
    const int n_fft = static_cast<int>(item["n_fft"].as_number());
    const std::vector<float> absent = run(n_fft, 0, SONARE_OK);
    CHECK_FALSE(absent.empty());
    CHECK(absent ==
          run(n_fft, static_cast<size_t>(item["derived_hop_length"].as_number()), SONARE_OK));
  }
  for (const auto& item : route["negative_hop"].as_array()) {
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");
    // A negative spelling reaches the C entry as a value past the int range.
    run(static_cast<int>(item["n_fft"].as_number()),
        static_cast<size_t>(static_cast<long long>(item["hop_length"].as_number())),
        SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(),
               Catch::Matchers::ContainsSubstring(item["message"].as_string()));
  }
}

TEST_CASE("phase_vocoder derives an absent hop and names a negative one",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["phase_vocoder"];
  const Value& input = route["input"];
  const int rate = static_cast<int>(input["sample_rate"].as_number());
  const std::vector<float> samples(static_cast<size_t>(input["length"].as_number()),
                                   static_cast<float>(input["fill"].as_number()));
  const float speed = static_cast<float>(input["speed_ratio"].as_number());

  const auto run = [&](int n_fft, size_t hop, SonareError expected) {
    float* audio = nullptr;
    size_t audio_length = 0;
    REQUIRE(sonare_phase_vocoder(samples.data(), samples.size(), rate, speed, n_fft, hop, &audio,
                                 &audio_length) == expected);
    std::vector<float> result(audio, audio + audio_length);
    sonare_free_floats(audio);
    return result;
  };
  for (const auto& item : route["absent_hop"].as_array()) {
    INFO(item["id"].as_string());
    const int n_fft = static_cast<int>(item["n_fft"].as_number());
    const std::vector<float> absent = run(n_fft, 0, SONARE_OK);
    CHECK_FALSE(absent.empty());
    CHECK(absent ==
          run(n_fft, static_cast<size_t>(item["derived_hop_length"].as_number()), SONARE_OK));
  }
  for (const auto& item : route["negative_hop"].as_array()) {
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");
    // A negative spelling reaches the C entry as a value past the int range.
    run(static_cast<int>(item["n_fft"].as_number()),
        static_cast<size_t>(static_cast<long long>(item["hop_length"].as_number())),
        SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(),
               Catch::Matchers::ContainsSubstring(item["message"].as_string()));
  }
}

TEST_CASE("native spectral stretch geometry refusals are owned by the core function",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["native_spectral_stretch"];
  const Value& input = route["input"];
  const int rate = static_cast<int>(input["sample_rate"].as_number());
  const std::vector<float> samples(static_cast<size_t>(input["length"].as_number()),
                                   static_cast<float>(input["fill"].as_number()));
  const sonare::Audio audio = sonare::Audio::from_buffer(samples.data(), samples.size(), rate);
  const float speed = static_cast<float>(input["speed_ratio"].as_number());
  const float semitones = static_cast<float>(input["pitch_semitones"].as_number());

  for (const auto& item : route["accepted"].as_array()) {
    const int n_fft = static_cast<int>(item["n_fft"].as_number());
    const int hop = static_cast<int>(item["hop_length"].as_number());
    INFO(item["id"].as_string());
    CHECK(!sonare::native_spectral_time_stretch(audio, speed, n_fft, hop).empty());
    sonare::PitchShiftConfig shift;
    shift.n_fft = n_fft;
    shift.hop_length = hop;
    shift.backend = sonare::StretchBackend::NativeSpectral;
    CHECK(!sonare::pitch_shift(audio, semitones, shift).empty());
    float* out = nullptr;
    size_t out_length = 0;
    SonareTimeStretchOptions stretch_options;
    sonare_time_stretch_options_init(&stretch_options);
    stretch_options.n_fft = n_fft;
    stretch_options.hop_length = static_cast<size_t>(hop);
    SonarePitchShiftOptions shift_options;
    sonare_pitch_shift_options_init(&shift_options);
    shift_options.n_fft = n_fft;
    shift_options.hop_length = static_cast<size_t>(hop);
    REQUIRE(sonare_time_stretch(samples.data(), samples.size(), rate, speed, &stretch_options, &out,
                                &out_length) == SONARE_OK);
    sonare_free_floats(out);
    REQUIRE(sonare_pitch_shift(samples.data(), samples.size(), rate, semitones, &shift_options,
                               &out, &out_length) == SONARE_OK);
    sonare_free_floats(out);
  }

  for (const auto& item : route["refused"].as_array()) {
    const int n_fft = static_cast<int>(item["n_fft"].as_number());
    const int hop = static_cast<int>(item["hop_length"].as_number());
    const std::string message = item["message"].as_string();
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");

    require_argument_refusal(
        [&] { sonare::native_spectral_time_stretch(audio, speed, n_fft, hop); }, message);
    sonare::TimeStretchConfig stretch;
    stretch.n_fft = n_fft;
    stretch.hop_length = hop;
    stretch.backend = sonare::StretchBackend::NativeSpectral;
    require_argument_refusal([&] { sonare::time_stretch(audio, speed, stretch); }, message);
    sonare::PitchShiftConfig shift;
    shift.n_fft = n_fft;
    shift.hop_length = hop;
    shift.backend = sonare::StretchBackend::NativeSpectral;
    require_argument_refusal([&] { sonare::pitch_shift(audio, semitones, shift); }, message);

    // The C options carry the hop unsigned, so a negative hop is not expressible there.
    if (hop < 0) continue;
    SonareTimeStretchOptions stretch_options;
    sonare_time_stretch_options_init(&stretch_options);
    stretch_options.n_fft = n_fft;
    stretch_options.hop_length = static_cast<size_t>(hop);
    SonarePitchShiftOptions shift_options;
    sonare_pitch_shift_options_init(&shift_options);
    shift_options.n_fft = n_fft;
    shift_options.hop_length = static_cast<size_t>(hop);
    float* out = nullptr;
    size_t out_length = 0;
    CHECK(sonare_time_stretch(samples.data(), samples.size(), rate, speed, &stretch_options, &out,
                              &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
    CHECK(sonare_pitch_shift(samples.data(), samples.size(), rate, semitones, &shift_options, &out,
                             &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
  }

  // A hop of 0 is absent and runs the derived hop in a core config and in a C entry alike; the
  // positional primitive takes the hop it is handed.
  for (const auto& item : route["absent_hop"].as_array()) {
    const int n_fft = static_cast<int>(item["n_fft"].as_number());
    const int hop = static_cast<int>(item["hop_length"].as_number());
    const int derived = static_cast<int>(item["derived_hop_length"].as_number());
    INFO(item["id"].as_string());
    require_argument_refusal(
        [&] { sonare::native_spectral_time_stretch(audio, speed, n_fft, hop); },
        item["primitive_message"].as_string());

    sonare::TimeStretchConfig absent;
    absent.n_fft = n_fft;
    absent.hop_length = hop;
    absent.backend = sonare::StretchBackend::NativeSpectral;
    sonare::TimeStretchConfig given = absent;
    given.hop_length = derived;
    const sonare::Audio a = sonare::time_stretch(audio, speed, absent);
    const sonare::Audio g = sonare::time_stretch(audio, speed, given);
    CHECK(std::vector<float>(a.begin(), a.end()) == std::vector<float>(g.begin(), g.end()));

    float* out = nullptr;
    size_t out_length = 0;
    float* expected = nullptr;
    size_t expected_length = 0;
    SonareTimeStretchOptions auto_stretch;
    sonare_time_stretch_options_init(&auto_stretch);
    auto_stretch.n_fft = n_fft;
    auto_stretch.hop_length = 0;
    SonareTimeStretchOptions derived_stretch = auto_stretch;
    derived_stretch.hop_length = static_cast<size_t>(derived);
    SonarePitchShiftOptions auto_shift;
    sonare_pitch_shift_options_init(&auto_shift);
    auto_shift.n_fft = n_fft;
    auto_shift.hop_length = 0;
    SonarePitchShiftOptions derived_shift = auto_shift;
    derived_shift.hop_length = static_cast<size_t>(derived);
    REQUIRE(sonare_time_stretch(samples.data(), samples.size(), rate, speed, &auto_stretch, &out,
                                &out_length) == SONARE_OK);
    REQUIRE(sonare_time_stretch(samples.data(), samples.size(), rate, speed, &derived_stretch,
                                &expected, &expected_length) == SONARE_OK);
    CHECK(std::vector<float>(out, out + out_length) ==
          std::vector<float>(expected, expected + expected_length));
    sonare_free_floats(out);
    sonare_free_floats(expected);
    REQUIRE(sonare_pitch_shift(samples.data(), samples.size(), rate, semitones, &auto_shift, &out,
                               &out_length) == SONARE_OK);
    REQUIRE(sonare_pitch_shift(samples.data(), samples.size(), rate, semitones, &derived_shift,
                               &expected, &expected_length) == SONARE_OK);
    CHECK(std::vector<float>(out, out + out_length) ==
          std::vector<float>(expected, expected + expected_length));
    sonare_free_floats(out);
    sonare_free_floats(expected);
  }
}

#if defined(SONARE_WITH_MASTERING)
TEST_CASE("streaming mastering chain prepare refusals are owned by the core function",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["streaming_mastering_chain_prepare"];
  const int channels = static_cast<int>(route["input"]["num_channels"].as_number());

  for (const auto& item : route["accepted"].as_array()) {
    const double rate = item["sample_rate"].as_number();
    const int block = static_cast<int>(item["max_block_size"].as_number());
    INFO(item["id"].as_string());
    sonare::mastering::api::StreamingMasteringChain core(
        (sonare::mastering::api::MasteringChainConfig()));
    CHECK_NOTHROW(core.prepare(rate, block, channels));
    SonareStreamingMasteringChain* handle = nullptr;
    REQUIRE(sonare_streaming_mastering_chain_create(nullptr, nullptr, &handle) == SONARE_OK);
    CHECK(sonare_streaming_mastering_chain_prepare(handle, static_cast<int>(rate), block,
                                                   channels) == SONARE_OK);
    sonare_streaming_mastering_chain_destroy(handle);
  }

  for (const auto& item : route["refused"].as_array()) {
    const double rate = item["sample_rate"].as_number();
    const int block = static_cast<int>(item["max_block_size"].as_number());
    const std::string message = item["message"].as_string();
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");

    sonare::mastering::api::StreamingMasteringChain core(
        (sonare::mastering::api::MasteringChainConfig()));
    require_argument_refusal([&] { core.prepare(rate, block, channels); }, message);

    SonareStreamingMasteringChain* handle = nullptr;
    REQUIRE(sonare_streaming_mastering_chain_create(nullptr, nullptr, &handle) == SONARE_OK);
    CHECK(sonare_streaming_mastering_chain_prepare(handle, static_cast<int>(rate), block,
                                                   channels) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
    sonare_streaming_mastering_chain_destroy(handle);
  }
}

TEST_CASE("equalizer prepare refusals are owned by the core function",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["equalizer_prepare"];

  for (const auto& item : route["accepted"].as_array()) {
    const double rate = item["sample_rate"].as_number();
    const int block = static_cast<int>(item["max_block_size"].as_number());
    INFO(item["id"].as_string());
    sonare::mastering::eq::EqualizerProcessor core;
    CHECK_NOTHROW(core.prepare(rate, block));
    SonareEq* handle = nullptr;
    CHECK(sonare_eq_create(static_cast<int>(rate), block, &handle) == SONARE_OK);
    sonare_eq_destroy(handle);
  }

  for (const auto& item : route["refused"].as_array()) {
    const double rate = item["sample_rate"].as_number();
    const int block = static_cast<int>(item["max_block_size"].as_number());
    const std::string message = item["message"].as_string();
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");

    sonare::mastering::eq::EqualizerProcessor core;
    require_argument_refusal([&] { core.prepare(rate, block); }, message);

    SonareEq* handle = nullptr;
    CHECK(sonare_eq_create(static_cast<int>(rate), block, &handle) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(handle == nullptr);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
  }
}
#endif  // defined(SONARE_WITH_MASTERING)

#if defined(SONARE_WITH_VOICE_CHANGER)
namespace {

float corpus_float(const Value& value) {
  if (value.is_string()) {
    return value.as_string() == "nan" ? std::numeric_limits<float>::quiet_NaN()
                                      : std::numeric_limits<float>::infinity();
  }
  return static_cast<float>(value.as_number());
}

sonare::editing::voice_changer::StreamingRetuneConfig retune_config(const Value& item) {
  sonare::editing::voice_changer::StreamingRetuneConfig config;
  config.semitones = corpus_float(item["semitones"]);
  config.mix = corpus_float(item["mix"]);
  config.grain_size = static_cast<int>(item["grain_size"].as_number());
  return config;
}

}  // namespace

TEST_CASE("streaming retune config refusals are owned by the core and named by each C entry",
          "[conformance][core_validation]") {
  using sonare::editing::voice_changer::StreamingRetune;
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["streaming_retune_config"];

  for (const auto& item : route["accepted"].as_array()) {
    INFO(item["id"].as_string());
    const auto config = retune_config(item);
    StreamingRetune core(config);
    // An accepted request is kept as written, never moved onto a nearby value.
    CHECK(core.config().semitones == config.semitones);
    CHECK(core.config().mix == config.mix);
    CHECK(core.config().grain_size == config.grain_size);
    CHECK_NOTHROW(core.set_config(config));
    SonareStreamingRetune* handle = nullptr;
    REQUIRE(sonare_streaming_retune_create(config.semitones, config.mix, config.grain_size,
                                           &handle) == SONARE_OK);
    CHECK(sonare_streaming_retune_set_config(handle, config.semitones, config.mix,
                                             config.grain_size) == SONARE_OK);
    if (config.grain_size != 0) {
      REQUIRE(sonare_streaming_retune_prepare(handle, 48000.0, 512) == SONARE_OK);
      int latency = 0;
      REQUIRE(sonare_streaming_retune_latency_samples(handle, &latency) == SONARE_OK);
      CHECK(latency == config.grain_size);
    }
    sonare_streaming_retune_destroy(handle);
  }

  for (const auto& item : route["refused"].as_array()) {
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");
    const auto config = retune_config(item);
    const std::string field = item["field"].as_string();
    const std::string predicate = item["predicate"].as_string();
    const std::string c_name = field == "semitones"    ? "pitch_semitones"
                               : field == "grain_size" ? "grain_size_samples"
                                                       : field;

    require_argument_refusal([&] { StreamingRetune core(config); },
                             "StreamingRetune: " + field + " " + predicate);
    StreamingRetune live;
    require_argument_refusal([&] { live.set_config(config); },
                             "StreamingRetune: " + field + " " + predicate);
    CHECK(live.config().semitones == 0.0f);

    SonareStreamingRetune* handle = nullptr;
    CHECK(sonare_streaming_retune_create(config.semitones, config.mix, config.grain_size,
                                         &handle) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(handle == nullptr);
    CHECK(last_error_message() == "streaming_retune_create: " + c_name + " " + predicate);

    REQUIRE(sonare_streaming_retune_create(0.0f, 1.0f, 0, &handle) == SONARE_OK);
    CHECK(sonare_streaming_retune_set_config(handle, config.semitones, config.mix,
                                             config.grain_size) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error_message() == "streaming_retune_set_config: " + c_name + " " + predicate);
    sonare_streaming_retune_destroy(handle);
  }
}

TEST_CASE("streaming retune prepare refusals are owned by the core function",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["streaming_retune_prepare"];

  for (const auto& item : route["accepted"].as_array()) {
    const double rate = item["sample_rate"].as_number();
    const int block = static_cast<int>(item["max_block_size"].as_number());
    INFO(item["id"].as_string());
    sonare::editing::voice_changer::StreamingRetune core;
    CHECK_NOTHROW(core.prepare(rate, block));
    SonareStreamingRetune* handle = nullptr;
    REQUIRE(sonare_streaming_retune_create(0.0f, 1.0f, 0, &handle) == SONARE_OK);
    CHECK(sonare_streaming_retune_prepare(handle, static_cast<int>(rate), block) == SONARE_OK);
    sonare_streaming_retune_destroy(handle);
  }

  for (const auto& item : route["refused"].as_array()) {
    const double rate = item["sample_rate"].as_number();
    const int block = static_cast<int>(item["max_block_size"].as_number());
    const std::string message = item["message"].as_string();
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");

    sonare::editing::voice_changer::StreamingRetune core;
    require_argument_refusal([&] { core.prepare(rate, block); }, message);

    SonareStreamingRetune* handle = nullptr;
    REQUIRE(sonare_streaming_retune_create(0.0f, 1.0f, 0, &handle) == SONARE_OK);
    CHECK(sonare_streaming_retune_prepare(handle, static_cast<int>(rate), block) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
    sonare_streaming_retune_destroy(handle);
  }
}
#endif  // defined(SONARE_WITH_VOICE_CHANGER)

namespace {

std::vector<float> resample_input(const Value& route, const Value& item) {
  const Value& input = route["input"];
  std::vector<float> samples(static_cast<size_t>(input["length"].as_number()),
                             static_cast<float>(input["fill"].as_number()));
  if (item.contains("nan_at")) {
    samples[static_cast<size_t>(item["nan_at"].as_number())] =
        std::numeric_limits<float>::quiet_NaN();
  }
  return samples;
}

}  // namespace

TEST_CASE("resample refusals are owned by the core function", "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["resample"];

  for (const auto& item : route["accepted"].as_array()) {
    const int src_sr = static_cast<int>(item["src_sr"].as_number());
    const int target_sr = static_cast<int>(item["target_sr"].as_number());
    const std::vector<float> samples = resample_input(route, item);
    INFO(item["id"].as_string());
    CHECK_FALSE(
        sonare::resample_checked(samples.data(), samples.size(), src_sr, target_sr).empty());
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_resample(samples.data(), samples.size(), src_sr, target_sr, &out, &out_length) ==
            SONARE_OK);
    CHECK(out_length > 0);
    sonare_free_floats(out);
  }

  for (const auto& item : route["refused"].as_array()) {
    const int src_sr = static_cast<int>(item["src_sr"].as_number());
    const int target_sr = static_cast<int>(item["target_sr"].as_number());
    const std::string message = item["message"].as_string();
    const std::vector<float> samples = resample_input(route, item);
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");

    require_argument_refusal(
        [&] { sonare::resample_checked(samples.data(), samples.size(), src_sr, target_sr); },
        message);

    float* out = nullptr;
    size_t out_length = 0;
    CHECK(sonare_resample(samples.data(), samples.size(), src_sr, target_sr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
  }
}

TEST_CASE("zero_crossings refusals are owned by the core function",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["zero_crossings"];

  for (const auto& item : route["accepted"].as_array()) {
    const std::vector<float> samples = resample_input(route, item);
    INFO(item["id"].as_string());
    CHECK_FALSE(sonare::zero_crossings(samples.data(), samples.size()).empty());
    int* out = nullptr;
    size_t out_count = 0;
    REQUIRE(sonare_zero_crossings(samples.data(), samples.size(), 1e-10f, 0, 1, 1, &out,
                                  &out_count) == SONARE_OK);
    CHECK(out_count > 0);
    sonare_free_ints(out);
  }

  for (const auto& item : route["refused"].as_array()) {
    const std::string message = item["message"].as_string();
    const std::vector<float> samples = resample_input(route, item);
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");

    require_argument_refusal([&] { sonare::zero_crossings(samples.data(), samples.size()); },
                             message);

    // The C entry names its own parameter where the core says y.
    const std::string c_message =
        item.contains("c_message") ? item["c_message"].as_string() : message;
    int* out = nullptr;
    size_t out_count = 0;
    CHECK(sonare_zero_crossings(samples.data(), samples.size(), 1e-10f, 0, 1, 1, &out,
                                &out_count) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(c_message));
  }
}

TEST_CASE("the resample engine still serves internal rates the checked entry refuses",
          "[conformance][core_validation]") {
  const std::vector<float> samples(4800, 0.25f);
  CHECK_FALSE(sonare::resample(samples.data(), samples.size(), 48000, 768000).empty());
  CHECK_THROWS_AS(sonare::resample_checked(samples.data(), samples.size(), 48000, 768000),
                  sonare::SonareException);
}

TEST_CASE("a mix-scene document is refused by the core reader through every C loader",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["mix_scene_document"];

  // Each loader returns the refusal code of one load, with sonare_last_error_message set.
  std::vector<std::pair<std::string, std::function<SonareError(const std::string&)>>> loaders;
  loaders.emplace_back("mixer_from_json", [](const std::string& json) {
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), 48000, 0);
    if (mixer == nullptr) return sonare_last_error_code();
    sonare_mixer_destroy(mixer);
    return SONARE_OK;
  });
#if defined(SONARE_WITH_ARRANGEMENT)
  loaders.emplace_back("project_set_mixer_scene", [](const std::string& json) {
    SonareProject* project = nullptr;
    REQUIRE(sonare_project_create(&project) == SONARE_OK);
    const SonareError rc = sonare_project_set_mixer_scene_json(project, json.c_str());
    // A release function leaves the thread's last error in place.
    sonare_project_destroy(project);
    return rc;
  });
#endif

  for (const auto& [name, load] : loaders) {
    INFO(name);
    for (const auto& item : route["accepted"].as_array()) {
      INFO(item["id"].as_string());
      CHECK(load(sonare::util::json::dump(item["scene"])) == SONARE_OK);
    }
    for (const auto& item : route["refused"].as_array()) {
      INFO(item["id"].as_string());
      REQUIRE(item["cause"].as_string() == "format");
      REQUIRE(item["code"].as_string() == "InvalidFormat");
      CHECK(load(sonare::util::json::dump(item["scene"])) == SONARE_ERROR_INVALID_FORMAT);
      CHECK_THAT(last_error_message(),
                 Catch::Matchers::ContainsSubstring(item["message"].as_string()));
    }
  }
}

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

constexpr int kChainMode = 2;

SonareError set_project_part_rig(const std::string& inserts) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  const SonareError rc = sonare_project_set_part_rig(project, 1, 0, kChainMode, inserts.c_str());
  sonare_project_destroy(project);
  return rc;
}

SonareError set_engine_part_rig(const std::string& inserts) {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  const SonareError rc = sonare_engine_set_part_rig(engine, 1, 0, kChainMode, inserts.c_str());
  sonare_engine_destroy(engine);
  return rc;
}

}  // namespace

TEST_CASE("part-rig inserts are a call's argument on both setters and a document inside a project",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["part_rig_inserts"];
  const std::vector<std::pair<std::string, std::function<SonareError(const std::string&)>>>
      setters = {{"project_set_part_rig", set_project_part_rig},
                 {"engine_set_part_rig", set_engine_part_rig}};

#if defined(SONARE_WITH_MASTERING)
  for (const auto& item : route["accepted"].as_array()) {
    INFO(item["id"].as_string());
    CHECK(set_project_part_rig(sonare::util::json::dump(item["inserts"])) == SONARE_OK);
  }
  for (const auto& [name, set] : setters) {
    INFO(name);
    for (const auto& item : route["refused"].as_array()) {
      INFO(item["id"].as_string());
      REQUIRE(item["code"].as_string() == "InvalidParameter");
      CHECK(set(sonare::util::json::dump(item["inserts"])) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK_THAT(last_error_message(),
                 Catch::Matchers::ContainsSubstring(item["message"].as_string()));
    }
  }
#endif

  SonareProject* empty = nullptr;
  REQUIRE(sonare_project_create(&empty) == SONARE_OK);
  char* serialized = nullptr;
  REQUIRE(sonare_project_serialize(empty, &serialized, nullptr) == SONARE_OK);
  Value document = sonare::util::json::parse_strict(serialized);
  sonare_free_string(serialized);
  sonare_project_destroy(empty);
  for (const auto& item : route["project_document"].as_array()) {
    INFO(item["id"].as_string());
    REQUIRE(item["code"].as_string() == "InvalidFormat");
    sonare::util::json::Object rig = {
        {"destinationId", 1.0}, {"part", 0.0}, {"mode", "chain"}, {"inserts", item["inserts"]}};
    document.as_object()["partRigs"] = sonare::util::json::Array{Value(std::move(rig))};
    const std::string text = sonare::util::json::dump(document);
    SonareProject* loaded = nullptr;
    char* diag = nullptr;
    CHECK(sonare_project_deserialize(text.c_str(), text.size(), &loaded, &diag) ==
          SONARE_ERROR_INVALID_FORMAT);
    // A refused load reports its reason through the diagnostics string.
    REQUIRE(diag != nullptr);
    CHECK_THAT(std::string(diag), Catch::Matchers::ContainsSubstring(item["message"].as_string()));
    sonare_free_string(diag);
    if (loaded != nullptr) sonare_project_destroy(loaded);
  }
}

TEST_CASE("a MIDI-FX config is a call's argument on every entry that takes one",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["midi_fx_config"];

  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);

  const std::vector<std::pair<std::string, std::function<SonareError(const std::string&)>>>
      setters = {
          {"engine_set_midi_fx",
           [&](const std::string& json) {
             return sonare_engine_set_midi_fx(engine, 5, json.c_str());
           }},
          {"project_bake_midi_fx",
           [&](const std::string& json) {
             return sonare_project_bake_midi_fx(project, clip, json.c_str(), nullptr, 0, nullptr);
           }},
          {"project_preview_midi_fx_count",
           [&](const std::string& json) {
             size_t count = 0;
             return sonare_project_preview_midi_fx_count(project, clip, json.c_str(), &count);
           }},
      };

  for (const auto& [name, set] : setters) {
    INFO(name);
    for (const auto& item : route["accepted"].as_array()) {
      INFO(item["id"].as_string());
      CHECK(set(sonare::util::json::dump(item["config"])) == SONARE_OK);
    }
    for (const auto& item : route["refused"].as_array()) {
      INFO(item["id"].as_string());
      REQUIRE(item["code"].as_string() == "InvalidParameter");
      CHECK(set(sonare::util::json::dump(item["config"])) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK_THAT(last_error_message(),
                 Catch::Matchers::ContainsSubstring(item["message"].as_string()));
    }
  }

  sonare_project_destroy(project);
  sonare_engine_destroy(engine);
}
#endif  // defined(SONARE_WITH_ARRANGEMENT)
