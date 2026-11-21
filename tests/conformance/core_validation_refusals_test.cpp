#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
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

  const auto run = [&](int n_fft, int hop, SonareError expected) {
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
    CHECK(run(n_fft, SONARE_STFT_HOP_AUTO, SONARE_OK) ==
          run(n_fft, static_cast<int>(item["derived_hop_length"].as_number()), SONARE_OK));
    run(n_fft, static_cast<int>(item["hop_length"].as_number()), SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error_message() == "hop_length must be an integer in [1, 2147483647]");
  }
  for (const auto& item : route["negative_hop"].as_array()) {
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");
    run(static_cast<int>(item["n_fft"].as_number()),
        static_cast<int>(item["hop_length"].as_number()), SONARE_ERROR_INVALID_PARAMETER);
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

  const auto run = [&](int n_fft, int hop, SonareError expected) {
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
    CHECK(run(n_fft, SONARE_STFT_HOP_AUTO, SONARE_OK) ==
          run(n_fft, static_cast<int>(item["derived_hop_length"].as_number()), SONARE_OK));
    run(n_fft, static_cast<int>(item["hop_length"].as_number()), SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error_message() == "hop_length must be an integer in [1, 2147483647]");
  }
  for (const auto& item : route["negative_hop"].as_array()) {
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");
    run(static_cast<int>(item["n_fft"].as_number()),
        static_cast<int>(item["hop_length"].as_number()), SONARE_ERROR_INVALID_PARAMETER);
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

  const auto run = [&](int n_fft, int hop, SonareError expected) {
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
    const std::vector<float> absent = run(n_fft, SONARE_STFT_HOP_AUTO, SONARE_OK);
    CHECK_FALSE(absent.empty());
    CHECK(absent ==
          run(n_fft, static_cast<int>(item["derived_hop_length"].as_number()), SONARE_OK));
    run(n_fft, static_cast<int>(item["hop_length"].as_number()), SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error_message() == "hop_length must be an integer in [1, 2147483647]");
  }
  for (const auto& item : route["negative_hop"].as_array()) {
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");
    run(static_cast<int>(item["n_fft"].as_number()),
        static_cast<int>(item["hop_length"].as_number()), SONARE_ERROR_INVALID_PARAMETER);
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

  const auto run = [&](int n_fft, int hop, SonareError expected) {
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
    const std::vector<float> absent = run(n_fft, SONARE_STFT_HOP_AUTO, SONARE_OK);
    CHECK_FALSE(absent.empty());
    CHECK(absent ==
          run(n_fft, static_cast<int>(item["derived_hop_length"].as_number()), SONARE_OK));
    run(n_fft, static_cast<int>(item["hop_length"].as_number()), SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error_message() == "hop_length must be an integer in [1, 2147483647]");
  }
  for (const auto& item : route["negative_hop"].as_array()) {
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");
    run(static_cast<int>(item["n_fft"].as_number()),
        static_cast<int>(item["hop_length"].as_number()), SONARE_ERROR_INVALID_PARAMETER);
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
    REQUIRE(sonare_time_stretch_ex(samples.data(), samples.size(), rate, speed, n_fft, hop, &out,
                                   &out_length) == SONARE_OK);
    sonare_free_floats(out);
    REQUIRE(sonare_pitch_shift_ex(samples.data(), samples.size(), rate, semitones, n_fft, hop, &out,
                                  &out_length) == SONARE_OK);
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

    // The C entries refuse a hop before the core sees it, in the ruled integer-domain text.
    const std::string c_message =
        item.contains("c_message") ? item["c_message"].as_string() : message;
    float* out = nullptr;
    size_t out_length = 0;
    CHECK(sonare_time_stretch_ex(samples.data(), samples.size(), rate, speed, n_fft, hop, &out,
                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(c_message));
    CHECK(sonare_pitch_shift_ex(samples.data(), samples.size(), rate, semitones, n_fft, hop, &out,
                                &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(c_message));
  }

  // A hop of 0 in a core config is absent and runs the derived hop, as SONARE_STFT_HOP_AUTO does
  // in a C entry, which refuses 0; the positional primitive takes the hop it is handed.
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
    REQUIRE(sonare_time_stretch_ex(samples.data(), samples.size(), rate, speed, n_fft,
                                   SONARE_STFT_HOP_AUTO, &out, &out_length) == SONARE_OK);
    REQUIRE(sonare_time_stretch_ex(samples.data(), samples.size(), rate, speed, n_fft, derived,
                                   &expected, &expected_length) == SONARE_OK);
    CHECK(std::vector<float>(out, out + out_length) ==
          std::vector<float>(expected, expected + expected_length));
    sonare_free_floats(out);
    sonare_free_floats(expected);
    REQUIRE(sonare_pitch_shift_ex(samples.data(), samples.size(), rate, semitones, n_fft,
                                  SONARE_STFT_HOP_AUTO, &out, &out_length) == SONARE_OK);
    REQUIRE(sonare_pitch_shift_ex(samples.data(), samples.size(), rate, semitones, n_fft, derived,
                                  &expected, &expected_length) == SONARE_OK);
    CHECK(std::vector<float>(out, out + out_length) ==
          std::vector<float>(expected, expected + expected_length));
    sonare_free_floats(out);
    sonare_free_floats(expected);

    CHECK(sonare_time_stretch_ex(samples.data(), samples.size(), rate, speed, n_fft, hop, &out,
                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error_message() == "hop_length must be an integer in [1, 2147483647]");
    CHECK(sonare_pitch_shift_ex(samples.data(), samples.size(), rate, semitones, n_fft, hop, &out,
                                &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error_message() == "hop_length must be an integer in [1, 2147483647]");
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
    REQUIRE(sonare_streaming_mastering_chain_create(nullptr, &handle) == SONARE_OK);
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
    REQUIRE(sonare_streaming_mastering_chain_create(nullptr, &handle) == SONARE_OK);
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
