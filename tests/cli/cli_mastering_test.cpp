/// @file cli_mastering_test.cpp
/// @brief Tests for the sonare CLI mastering, repair and mixing commands.

#include "cli/cli_test_helpers.h"

#ifdef SONARE_WITH_MASTERING
TEST_CASE("CLI mastering command", "[cli][mastering]") {
  create_test_wav(TEST_WAV);
  const std::string ref = unique_temp_path("_reference.wav");
  create_test_wav(ref, 3.0f, 880.0f);
  const std::string out = unique_temp_path("_mastered.wav");
  std::remove(out.c_str());

  SECTION("writes mastered wav and reports metadata") {
    auto [code, output] = exec_command(CLI + " mastering " + TEST_WAV + " -o " + out +
                                       " --target-lufs -18 --ceiling-db -1 --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"input_lufs\""));
    REQUIRE_THAT(output, ContainsSubstring("\"output_lufs\""));
    REQUIRE_THAT(output, ContainsSubstring("\"applied_gain_db\""));
    REQUIRE_THAT(output, ContainsSubstring("\"target_lufs\": -18"));

    std::ifstream f(out);
    REQUIRE(f.good());
  }

  SECTION("report JSON always publishes zero latency") {
    const std::string report = unique_temp_path("_mastering_report.json");
    auto [code, output] =
        exec_command(CLI + " mastering " + TEST_WAV + " --report " + report + " --json -q");
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE(payload.contains("latency_samples"));
    REQUIRE(payload["latency_samples"].as_int() == 0);
    REQUIRE(std::ifstream(report).good());

    const std::string preset_report = unique_temp_path("_mastering_preset_report.json");
    auto [preset_code, preset_output] = exec_command(
        CLI + " mastering " + TEST_WAV + " --preset pop --report " + preset_report + " --json -q");
    REQUIRE(preset_code == 0);
    const auto preset_payload = sonare::util::json::parse_strict(preset_output);
    REQUIRE(preset_payload.contains("latency_samples"));
    REQUIRE(preset_payload["latency_samples"].as_int() == 0);
    REQUIRE(std::ifstream(preset_report).good());
    std::remove(report.c_str());
    std::remove(preset_report.c_str());
  }

  SECTION("loudness_target_limited reports whether the ceiling decided the level") {
    // The field is the only machine-readable answer to "did the master reach the
    // delivery target", so it has to be read as a value here rather than as a
    // key of the right type: a constant would satisfy one of these two runs and
    // not the other. The material is a -6 dBFS tone, so a -6 LUFS target under a
    // -3 dBTP ceiling cannot be reached, while -20 LUFS is reached outright.
    const std::string limited_out = unique_temp_path("_ceiling_limited.wav");
    auto [limited_code, limited_output] =
        exec_command(CLI + " mastering " + TEST_WAV + " -o " + limited_out +
                     " --target-lufs -6 --ceiling-db -3 --json -q");
    REQUIRE(limited_code == 0);
    const auto limited_payload = sonare::util::json::parse_strict(limited_output);
    REQUIRE(limited_payload.contains("loudness_target_limited"));
    CHECK(limited_payload["loudness_target_limited"].as_bool());
    // ...and the reason it could not be reached: the output stopped short of the
    // target it was asked for.
    CHECK(limited_payload["output_lufs"].as_number() < -6.5);

    const std::string reached_out = unique_temp_path("_ceiling_clear.wav");
    auto [reached_code, reached_output] =
        exec_command(CLI + " mastering " + TEST_WAV + " -o " + reached_out +
                     " --target-lufs -20 --ceiling-db -1 --json -q");
    REQUIRE(reached_code == 0);
    const auto reached_payload = sonare::util::json::parse_strict(reached_output);
    REQUIRE(reached_payload.contains("loudness_target_limited"));
    CHECK_FALSE(reached_payload["loudness_target_limited"].as_bool());
    CHECK(reached_payload["output_lufs"].as_number() < -19.5);
    CHECK(reached_payload["output_lufs"].as_number() > -20.5);

    std::remove(limited_out.c_str());
    std::remove(reached_out.c_str());
  }

  SECTION("runs preset mastering chain") {
    std::string preset_out = unique_temp_path("_preset_mastered.wav");
    std::remove(preset_out.c_str());
    auto [code, output] = exec_command(CLI + " mastering " + TEST_WAV + " -o " + preset_out +
                                       " --preset pop --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"mode\": \"preset\""));
    REQUIRE_THAT(output, ContainsSubstring("\"preset\": \"pop\""));
    REQUIRE_THAT(output, ContainsSubstring("\"stages\""));

    std::ifstream f(preset_out);
    REQUIRE(f.good());
  }

  SECTION("runs JSON config mastering chain") {
    std::string config_path = unique_temp_path("_chain.json");
    {
      std::ofstream config(config_path);
      config << "{\"version\":1,\"params\":{\"eq.tilt.enabled\":true,"
                "\"eq.tilt.tiltDb\":0.25,\"loudness.enabled\":true,"
                "\"loudness.targetLufs\":-18}}";
    }
    auto [code, output] =
        exec_command(CLI + " mastering " + TEST_WAV + " --config " + config_path + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"mode\": \"config\""));
    REQUIRE_THAT(output, ContainsSubstring("\"stages\""));
  }

  SECTION("runs assistant mastering chain with explanations") {
    auto [code, output] =
        exec_command(CLI + " mastering " + TEST_WAV + " --assistant --explain --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"mode\": \"assistant\""));
    REQUIRE_THAT(output, ContainsSubstring("\"explanation\""));
    REQUIRE_THAT(output, ContainsSubstring("\"stages\""));
  }

  SECTION("lists named processors") {
    auto [code, output] = exec_command(CLI + " mastering-processors --json");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("dynamics.compressor"));
    REQUIRE_THAT(output, ContainsSubstring("eq.equalizer"));
    REQUIRE_THAT(output, ContainsSubstring("stereo.imager"));

    auto [pair_code, pair_output] = exec_command(CLI + " mastering-pair-processors --json");
    REQUIRE(pair_code == 0);
    REQUIRE_THAT(pair_output, ContainsSubstring("match.abCrossfade"));

    auto [pair_analysis_code, pair_analysis_output] =
        exec_command(CLI + " mastering-pair-analyses --json");
    REQUIRE(pair_analysis_code == 0);
    REQUIRE_THAT(pair_analysis_output, ContainsSubstring("match.referenceLoudness"));

    auto [stereo_analysis_code, stereo_analysis_output] =
        exec_command(CLI + " mastering-stereo-analyses --json");
    REQUIRE(stereo_analysis_code == 0);
    REQUIRE_THAT(stereo_analysis_output, ContainsSubstring("stereo.monoCompatCheck"));
  }

  SECTION("runs named processor") {
    auto [code, output] = exec_command(
        CLI + " mastering-processor " + TEST_WAV +
        " --processor dynamics.compressor --params thresholdDb=-24,ratio=1.5 --json -q");
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE(payload.size() == 8);
    for (const char* key : {"processor", "stereo", "input_lufs", "output_lufs", "applied_gain_db",
                            "latency_samples", "sample_rate", "output"}) {
      REQUIRE(payload.contains(key));
    }
    REQUIRE(payload["processor"].as_string() == "dynamics.compressor");
    REQUIRE_FALSE(payload["stereo"].as_bool());
    REQUIRE(payload["sample_rate"].as_int() == 22050);
    REQUIRE(payload["output"].as_string().empty());
  }

  SECTION("rejects malformed parameter entries") {
    auto [code, output] = exec_command(CLI + " mastering-processor " + TEST_WAV +
                                       " --processor dynamics.compressor --params ratio-2 -q");
    REQUIRE(code != 0);
    REQUIRE_THAT(output, ContainsSubstring("expected key=value"));
  }

  SECTION("runs unified equalizer shortcut") {
    auto [code, output] = exec_command(CLI + " eq " + TEST_WAV +
                                       " --frequency-hz 440 --gain-db 3 --q 1 --coeff-mode 1"
                                       " --phase-mode 3 --resolution 1 --auto-gain --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"processor\": \"eq.equalizer\""));
    REQUIRE_THAT(output, ContainsSubstring("\"latency_samples\""));
    REQUIRE_THAT(output, ContainsSubstring("\"latency_samples\": 512"));

    auto [dynamic_code, dynamic_output] =
        exec_command(CLI + " eq " + TEST_WAV +
                     " --frequency-hz 440 --gain-db 3 --q 1 --dynamic"
                     " --threshold-db -36 --ratio 2 --range-db -3 --json -q");
    REQUIRE(dynamic_code == 0);
    REQUIRE_THAT(dynamic_output, ContainsSubstring("\"processor\": \"eq.equalizer\""));

    const auto eq_payload = sonare::util::json::parse_strict(dynamic_output);
    REQUIRE(eq_payload.size() == 7);
    for (const char* key : {"processor", "input_lufs", "output_lufs", "applied_gain_db",
                            "latency_samples", "sample_rate", "output"}) {
      REQUIRE(eq_payload.contains(key));
    }
    REQUIRE(eq_payload["processor"].as_string() == "eq.equalizer");
    REQUIRE(eq_payload["sample_rate"].as_int() == 22050);
    REQUIRE(eq_payload["output"].as_string().empty());

    auto [params_code, params_output] = exec_command(
        CLI + " eq " + TEST_WAV +
        " --params band0.enabled=1 --auto-threshold --sidechain-freq-hz 1000 --sidechain-q 0.7 -q");
    REQUIRE(params_code == 3);
    REQUIRE_THAT(params_output,
                 ContainsSubstring("--auto-threshold cannot be combined with --params"));
  }

  SECTION("runs pair processor and analysis") {
    auto [pair_code, pair_output] =
        exec_command(CLI + " mastering-pair-processor " + TEST_WAV + " --reference " + ref +
                     " --processor match.abCrossfade --params mix=0.25 --json -q");
    REQUIRE(pair_code == 0);
    REQUIRE_THAT(pair_output, ContainsSubstring("\"processor\": \"match.abCrossfade\""));

    auto [analysis_code, analysis_output] =
        exec_command(CLI + " mastering-pair-analyze " + TEST_WAV + " --reference " + ref +
                     " --analysis match.referenceLoudness -q");
    REQUIRE(analysis_code == 0);
    REQUIRE_THAT(analysis_output, ContainsSubstring("\"source_lufs\""));
    REQUIRE_THAT(analysis_output, ContainsSubstring("\"reference_lufs\""));
  }

  SECTION("pair analysis accepts independent source and reference lengths") {
    const std::string short_ref = unique_temp_path("_short_reference.wav");
    create_test_wav(short_ref, 1.0f, 660.0f);
    auto [analysis_code, analysis_output] =
        exec_command(CLI + " mastering-pair-analyze " + TEST_WAV + " --reference " + short_ref +
                     " --analysis match.referenceLoudness -q");
    REQUIRE(analysis_code == 0);
    REQUIRE_THAT(analysis_output, ContainsSubstring("\"source_lufs\""));
    REQUIRE_THAT(analysis_output, ContainsSubstring("\"reference_lufs\""));
  }

  SECTION("runs stereo analysis") {
    auto [code, output] =
        exec_command(CLI + " mastering-stereo-analyze " + TEST_WAV + " --reference " + ref +
                     " --analysis stereo.monoCompatCheck -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"correlation\""));
  }

  SECTION("--target-platform moves the loudness the assistant masters to") {
    // The delivery target is the whole point of the option, so this reads the
    // rendered output loudness rather than a flag round-trip: with the target
    // unreachable from the CLI, someone mastering for EBU R128 broadcast
    // silently got the -14 LUFS streaming convention, a 9 dB error under exit 0.
    const auto master_to = [&](const std::string& platform) {
      const std::string out_path = unique_temp_path("_platform.wav");
      const std::string option = platform.empty() ? "" : " --target-platform " + platform;
      auto [code, output] = exec_command(CLI + " mastering " + TEST_WAV + " --assistant" + option +
                                         " -o " + out_path + " --json -q");
      REQUIRE(code == 0);
      const auto payload = sonare::util::json::parse_strict(output);
      std::remove(out_path.c_str());
      return payload["output_lufs"].as_number();
    };

    const double streaming = master_to("");
    CHECK(streaming < -13.5);
    CHECK(streaming > -14.5);
    // Named explicitly, the default target must land in the same place as the
    // omitted one.
    CHECK(master_to("streaming") == streaming);

    const double broadcast = master_to("broadcast");
    CHECK(broadcast < -22.5);
    CHECK(broadcast > -23.5);

    const double club = master_to("club");
    CHECK(club < -8.5);
    CHECK(club > -9.5);
  }

  SECTION(
      "the assistant controls are refused without --assistant and validated against the table") {
    // Every accepted name comes from the delivery-target table, so an unknown
    // one is a usage error naming the set rather than a silently kept default.
    auto [unknown_code, unknown_output] = exec_command(
        CLI + " mastering " + TEST_WAV + " --assistant --target-platform bogus --json -q");
    REQUIRE(unknown_code == 2);
    REQUIRE_THAT(unknown_output, ContainsSubstring("invalid value for --target-platform"));
    REQUIRE_THAT(unknown_output, ContainsSubstring("broadcast"));

    // These reach an AssistantConfig field and nothing else, so supplying one
    // without --assistant is refused instead of accepted and dropped.
    for (const std::string option :
         {"--target-platform broadcast", "--no-streaming-safe", "--speech-mono-amount 0.5"}) {
      CAPTURE(option);
      auto [code, output] = exec_command(CLI + " mastering " + TEST_WAV + " " + option + " -q");
      REQUIRE(code == 3);
      REQUIRE_THAT(output, ContainsSubstring("requires --assistant"));
    }
  }

  SECTION("--no-streaming-safe reaches the suggester") {
    // prefer_streaming_safe defaults to true, so the reachable control is the one
    // that turns it off. Both settings now select denoise; what the flag decides
    // is the noise estimator, because the default one ranks every frame of the
    // whole signal and a stream has no whole signal. The material has to carry the
    // defect the flag governs: repair is selected from measurement, so on a clean
    // tone neither branch says anything and the flag looks unreachable.
    const std::string noisy = unique_temp_path("_noisy.wav");
    create_noisy_wav(noisy);

    auto [safe_code, safe_output] = exec_command(
        CLI + " mastering " + noisy + " --assistant --enable-repair --explain --json -q");
    REQUIRE(safe_code == 0);
    REQUIRE_THAT(safe_output, ContainsSubstring("streaming-safe was asked for"));
    // The stage list is where this output reports what ran. Which estimator it
    // ran with is not in this JSON at all; assistant_test covers that.
    REQUIRE_THAT(safe_output, ContainsSubstring("\"repair.denoise\""));

    auto [open_code, open_output] =
        exec_command(CLI + " mastering " + noisy +
                     " --assistant --enable-repair --no-streaming-safe --explain --json -q");
    REQUIRE(open_code == 0);
    REQUIRE_THAT(open_output, !ContainsSubstring("streaming-safe was asked for"));
    REQUIRE_THAT(open_output, ContainsSubstring("the noise floor is loud under the programme"));
    REQUIRE_THAT(open_output, ContainsSubstring("\"repair.denoise\""));
    std::remove(noisy.c_str());
  }

  SECTION("a --params key the processor does not read is named and refused") {
    // insert_param_names() reports every key the processor's config builder
    // probes, so a key outside that set took no effect at all: a typo used to
    // ship a chain containing none of the edit the caller asked for, under exit
    // 0 and a normal --json payload.
    const std::string eq_out = unique_temp_path("_eq_params.wav");
    auto [code, output] =
        exec_command(CLI + " eq " + TEST_WAV + " --params band0.bogusKey=42 -o " + eq_out + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("unknown --params key for eq.equalizer"));
    REQUIRE_THAT(output, ContainsSubstring("band0.bogusKey"));

    // A key the processor does read still runs.
    auto [ok_code, ok_output] =
        exec_command(CLI + " eq " + TEST_WAV + " --params band0.gainDb=3 -o " + eq_out + " -q");
    REQUIRE(ok_code == 0);

    // The same check applies to the named-processor entry point.
    auto [processor_code, processor_output] =
        exec_command(CLI + " mastering-processor " + TEST_WAV +
                     " --processor dynamics.compressor --params bogusKey=1 -q");
    REQUIRE(processor_code == 3);
    REQUIRE_THAT(processor_output, ContainsSubstring("unknown --params key"));
    std::remove(eq_out.c_str());
  }
}

TEST_CASE("CLI mastering-processor acts on the image a stereo file carries", "[cli][mastering]") {
  // Two files that are mirror images of each other: A = (L, R), B = (R, L).
  // Their mono downmixes are bit-identical, because 0.5*(L+R) is symmetric, so a
  // handler that processes the downmix cannot produce different output for them.
  // That is the discriminator this needs, and a pair differing only in R is not:
  // changing R changes the downmix too, so the downmixing handler would also
  // answer differently and pass.
  constexpr size_t kFrames = 11025;
  constexpr int kRate = 22050;
  const float two_pi = 2.0f * static_cast<float>(sonare::constants::kPiD);
  std::vector<float> left(kFrames);
  std::vector<float> right(kFrames);
  for (size_t frame = 0; frame < kFrames; ++frame) {
    const float t = static_cast<float>(frame) / kRate;
    left[frame] = 0.6f * std::sin(two_pi * 440.0f * t);
    right[frame] = 0.45f * std::sin(two_pi * 277.0f * t) + 0.2f * std::sin(two_pi * 93.0f * t);
  }
  const auto write_pair = [&](const std::string& path, const std::vector<float>& a,
                              const std::vector<float>& b) {
    std::vector<float> interleaved(2 * kFrames);
    for (size_t frame = 0; frame < kFrames; ++frame) {
      interleaved[2 * frame] = a[frame];
      interleaved[2 * frame + 1] = b[frame];
    }
    save_wav_multichannel(path, interleaved.data(), kFrames, 2, ChannelLayout::Stereo, kRate);
  };
  const std::string input_a = unique_temp_path("_image_a.wav");
  const std::string input_b = unique_temp_path("_image_b.wav");
  write_pair(input_a, left, right);
  write_pair(input_b, right, left);

  const auto read_all = [](const std::string& path) {
    auto [samples, rate, channels] = load_audio_interleaved(path);
    return std::make_pair(samples, channels);
  };

  // The premise, checked rather than assumed: if the two downmixes differed, a
  // difference downstream would prove nothing about the image.
  const std::string mix_a = unique_temp_path("_image_a_mix.wav");
  const std::string mix_b = unique_temp_path("_image_b_mix.wav");
  for (const auto& [in, out] : {std::make_pair(input_a, mix_a), std::make_pair(input_b, mix_b)}) {
    auto [code, ignored] = exec_command(CLI + " gain " + in + " --gain-db 0 -o " + out + " -q");
    REQUIRE(code == 0);
  }
  REQUIRE(read_all(mix_a).first == read_all(mix_b).first);
  std::remove(mix_a.c_str());
  std::remove(mix_b.c_str());

  // One stereo-only processor and one the mono entry point also implements, so
  // this covers both reasons the stereo path is taken.
  for (const std::string processor : {"stereo.imager", "dynamics.compressor"}) {
    CAPTURE(processor);
    const std::string out_a = unique_temp_path("_image_a_out.wav");
    const std::string out_b = unique_temp_path("_image_b_out.wav");
    for (const auto& [in, out] : {std::make_pair(input_a, out_a), std::make_pair(input_b, out_b)}) {
      auto [code, output] = exec_command(CLI + " mastering-processor " + in + " --processor " +
                                         processor + " -o " + out + " --json -q");
      REQUIRE(code == 0);
      REQUIRE_THAT(output, !ContainsSubstring("downmixed to mono"));
    }
    const auto [samples_a, channels_a] = read_all(out_a);
    const auto [samples_b, channels_b] = read_all(out_b);
    CHECK(channels_a == 2);
    CHECK(channels_b == 2);
    CHECK(samples_a != samples_b);
    std::remove(out_a.c_str());
    std::remove(out_b.c_str());
  }
  std::remove(input_a.c_str());
  std::remove(input_b.c_str());
}

TEST_CASE("CLI normalize carries a stereo input through on one gain", "[cli][effects]") {
  // Two channels of one signal a known distance apart. Channel count alone does
  // not separate a stereo normalize from a per-channel one -- both write two
  // channels -- so what is checked is that the distance survives: a per-channel
  // gain would bring the quieter side up to the target as well and erase it.
  constexpr size_t kFrames = 22050;
  constexpr int kRate = 22050;
  constexpr float kRatio = 0.25f;  // the pair sits 12.04 dB apart
  const std::string input = unique_temp_path("_stereo_normalize.wav");
  std::vector<float> interleaved(2 * kFrames);
  const float two_pi = 2.0f * static_cast<float>(sonare::constants::kPiD);
  for (size_t frame = 0; frame < kFrames; ++frame) {
    const float sample = 0.5f * std::sin(two_pi * 440.0f * (static_cast<float>(frame) / kRate));
    interleaved[2 * frame] = sample;
    interleaved[2 * frame + 1] = kRatio * sample;
  }
  save_wav_multichannel(input, interleaved.data(), kFrames, 2, ChannelLayout::Stereo, kRate);

  const auto peak_db = [](const std::vector<float>& samples, int channels, int index) {
    double peak = 0.0;
    for (size_t frame = 0; (frame + 1) * channels <= samples.size(); ++frame) {
      peak = std::max(peak, std::abs(static_cast<double>(samples[frame * channels + index])));
    }
    return 20.0 * std::log10(peak);
  };

  const std::string out = unique_temp_path("_stereo_normalized.wav");
  auto [code, output] = exec_command(CLI + " normalize " + input + " -o " + out +
                                     " --mode peak --target-db -1 --json -q");
  REQUIRE(code == 0);
  REQUIRE_THAT(output, !ContainsSubstring("downmixed to mono"));

  auto [samples, rate, channels] = load_audio_interleaved(out);
  REQUIRE(channels == 2);
  const double left_db = peak_db(samples, channels, 0);
  const double right_db = peak_db(samples, channels, 1);
  const double expected_gap = -20.0 * std::log10(static_cast<double>(kRatio));
  CHECK(std::abs(left_db - -1.0) < 0.01);
  CHECK(std::abs((left_db - right_db) - expected_gap) < 0.02);
  // Named rather than implied by the gap: this is the value a per-channel gain
  // would produce, and it is the reading the assertion above rules out.
  CHECK(right_db < -12.0);
  std::remove(out.c_str());
  std::remove(input.c_str());

  // A mono source keeps the mono writer, so the stereo branch is entered on the
  // file's own channel count rather than on the command being normalize.
  const std::string mono_in = unique_temp_path("_mono_normalize.wav");
  create_test_wav(mono_in, 1.0f, 440.0f, kRate);
  const std::string mono_out = unique_temp_path("_mono_normalized.wav");
  auto [mono_code, mono_output] = exec_command(CLI + " normalize " + mono_in + " -o " + mono_out +
                                               " --mode peak --target-db -1 --json -q");
  REQUIRE(mono_code == 0);
  auto [mono_samples, mono_rate, mono_channels] = load_audio_interleaved(mono_out);
  CHECK(mono_channels == 1);
  CHECK(std::abs(peak_db(mono_samples, 1, 0) - -1.0) < 0.01);
  std::remove(mono_in.c_str());
  std::remove(mono_out.c_str());
}

TEST_CASE("CLI mastering carries a stereo input through as a stereo pair", "[cli][mastering]") {
  // A mono master written to two channels also reports two channels, so the
  // side signal is what separates a stereo output from a duplicated mono one.
  const std::string input = unique_temp_path("_stereo_master.wav");
  constexpr size_t kFrames = 22050;
  constexpr int kRate = 22050;
  std::vector<float> interleaved(2 * kFrames);
  const float two_pi = 2.0f * static_cast<float>(sonare::constants::kPiD);
  for (size_t frame = 0; frame < kFrames; ++frame) {
    const float t = static_cast<float>(frame) / kRate;
    interleaved[2 * frame] = 0.5f * std::sin(two_pi * 440.0f * t);
    interleaved[2 * frame + 1] = 0.3f * std::sin(two_pi * 220.0f * t);
  }
  save_wav_multichannel(input, interleaved.data(), kFrames, 2, ChannelLayout::Stereo, kRate);

  struct StereoContent {
    int channels = 0;
    double side_rms = 0.0;
    bool channels_identical = true;
  };
  const auto measure = [](const std::string& path) {
    auto [samples, rate, channels] = load_audio_interleaved(path);
    StereoContent content;
    content.channels = channels;
    if (channels != 2 || samples.empty()) return content;
    const size_t frames = samples.size() / 2;
    double energy = 0.0;
    for (size_t frame = 0; frame < frames; ++frame) {
      const double side = 0.5 * (samples[2 * frame] - samples[2 * frame + 1]);
      energy += side * side;
      if (samples[2 * frame] != samples[2 * frame + 1]) content.channels_identical = false;
    }
    content.side_rms = std::sqrt(energy / static_cast<double>(frames));
    return content;
  };

  // Every selector, because each drives a different path through the handler:
  // the loudness-only chain, the preset/config chain, the assistant chain, and
  // the loudness chain the --report request composes.
  const std::string report_path = unique_temp_path("_stereo_master_report.json");
  const std::vector<std::pair<std::string, std::string>> runs = {
      {"loudness", " --target-lufs -18"},
      {"preset", " --preset pop"},
      {"assistant", " --assistant"},
      {"report", " --report " + report_path},
  };
  for (const auto& [label, selector] : runs) {
    CAPTURE(label);
    const std::string out = unique_temp_path("_stereo_mastered.wav");
    auto [code, output] =
        exec_command(CLI + " mastering " + input + " -o " + out + selector + " --json -q");
    REQUIRE(code == 0);
    // The downmix warning is main()'s answer to the same question, so a stereo
    // run that still announced a downmix would contradict its own output.
    REQUIRE_THAT(output, !ContainsSubstring("downmixed to mono"));

    const StereoContent content = measure(out);
    CHECK(content.channels == 2);
    CHECK_FALSE(content.channels_identical);
    CHECK(content.side_rms > 0.01);
    std::remove(out.c_str());
  }

  // A mono input keeps the mono writer, and a command that does downmix keeps
  // the warning -- the exception is the mastering leaf's own declared
  // behaviour, not a blanket silence on two-channel input.
  const std::string mono_input = unique_temp_path("_mono_master.wav");
  create_test_wav(mono_input, 1.0f, 440.0f, kRate);
  const std::string mono_out = unique_temp_path("_mono_mastered.wav");
  auto [mono_code, mono_output] = exec_command(CLI + " mastering " + mono_input + " -o " +
                                               mono_out + " --target-lufs -18 --json -q");
  REQUIRE(mono_code == 0);
  auto [mono_samples, mono_rate, mono_channels] = load_audio_interleaved(mono_out);
  CHECK(mono_channels == 1);

  // `pitch-shift` stands in for "a command that does downmix" only because it is
  // currently one. When it learns to carry a pair, move this case to whichever
  // leaf is still mono rather than deleting it -- the assertion is about the
  // warning firing where a leaf has not declared preservation, and it stops
  // being checked at all if the last named example quietly becomes an exception.
  const std::string downmixed_out = unique_temp_path("_downmixed.wav");
  auto [downmix_code, downmix_output] = exec_command(
      CLI + " pitch-shift " + input + " --semitones 3 -o " + downmixed_out + " --json -q");
  REQUIRE(downmix_code == 0);
  REQUIRE_THAT(downmix_output, ContainsSubstring("downmixed to mono"));

  // Shape, not just channel count: the stereo run publishes the keys its mono
  // counterpart publishes. Both come from one writer, which is what keeps their
  // order identical too -- parsing sorts the keys, so only the set is compared
  // here.
  auto [stereo_json_code, stereo_json] =
      exec_command(CLI + " mastering " + input + " --preset pop --json -q");
  auto [mono_json_code, mono_json] =
      exec_command(CLI + " mastering " + mono_input + " --preset pop --json -q");
  REQUIRE(stereo_json_code == 0);
  REQUIRE(mono_json_code == 0);
  const auto stereo_payload = sonare::util::json::parse_strict(stereo_json);
  const auto mono_payload = sonare::util::json::parse_strict(mono_json);
  REQUIRE(stereo_payload.size() == mono_payload.size());
  for (const auto& [key, value] : mono_payload.as_object()) {
    CAPTURE(key);
    REQUIRE(stereo_payload.contains(key));
  }

  std::remove(input.c_str());
  std::remove(mono_input.c_str());
  std::remove(mono_out.c_str());
  std::remove(downmixed_out.c_str());
  std::remove(report_path.c_str());
}

TEST_CASE("CLI repair command", "[cli][mastering][repair]") {
  create_noisy_wav(TEST_WAV);

  SECTION("default path measures, repairs and reports the defects it found") {
    const std::string out = unique_temp_path("_repaired.wav");
    std::remove(out.c_str());
    auto [code, output] =
        exec_command(CLI + " repair " + TEST_WAV + " -o " + out + " --explain --json -q");
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE(payload["mode"].as_string() == "assistant");
    REQUIRE_THAT(output, ContainsSubstring("\"repair.denoise\""));
    // A repair-only chain never turns on a mastering stage, even one the
    // suggester would otherwise have picked for this noisy material.
    REQUIRE_THAT(output, !ContainsSubstring("\"eq."));
    REQUIRE_THAT(output, !ContainsSubstring("\"dynamics."));
    REQUIRE_THAT(output, !ContainsSubstring("\"loudness"));
    REQUIRE(payload.contains("explanation"));
    REQUIRE(payload["defects"]["measured"].as_bool());
    REQUIRE(payload["defects"]["noise_band_measured"].as_bool());
    REQUIRE(payload["defects"].contains("hum_peak_found"));
    // The fixed cross-surface shape carries no loudness fields: applied_gain_db
    // is structurally always 0 (repair never touches loudness) and the LUFS
    // pair would otherwise be the only numbers here that vary with the field
    // this command is not about.
    REQUIRE_FALSE(payload.contains("input_lufs"));
    REQUIRE_FALSE(payload.contains("output_lufs"));
    REQUIRE_FALSE(payload.contains("applied_gain_db"));

    std::ifstream f(out);
    REQUIRE(f.good());
    std::remove(out.c_str());
  }

  SECTION("--preset strips every non-repair stage the named preset turns on") {
    // "speech" enables repair.denoise plus eq.tilt, dynamics.deesser,
    // dynamics.compressor and loudness -- exactly the stages that must not
    // survive into a command that promises never to master.
    const std::string out = unique_temp_path("_preset_repaired.wav");
    std::remove(out.c_str());
    auto [code, output] =
        exec_command(CLI + " repair " + TEST_WAV + " --preset speech -o " + out + " --json -q");
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE(payload["mode"].as_string() == "preset");
    REQUIRE(payload["preset"].as_string() == "speech");
    REQUIRE_THAT(output, ContainsSubstring("\"repair.denoise\""));
    REQUIRE_THAT(output, !ContainsSubstring("\"eq."));
    REQUIRE_THAT(output, !ContainsSubstring("\"dynamics."));
    REQUIRE_THAT(output, !ContainsSubstring("\"loudness"));
    // The report is unconditional: a caller scripting against --json should not
    // have to branch on mode to find out whether it exists.
    REQUIRE(payload["defects"]["measured"].as_bool());

    std::ifstream f(out);
    REQUIRE(f.good());
    std::remove(out.c_str());
  }

  SECTION("a repair that rebuilds peaks past full scale is fitted, not clamped") {
    // Declipping restores the peaks a clipper cut off, so its output routinely
    // exceeds full scale, and this command runs no limiter. The integer writer
    // clamps, which pinned the rebuilt samples back onto the ceiling they had
    // just been rescued from: the stage ran and its result was discarded at the
    // last step.
    const std::string clipped = unique_temp_path("_clipped_in.wav");
    create_overdriven_wav(clipped);
    const std::string out = unique_temp_path("_declipped.wav");
    std::remove(out.c_str());
    auto [code, output] = exec_command(CLI + " repair " + clipped + " -o " + out + " --json -q");
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE_THAT(output, ContainsSubstring("\"repair.declip\""));
    // Reported, not silent: the caller's file is quieter than the chain made it
    // and has to be able to find out by how much.
    REQUIRE(payload.contains("output_gain_db"));
    REQUIRE(payload["output_gain_db"].as_number() < 0.0);

    // The measurement that settles it: the input is pinned at full scale over
    // its clipped run, and the repaired file must not be. Counting the pinned
    // samples rather than reading the peak is what separates "the ceiling
    // moved" from "the waveform came back" -- a clamped output also peaks at
    // full scale, and so does a correctly fitted one.
    const auto pinned_at_full_scale = [](const std::string& path) {
      auto [samples, rate, channels] = load_audio_interleaved(path);
      (void)rate;
      (void)channels;
      size_t pinned = 0;
      for (const float sample : samples) {
        if (std::abs(sample) >= 0.9999f) ++pinned;
      }
      return pinned;
    };
    const size_t before = pinned_at_full_scale(clipped);
    const size_t after = pinned_at_full_scale(out);
    REQUIRE(before > 0);
    REQUIRE(after * 10 < before);

    std::remove(clipped.c_str());
    std::remove(out.c_str());
  }

  SECTION("a repair whose output already fits is written untouched") {
    // The other direction, and the reason the fit is conditional: a file that
    // never exceeds full scale must come back with no gain at all, so the key
    // reports 0 rather than a small correction nobody asked for.
    const std::string out = unique_temp_path("_fits.wav");
    std::remove(out.c_str());
    auto [code, output] = exec_command(CLI + " repair " + TEST_WAV + " -o " + out + " --json -q");
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE(payload["output_gain_db"].as_number() == 0.0);
    std::remove(out.c_str());
  }

  SECTION("--detect measures and reports without writing anything") {
    const std::string out = unique_temp_path("_detect_should_not_exist.wav");
    std::remove(out.c_str());
    auto [code, output] = exec_command(CLI + " repair " + TEST_WAV + " --detect --json -q");
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE(payload["mode"].as_string() == "detect");
    REQUIRE(payload["defects"]["measured"].as_bool());
    REQUIRE(payload["defects"]["noise_floor_dbfs"].as_number() < 0.0);

    std::ifstream f(out);
    REQUIRE_FALSE(f.good());
  }

  SECTION("an input too short for the detectors reports unmeasured, not clean") {
    const std::string tiny = unique_temp_path("_tiny.wav");
    create_test_wav(tiny, 0.02f);
    auto [code, output] = exec_command(CLI + " repair " + tiny + " --detect --json -q");
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE_FALSE(payload["defects"]["measured"].as_bool());
    REQUIRE(payload["defects"].size() == 1);
    std::remove(tiny.c_str());
  }

  SECTION("--output is required unless --detect is given") {
    auto [code, output] = exec_command(CLI + " repair " + TEST_WAV + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--output is required unless --detect is given"));
  }

  SECTION("--explain is refused with --preset or --detect, where nothing was chosen to explain") {
    auto [preset_code, preset_output] =
        exec_command(CLI + " repair " + TEST_WAV + " --preset speech --explain -q -o " + TEST_OUT);
    REQUIRE(preset_code == 3);
    REQUIRE_THAT(preset_output, ContainsSubstring("--explain requires the default"));

    auto [detect_code, detect_output] =
        exec_command(CLI + " repair " + TEST_WAV + " --detect --explain -q");
    REQUIRE(detect_code == 3);
    REQUIRE_THAT(detect_output, ContainsSubstring("--explain requires the default"));
  }

  SECTION("--params overrides must stay inside the repair config") {
    auto [code, output] = exec_command(CLI + " repair " + TEST_WAV +
                                       " --params loudness.enabled=1 "
                                       "-o " +
                                       TEST_OUT + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--params key for repair must start with 'repair.'"));

    auto [ok_code, ok_output] = exec_command(
        CLI + " repair " + TEST_WAV + " --params repair.denoise.enabled=1 -o " + TEST_OUT + " -q");
    REQUIRE(ok_code == 0);
  }
}
#endif

#ifdef SONARE_WITH_MIXING
TEST_CASE("CLI mixing command", "[cli][mixing]") {
  create_test_wav(TEST_WAV);

  SECTION("lists and prints mixer presets") {
    auto [list_code, list_output] = exec_command(CLI + " mixing-presets --json");
    REQUIRE(list_code == 0);
    REQUIRE_THAT(list_output, ContainsSubstring("\"presets\""));
    REQUIRE_THAT(list_output, ContainsSubstring("vocalReverbSend"));

    auto [preset_code, preset_output] =
        exec_command(CLI + " mixing-preset --preset vocalReverbSend --json");
    REQUIRE(preset_code == 0);
    REQUIRE_THAT(preset_output, ContainsSubstring("\"strips\""));
    REQUIRE_THAT(preset_output, ContainsSubstring("\"buses\""));
  }

  SECTION("processes mixer strip") {
    const std::string out = unique_temp_path("_mixed.wav");
    std::remove(out.c_str());
    auto [code, output] = exec_command(CLI + " mix-strip " + TEST_WAV + " -o " + out +
                                       " --input-trim-db 1 --fader-db -3 --pan 0.25 --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"meter\""));
    REQUIRE_THAT(output, ContainsSubstring("\"correlation\""));

    std::ifstream f(out);
    REQUIRE(f.good());
  }
}
#endif
