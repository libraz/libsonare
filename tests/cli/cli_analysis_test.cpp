/// @file cli_analysis_test.cpp
/// @brief Tests for the sonare CLI analysis commands.

#include <catch2/catch_approx.hpp>

#include "cli/cli_test_helpers.h"

TEST_CASE("CLI info command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " info " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Duration"));
    REQUIRE_THAT(output, ContainsSubstring("Sample Rate"));
    REQUIRE_THAT(output, ContainsSubstring("Samples"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " info " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"duration\""));
    REQUIRE_THAT(output, ContainsSubstring("\"sample_rate\""));
  }
}

TEST_CASE("CLI applies the offline-input policy to the file it loads", "[cli]") {
  // The CLI built its Audio with load_audio() plus Audio::from_vector, and
  // from_vector checks only sample_rate > 0. That pairing was the one way into
  // the library that skipped the policy every other surface applies, so a float
  // WAV carrying a NaN analysed to quietly null fields and exited 0, and a rate
  // outside [8000, 384000] was accepted here and refused everywhere else --
  // including by this project's own Python CLI, which returned 6 and 5 for the
  // same two files.
  const auto write_float_wav = [](const std::string& path, const std::vector<float>& samples,
                                  uint32_t sample_rate) {
    std::vector<uint8_t> out;
    const auto push_u16 = [&out](uint16_t value) {
      out.push_back(static_cast<uint8_t>(value & 0xFFu));
      out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    };
    const auto push_u32 = [&out](uint32_t value) {
      for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
      }
    };
    const auto push_tag = [&out](const char* tag) {
      for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(tag[i]));
    };
    const auto data_bytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    push_tag("RIFF");
    push_u32(36u + data_bytes);
    push_tag("WAVE");
    push_tag("fmt ");
    push_u32(16u);
    push_u16(3u);  // IEEE float, so a NaN survives the round trip
    push_u16(1u);
    push_u32(sample_rate);
    push_u32(sample_rate * 4u);
    push_u16(4u);
    push_u16(32u);
    push_tag("data");
    push_u32(data_bytes);
    for (const float sample : samples) {
      uint32_t bits = 0;
      std::memcpy(&bits, &sample, sizeof(bits));
      push_u32(bits);
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  };

  std::vector<float> tone(4410);
  for (size_t i = 0; i < tone.size(); ++i) {
    tone[i] = 0.5f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 220.0f *
                              static_cast<float>(i) / 22050.0f);
  }

  SECTION("a non-finite sample is a decode failure, not a null analysis field") {
    const std::string path = unique_temp_path("_policy_nan.wav");
    std::vector<float> poisoned = tone;
    poisoned[100] = std::numeric_limits<float>::quiet_NaN();
    write_float_wav(path, poisoned, 22050);
    auto [code, output] = exec_command(CLI + " bpm " + path + " --json -q");
    std::remove(path.c_str());
    CAPTURE(output);
    REQUIRE(code == 6);  // DecodeFailed, matching the Python CLI
  }

  SECTION("a sample rate outside the supported range is an invalid format") {
    const std::string path = unique_temp_path("_policy_rate.wav");
    write_float_wav(path, tone, 4000);
    // Not an analysis command: even a pure format conversion goes through the
    // same handle, and the Python CLI refuses this file for resample too.
    const std::string out_path = unique_temp_path("_policy_rate_out.wav");
    auto [code, output] = exec_command(CLI + " resample " + path +
                                       " --target-rate 44100 --output " + out_path + " -q");
    std::remove(path.c_str());
    std::remove(out_path.c_str());
    CAPTURE(output);
    REQUIRE(code == 5);  // InvalidFormat, matching the Python CLI
  }

  SECTION("a well-formed file in range still loads") {
    const std::string path = unique_temp_path("_policy_ok.wav");
    write_float_wav(path, tone, 22050);
    auto [code, output] = exec_command(CLI + " info " + path + " --json -q");
    std::remove(path.c_str());
    CAPTURE(output);
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"sample_rate\": 22050"));
  }
}

TEST_CASE("CLI identifies and warns about downmixed stereo input", "[cli]") {
  const std::string path = unique_temp_path("_stereo.wav");
  create_test_stereo_wav(path);

  const std::string cli = get_cli_path();
  auto [info_code, info_output] = exec_command(cli + " info " + path + " --json -q");
  REQUIRE(info_code == 0);
  REQUIRE_THAT(info_output, ContainsSubstring("\"channels\": 2"));

  auto [lufs_code, lufs_output] = exec_command(cli + " lufs " + path + " --json -q");
  REQUIRE(lufs_code == 0);
  REQUIRE_THAT(lufs_output, ContainsSubstring("downmixed to mono"));
  std::remove(path.c_str());
}

TEST_CASE("CLI bpm command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " bpm " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("BPM"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " bpm " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"bpm\""));
  }
}

TEST_CASE("CLI key command", "[.][slow][cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " key " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Key"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " key " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"root\""));
    REQUIRE_THAT(output, ContainsSubstring("\"mode\""));
  }

  SECTION("json candidates output") {
    auto [code, output] = exec_command(CLI + " key " + TEST_WAV + " --json --candidates 3 -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"candidates\""));
    REQUIRE_THAT(output, ContainsSubstring("\"correlation\""));
  }

  SECTION("json candidates output with key options") {
    auto [code, output] = exec_command(CLI + " key " + TEST_WAV +
                                       " --json --candidates 3 --use-hpss "
                                       "--loudness-weighted --high-pass-hz 40 "
                                       "--genre-hint edm --profile edma -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"candidates\""));
    REQUIRE_THAT(output, ContainsSubstring("\"correlation\""));
  }

  SECTION("json modal candidates output") {
    auto [code, output] =
        exec_command(CLI + " key " + TEST_WAV + " --json --candidates 14 --modes all -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"candidates\""));
    REQUIRE_THAT(output, ContainsSubstring("\"mode\": 2"));
  }

  SECTION("text candidates output") {
    auto [code, output] = exec_command(CLI + " key " + TEST_WAV + " --candidates 3 -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Key candidates"));
    REQUIRE_THAT(output, ContainsSubstring("corr"));
  }
}

TEST_CASE("CLI beats command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " beats " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Beat times"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " beats " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("["));
  }
}

TEST_CASE("CLI downbeats command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " downbeats " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Downbeat times"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " downbeats " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("["));
  }
}

TEST_CASE("CLI onsets command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " onsets " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Onset times"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " onsets " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("["));
  }
}

TEST_CASE("CLI chords command", "[.][slow][cli]") {
  create_test_wav(TEST_WAV, 0.5f);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " chords " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Chord"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " chords " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"chords\""));
    REQUIRE_THAT(output, ContainsSubstring("\"progression\""));
  }

  SECTION("json output with advanced chord options") {
    auto [code, output] = exec_command(CLI + " chords " + TEST_WAV +
                                       " --json --nnls --use-hmm --detect-inversions --key-context "
                                       "--key-root C --key-mode major --hmm-beam-width 12 -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"chords\""));
    REQUIRE_THAT(output, ContainsSubstring("\"bass\""));
  }

  SECTION("boolean flag before positional input is not swallowed") {
    auto [code, output] = exec_command(CLI + " chords --nnls " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"chords\""));
  }
}

TEST_CASE("CLI sections command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " sections " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Structure"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " sections " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"form\""));
    REQUIRE_THAT(output, ContainsSubstring("\"sections\""));
  }

  SECTION("section type uses the same canonical spelling analyze does") {
    // Both commands serialize the same enum from the same translation unit, so a
    // consumer matching on `type == "chorus"` must not have to know which one
    // produced the document. `sections` used the human-facing Title-Case
    // rendering, which matched nothing a script written against `analyze` looks
    // for and produced no error to explain it.
    auto [sections_code, sections_output] =
        exec_command(CLI + " sections " + TEST_WAV + " --json -q");
    REQUIRE(sections_code == 0);
    const auto sections_json = sonare::util::json::parse_strict(sections_output);
    REQUIRE_FALSE(sections_json["sections"].as_array().empty());

    std::set<std::string> spellings;
    for (const auto& section : sections_json["sections"].as_array()) {
      spellings.insert(section["type"].as_string());
    }
    for (const std::string& spelling : spellings) {
      CAPTURE(spelling);
      REQUIRE(spelling == to_lowercase(spelling));
      REQUIRE(spelling.find(' ') == std::string::npos);
    }
  }
}

TEST_CASE("CLI timbre command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " timbre " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Timbre Analysis"));
    REQUIRE_THAT(output, ContainsSubstring("Brightness"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " timbre " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"brightness\""));
    REQUIRE_THAT(output, ContainsSubstring("\"warmth\""));
  }
}

TEST_CASE("CLI dynamics command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " dynamics " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Dynamics Analysis"));
    REQUIRE_THAT(output, ContainsSubstring("Peak Level"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " dynamics " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"peak_db\""));
    REQUIRE_THAT(output, ContainsSubstring("\"rms_db\""));
  }
}

TEST_CASE("CLI rhythm command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " rhythm " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Rhythm Analysis"));
    REQUIRE_THAT(output, ContainsSubstring("Time Signature"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " rhythm " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE(payload.size() == 7);
    for (const char* key : {"bpm", "time_signature", "groove_type", "syncopation",
                            "pattern_regularity", "tempo_stability", "beat_intervals"}) {
      REQUIRE(payload.contains(key));
    }
    REQUIRE(payload["bpm"].is_number());
    const auto& time_signature = payload["time_signature"];
    REQUIRE(time_signature.size() == 3);
    for (const char* key : {"numerator", "denominator", "confidence"}) {
      REQUIRE(time_signature.contains(key));
      REQUIRE(time_signature[key].is_number());
    }
    REQUIRE(payload["groove_type"].is_string());
    for (const char* key : {"syncopation", "pattern_regularity", "tempo_stability"}) {
      REQUIRE(payload[key].is_number());
    }
    const auto& intervals = payload["beat_intervals"];
    REQUIRE(intervals.size() == 5);
    REQUIRE(intervals["count"].is_number());
    for (const char* key : {"mean", "std", "min", "max"}) {
      REQUIRE(intervals.contains(key));
      REQUIRE(intervals[key].is_number());
    }
  }
}

TEST_CASE("CLI melody command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " melody " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Melody Analysis"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " melody " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"has_melody\""));
    REQUIRE_THAT(output, ContainsSubstring("\"mean_frequency\""));
  }
}

TEST_CASE("CLI boundaries command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " boundaries " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Structural Boundaries"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " boundaries " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"count\""));
    REQUIRE_THAT(output, ContainsSubstring("\"boundaries\""));
  }

  SECTION("reference-window changes the count and a negative value is refused") {
    // Segments of different pitch and level: the quieter changes fall under the
    // relative threshold of the largest one, unless the reference is the frame itself.
    const std::vector<float> freqs = {220.0f, 440.0f, 330.0f, 880.0f, 262.0f, 523.0f, 349.0f};
    const std::vector<float> gains = {0.5f, 0.5f, 0.1f, 0.5f, 0.05f, 0.5f, 0.1f};
    const int rate = 22050;
    const size_t segment = static_cast<size_t>(3 * rate);
    std::vector<float> samples(segment * freqs.size());
    for (size_t i = 0; i < samples.size(); ++i) {
      const size_t k = i / segment;
      const float t = static_cast<float>(i) / rate;
      samples[i] =
          gains[k] * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freqs[k] * t);
    }
    save_wav(TEST_WAV, samples, rate);
    const std::string base = CLI + " boundaries " + TEST_WAV + " --absolute-threshold 0 --json -q";
    auto [local_code, local_output] = exec_command(base + " --reference-window 0");
    auto [global_code, global_output] = exec_command(base);
    REQUIRE(local_code == 0);
    REQUIRE(global_code == 0);
    const auto local = sonare::util::json::parse_strict(local_output);
    const auto global = sonare::util::json::parse_strict(global_output);
    REQUIRE(local["count"].as_int() != global["count"].as_int());

    auto [bad_code, bad_output] = exec_command(base + " --reference-window -1");
    REQUIRE(bad_code == 3);
  }
}

TEST_CASE("CLI mel command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " mel " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Mel Spectrogram"));
    REQUIRE_THAT(output, ContainsSubstring("Shape"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " mel " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"n_mels\""));
    REQUIRE_THAT(output, ContainsSubstring("\"n_frames\""));
  }

  SECTION("htk flag is accepted and changes the mel filterbank") {
    const std::string options =
        " --n-fft 512 --hop-length 128 --n-mels 40 --fmin 20 --fmax 10000 --json -q";
    auto [slaney_code, slaney_output] = exec_command(CLI + " mel " + TEST_WAV + options);
    auto [htk_code, htk_output] = exec_command(CLI + " mel " + TEST_WAV + " --htk" + options);
    REQUIRE(slaney_code == 0);
    REQUIRE(htk_code == 0);
    const auto slaney = sonare::util::json::parse_strict(slaney_output);
    const auto htk = sonare::util::json::parse_strict(htk_output);
    REQUIRE(slaney["n_mels"].as_int() == 40);
    REQUIRE(htk["n_mels"].as_int() == 40);
    REQUIRE(slaney["stats"]["mean"].as_number() != htk["stats"]["mean"].as_number());
  }
}

TEST_CASE("CLI chroma command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " chroma " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Chromagram"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " chroma " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"n_chroma\""));
    REQUIRE_THAT(output, ContainsSubstring("\"mean_energy\""));
    REQUIRE_THAT(output, ContainsSubstring("\"mean_energy\": ["));
    REQUIRE_THAT(output, !ContainsSubstring("\"mean_energy\": {"));
  }
}

TEST_CASE("CLI chroma text output survives a silent input", "[cli]") {
  // Silent input leaves every pitch-class energy at 0; the bar renderer must not
  // divide by a zero max and cast a NaN to int (undefined behavior; UBSan trap).
  create_test_wav(TEST_WAV, 1.0f, 0.0f);
  auto [code, output] = exec_command(CLI + " chroma " + TEST_WAV + " -q");
  REQUIRE(code == 0);
  REQUIRE_THAT(output, ContainsSubstring("Chromagram"));
}

TEST_CASE("CLI spectral command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " spectral " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Spectral Features"));
    REQUIRE_THAT(output, ContainsSubstring("centroid"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " spectral " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"features\""));
    REQUIRE_THAT(output, ContainsSubstring("\"centroid\""));

    const auto payload = sonare::util::json::parse_strict(output);
    const auto& features = payload["features"];
    for (const char* feature_name :
         {"centroid", "bandwidth", "rolloff", "flatness", "zcr", "rms"}) {
      const auto& stats = features[feature_name];
      REQUIRE(stats.size() == 4);
      for (const char* stat_name : {"mean", "std", "min", "max"}) {
        REQUIRE(stats.contains(stat_name));
        REQUIRE(stats[stat_name].is_number());
      }
    }
  }
}

TEST_CASE("CLI pitch command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " pitch " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Pitch Tracking"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " pitch " + TEST_WAV + " --json -q");
    INFO("CLI output: " << output);
    REQUIRE(code == 0);
    auto [explicit_code, explicit_output] =
        exec_command(CLI + " pitch " + TEST_WAV + " --threshold 0.1 --json -q");
    REQUIRE(explicit_code == 0);
    REQUIRE(output == explicit_output);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE(payload.size() == 6);
    for (const char* key :
         {"algorithm", "n_frames", "voiced_count", "voiced_ratio", "median_f0", "mean_f0"}) {
      REQUIRE(payload.contains(key));
    }
    REQUIRE(payload["algorithm"].is_string());
    REQUIRE(payload["n_frames"].is_number());
    REQUIRE(payload["voiced_count"].is_number());
    REQUIRE(payload["voiced_ratio"].is_number());
    REQUIRE(payload["median_f0"].is_number());
    REQUIRE(payload["mean_f0"].is_number());
  }

  SECTION("with yin algorithm") {
    auto [code, output] = exec_command(CLI + " pitch " + TEST_WAV + " --algorithm yin -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("yin"));
  }

  SECTION("rejects unknown algorithm") {
    auto [code, output] = exec_command(CLI + " pitch " + TEST_WAV + " --algorithm typo -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("invalid value for --algorithm"));
  }

  SECTION("an fmin whose period does not fit the frame is refused, as librosa refuses it") {
    // 22050 / 5 = 4410 samples does not fit a 2048-sample frame. This used to
    // run with the period search silently clipped and report zero frames.
    const std::string short_wav = unique_temp_path("_pitch_infeasible_fmin.wav");
    create_test_wav(short_wav, 0.02f, 440.0f);
    auto [code, output] =
        exec_command(CLI + " pitch " + short_wav + " --fmin 5 --fmax 10 --json -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("fmin=5 is too small for frame_length=2048"));
    std::remove(short_wav.c_str());
  }
}

TEST_CASE("CLI pitch --frames adds the per-frame contour", "[cli][pitch]") {
  // A tone followed by silence, so the contour holds both voiced and unvoiced
  // frames and the null encoding of a non-finite f0 is exercised.
  const std::string wav = unique_temp_path("_pitch_frames.wav");
  {
    std::vector<float> samples(22050 * 2, 0.0f);
    for (size_t i = 0; i < 22050; ++i) {
      samples[i] = 0.5f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 440.0f *
                                   static_cast<float>(i) / 22050.0f);
    }
    save_wav(wav, samples, 22050);
  }

  const auto [plain_code, plain_text] = exec_command(CLI + " pitch " + wav + " --json -q");
  REQUIRE(plain_code == 0);
  REQUIRE(sonare::util::json::parse_strict(plain_text).size() == 6);

  const auto [code, text] = exec_command(CLI + " pitch " + wav + " --frames --json -q");
  INFO(text);
  REQUIRE(code == 0);
  const auto payload = sonare::util::json::parse_strict(text);
  REQUIRE(payload.size() == 10);
  const size_t n_frames = static_cast<size_t>(payload["n_frames"].as_int());
  REQUIRE(n_frames > 0);
  for (const char* key : {"times", "f0", "voiced_flag", "voiced_prob"}) {
    REQUIRE(payload[key].as_array().size() == n_frames);
  }
  // Frame i sits i * hop / sr seconds in; the default hop is 512.
  REQUIRE(payload["times"][1].as_number() == Catch::Approx(512.0 / 22050.0));
  size_t voiced = 0;
  size_t unvoiced_null = 0;
  for (size_t i = 0; i < n_frames; ++i) {
    if (payload["voiced_flag"][i].as_bool()) {
      ++voiced;
      REQUIRE(payload["f0"][i].as_number() == Catch::Approx(440.0).margin(5.0));
    } else if (payload["f0"][i].is_null()) {
      ++unvoiced_null;
    }
  }
  REQUIRE(voiced == static_cast<size_t>(payload["voiced_count"].as_int()));
  REQUIRE(voiced > 0);
  REQUIRE(unvoiced_null > 0);

  // Text mode has no per-frame form, so the flag changes nothing there.
  const auto [text_code, text_output] = exec_command(CLI + " pitch " + wav + " --frames -q");
  REQUIRE(text_code == 0);
  REQUIRE_THAT(text_output, ContainsSubstring("Pitch Tracking"));
  std::remove(wav.c_str());
}

TEST_CASE("CLI onset-env command", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " onset-env " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Onset Strength Envelope"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " onset-env " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"n_frames\""));
    REQUIRE_THAT(output, ContainsSubstring("\"peak_strength\""));
  }
}

TEST_CASE("CLI cqt command", "[.][slow][cli]") {
  create_test_wav(TEST_WAV, 0.5f);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " cqt " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Constant-Q Transform"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " cqt " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"n_bins\""));
    REQUIRE_THAT(output, ContainsSubstring("\"n_frames\""));
  }

  SECTION("global --fmin reaches the cqt handler") {
    // Regression: cqt (like pitch/melody) read the global --fmin/--fmax through
    // args.options, which never receives them, so the flag was silently ignored
    // and the hardcoded default (32.7 Hz) was used regardless.
    auto [code, output] =
        exec_command(CLI + " cqt " + TEST_WAV + " --fmin 100 --n-bins 72 --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"fmin\": 100"));
    // The default still applies when the flag is absent.
    auto [defCode, defOutput] = exec_command(CLI + " cqt " + TEST_WAV + " --json -q");
    REQUIRE(defCode == 0);
    REQUIRE_THAT(defOutput, ContainsSubstring("\"fmin\": 32.7"));
  }
}

TEST_CASE("CLI analyze command", "[.][slow][cli]") {
  create_test_wav(TEST_WAV);

  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " analyze " + TEST_WAV + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("BPM"));
    REQUIRE_THAT(output, ContainsSubstring("Key"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " analyze " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    for (const char* key : {"bpm", "bpm_confidence", "key", "time_signature", "beats", "chords",
                            "sections", "timbre", "dynamics", "rhythm", "form"}) {
      REQUIRE_THAT(output, ContainsSubstring(std::string("\"") + key + "\""));
    }

    const auto payload = sonare::util::json::parse_strict(output);
    const auto& sections = payload["sections"];
    REQUIRE(sections.is_array());
    REQUIRE_FALSE(sections.as_array().empty());
    for (const auto& section : sections.as_array()) {
      const auto type = section["type"].as_string();
      bool canonical = false;
      for (const char* expected : {"intro", "verse", "pre-chorus", "chorus", "bridge",
                                   "instrumental", "outro", "unknown"}) {
        if (type == expected) {
          canonical = true;
          break;
        }
      }
      REQUIRE(canonical);
    }
  }
}

namespace {

// Three sung-like tones with short gaps: A4, C5, and an E5 sung 30 cents sharp.
// 22.05 kHz, 0.93 s in all.
std::string write_vocal_notes_take() {
  const std::string path = unique_temp_path("_vocal_notes.wav");
  constexpr int kRate = 22050;
  const double e5_sharp = 659.2551138257398 * std::pow(2.0, 30.0 / 1200.0);
  const double tones[] = {440.0, 523.2511306011972, e5_sharp};
  std::vector<float> samples;
  for (const double hz : tones) {
    for (int i = 0; i < kRate * 28 / 100; ++i) {
      samples.push_back(
          0.5f * static_cast<float>(std::sin(2.0 * sonare::constants::kPiD * hz * i / kRate)));
    }
    samples.insert(samples.end(), static_cast<size_t>(kRate * 3 / 100), 0.0f);
  }
  save_wav(path, samples, kRate);
  return path;
}

}  // namespace

TEST_CASE("CLI vocal-notes lists the notes a vocal session finds", "[cli][vocal]") {
  const std::string wav = write_vocal_notes_take();
  const auto [code, text] = exec_command(CLI + " vocal-notes " + wav + " --json -q");
  INFO(text);
  REQUIRE(code == 0);
  const auto payload = sonare::util::json::parse_strict(text);
  REQUIRE(payload["sample_rate"].as_int() == 22050);
  REQUIRE(payload["note_count"].as_int() == 3);
  const auto& notes = payload["notes"].as_array();
  REQUIRE(notes.size() == 3);

  const char* names[] = {"A4", "C5", "E5"};
  const double cents[] = {0.0, 0.0, 30.0};
  double previous_end = 0.0;
  for (size_t i = 0; i < notes.size(); ++i) {
    const auto& note = notes[i];
    REQUIRE(note["index"].as_int() == static_cast<int>(i));
    REQUIRE(note["has_pitch"].as_bool());
    REQUIRE(note["note_name"].as_string() == names[i]);
    REQUIRE(note["cents"].as_number() == Catch::Approx(cents[i]).margin(12.0));
    // The seconds are the sample positions over the take's own rate.
    REQUIRE(note["start_sec"].as_number() ==
            Catch::Approx(note["source_start_sample"].as_number() / 22050.0));
    REQUIRE(note["end_sec"].as_number() ==
            Catch::Approx(note["source_end_sample"].as_number() / 22050.0));
    REQUIRE(note["start_sec"].as_number() >= previous_end);
    REQUIRE(note["end_sec"].as_number() > note["start_sec"].as_number());
    previous_end = note["end_sec"].as_number();
    REQUIRE(note["median_hz"].as_number() > 0.0);
    REQUIRE(note["f0_stability"].as_number() >= 0.0);
    REQUIRE_FALSE(note["amplitude"].as_array().empty());
    // The name and the offset are both read off center_midi.
    const double midi = note["center_midi"].as_number();
    REQUIRE(note["cents"].as_number() ==
            Catch::Approx((midi - std::floor(midi + 0.5)) * 100.0).margin(1e-9));
  }

  SECTION("text mode shows the note name and the offset from it") {
    const auto [text_code, text_output] = exec_command(CLI + " vocal-notes " + wav + " -q");
    REQUIRE(text_code == 0);
    REQUIRE_THAT(text_output, ContainsSubstring("Vocal notes: 3"));
    REQUIRE_THAT(text_output, ContainsSubstring("A4"));
    REQUIRE_THAT(text_output, ContainsSubstring("E5"));
    REQUIRE_THAT(text_output, ContainsSubstring("cents"));
  }

  SECTION("a spelled setting reaches the session; an unspelled one is not restated") {
    // Spelling every default is the same as spelling none.
    const auto [same_code, same_text] = exec_command(
        CLI + " vocal-notes " + wav +
        " --segmentation-threshold-cents 50 --min-note-ms 30 --frame-length-samples 2048"
        " --hop-length-samples 512 --fmin-hz 65 --fmax-hz 2093 --yin-threshold 0.1"
        " --voiced-threshold 0.5 --reference-hz 440 --json -q");
    REQUIRE(same_code == 0);
    REQUIRE(same_text == text);

    // Notes shorter than the floor no longer stand alone.
    const auto [long_code, long_text] =
        exec_command(CLI + " vocal-notes " + wav + " --min-note-ms 400 --json -q");
    REQUIRE(long_code == 0);
    REQUIRE(sonare::util::json::parse_strict(long_text)["note_count"].as_int() < 3);

    const auto [uncentered_code, uncentered_text] =
        exec_command(CLI + " vocal-notes " + wav + " --no-centered --json -q");
    REQUIRE(uncentered_code == 0);
    REQUIRE(sonare::util::json::parse_strict(uncentered_text)["note_count"].as_int() == 3);
  }

  SECTION("an out-of-domain setting is a usage error") {
    for (const char* bad :
         {"--segmentation-threshold-cents 0", "--min-note-ms -1", "--frame-length-samples 0",
          "--hop-length-samples 0", "--fmin-hz 0", "--fmax-hz 0", "--yin-threshold 0",
          "--yin-threshold 1.5", "--voiced-threshold 1.5", "--voiced-threshold -0.1",
          "--reference-hz 0"}) {
      const auto [bad_code, bad_text] =
          exec_command(CLI + " vocal-notes " + wav + " " + bad + " -q");
      INFO(bad << ": " << bad_text);
      REQUIRE(bad_code == 2);
    }
    // The effective range is checked, so `--fmin-hz` alone can invert it.
    const auto [inverted_code, inverted_text] =
        exec_command(CLI + " vocal-notes " + wav + " --fmin-hz 3000 -q");
    REQUIRE(inverted_code == 2);
    REQUIRE_THAT(inverted_text, ContainsSubstring("--fmax-hz must be greater than --fmin-hz"));
  }
  std::remove(wav.c_str());
}
