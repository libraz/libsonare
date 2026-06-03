/// @file sonare_c_core_test.cpp
/// @brief Core C API tests.

#include "sonare_c_test_helpers.h"
#include "util/json.h"

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

TEST_CASE("sonare_analyze_json", "[c_api]") {
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
    REQUIRE(root.contains("key"));
    REQUIRE(root.contains("timeSignature"));
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
    // Dynamics exposes peakDb/rmsDb; rhythm exposes tempoStability (LOW-59).
    REQUIRE(root["dynamics"].contains("peakDb"));
    REQUIRE(root["dynamics"].contains("rmsDb"));
    REQUIRE(root["rhythm"].contains("tempoStability"));
    REQUIRE(root["melody"].contains("pitches"));

    sonare_free_string(json);
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

  SECTION("rejects a null out pointer") {
    REQUIRE(sonare_analyze_json(samples.data(), samples.size(), 22050, nullptr) ==
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
