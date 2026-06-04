/// @file sonare_c_mastering_test.cpp
/// @brief Mastering C API tests.

#include "sonare_c_test_helpers.h"

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
    REQUIRE(result.latency_samples > 0);
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

  // A valid mode still succeeds.
  SonareMasteringParam good_mode[] = {{"mode", 0.0}};
  REQUIRE(sonare_mastering_apply_processor("repair.denoiseClassical", samples.data(),
                                           samples.size(), 44100, good_mode, 1, &out) == SONARE_OK);
  sonare_free_mastering_result(&out);
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
#endif
