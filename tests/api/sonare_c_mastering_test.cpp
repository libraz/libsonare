/// @file sonare_c_mastering_test.cpp
/// @brief Mastering C API tests.

#include "c_api/eq_band_json.h"
#include "c_api/sonare_c_mastering_helpers.h"
#include "core/audio.h"
#include "mastering/api/audio_utils.h"
#include "mastering/api/named_processor.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "sonare_c_test_helpers.h"
#include "support/schema_paths.h"
#include "util/db.h"
#include "util/json.h"

#ifdef SONARE_WITH_MASTERING
TEST_CASE("sonare_mastering_process", "[c_api][mastering]") {
  SECTION("streaming EQ handle processes JSON bands and exposes spectrum") {
    SonareEq* eq = sonare_eq_create(48000.0, 512);
    REQUIRE(eq != nullptr);
    REQUIRE(sonare_eq_set_band(eq, 0,
                               "{\"type\":\"Peak\",\"frequencyHz\":1000,\"gainDb\":9,"
                               "\"q\":1,\"enabled\":true,\"coeffMode\":\"Vicanek\","
                               "\"proportionalQ\":true}") == SONARE_OK);
    // A syntactically malformed document is INVALID_FORMAT, matching every
    // other JSON-accepting C-ABI entry point; a well-formed document with a bad
    // field stays INVALID_PARAMETER.
    REQUIRE(sonare_eq_set_band(eq, 0, "not json") == SONARE_ERROR_INVALID_FORMAT);
    REQUIRE(sonare_eq_set_band(eq, 0, "{\"type\":\"Peak\",\"enabled\":truish}") ==
            SONARE_ERROR_INVALID_FORMAT);
    REQUIRE(sonare_eq_set_band(eq, 0, "{\"type\":\"Peak\" \"enabled\":true}") ==
            SONARE_ERROR_INVALID_FORMAT);
    REQUIRE(sonare_eq_set_band(eq, 0, "{\"type\":\"Peak\",\"type\":\"Notch\"}") ==
            SONARE_ERROR_INVALID_FORMAT);
    REQUIRE(sonare_eq_set_band(eq, 0, "{\"type\":\"Unknown\",\"enabled\":true}") ==
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
    // The offline helper returns time-aligned audio (it compensates the internal
    // true-peak limiter's look-ahead itself), so it reports zero latency.
    REQUIRE(result.latency_samples == 0);
    REQUIRE(result.output_lufs > -18.2f);
    REQUIRE(result.output_lufs < -17.8f);
    REQUIRE(result.loudness_target_limited == 0);

    sonare_free_mastering_result(&result);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);

    config.target_lufs = -2.0f;
    REQUIRE(sonare_mastering_process(samples.data(), samples.size(), 22050, &config, &result) ==
            SONARE_OK);
    REQUIRE(result.loudness_target_limited == 1);
    sonare_free_mastering_result(&result);
  }

  SECTION("accepts release_ms / apply_gain_at_input_rate and keeps zero-init behavior") {
    // These appended fields complete the simple helper's coverage of the
    // maximizer config (release_ms / apply_gain_at_input_rate already exist on
    // the chain and named-processor paths). The single-pass helper pre-clamps the
    // static gain to the ceiling, so the limiter rarely reduces gain and the
    // knobs are near-inert here; the contract verified is that they are accepted,
    // produce valid output, and that a zero-initialized config still reproduces
    // the library default (50 ms release, input-rate gain off).
    auto samples = generate_sine(440.0f, 22050, 0.5f);
    for (auto& sample : samples) sample *= 0.2f;

    auto run = [&](float release_ms, int apply_gain_at_input_rate) {
      SonareMasteringConfig config{};
      config.target_lufs = -14.0f;
      config.ceiling_db = -1.0f;
      config.true_peak_oversample = 4;
      config.release_ms = release_ms;
      config.apply_gain_at_input_rate = apply_gain_at_input_rate;
      SonareMasteringResult result{};
      REQUIRE(sonare_mastering_process(samples.data(), samples.size(), 22050, &config, &result) ==
              SONARE_OK);
      std::vector<float> out(result.samples, result.samples + result.length);
      sonare_free_mastering_result(&result);
      return out;
    };

    // Non-default values must still produce valid, finite, full-length output.
    const std::vector<float> tuned = run(250.0f, 1);
    REQUIRE(tuned.size() == samples.size());
    for (float value : tuned) REQUIRE(std::isfinite(value));

    // A zero-initialized config (release_ms == 0) must reproduce the explicit
    // library default (50 ms) so older callers see no behavior change.
    const std::vector<float> zero = run(0.0f, 0);
    const std::vector<float> fifty = run(50.0f, 0);
    REQUIRE(zero.size() == fifty.size());
    for (size_t i = 0; i < zero.size(); ++i) {
      REQUIRE(std::abs(zero[i] - fifty[i]) <= 1.0e-6f);
    }
  }

  SECTION("an out-of-domain oversample or release is refused, not defaulted") {
    auto samples = generate_sine(440.0f, 22050, 0.5f);
    for (auto& sample : samples) sample *= 0.2f;

    auto run = [&](int oversample, float release_ms) {
      SonareMasteringConfig config{};
      config.target_lufs = -14.0f;
      config.ceiling_db = -1.0f;
      config.true_peak_oversample = oversample;
      config.release_ms = release_ms;
      SonareMasteringResult result{};
      const SonareError err =
          sonare_mastering_process(samples.data(), samples.size(), 22050, &config, &result);
      if (err == SONARE_OK) sonare_free_mastering_result(&result);
      return err;
    };

    // The positive control the refusals are read against: an in-range invalid
    // oversample is refused, so the validator is live and reachable on this
    // path and a negative reaching the default is a substitution rather than a
    // check happening somewhere else.
    REQUIRE(run(3, 50.0f) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(run(4, 50.0f) == SONARE_OK);
    REQUIRE(run(0, 0.0f) == SONARE_OK);

    REQUIRE(run(-1, 50.0f) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(run(-4, 50.0f) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(run(4, -1.0f) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(run(4, std::numeric_limits<float>::quiet_NaN()) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(run(4, std::numeric_limits<float>::infinity()) == SONARE_ERROR_INVALID_PARAMETER);
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

    config.target_lufs = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(sonare_mastering_process(samples.data(), samples.size(), 22050, &config, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    config.target_lufs = -18.0f;
    config.ceiling_db = std::numeric_limits<float>::infinity();
    REQUIRE(sonare_mastering_process(samples.data(), samples.size(), 22050, &config, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);

    config.ceiling_db = -1.0f;
    REQUIRE(sonare_mastering_process(samples.data(), samples.size(), 22050, &config, &result) ==
            SONARE_OK);
    for (size_t i = 0; i < result.length; ++i) REQUIRE(std::isfinite(result.samples[i]));
    sonare_free_mastering_result(&result);
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

  SECTION("streaming mastering chain rejects non-finite blocks") {
    constexpr int kSampleRate = 48000;
    constexpr int kBlockSize = 128;

    SonareStreamingMasteringChain* mono_chain =
        sonare_streaming_mastering_chain_create_ex(nullptr, 0, 0.0f, 0.0f);
    REQUIRE(mono_chain != nullptr);
    REQUIRE(sonare_streaming_mastering_chain_prepare(mono_chain, kSampleRate, kBlockSize, 1) ==
            SONARE_OK);

    std::vector<float> mono(kBlockSize, 0.0f);
    mono[7] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(sonare_streaming_mastering_chain_process_mono(mono_chain, mono.data(), mono.size()) ==
            SONARE_ERROR_INVALID_PARAMETER);
    std::fill(mono.begin(), mono.end(), 0.0f);
    REQUIRE(sonare_streaming_mastering_chain_process_mono(mono_chain, mono.data(), mono.size()) ==
            SONARE_OK);
    sonare_streaming_mastering_chain_destroy(mono_chain);

    SonareStreamingMasteringChain* stereo_chain =
        sonare_streaming_mastering_chain_create_ex(nullptr, 0, 0.0f, 0.0f);
    REQUIRE(stereo_chain != nullptr);
    REQUIRE(sonare_streaming_mastering_chain_prepare(stereo_chain, kSampleRate, kBlockSize, 2) ==
            SONARE_OK);

    std::vector<float> left(kBlockSize, 0.0f);
    std::vector<float> right(kBlockSize, 0.0f);
    right[11] = std::numeric_limits<float>::infinity();
    REQUIRE(sonare_streaming_mastering_chain_process_stereo(stereo_chain, left.data(), right.data(),
                                                            left.size()) ==
            SONARE_ERROR_INVALID_PARAMETER);
    std::fill(right.begin(), right.end(), 0.0f);
    REQUIRE(sonare_streaming_mastering_chain_process_stereo(stereo_chain, left.data(), right.data(),
                                                            left.size()) == SONARE_OK);
    sonare_streaming_mastering_chain_destroy(stereo_chain);
  }

  SECTION("streaming mastering chain rejects an oversized block without reading it") {
    // The non-finite scan below runs over the caller's buffer before the core
    // sees it, so the block bound has to be enforced first: an oversized
    // num_samples must be refused without dereferencing a single sample past
    // the buffer. The buffers here are sized to exactly the prepared block, so
    // an unbounded scan reads out of bounds (ASan-visible). The observable
    // signal that the rejection happened at the boundary rather than inside the
    // core is the detailed-error channel: a pre-scan validation early return
    // records no message, while the core's own rejection throws and leaves its
    // what() text behind.
    constexpr int kSampleRate = 48000;
    constexpr int kBlockSize = 128;

    SonareStreamingMasteringChain* chain =
        sonare_streaming_mastering_chain_create_ex(nullptr, 0, 0.0f, 0.0f);
    REQUIRE(chain != nullptr);

    std::vector<float> mono(kBlockSize, 0.0f);
    REQUIRE(sonare_streaming_mastering_chain_prepare(chain, kSampleRate, kBlockSize, 1) ==
            SONARE_OK);
    REQUIRE(sonare_streaming_mastering_chain_process_mono(chain, mono.data(), mono.size()) ==
            SONARE_OK);
    REQUIRE(sonare_streaming_mastering_chain_process_mono(chain, mono.data(), mono.size() + 1) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()).empty());
    sonare_streaming_mastering_chain_destroy(chain);

    // Never prepared: rejected as a state error, again without reading input.
    SonareStreamingMasteringChain* unprepared =
        sonare_streaming_mastering_chain_create_ex(nullptr, 0, 0.0f, 0.0f);
    REQUIRE(unprepared != nullptr);
    REQUIRE(sonare_streaming_mastering_chain_process_mono(unprepared, mono.data(), mono.size()) ==
            SONARE_ERROR_INVALID_STATE);
    REQUIRE(std::string(sonare_last_error_message()).empty());
    sonare_streaming_mastering_chain_destroy(unprepared);

    SonareStreamingMasteringChain* stereo_chain =
        sonare_streaming_mastering_chain_create_ex(nullptr, 0, 0.0f, 0.0f);
    REQUIRE(stereo_chain != nullptr);
    REQUIRE(sonare_streaming_mastering_chain_prepare(stereo_chain, kSampleRate, kBlockSize, 2) ==
            SONARE_OK);
    std::vector<float> left(kBlockSize, 0.0f);
    std::vector<float> right(kBlockSize, 0.0f);
    REQUIRE(sonare_streaming_mastering_chain_process_stereo(stereo_chain, left.data(), right.data(),
                                                            left.size() + 1) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()).empty());
    sonare_streaming_mastering_chain_destroy(stereo_chain);
  }

  SECTION("streaming mastering chain flush drains delayed samples once") {
    constexpr int kSampleRate = 48000;
    constexpr int kBlockSize = 64;
    const SonareMasteringParam params[] = {{"maximizer.truePeakLimiter.enabled", 1.0}};
    SonareStreamingMasteringChain* chain = sonare_streaming_mastering_chain_create(params, 1);
    REQUIRE(chain != nullptr);
    REQUIRE(sonare_streaming_mastering_chain_prepare(chain, kSampleRate, kBlockSize, 1) ==
            SONARE_OK);
    REQUIRE(sonare_streaming_mastering_chain_latency_samples(chain) > 0);

    std::vector<float> block(kBlockSize, 0.0f);
    block[0] = 0.5f;
    REQUIRE(sonare_streaming_mastering_chain_process_mono(chain, block.data(), block.size()) ==
            SONARE_OK);

    size_t written = 0;
    size_t total = 0;
    do {
      std::fill(block.begin(), block.end(), -1.0f);
      REQUIRE(sonare_streaming_mastering_chain_flush_mono(chain, block.data(), block.size(),
                                                          &written) == SONARE_OK);
      total += written;
    } while (written > 0);
    REQUIRE(total >= static_cast<size_t>(sonare_streaming_mastering_chain_latency_samples(chain)));
    REQUIRE(sonare_streaming_mastering_chain_flush_mono(chain, block.data(), block.size(),
                                                        &written) == SONARE_OK);
    REQUIRE(written == 0);
    sonare_streaming_mastering_chain_destroy(chain);
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

  SECTION("audio validation failures clear stale detailed error messages") {
    auto samples = generate_sine(440.0f, 22050, 0.5f);
    SonareMasteringParam params[] = {{"width", 3.5}};
    SonareMasteringStereoResult stereo{};

    REQUIRE(sonare_mastering_apply_processor_stereo("stereo.imager", samples.data(), samples.data(),
                                                    samples.size(), 22050, params, 1,
                                                    &stereo) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::strlen(sonare_last_error_message()) > 0);

    SonareMasteringResult mono{};
    REQUIRE(sonare_mastering_apply_processor("dynamics.compressor", nullptr, samples.size(), 22050,
                                             nullptr, 0, &mono) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()).empty());
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

  SECTION("assistant suggestion follows the requested delivery target") {
    auto samples = generate_sine(220.0f, 48000, 3.0f);
    for (auto& sample : samples) sample *= 0.2f;

    const char* names = sonare_mastering_platform_names();
    REQUIRE(names != nullptr);
    REQUIRE(std::strstr(names, "broadcast") != nullptr);
    REQUIRE(std::strstr(names, "club") != nullptr);
    REQUIRE(sonare_mastering_platform_from_name("not-a-platform") == -1);
    REQUIRE(sonare_mastering_platform_from_name(nullptr) == -1);

    const int broadcast = sonare_mastering_platform_from_name("broadcast");
    REQUIRE(broadcast >= 0);
    SonareMasteringParam broadcast_params[] = {{"targetPlatform", static_cast<double>(broadcast)}};
    char* json = nullptr;
    REQUIRE(sonare_mastering_assistant_suggest(samples.data(), samples.size(), 48000,
                                               broadcast_params, 1, &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    // Without the target the assistant returns the streaming default of -14.
    REQUIRE(std::strstr(json, "\"loudness.targetLufs\":-23") != nullptr);
    sonare_free_string(json);

    // Only a loud delivery format moves the ceiling: broadcast and podcast ask
    // for the ceiling the default already carries.
    const int club = sonare_mastering_platform_from_name("club");
    REQUIRE(club >= 0);
    SonareMasteringParam club_params[] = {{"targetPlatform", static_cast<double>(club)}};
    json = nullptr;
    REQUIRE(sonare_mastering_assistant_suggest(samples.data(), samples.size(), 48000, club_params,
                                               1, &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    REQUIRE(std::strstr(json, "\"loudness.targetLufs\":-9") != nullptr);
    REQUIRE(std::strstr(json, "\"loudness.ceilingDb\":-0.3") != nullptr);
    sonare_free_string(json);

    // An index naming no target, and a fractional index, are rejected rather
    // than truncated toward a neighbouring target.
    SonareMasteringParam unknown[] = {{"targetPlatform", 4096.0}};
    json = nullptr;
    REQUIRE(sonare_mastering_assistant_suggest(samples.data(), samples.size(), 48000, unknown, 1,
                                               &json) == SONARE_ERROR_INVALID_PARAMETER);
    SonareMasteringParam fractional[] = {{"targetPlatform", static_cast<double>(broadcast) + 0.25}};
    REQUIRE(sonare_mastering_assistant_suggest(samples.data(), samples.size(), 48000, fractional, 1,
                                               &json) == SONARE_ERROR_INVALID_PARAMETER);
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

  SECTION("stereo analysis entry points measure the pair, not a downmix") {
    auto left = generate_sine(1000.0f, 48000, 4.0f);
    auto right = generate_sine(1731.0f, 48000, 4.0f);
    for (auto& sample : left) sample *= 0.2f;
    for (auto& sample : right) sample *= 0.2f;
    std::vector<float> downmix(left.size());
    for (size_t index = 0; index < left.size(); ++index) {
      downmix[index] = 0.5f * (left[index] + right[index]);
    }

    auto integrated_lufs = [](const char* json) {
      const auto root = sonare::util::json::parse(json);
      const auto* platforms = root.find("platforms");
      REQUIRE(platforms != nullptr);
      return platforms->as_array().at(0).find("integratedLufs")->as_number();
    };

    char* stereo_json = nullptr;
    REQUIRE(sonare_mastering_streaming_preview_stereo(left.data(), right.data(), left.size(), 48000,
                                                      nullptr, 0, &stereo_json) == SONARE_OK);
    char* mono_json = nullptr;
    REQUIRE(sonare_mastering_streaming_preview(downmix.data(), downmix.size(), 48000, nullptr, 0,
                                               &mono_json) == SONARE_OK);
    REQUIRE(std::strstr(stereo_json, "\"Spotify\"") != nullptr);
    // BS.1770 sums the channel powers; the downmix quarters them, so the pair
    // reads 6.02 dB above what a mono caller can measure.
    REQUIRE(integrated_lufs(stereo_json) - integrated_lufs(mono_json) > 5.5);
    REQUIRE(integrated_lufs(stereo_json) - integrated_lufs(mono_json) < 6.5);
    sonare_free_string(stereo_json);
    sonare_free_string(mono_json);

    SonareMasteringParam params[] = {{"nFft", 1024.0}, {"hopLength", 256.0}};
    char* profile_json = nullptr;
    REQUIRE(sonare_mastering_audio_profile_stereo(left.data(), right.data(), left.size(), 48000,
                                                  params, 2, &profile_json) == SONARE_OK);
    REQUIRE(std::strstr(profile_json, "\"integratedLufs\"") != nullptr);
    REQUIRE(std::strstr(profile_json, "\"centroidHz\"") != nullptr);
    sonare_free_string(profile_json);

    char* suggest_json = nullptr;
    REQUIRE(sonare_mastering_assistant_suggest_stereo(left.data(), right.data(), left.size(), 48000,
                                                      nullptr, 0, &suggest_json) == SONARE_OK);
    REQUIRE(std::strstr(suggest_json, "\"chainConfig\"") != nullptr);
    REQUIRE(std::strstr(suggest_json, "\"explanation\"") != nullptr);
    sonare_free_string(suggest_json);

    float stereo_crest = 0.0f;
    float mono_crest = 0.0f;
    REQUIRE(sonare_metering_crest_factor_db_stereo(left.data(), right.data(), left.size(), 48000,
                                                   &stereo_crest) == SONARE_OK);
    REQUIRE(sonare_metering_crest_factor_db(downmix.data(), downmix.size(), 48000, &mono_crest) ==
            SONARE_OK);
    REQUIRE(std::isfinite(stereo_crest));
    REQUIRE(std::isfinite(mono_crest));
  }

  SECTION("stereo analysis entry points reject invalid input") {
    auto left = generate_sine(440.0f, 48000, 0.25f);
    auto right = generate_sine(660.0f, 48000, 0.25f);
    char* json = nullptr;
    float value = 0.0f;

    REQUIRE(sonare_mastering_streaming_preview_stereo(left.data(), right.data(), left.size(), 48000,
                                                      nullptr, 0,
                                                      nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_streaming_preview_stereo(nullptr, right.data(), left.size(), 48000,
                                                      nullptr, 0,
                                                      &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_streaming_preview_stereo(left.data(), nullptr, left.size(), 48000,
                                                      nullptr, 0,
                                                      &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_streaming_preview_stereo(left.data(), right.data(), left.size(), 0,
                                                      nullptr, 0,
                                                      &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_audio_profile_stereo(nullptr, right.data(), left.size(), 48000,
                                                  nullptr, 0,
                                                  &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_assistant_suggest_stereo(left.data(), nullptr, left.size(), 48000,
                                                      nullptr, 0,
                                                      &json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_metering_crest_factor_db_stereo(left.data(), right.data(), left.size(), 48000,
                                                   nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_metering_crest_factor_db_stereo(nullptr, right.data(), left.size(), 48000,
                                                   &value) == SONARE_ERROR_INVALID_PARAMETER);
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

TEST_CASE("sonare_eq_set_band accepts detectorDelayMs and its former lookaheadMs spelling",
          "[c_api][mastering][eq]") {
  // detectorDelayMs is the corrected name (see dynamic_eq.h); lookaheadMs is
  // kept accepted so a stored band config using the old spelling is not
  // silently dropped. Checked directly against the shared JSON parser (not
  // just SONARE_OK) so a regression that silently ignored the key would be
  // caught here rather than only showing up as a behavioural difference.
  using sonare::c_api::parse_eq_band_json;

  const auto with_new =
      parse_eq_band_json(R"({"type":"Peak","dynamic":true,"detectorDelayMs":12})");
  REQUIRE(with_new.dyn.detector_delay_ms == 12.0f);

  const auto with_old_camel =
      parse_eq_band_json(R"({"type":"Peak","dynamic":true,"lookaheadMs":8})");
  REQUIRE(with_old_camel.dyn.detector_delay_ms == 8.0f);

  const auto with_old_snake =
      parse_eq_band_json(R"({"type":"Peak","dynamic":true,"lookahead_ms":6})");
  REQUIRE(with_old_snake.dyn.detector_delay_ms == 6.0f);

  // The canonical spelling wins when both are present.
  const auto both =
      parse_eq_band_json(R"({"type":"Peak","dynamic":true,"lookaheadMs":8,"detectorDelayMs":12})");
  REQUIRE(both.dyn.detector_delay_ms == 12.0f);

  // Also reachable end to end through the C ABI, not just the parser.
  SonareEq* eq = sonare_eq_create(48000.0, 512);
  REQUIRE(eq != nullptr);
  REQUIRE(sonare_eq_set_band(eq, 0,
                             "{\"type\":\"Peak\",\"dynamic\":true,"
                             "\"lookaheadMs\":8}") == SONARE_OK);
  sonare_eq_destroy(eq);
}

TEST_CASE("sonare_mastering name getters return a stable pointer across calls",
          "[c_api][mastering]") {
  // The header promises the returned pointer stays valid across later API calls
  // on the thread. The name caches are gated on a write-once flag (not on the
  // string being empty), so repeated calls must return the very same pointer
  // without recomputing/reassigning the thread_local.
  REQUIRE(sonare_mastering_processor_names() == sonare_mastering_processor_names());
  REQUIRE(sonare_mastering_pair_processor_names() == sonare_mastering_pair_processor_names());
  REQUIRE(sonare_mastering_pair_analysis_names() == sonare_mastering_pair_analysis_names());
  REQUIRE(sonare_mastering_stereo_analysis_names() == sonare_mastering_stereo_analysis_names());
  REQUIRE(sonare_mastering_insert_names() == sonare_mastering_insert_names());
  REQUIRE(sonare_mastering_processor_catalog() == sonare_mastering_processor_catalog());
  // sonare_mastering_preset_names now follows the same SONARE_C_TRY-guarded,
  // write-once-flag pattern as its siblings above (it previously had neither
  // the exception guard nor the stable-pointer cache).
  REQUIRE(sonare_mastering_preset_names() == sonare_mastering_preset_names());
  const char* presets = sonare_mastering_preset_names();
  REQUIRE(presets != nullptr);
  REQUIRE(std::strlen(presets) > 0);
  const char* catalog = sonare_mastering_processor_catalog();
  REQUIRE(catalog != nullptr);
  REQUIRE(std::strstr(catalog, "\"latencySamples\":") != nullptr);
  REQUIRE(std::strstr(catalog, "\"tailSamples\":") != nullptr);
  REQUIRE(std::strstr(catalog, "\"realtimeCost\":") != nullptr);
}

TEST_CASE("sonare_capability_catalog_json aggregates processors and presets",
          "[c_api][mastering]") {
  const char* json = sonare_capability_catalog_json();
  REQUIRE(json != nullptr);
  const auto catalog = sonare::util::json::parse_strict(json);

  REQUIRE(catalog["version"].as_string() == sonare_version());
  REQUIRE(catalog["abi"]["project"].as_int() == SONARE_PROJECT_ABI_VERSION);
  REQUIRE(catalog["abi"]["engine"].as_int() == static_cast<int>(sonare_engine_abi_version()));
  REQUIRE(catalog["processors"].is_array());
  REQUIRE_FALSE(catalog["processors"].as_array().empty());
  REQUIRE(catalog["presets"].is_object());
  for (const char* name : {"mastering", "synth", "mixingScene", "voiceChanger"}) {
    REQUIRE(catalog["presets"].contains(name));
    REQUIRE(catalog["presets"][name].is_array());
  }

  const auto processor = std::find_if(
      catalog["processors"].as_array().begin(), catalog["processors"].as_array().end(),
      [](const auto& entry) { return entry["id"].as_string() == "dynamics.compressor"; });
  REQUIRE(processor != catalog["processors"].as_array().end());
  REQUIRE((*processor)["category"].as_string() == "dynamics");
  REQUIRE((*processor)["params"].is_array());
  REQUIRE_FALSE((*processor)["params"].as_array().empty());
  const auto& param = (*processor)["params"][0];
  REQUIRE(param.contains("name"));
  REQUIRE(param.contains("type"));
  REQUIRE(param.contains("min"));
  REQUIRE(param.contains("max"));
  REQUIRE(param.contains("default"));
  REQUIRE(param.contains("unit"));
}

TEST_CASE("the capability catalog schema list matches what the writer emits",
          "[c_api][mastering]") {
  const char* json = sonare_capability_catalog_json();
  REQUIRE(json != nullptr);
  const auto actual = sonare::test::schema_paths_of(json);
  const auto& expected_paths = sonare_c_mastering_detail::capability_catalog_schema_paths();
  const std::set<std::string> expected(expected_paths.begin(), expected_paths.end());
  REQUIRE(actual == expected);

  // The processors interior is the processor catalog schema under a prefix.
  // Both lists are written out literally so a reader outside this language can
  // parse them, which is exactly what lets the two copies drift.
  std::set<std::string> prefixed;
  for (const auto& path : sonare::mastering::api::processor_catalog_schema_paths()) {
    prefixed.insert("processors" + path);
  }
  std::set<std::string> interior;
  for (const auto& path : expected) {
    if (path.rfind("processors[]", 0) == 0) interior.insert(path);
  }
  REQUIRE(interior == prefixed);
}

TEST_CASE("sonare_mastering named-processor rejects out-of-range repair modes",
          "[c_api][mastering]") {
  // The one-shot named-processor path must validate repair enum params the same
  // way the dedicated repair C-ABI does, instead of silently passing audio
  // through on an out-of-range mode.
  std::vector<float> samples(2048, 0.1f);
  SonareMasteringResult out{};

  SonareMasteringParam bad_mode[] = {{"mode", 99.0}};
  REQUIRE(sonare_mastering_apply_processor("repair.denoiseClassical", samples.data(),
                                           samples.size(), 44100, bad_mode, 1,
                                           &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mastering_apply_processor("repair.decrackle", samples.data(), samples.size(),
                                           44100, bad_mode, 1,
                                           &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mastering_apply_processor("repair.trimSilence", samples.data(), samples.size(),
                                           44100, bad_mode, 1,
                                           &out) == SONARE_ERROR_INVALID_PARAMETER);

  SonareMasteringStereoResult stereo_out{};
  REQUIRE(sonare_mastering_apply_processor_stereo(
              "repair.trimSilence", samples.data(), samples.data(), samples.size(), 44100, bad_mode,
              1, &stereo_out) == SONARE_ERROR_INVALID_PARAMETER);

  SonareMasteringParam negative_padding[] = {{"paddingSamples", -1.0}};
  REQUIRE(sonare_mastering_apply_processor("repair.trimSilence", samples.data(), samples.size(),
                                           44100, negative_padding, 1,
                                           &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mastering_apply_processor_stereo(
              "repair.trimSilence", samples.data(), samples.data(), samples.size(), 44100,
              negative_padding, 1, &stereo_out) == SONARE_ERROR_INVALID_PARAMETER);

  // A valid mode still succeeds.
  SonareMasteringParam good_mode[] = {{"mode", 0.0}};
  REQUIRE(sonare_mastering_apply_processor("repair.denoiseClassical", samples.data(),
                                           samples.size(), 44100, good_mode, 1, &out) == SONARE_OK);
  sonare_free_mastering_result(&out);
}

TEST_CASE("sonare_mastering trimSilence measures an all-silent empty result safely",
          "[c_api][mastering][trim_silence][empty]") {
  constexpr int sample_rate = 48000;
  std::vector<float> silence(2048, 0.0f);

  SonareMasteringResult mono{};
  REQUIRE(sonare_mastering_apply_processor("repair.trimSilence", silence.data(), silence.size(),
                                           sample_rate, nullptr, 0, &mono) == SONARE_OK);
  REQUIRE(mono.length == 0);
  REQUIRE(std::isinf(mono.output_lufs));
  REQUIRE(mono.output_lufs < 0.0f);
  sonare_free_mastering_result(&mono);

  SonareMasteringStereoResult stereo{};
  REQUIRE(sonare_mastering_apply_processor_stereo("repair.trimSilence", silence.data(),
                                                  silence.data(), silence.size(), sample_rate,
                                                  nullptr, 0, &stereo) == SONARE_OK);
  REQUIRE(stereo.length == 0);
  REQUIRE(std::isinf(stereo.output_lufs));
  REQUIRE(stereo.output_lufs < 0.0f);
  sonare_free_mastering_stereo_result(&stereo);
}

TEST_CASE("sonare_mastering pair processors reject invalid enums consistently",
          "[c_api][mastering][match]") {
  std::vector<float> source(2048, 0.1f);
  std::vector<float> reference(2048, 0.2f);
  SonareMasteringResult out{};

  SonareMasteringParam bad_phase[] = {{"phase", 2.0}};
  REQUIRE(sonare_mastering_apply_pair_processor("match.applyMatchEq", source.data(),
                                                reference.data(), source.size(), 44100, bad_phase,
                                                1, &out) == SONARE_ERROR_INVALID_PARAMETER);

  SonareMasteringParam bad_selection[] = {{"selection", -1.0}};
  REQUIRE(sonare_mastering_apply_pair_processor("match.abSwitch", source.data(), reference.data(),
                                                source.size(), 44100, bad_selection, 1,
                                                &out) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_mastering pair _ex accepts differing reference length", "[c_api][mastering]") {
  // Reference masters are commonly a different length than the source; the _ex
  // pair variants take an independent reference_length.
  std::vector<float> source(4096, 0.2f);
  std::vector<float> reference(1024, 0.3f);  // intentionally shorter

  SonareMasteringParam pair_params[] = {{"mix", 0.5}};
  SonareMasteringResult paired{};
  REQUIRE(sonare_mastering_apply_pair_processor_ex(
              "match.abCrossfade", source.data(), source.size(), reference.data(), reference.size(),
              44100, pair_params, 1, &paired) == SONARE_OK);
  REQUIRE(paired.samples != nullptr);
  sonare_free_mastering_result(&paired);

  char* json = nullptr;
  REQUIRE(sonare_mastering_analyze_pair_ex("match.referenceLoudness", source.data(), source.size(),
                                           reference.data(), reference.size(), 44100, nullptr, 0,
                                           &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  REQUIRE(std::strstr(json, "referenceLufs") != nullptr);
  sonare_free_string(json);
}

TEST_CASE("mastering chain C result includes the before and after report", "[c_api][mastering]") {
  constexpr int sample_rate = 22050;
  auto samples = generate_sine(440.0f, sample_rate, 4.0f);
  for (auto& sample : samples) sample *= 0.2f;
  const SonareMasteringParam params[] = {{"loudness.targetLufs", -14.0},
                                         {"loudness.ceilingDb", -1.0}};
  SonareMasteringChainResult result{};
  REQUIRE(sonare_mastering_chain(samples.data(), samples.size(), sample_rate, params, 2, &result) ==
          SONARE_OK);
  REQUIRE(result.report.before.integrated_lufs == result.input_lufs);
  REQUIRE(result.report.after.integrated_lufs == result.output_lufs);
  REQUIRE(result.report.after.true_peak_dbtp == result.output_true_peak_dbtp);
  REQUIRE(std::isfinite(result.report.before.max_momentary_lufs));
  REQUIRE(std::isfinite(result.report.after.max_short_term_lufs));
  REQUIRE(SONARE_MASTERING_REPORT_BAND_COUNT == 32);
  REQUIRE(result.report.max_gain_reduction_db <= 0.0f);
  sonare_free_mastering_chain_result(&result);
  REQUIRE(result.samples == nullptr);
  REQUIRE(result.report.band_energy_delta_db[0] == 0.0f);
}

TEST_CASE("loudness_target_limited agrees across the mastering entry points",
          "[c_api][mastering]") {
  constexpr int sample_rate = 22050;
  auto samples = generate_sine(440.0f, sample_rate, 2.0f);
  for (auto& sample : samples) sample *= 0.5f;

  // Drives all three entry points that normalize loudness and publish the flag,
  // with the same material and the same target / ceiling, and returns their
  // reported values. The chain stands in for master_audio: a preset chain runs
  // the same loudness stage, just with preset-chosen values.
  const auto run_all = [&](float target_lufs, float ceiling_db) {
    SonareMasteringConfig config{};
    config.target_lufs = target_lufs;
    config.ceiling_db = ceiling_db;
    config.true_peak_oversample = 4;
    SonareMasteringResult simple{};
    REQUIRE(sonare_mastering_process(samples.data(), samples.size(), sample_rate, &config,
                                     &simple) == SONARE_OK);

    const SonareMasteringParam params[] = {{"targetLufs", target_lufs}, {"ceilingDb", ceiling_db}};
    SonareMasteringResult named{};
    REQUIRE(sonare_mastering_apply_processor("maximizer.loudnessOptimize", samples.data(),
                                             samples.size(), sample_rate, params, 2,
                                             &named) == SONARE_OK);

    const SonareMasteringParam chain_params[] = {{"loudness.targetLufs", target_lufs},
                                                 {"loudness.ceilingDb", ceiling_db}};
    SonareMasteringChainResult chain{};
    REQUIRE(sonare_mastering_chain(samples.data(), samples.size(), sample_rate, chain_params, 2,
                                   &chain) == SONARE_OK);

    const std::array<int, 4> reported = {
        simple.loudness_target_limited, named.loudness_target_limited,
        chain.loudness_target_limited, chain.report.loudness_target_limited};
    sonare_free_mastering_result(&simple);
    sonare_free_mastering_result(&named);
    sonare_free_mastering_chain_result(&chain);
    return reported;
  };

  SECTION("a ceiling that provably blocks the target reports true everywhere") {
    // The ceiling sits below the material's true peak while the target asks for
    // a large boost, so the normalization gain is clamped on every path.
    const auto reported = run_all(-6.0f, -12.0f);
    for (int value : reported) REQUIRE(value == 1);
  }

  SECTION("a reachable target reports false everywhere") {
    const auto reported = run_all(-20.0f, -1.0f);
    for (int value : reported) REQUIRE(value == 0);
  }
}

TEST_CASE("cancellation-capable mastering C APIs leave outputs empty", "[c_api][mastering]") {
  auto samples = generate_sine(440.0f, 22050, 1.0f);
  struct CancelState {
    float last_progress = -1.0f;
    bool should_cancel = false;
  } state;
  const auto progress = [](float value, const char* /*stage*/, void* user_data) {
    auto* current = static_cast<CancelState*>(user_data);
    current->last_progress = value;
    current->should_cancel = value > 0.5f;
  };
  const auto cancel = [](void* user_data) {
    return static_cast<CancelState*>(user_data)->should_cancel ? 1 : 0;
  };
  const SonareMasteringParam chain_params[] = {{"eq.tilt.enabled", 1.0},
                                               {"dynamics.compressor.enabled", 1.0},
                                               {"saturation.tape.enabled", 1.0}};

  SonareMasteringChainResult chain_mono{};
  REQUIRE(sonare_mastering_chain_with_progress_ex(samples.data(), samples.size(), 22050,
                                                  chain_params, 3, progress, &state, &chain_mono,
                                                  cancel, &state) == SONARE_ERROR_CANCELLED);
  REQUIRE(state.last_progress > 0.5f);
  REQUIRE(chain_mono.samples == nullptr);
  REQUIRE(chain_mono.stages == nullptr);

  state = {};
  SonareMasteringChainStereoResult chain_stereo{};
  REQUIRE(sonare_mastering_chain_stereo_with_progress_ex(
              samples.data(), samples.data(), samples.size(), 22050, chain_params, 3, progress,
              &state, &chain_stereo, cancel, &state) == SONARE_ERROR_CANCELLED);
  REQUIRE(state.last_progress > 0.5f);
  REQUIRE(chain_stereo.left == nullptr);
  REQUIRE(chain_stereo.right == nullptr);
  REQUIRE(chain_stereo.stages == nullptr);

  state = {};
  SonareMasteringChainResult preset_mono{};
  REQUIRE(sonare_master_audio_with_progress_ex("pop", samples.data(), samples.size(), 22050,
                                               nullptr, 0, progress, &state, &preset_mono, cancel,
                                               &state) == SONARE_ERROR_CANCELLED);
  REQUIRE(state.last_progress > 0.5f);
  REQUIRE(preset_mono.samples == nullptr);
  REQUIRE(preset_mono.stages == nullptr);

  state = {};
  SonareMasteringChainStereoResult preset_stereo{};
  REQUIRE(sonare_master_audio_stereo_with_progress_ex(
              "pop", samples.data(), samples.data(), samples.size(), 22050, nullptr, 0, progress,
              &state, &preset_stereo, cancel, &state) == SONARE_ERROR_CANCELLED);
  REQUIRE(state.last_progress > 0.5f);
  REQUIRE(preset_stereo.left == nullptr);
  REQUIRE(preset_stereo.right == nullptr);
  REQUIRE(preset_stereo.stages == nullptr);
}

TEST_CASE("a peak-normalized source under a blocking ceiling reports limited on every path",
          "[c_api][mastering][loudness]") {
  // Peak-normalized material asked for -6 LUFS under a -3 dBTP ceiling: the
  // requested boost is far larger than the remaining headroom, so no path can
  // honestly claim it reached the target. Complements the entry-point agreement
  // case above by adding the CLI-facing helper and by pinning the direction of
  // the value rather than only that the paths agree.
  constexpr int kSampleRate = 48000;
  constexpr float kTargetLufs = -6.0f;
  constexpr float kCeilingDb = -3.0f;
  std::vector<float> samples = generate_sine(440.0f, kSampleRate, 1.0f);
  const float peak = *std::max_element(samples.begin(), samples.end());
  REQUIRE(peak > 0.0f);
  for (float& sample : samples) sample /= peak;

  // The named-processor route the header points detect-only / chain-composing
  // callers at. Its result type had no field to carry the flag, so this path
  // used to report a confident false.
  const SonareMasteringParam processor_params[] = {
      {"targetLufs", kTargetLufs},
      {"ceilingDb", kCeilingDb},
      {"truePeakOversample", 4.0},
  };
  SonareMasteringResult named{};
  REQUIRE(sonare_mastering_apply_processor("maximizer.loudnessOptimize", samples.data(),
                                           samples.size(), kSampleRate, processor_params,
                                           std::size(processor_params), &named) == SONARE_OK);
  REQUIRE(named.loudness_target_limited == 1);
  REQUIRE(named.output_lufs < kTargetLufs);
  sonare_free_mastering_result(&named);

  // The chain shape master_audio and the CLI --report path return.
  const SonareMasteringParam chain_params[] = {
      {"loudness.enabled", 1.0},
      {"loudness.targetLufs", kTargetLufs},
      {"loudness.ceilingDb", kCeilingDb},
  };
  SonareMasteringChainResult chain{};
  REQUIRE(sonare_mastering_chain(samples.data(), samples.size(), kSampleRate, chain_params,
                                 std::size(chain_params), &chain) == SONARE_OK);
  REQUIRE(chain.loudness_target_limited == 1);
  REQUIRE(chain.report.loudness_target_limited == chain.loudness_target_limited);
  sonare_free_mastering_chain_result(&chain);

  // The C++ helper whose result the native CLI prints verbatim in its default
  // JSON payload, so this pins the value that CLI reports.
  sonare::mastering::maximizer::LoudnessOptimizeConfig cpp_config;
  cpp_config.target_lufs = kTargetLufs;
  cpp_config.ceiling_db = kCeilingDb;
  cpp_config.true_peak_oversample = 4;
  const auto direct = sonare::mastering::maximizer::loudness_optimize(
      sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate), cpp_config);
  REQUIRE(direct.loudness_target_limited);
}

TEST_CASE("the stereo named-processor path reports loudness_target_limited like the mono path",
          "[c_api][mastering][loudness]") {
  // A peak-normalized source whose program loudness sits far below the -6 LUFS
  // target: the -3 dBTP ceiling leaves no headroom for the requested boost, so
  // both entry points are blocked by a margin far wider than the 3 dB BS.1770
  // channel-summing offset between the mono measurement and the dual-mono
  // stereo one. The stereo result type had no field to carry the flag, so this
  // path used to report a confident false while the core computed true.
  constexpr int kSampleRate = 48000;
  constexpr float kTargetLufs = -6.0f;
  constexpr float kCeilingDb = -3.0f;
  std::vector<float> samples = generate_sine(440.0f, kSampleRate, 1.0f);
  for (float& sample : samples) sample *= 0.05f;
  // A lone full-scale transient normalizes the peak without lifting the program
  // loudness, which is what makes the ceiling the binding constraint.
  samples[samples.size() / 2] = 1.0f;

  const SonareMasteringParam params[] = {
      {"targetLufs", kTargetLufs},
      {"ceilingDb", kCeilingDb},
      {"truePeakOversample", 4.0},
  };

  SonareMasteringResult mono{};
  REQUIRE(sonare_mastering_apply_processor("maximizer.loudnessOptimize", samples.data(),
                                           samples.size(), kSampleRate, params, std::size(params),
                                           &mono) == SONARE_OK);
  REQUIRE(mono.loudness_target_limited == 1);
  REQUIRE(mono.output_lufs < kTargetLufs);

  SonareMasteringStereoResult stereo{};
  REQUIRE(sonare_mastering_apply_processor_stereo("maximizer.loudnessOptimize", samples.data(),
                                                  samples.data(), samples.size(), kSampleRate,
                                                  params, std::size(params), &stereo) == SONARE_OK);
  REQUIRE(stereo.loudness_target_limited == mono.loudness_target_limited);
  REQUIRE(stereo.output_lufs < kTargetLufs);

  sonare_free_mastering_result(&mono);
  sonare_free_mastering_stereo_result(&stereo);
}

TEST_CASE("EQ band JSON narrows every numeric field through a checked cast",
          "[c_api][mastering][eq]") {
  struct NumericField {
    const char* camel;
    const char* snake;  // nullptr when the field accepts only one spelling
    bool gain_db;       // reaches the biquad design as a dB gain
  };
  // Every numeric field parse_eq_band_json reads, in both spellings where it
  // takes two. A table rather than a hand-picked sample, so a field added to
  // the parser without a row here shows up as an uncovered name.
  const NumericField fields[] = {
      {"frequencyHz", "frequency_hz", false},
      {"gainDb", "gain_db", true},
      {"q", nullptr, false},
      {"slopeDbOct", "slope_db_oct", false},
      {"proportionalQStrength", "proportional_q_strength", false},
      {"thresholdDb", "threshold_db", true},
      {"ratio", nullptr, false},
      {"rangeDb", "range_db", true},
      {"attackMs", "attack_ms", false},
      {"releaseMs", "release_ms", false},
      {"lookaheadMs", "lookahead_ms", false},
      {"detectorDelayMs", "detector_delay_ms", false},
      {"sidechainFreqHz", "sidechain_freq_hz", false},
      {"sidechainQ", "sidechain_q", false},
  };
  // Value classes crossed with every field above. `accepted` says whether the
  // narrowing guard must let the literal through, not whether the value is
  // musically sensible: parse_eq_band_json does not domain-validate.
  struct ValueClass {
    const char* literal;
    bool accepted;
    bool accepted_for_gain_db;
  };
  const ValueClass classes[] = {
      {"1", true, true},        // finite, in float range
      {"20000", true, false},   // finite, but 10^(dB/40) overflows
      {"1e39", false, false},   // finite double, out of float range
      {"-1e39", false, false},  // likewise, negative
      {"1e400", false, false},  // out of double range
      {"\"1\"", false, false},  // wrong type: string
      {"true", false, false},   // wrong type: bool
      {"null", false, false},   // wrong type: null
      {"{}", false, false},     // wrong type: object
      {"[]", false, false},     // wrong type: array
  };

  const auto parses = [](const std::string& json) {
    try {
      (void)sonare::c_api::parse_eq_band_json(json.c_str());
      return true;
    } catch (const sonare::SonareException&) {
      return false;
    }
  };

  // Positive controls. Without these a parser that rejects unconditionally
  // satisfies every rejection row below.
  REQUIRE(parses("{\"type\":\"Peak\"}"));
  REQUIRE(
      parses("{\"type\":\"Peak\",\"frequencyHz\":1000,\"gainDb\":6,\"q\":1.2,\"slopeDbOct\":12,"
             "\"proportionalQStrength\":0.5,\"dynamic\":true,\"thresholdDb\":-24,\"ratio\":4,"
             "\"rangeDb\":12,\"attackMs\":5,\"releaseMs\":80,\"detectorDelayMs\":2,"
             "\"sidechainFreqHz\":800,\"sidechainQ\":0.7}"));

  // Axis 1: every field x every class, with every other field left at its
  // default, so nothing else in the object can raise before the injected value
  // is read.
  for (const NumericField& field : fields) {
    for (const ValueClass& value : classes) {
      const bool expected = field.gain_db ? value.accepted_for_gain_db : value.accepted;
      for (const char* key : {field.camel, field.snake}) {
        if (key == nullptr) continue;
        const std::string json =
            std::string("{\"type\":\"Peak\",\"") + key + "\":" + value.literal + "}";
        INFO(json);
        REQUIRE(parses(json) == expected);
        // Every accepted row must survive the whole C ABI, not just the parser.
        if (!expected) continue;
        SonareEq* eq = sonare_eq_create(48000.0, 512);
        REQUIRE(eq != nullptr);
        const SonareError err = sonare_eq_set_band(eq, 0, json.c_str());
        REQUIRE((err == SONARE_OK || err == SONARE_ERROR_INVALID_PARAMETER));
        sonare_eq_destroy(eq);
      }
    }
  }

  // Axis 2: masking. frequencyHz and q are caught downstream (design_eq_biquad
  // and checked_q) while gainDb, ratio, attackMs, releaseMs and
  // detectorDelayMs are admitted, so a document poisoning one of each could
  // pass for the wrong reason — the caught field raising before the admitting
  // one is read. Pin that both orderings still reject.
  for (const char* admitting : {"gainDb", "ratio", "attackMs", "releaseMs", "detectorDelayMs"}) {
    for (const char* caught : {"frequencyHz", "q"}) {
      const std::string caught_first =
          std::string("{\"type\":\"Peak\",\"") + caught + "\":1e39,\"" + admitting + "\":1e39}";
      const std::string admitting_first =
          std::string("{\"type\":\"Peak\",\"") + admitting + "\":1e39,\"" + caught + "\":1e39}";
      INFO(caught_first << " | " << admitting_first);
      REQUIRE_FALSE(parses(caught_first));
      REQUIRE_FALSE(parses(admitting_first));
      // And the admitting field alone still rejects, so the pair above cannot
      // be passing only because of its partner.
      REQUIRE_FALSE(parses(std::string("{\"type\":\"Peak\",\"") + admitting + "\":1e39}"));
    }
  }
}

TEST_CASE("sonare_eq_set_band refuses a band that would design non-finite taps",
          "[c_api][mastering][eq]") {
  SonareEq* eq = sonare_eq_create(48000.0, 512);
  REQUIRE(eq != nullptr);

  const char* good =
      "{\"type\":\"Peak\",\"frequencyHz\":1000,\"gainDb\":6,\"q\":1,"
      "\"enabled\":true}";
  REQUIRE(sonare_eq_set_band(eq, 0, good) == SONARE_OK);

  // An out-of-float-range double and a finite-but-unrealizable dB value both
  // have to be refused before the band is installed, not absorbed as +inf.
  REQUIRE(sonare_eq_set_band(eq, 0,
                             "{\"type\":\"Peak\",\"frequencyHz\":1000,\"gainDb\":1e39,\"q\":1,"
                             "\"enabled\":true}") == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_eq_set_band(eq, 0,
                             "{\"type\":\"Peak\",\"frequencyHz\":1000,\"gainDb\":20000,\"q\":1,"
                             "\"enabled\":true}") == SONARE_ERROR_INVALID_PARAMETER);

  // The refused bands left no infinite tap behind: the still-installed band
  // must produce finite audio.
  std::vector<float> left = generate_sine(1000.0f, 48000, 512.0f / 48000.0f);
  std::vector<float> right = left;
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_eq_process(eq, channels, 2, static_cast<int>(left.size())) == SONARE_OK);
  for (size_t i = 0; i < left.size(); ++i) {
    REQUIRE(std::isfinite(left[i]));
    REQUIRE(std::isfinite(right[i]));
  }

  sonare_eq_destroy(eq);
}

TEST_CASE("mastering *_names getters keep their documented write-once storage",
          "[c_api][mastering]") {
  // The header promises these stay valid across later API calls on the thread,
  // which only holds because the thread_local is built once. Pin the pointer
  // identity: a change to rebuild-per-call would silently demote them to the
  // weaker contract the mixing *_names getters carry, and nothing else would
  // notice.
  for (auto* fn : {&sonare_mastering_processor_names, &sonare_mastering_pair_processor_names,
                   &sonare_mastering_pair_analysis_names, &sonare_mastering_stereo_analysis_names,
                   &sonare_mastering_insert_names, &sonare_mastering_preset_names,
                   &sonare_mastering_platform_names}) {
    const char* first = (*fn)();
    REQUIRE(first != nullptr);
    // Another API call in between is exactly what the doc says is survivable.
    (void)sonare_mastering_processor_catalog();
    REQUIRE((*fn)() == first);
  }
}

TEST_CASE("an unrealizable dynamic EQ gain never reaches the audio as NaN",
          "[c_api][mastering][eq]") {
  // The composed path the per-field guards cannot cover on their own: the
  // detector delta is added to the static gain, so values that are each
  // realizable can still sum past the overflow point. normalize() is the
  // backstop, so drive several blocks and require the output stays finite.
  SonareEq* eq = sonare_eq_create(48000.0, 512);
  REQUIRE(eq != nullptr);
  REQUIRE(sonare_eq_set_band(eq, 0,
                             "{\"type\":\"Peak\",\"frequencyHz\":1000,\"gainDb\":12300,\"q\":1,"
                             "\"enabled\":true,\"dynamic\":true,\"thresholdDb\":-60,"
                             "\"ratio\":20,\"rangeDb\":300,\"attackMs\":1,\"releaseMs\":10}") ==
          SONARE_OK);
  // 12300 dB is realizable on its own (10^307.5 is finite) so the per-field
  // guard admits it; adding the 300 dB range crosses the overflow point.

  std::vector<float> left = generate_sine(1000.0f, 48000, 512.0f / 48000.0f);
  std::vector<float> right = left;
  for (int block = 0; block < 8; ++block) {
    std::vector<float> l = left;
    std::vector<float> r = right;
    float* channels[] = {l.data(), r.data()};
    REQUIRE(sonare_eq_process(eq, channels, 2, static_cast<int>(l.size())) == SONARE_OK);
    for (size_t i = 0; i < l.size(); ++i) {
      INFO("block " << block << " sample " << i);
      REQUIRE(std::isfinite(l[i]));
      REQUIRE(std::isfinite(r[i]));
    }
  }

  sonare_eq_destroy(eq);
}
TEST_CASE("the true-peak limiter's residual guard is channel-linked", "[mastering][maximizer]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 512;
  constexpr float kCeilingDb = -1.0f;

  // A burst out of silence with no lookahead: the smoothed gain can only catch up
  // after the fact, so the post-gain sample still sits over the ceiling and the
  // residual guard runs. The right channel is an eighth of the left and stays
  // under the ceiling throughout, so a per-channel guard scales only the left and
  // destroys the ratio while a linked one preserves it.
  std::vector<float> left(kBlock, 0.0f);
  std::vector<float> right(kBlock, 0.0f);
  for (int i = 128; i < kBlock; ++i) {
    const float phase = 2.0f * static_cast<float>(sonare::constants::kPi) * 1000.0f *
                        static_cast<float>(i) / static_cast<float>(kSr);
    left[static_cast<size_t>(i)] = 4.0f * std::sin(phase);
    right[static_cast<size_t>(i)] = 0.125f * left[static_cast<size_t>(i)];
  }

  sonare::mastering::maximizer::TruePeakLimiterConfig config;
  config.ceiling_db = kCeilingDb;
  config.lookahead_ms = 0.0f;
  sonare::mastering::maximizer::TruePeakLimiter limiter(config);
  limiter.prepare(static_cast<double>(kSr), kBlock, 2);

  float* channels[2] = {left.data(), right.data()};
  limiter.process(channels, 2, kBlock);

  // Exact ==: every gain factor is applied to both channels, and 0.125 scales a
  // float without rounding, so a linked chain reproduces the input ratio bit for
  // bit. A per-channel residual gain differs by whole dB, not by an ulp.
  for (int i = 0; i < kBlock; ++i) {
    CAPTURE(i);
    CHECK(right[static_cast<size_t>(i)] == 0.125f * left[static_cast<size_t>(i)]);
  }

  // Non-vacuity at the assertion: a run where nothing was pulled to the ceiling
  // would satisfy the ratio without ever exercising the guard.
  const float ceiling = sonare::db_to_linear(kCeilingDb);
  int at_ceiling = 0;
  for (int i = 0; i < kBlock; ++i) {
    if (std::abs(std::abs(left[static_cast<size_t>(i)]) - ceiling) < 1.0e-6f) ++at_ceiling;
  }
  CHECK(at_ceiling > 0);
}

TEST_CASE("planar stereo true peak matches the Audio-copy path exactly", "[mastering][loudness]") {
  constexpr int kSr = 48000;
  constexpr int kFrames = 2048;
  constexpr int kOversample = 4;

  std::vector<float> left(kFrames);
  std::vector<float> right(kFrames);
  for (int i = 0; i < kFrames; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSr);
    left[static_cast<size_t>(i)] =
        0.8f * std::sin(2.0f * static_cast<float>(sonare::constants::kPi) * 997.0f * t);
    // A quieter, differently phased right channel so the maximum comes from one
    // channel rather than from both at once.
    right[static_cast<size_t>(i)] =
        0.3f * std::sin(2.0f * static_cast<float>(sonare::constants::kPi) * 311.0f * t + 0.7f);
  }

  const auto copied = [&](const std::vector<float>& l, const std::vector<float>& r) {
    const sonare::Audio la = sonare::Audio::from_buffer(l.data(), l.size(), kSr);
    const sonare::Audio ra = sonare::Audio::from_buffer(r.data(), r.size(), kSr);
    return std::max(sonare::mastering::common::measure_true_peak_dbtp(la, kOversample),
                    sonare::mastering::common::measure_true_peak_dbtp(ra, kOversample));
  };

  SECTION("loud program") {
    CHECK(sonare::mastering::common::measure_true_peak_dbtp_stereo_planar(
              left.data(), right.data(), left.size(), kOversample) == copied(left, right));
    CHECK(sonare::mastering::api::detail::stereo_true_peak_dbtp(left, right, kSr, kOversample) ==
          copied(left, right));
  }

  SECTION("the louder channel on either side") {
    CHECK(sonare::mastering::common::measure_true_peak_dbtp_stereo_planar(
              right.data(), left.data(), left.size(), kOversample) == copied(right, left));
  }

  SECTION("both channels under the silence floor report the shared dB floor") {
    const std::vector<float> quiet(kFrames, 0.0f);
    CHECK(sonare::mastering::common::measure_true_peak_dbtp_stereo_planar(
              quiet.data(), quiet.data(), quiet.size(), kOversample) == copied(quiet, quiet));
  }
}

TEST_CASE("sonare_mastering_ab_match_loudness", "[c_api][mastering]") {
  constexpr int kSampleRate = 48000;
  // The two takes are deliberately a different tone AND a different duration.
  // Scaled copies of one tone would make the matched result numerically equal to
  // the reference, and every assertion below would then also hold for a call
  // that returned the reference instead of the matched take.
  std::vector<float> reference = generate_sine(1000.0f, kSampleRate, 2.0f);
  std::vector<float> source = generate_sine(220.0f, kSampleRate, 1.5f);
  for (auto& sample : reference) sample *= 0.5f;
  for (auto& sample : source) sample *= 0.05f;

  SECTION("the matched take measures at the reference's loudness") {
    float* out = nullptr;
    size_t out_length = 0;
    SonareLoudnessMatch match{};
    REQUIRE(sonare_mastering_ab_match_loudness(source.data(), source.size(), reference.data(),
                                               reference.size(), kSampleRate, &out, &out_length,
                                               &match) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == source.size());

    // The reported gain is reference - source by construction, so asserting that
    // relation would only restate the struct. What has to hold is that applying
    // it landed the take where it was aimed: re-measure the output and compare
    // it against the reference's own loudness.
    const sonare::Audio matched = sonare::Audio::from_buffer(out, out_length, kSampleRate);
    CHECK(sonare::mastering::common::measure_lufs(matched) ==
          Catch::Approx(match.reference_lufs).margin(0.05));

    // Landing at the right loudness does not say which take got there. The
    // output has to be the source with the reported gain on it, sample for
    // sample, or a call that returned the reference would satisfy the line above.
    const float gain = sonare::db_to_linear(match.applied_gain_db);
    for (size_t i = 0; i < source.size(); i += source.size() / 8) {
      CHECK(out[i] == Catch::Approx(source[i] * gain).margin(1e-5));
    }

    // The fixture is only a witness if the two takes really differed; at equal
    // loudness the assertions above pass for a function that does nothing.
    CHECK(match.source_lufs < match.reference_lufs - 5.0f);
    CHECK(match.applied_gain_db > 0.0f);

    sonare_free_floats(out);
  }

  SECTION("the measurement struct is optional") {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_ab_match_loudness(source.data(), source.size(), reference.data(),
                                               reference.size(), kSampleRate, &out, &out_length,
                                               nullptr) == SONARE_OK);
    REQUIRE(out_length == source.size());
    sonare_free_floats(out);
  }

  SECTION("a refused call leaves no measurement behind to be read as one") {
    float* out = nullptr;
    size_t out_length = 0;
    SonareLoudnessMatch match{};
    match.applied_gain_db = 99.0f;
    CHECK(sonare_mastering_ab_match_loudness(source.data(), source.size(), reference.data(),
                                             reference.size(), 0, &out, &out_length,
                                             &match) != SONARE_OK);
    CHECK(match.applied_gain_db == 0.0f);
    CHECK(out == nullptr);
    CHECK(out_length == 0);

    match.applied_gain_db = 99.0f;
    CHECK(sonare_mastering_ab_match_loudness(nullptr, source.size(), reference.data(),
                                             reference.size(), kSampleRate, &out, &out_length,
                                             &match) != SONARE_OK);
    CHECK(match.applied_gain_db == 0.0f);

    // The audio-output check comes after the struct is cleared, so refusing on a
    // missing output buffer must still not leave a stale measurement readable.
    match.applied_gain_db = 99.0f;
    CHECK(sonare_mastering_ab_match_loudness(source.data(), source.size(), reference.data(),
                                             reference.size(), kSampleRate, nullptr, &out_length,
                                             &match) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(match.applied_gain_db == 0.0f);
  }
}

TEST_CASE("sonare_eq_non_finite_discard_count reports the state the EQ discarded",
          "[c_api][mastering][non_finite]") {
  SonareEq* eq = sonare_eq_create(48000.0, 512);
  REQUIRE(eq != nullptr);
  // Without an enabled band the EQ holds no recursive state, so its count would
  // stay at zero for a reason unrelated to the entry.
  REQUIRE(sonare_eq_set_band(eq, 0,
                             "{\"type\":\"Peak\",\"frequencyHz\":1000,\"gainDb\":9,"
                             "\"q\":2,\"enabled\":true}") == SONARE_OK);

  // Pre-set to a value the entry must overwrite, so a read that never writes
  // cannot pass as a zero count.
  uint32_t count = 0xDEADu;
  REQUIRE(sonare_eq_non_finite_discard_count(eq, &count) == SONARE_OK);
  REQUIRE(count == 0u);

  constexpr int kBlock = 64;
  std::vector<float> left(kBlock, 0.25f);
  std::vector<float> right(kBlock, 0.25f);
  float* channels[] = {left.data(), right.data()};

  // Control: an ordinary block counts nothing, so the increment below is
  // attributable to the poison rather than to processing at all.
  REQUIRE(sonare_eq_process(eq, channels, 2, kBlock) == SONARE_OK);
  REQUIRE(sonare_eq_non_finite_discard_count(eq, &count) == SONARE_OK);
  REQUIRE(count == 0u);

  // This entry scrubs nothing, unlike the mixer's block entry, so the poison
  // goes in as supplied and the recursive cells behind the band take it.
  left[8] = std::numeric_limits<float>::quiet_NaN();
  right[8] = std::numeric_limits<float>::quiet_NaN();
  REQUIRE(sonare_eq_process(eq, channels, 2, kBlock) == SONARE_OK);
  REQUIRE(sonare_eq_non_finite_discard_count(eq, &count) == SONARE_OK);
  // Both channels lost their cells and the block still adds one. Moving by two
  // here would report a stereo stream as twice as degraded as a mono one for the
  // same defect, over a width the caller passed rather than asked for.
  REQUIRE(count == 1u);

  REQUIRE(sonare_eq_non_finite_discard_count(nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(count == 1u);
  REQUIRE(sonare_eq_non_finite_discard_count(eq, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_eq_destroy(eq);
}

TEST_CASE("sonare_streaming_mastering_chain_non_finite_discard_count reports a lost stage",
          "[c_api][mastering][non_finite]") {
  constexpr int kSampleRate = 48000;
  constexpr int kBlockSize = 128;
  // A tilt shelf keeps recursive cells, which is what a discard is about. The
  // default chain has no such stage, so its count could stay at zero for a
  // reason unrelated to the entry.
  const SonareMasteringParam params[] = {{"eq.tilt.tiltDb", 24.0}};
  SonareStreamingMasteringChain* chain = sonare_streaming_mastering_chain_create_ex(
      params, sizeof(params) / sizeof(params[0]), 0.0f, 0.0f);
  REQUIRE(chain != nullptr);
  REQUIRE(sonare_streaming_mastering_chain_prepare(chain, kSampleRate, kBlockSize, 1) == SONARE_OK);

  // Pre-set to a value the entry must overwrite, so a read that never writes
  // cannot pass as a zero count.
  uint32_t count = 0xDEADu;
  REQUIRE(sonare_streaming_mastering_chain_non_finite_discard_count(chain, &count) == SONARE_OK);
  REQUIRE(count == 0u);

  // Control: an ordinary block counts nothing, so the increment below is
  // attributable to the level rather than to processing at all.
  std::vector<float> mono(kBlockSize, 0.25f);
  REQUIRE(sonare_streaming_mastering_chain_process_mono(chain, mono.data(), mono.size()) ==
          SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_non_finite_discard_count(chain, &count) == SONARE_OK);
  REQUIRE(count == 0u);

  // Finite, so the entry accepts it, and large enough that the shelf's own
  // multiply leaves float range -- the chain refuses a non-finite sample, so
  // what a stage discards is always something the chain produced.
  std::fill(mono.begin(), mono.end(), 3.0e38f);
  REQUIRE(sonare_streaming_mastering_chain_process_mono(chain, mono.data(), mono.size()) ==
          SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_non_finite_discard_count(chain, &count) == SONARE_OK);
  REQUIRE(count == 1u);

  REQUIRE(sonare_streaming_mastering_chain_non_finite_discard_count(nullptr, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(count == 1u);
  REQUIRE(sonare_streaming_mastering_chain_non_finite_discard_count(chain, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_streaming_mastering_chain_destroy(chain);
}

#endif
