/// @file cli_effects_test.cpp
/// @brief Tests for the sonare CLI effect, acoustic and transform commands.

#include "analysis/analysis_rate.h"
#include "cli/cli_test_helpers.h"
#include "editing/pitch_editor/f0_provider.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "feature/pitch.h"

TEST_CASE("CLI effect commands require an output destination", "[cli]") {
  // Offline-effect output contract: commands that render audio require -o and
  // report the same invalid-parameter exit code when it is missing.
  create_test_wav(TEST_WAV);

  SECTION("normalize") {
    auto [code, output] = exec_command(CLI + " normalize " + TEST_WAV + " -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--output'"));
  }

  SECTION("resample") {
    auto [code, output] =
        exec_command(CLI + " resample --target-sample-rate 16000 " + TEST_WAV + " -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--output'"));
  }
}

TEST_CASE("CLI resample names both of its rates as sample rates", "[cli]") {
  create_test_wav(TEST_WAV, 0.25f);
  const std::string out = unique_temp_path("_resample_rates.wav");
  auto [code, output] = exec_command(CLI + " resample " + TEST_WAV +
                                     " --target-sample-rate 16000 -o " + out + " --json -q");
  std::remove(out.c_str());
  REQUIRE(code == 0);
  const auto json = sonare::util::json::parse_strict(output);
  CHECK(json["source_sample_rate"].as_int() == 22050);
  CHECK(json["target_sample_rate"].as_int() == 16000);
  CHECK_FALSE(json.contains("source_rate"));
  CHECK_FALSE(json.contains("sample_rate"));
}

TEST_CASE("CLI normalize keeps stereo source order and validates mode first", "[cli]") {
  const std::string input = unique_temp_path("_normalize_stereo_input.wav");
  const std::string output = unique_temp_path("_normalize_stereo_output.wav");
  const std::string invalid_output = unique_temp_path("_normalize_stereo_invalid.wav");
  std::vector<float> source(2 * 128);
  for (size_t frame = 0; frame < 128; ++frame) {
    source[2 * frame] = 0.25f;
    source[2 * frame + 1] = -0.5f;
  }
  save_wav_multichannel(input, source.data(), 128, 2, ChannelLayout::Stereo, 22050);

  const auto [code, message] = exec_command(CLI + " normalize " + input + " -o " + output +
                                            " --mode peak --target-db -1 -q");
  REQUIRE(code == 0);
  const auto [rendered, sample_rate, channels] = load_audio_interleaved(output);
  REQUIRE(sample_rate == 22050);
  REQUIRE(channels == 2);
  REQUIRE(rendered.size() == source.size());
  REQUIRE(rendered[0] > 0.0f);
  REQUIRE(rendered[1] < 0.0f);
  REQUIRE(std::fabs(rendered[1]) > std::fabs(rendered[0]));

  const auto [invalid_code, invalid_message] =
      exec_command(CLI + " normalize " + input + " -o " + invalid_output + " --mode invalid -q");
  REQUIRE(invalid_code == 2);
  REQUIRE_THAT(invalid_message, ContainsSubstring("argument --mode: must be one of peak, rms"));
  REQUIRE_FALSE(std::ifstream(invalid_output).good());

  std::remove(input.c_str());
  std::remove(output.c_str());
  std::remove(invalid_output.c_str());
}

#ifdef SONARE_WITH_FFMPEG
TEST_CASE("CLI normalize preserves retained-layout error ordering", "[cli][ffmpeg]") {
  if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg CLI not found on PATH");
  }

  const std::string mono = unique_temp_path("_layout_mono.aac");
  const std::string stereo = unique_temp_path("_layout_stereo.aac");
  const std::string input = unique_temp_path("_layout_change.ts");
  const std::string valid_output = unique_temp_path("_layout_valid.wav");
  const std::string invalid_output = unique_temp_path("_layout_invalid.wav");
  const auto cleanup = [&] {
    std::remove(mono.c_str());
    std::remove(stereo.c_str());
    std::remove(input.c_str());
    std::remove(valid_output.c_str());
    std::remove(invalid_output.c_str());
  };

  const std::string generate_mono =
      "ffmpeg -loglevel error -f lavfi -i "
      "'sine=frequency=440:sample_rate=22050:duration=0.3' "
      "-ac 1 -c:a aac -b:a 96k -f adts -y " +
      mono;
  const std::string generate_stereo =
      "ffmpeg -loglevel error -f lavfi -i "
      "'sine=frequency=880:sample_rate=22050:duration=0.3' "
      "-ac 2 -c:a aac -b:a 96k -f adts -y " +
      stereo;
  const std::string concatenate = "ffmpeg -loglevel error -i 'concat:" + stereo + "|" + mono +
                                  "' -c copy -f mpegts -y " + input;
  REQUIRE(std::system(generate_mono.c_str()) == 0);
  REQUIRE(std::system(generate_stereo.c_str()) == 0);
  REQUIRE(std::system(concatenate.c_str()) == 0);

  // A valid mode reaches the stereo-plane load and surfaces the deferred retained-layout failure.
  const auto [valid_code, valid_message] =
      exec_command(CLI + " normalize " + input + " -o " + valid_output + " --mode peak -q");
  REQUIRE(valid_code == 6);
  REQUIRE_THAT(
      valid_message,
      ContainsSubstring(
          "FFmpeg: interleaved decode does not support a channel layout change midstream"));

  // Option validation still wins over that deferred decode failure.
  const auto [invalid_code, invalid_message] =
      exec_command(CLI + " normalize " + input + " -o " + invalid_output + " --mode invalid -q");
  REQUIRE(invalid_code == 3);
  REQUIRE_THAT(invalid_message, ContainsSubstring("argument --mode: must be one of peak, rms"));
  REQUIRE_FALSE(std::ifstream(invalid_output).good());
  cleanup();
}
#endif

TEST_CASE("CLI trim-silence keeps the native threshold fallback dynamic", "[cli]") {
  create_test_wav(TEST_WAV);

  auto [default_code, default_output] =
      exec_command(CLI + " trim-silence " + TEST_WAV + " --json -q");
  auto [explicit_code, explicit_output] =
      exec_command(CLI + " trim-silence " + TEST_WAV + " --threshold-db -60 --json -q");
  REQUIRE(default_code == 0);
  REQUIRE(explicit_code == 0);

  const auto default_payload = sonare::util::json::parse_strict(default_output);
  const auto explicit_payload = sonare::util::json::parse_strict(explicit_output);
  REQUIRE(default_payload["threshold_db"].as_number() == -60.0);
  REQUIRE(default_payload["length"].as_int() == explicit_payload["length"].as_int());
}

TEST_CASE("CLI split-silence unions the takes instead of intersecting them", "[cli]") {
  // Take B sounds through the middle of take A's silence, so that stretch is
  // reported only by the union. An intersection would drop it and a cut placed
  // there would land mid-phrase in B.
  const std::string take_a = unique_temp_path("_take_a.wav");
  const std::string take_b = unique_temp_path("_take_b.wav");
  const int sample_rate = 22050;
  create_blocked_level_wav(take_a, {{0.5f, 0.30f}, {0.0f, 0.60f}, {0.5f, 0.30f}}, sample_rate);
  create_blocked_level_wav(
      take_b, {{0.5f, 0.30f}, {0.0f, 0.20f}, {0.5f, 0.15f}, {0.0f, 0.25f}, {0.5f, 0.30f}},
      sample_rate);
  // Middle of the 0.50 s - 0.65 s stretch only take B sounds in.
  const int only_b_sample = static_cast<int>(0.575f * static_cast<float>(sample_rate));

  auto [alone_code, alone_output] = exec_command(CLI + " split-silence " + take_a + " --json -q");
  REQUIRE(alone_code == 0);
  const auto alone = parse_split_silence_json(alone_output);
  REQUIRE_FALSE(split_silence_covers(alone, only_b_sample));

  auto [both_code, both_output] =
      exec_command(CLI + " split-silence " + take_a + " " + take_b + " --json -q");
  REQUIRE(both_code == 0);
  const auto both = parse_split_silence_json(both_output);
  REQUIRE(split_silence_covers(both, only_b_sample));
  // The union adds; it never narrows what one take alone reported.
  for (const auto& range : alone) {
    REQUIRE(split_silence_covers(both, range.first));
    REQUIRE(split_silence_covers(both, range.second - 1));
  }
}

TEST_CASE("CLI split-silence with one take reports what split() reports", "[cli]") {
  const std::string take = unique_temp_path("_single_take.wav");
  create_blocked_level_wav(take, {{0.5f, 0.30f}, {0.0f, 0.40f}, {0.5f, 0.30f}});

  auto [code, output] = exec_command(CLI + " split-silence " + take + " --json -q");
  REQUIRE(code == 0);
  const auto samples = std::get<0>(load_wav(take));
  REQUIRE(parse_split_silence_json(output) == sonare::split(samples, 60.0f, 2048, 512));
}

TEST_CASE("CLI split-silence refuses a take at another sample rate", "[cli]") {
  // Frame indices from two rates are not comparable, so accepting the take would
  // report intervals in units the caller cannot map back onto either of them.
  const std::string take_a = unique_temp_path("_rate_a.wav");
  const std::string take_b = unique_temp_path("_rate_b.wav");
  create_blocked_level_wav(take_a, {{0.5f, 0.30f}, {0.0f, 0.40f}}, 22050);
  create_blocked_level_wav(take_b, {{0.5f, 0.30f}, {0.0f, 0.40f}}, 44100);

  auto [code, output] =
      exec_command(CLI + " split-silence " + take_a + " " + take_b + " --json -q");
  REQUIRE(code == 3);
  REQUIRE_THAT(output, ContainsSubstring("44100"));
  REQUIRE_THAT(output, ContainsSubstring("22050"));
}

TEST_CASE("CLI split-silence --write-takes slices every take at every interval", "[cli]") {
  const std::string take_a = unique_temp_path("_written_a.wav");
  const std::string take_b = unique_temp_path("_written_b.wav");
  const std::string prefix = unique_temp_path("_written_");
  create_blocked_level_wav(take_a, {{0.5f, 0.30f}, {0.0f, 0.40f}, {0.5f, 0.30f}});
  // A different level in the second take, so two takes' slices of one interval
  // cannot compare equal by both carrying take 1.
  create_blocked_level_wav(take_b, {{0.2f, 0.30f}, {0.0f, 0.40f}, {0.2f, 0.30f}});

  auto [code, output] = exec_command(CLI + " split-silence " + take_a + " " + take_b +
                                     " --write-takes " + prefix + " --json -q");
  REQUIRE(code == 0);
  const auto ranges = parse_split_silence_json(output);
  REQUIRE(ranges.size() >= 2);

  for (size_t interval = 0; interval < ranges.size(); ++interval) {
    std::vector<std::vector<float>> slices;
    for (size_t take = 1; take <= 2; ++take) {
      char suffix[32];
      std::snprintf(suffix, sizeof(suffix), "%02zu_%03zu.wav", take, interval + 1);
      const auto written = std::get<0>(load_wav(prefix + suffix));
      REQUIRE(written.size() ==
              static_cast<size_t>(ranges[interval].second - ranges[interval].first));
      slices.push_back(written);
    }
    REQUIRE(slices[0] != slices[1]);
  }
}

#ifdef SONARE_WITH_ACOUSTIC_SIM
TEST_CASE("CLI estimate-room leaves an omitted band count to the library and refuses 0",
          "[cli][argument-contract]") {
  // Argument validation runs before audio loading, so a missing input file
  // (usage error) still proves the option itself was accepted.
  auto [code, output] = exec_command(CLI + " estimate-room --n-octave-bands 6 -q");
  REQUIRE_THAT(output, !ContainsSubstring("Unknown option"));

  auto [zero_code, zero_output] = exec_command(CLI + " estimate-room --n-octave-bands 0 -q");
  REQUIRE(zero_code == 2);
  REQUIRE_THAT(zero_output, ContainsSubstring("--n-octave-bands"));
}

// Zero keeps the library default because that is how the C ABI spells the same
// sentinel (`sonare_c_acoustic.cpp`: `if (config->seed != 0)`), so a zeroed POD and
// an omitted flag agree across the surfaces. A negative has no such counterpart --
// the C field is `unsigned int`, so no other surface can express one -- and the
// handler used to fold it into that same default, leaving `--seed -1` and a
// deliberate `--seed 1` indistinguishable under a green exit.
TEST_CASE("CLI acoustic commands keep zero as the default seed and refuse a negative one",
          "[cli][acoustic]") {
  const std::string synth_options =
      " --length 7 --width 5 --height 3 --absorption 0.2 --ism-order 0"
      " --max-sec 0.3 --sample-rate 16000 --json -q";
  auto run_synthesize = [&](const std::string& label, const std::string& seed_option) {
    const std::string output = unique_temp_path("_seed_synth_" + label + ".wav");
    const auto [code, command_output] =
        exec_command(CLI + " synthesize-rir -o " + output + synth_options + seed_option);
    REQUIRE(code == 0);
    const auto [samples, sample_rate] = load_wav(output);
    REQUIRE(sample_rate == 16000);
    REQUIRE_FALSE(samples.empty());
    std::remove(output.c_str());
    return samples;
  };

  const auto synth_default = run_synthesize("default", "");
  const auto synth_zero = run_synthesize("zero", " --seed 0");
  const auto synth_one = run_synthesize("one", " --seed 1");
  const auto synth_other = run_synthesize("other", " --seed 7");
  REQUIRE(synth_default == synth_zero);
  REQUIRE(synth_default == synth_one);
  REQUIRE(synth_default != synth_other);

  // Named rather than merely non-zero: an exit of 3 with no mention of the option
  // would also pass a bare code check, and the refusal has to be the domain's.
  const auto [synth_negative_code, synth_negative_output] =
      exec_command(CLI + " synthesize-rir -o " + unique_temp_path("_seed_synth_negative.wav") +
                   synth_options + " --seed -1");
  REQUIRE(synth_negative_code == 2);
  REQUIRE_THAT(synth_negative_output, ContainsSubstring("--seed"));

  // The C field is a uint32 and every other surface reaches all of it. This
  // front-end read the value through `int`, so the whole upper half answered
  // "invalid integer value" -- a refusal, but of a seed the library accepts.
  const auto synth_high = run_synthesize("high", " --seed 2147483648");
  const auto synth_top = run_synthesize("top", " --seed 4294967295");
  REQUIRE(synth_high != synth_default);
  REQUIRE(synth_top != synth_default);
  REQUIRE(synth_high != synth_top);

  const auto [synth_over_code, synth_over_output] =
      exec_command(CLI + " synthesize-rir -o " + unique_temp_path("_seed_synth_over.wav") +
                   synth_options + " --seed 4294967296");
  REQUIRE(synth_over_code == 2);
  REQUIRE_THAT(synth_over_output, ContainsSubstring("--seed"));
  // The bound is written out rather than left to the stream's default precision,
  // which rendered it as "4.29497e+09" -- a number the caller cannot type back.
  REQUIRE_THAT(synth_over_output, ContainsSubstring("4294967295"));

  const std::string input = unique_temp_path("_seed_morph_input.wav");
  std::vector<float> impulse(256, 0.0f);
  impulse[0] = 1.0f;
  save_wav(input, impulse, 16000);
  const std::string morph_options =
      " --length 7 --width 5 --height 3 --absorption 0.2 --ism-order 0"
      " --max-sec 0.3 --wet 1 --suppression 0 --json -q";
  auto run_morph = [&](const std::string& label, const std::string& seed_option) {
    const std::string output = unique_temp_path("_seed_morph_" + label + ".wav");
    const auto [code, command_output] =
        exec_command(CLI + " room-morph " + input + " -o " + output + morph_options + seed_option);
    REQUIRE(code == 0);
    const auto [samples, sample_rate] = load_wav(output);
    REQUIRE(sample_rate == 16000);
    REQUIRE_FALSE(samples.empty());
    std::remove(output.c_str());
    return samples;
  };

  const auto morph_default = run_morph("default", "");
  const auto morph_zero = run_morph("zero", " --seed 0");
  const auto morph_one = run_morph("one", " --seed 1");
  const auto morph_other = run_morph("other", " --seed 7");
  REQUIRE(morph_default == morph_zero);
  REQUIRE(morph_default == morph_one);
  REQUIRE(morph_default != morph_other);

  const auto [morph_negative_code, morph_negative_output] =
      exec_command(CLI + " room-morph " + input + " -o " +
                   unique_temp_path("_seed_morph_negative.wav") + morph_options + " --seed -1");
  REQUIRE(morph_negative_code == 2);
  REQUIRE_THAT(morph_negative_output, ContainsSubstring("--seed"));
  std::remove(input.c_str());
}

TEST_CASE("CLI synthesize-rir reports its warning diagnostics and keeps the tail's headroom",
          "[cli][acoustic]") {
  // A --max-sec below the direct sound's own arrival raises three warnings at
  // once, none of which sets has_error: the length was clamped, the cap was
  // raised to fit the direct sound, and the request came back as early
  // reflections with no diffuse tail. Discarding them made a truncated RIR
  // indistinguishable from a complete one under a green exit.
  const std::string truncated = unique_temp_path("_rir_truncated.wav");
  auto [code, output] = exec_command(CLI + " synthesize-rir --max-sec 0.005 -o " + truncated +
                                     " --sample-rate 22050 --json");
  REQUIRE(code == 0);
  for (const char* diagnostic :
       {"acoustic.rir_length_clamped", "acoustic.rir_length_floored", "acoustic.no_late_tail"}) {
    CAPTURE(diagnostic);
    REQUIRE_THAT(output, ContainsSubstring(diagnostic));
  }

  // The diagnostics go to stderr, so the JSON document on stdout stays exactly
  // the payload both CLIs publish.
  const std::string stdout_only = output.substr(output.find('{'));
  const auto payload = sonare::util::json::parse_strict(stdout_only);
  REQUIRE(payload.size() == 3);
  for (const char* key : {"output", "samples", "sample_rate"}) REQUIRE(payload.contains(key));

  // A RIR carries its physical 1/(4*pi*d) attenuation, so its peak sits far
  // below full scale; 16-bit PCM spends roughly 36 dB of the headroom the tail
  // needs, and half the reported samples came back exactly zero.
  REQUIRE(wav_header_bits_per_sample(truncated) == 24);
  std::remove(truncated.c_str());

  // A request the synthesizer can satisfy in full stays silent, so a warning
  // line is evidence about that run rather than boilerplate.
  const std::string complete = unique_temp_path("_rir_complete.wav");
  auto [full_code, full_output] = exec_command(CLI + " synthesize-rir --max-sec 2 -o " + complete +
                                               " --sample-rate 22050 --json");
  REQUIRE(full_code == 0);
  REQUIRE_THAT(full_output, !ContainsSubstring("warning:"));
  REQUIRE(wav_header_bits_per_sample(complete) == 24);
  std::remove(complete.c_str());
}

TEST_CASE("CLI room-morph reports the same warning diagnostics its sibling does",
          "[cli][acoustic]") {
  // The morph synthesizes its target RIR with the same code, so the same clamp
  // fires -- and it was read for Errors and then dropped, so a morph through a
  // room the caller did not ask for exited green and said nothing.
  const std::string input = unique_temp_path("_morph_warn_input.wav");
  std::vector<float> impulse(2048, 0.0f);
  impulse[0] = 1.0f;
  save_wav(input, impulse, 22050);

  const std::string clamped = unique_temp_path("_morph_warn_clamped.wav");
  auto [code, output] = exec_command(CLI + " room-morph " + input + " -o " + clamped +
                                     " --ism-order 99 --max-sec 2 --json");
  REQUIRE(code == 0);
  REQUIRE_THAT(output, ContainsSubstring("acoustic.ism_order_clamped"));

  // stderr, so the JSON document on stdout stays the payload both CLIs publish.
  const std::string stdout_only = output.substr(output.find('{'));
  const auto payload = sonare::util::json::parse_strict(stdout_only);
  for (const char* key : {"output", "samples"}) REQUIRE(payload.contains(key));
  std::remove(clamped.c_str());

  // An order the synthesizer honours stays silent, so the line above is evidence
  // about that run rather than boilerplate.
  const std::string quiet = unique_temp_path("_morph_warn_quiet.wav");
  auto [quiet_code, quiet_output] = exec_command(CLI + " room-morph " + input + " -o " + quiet +
                                                 " --ism-order 2 --max-sec 2 --json");
  REQUIRE(quiet_code == 0);
  REQUIRE_THAT(quiet_output, !ContainsSubstring("warning:"));
  std::remove(quiet.c_str());
  std::remove(input.c_str());
}

TEST_CASE("CLI room-morph keeps a two-channel input stereo", "[cli][acoustic]") {
  const int sample_rate = 48000;
  const size_t frames = 24000;
  std::vector<float> interleaved(2 * frames);
  for (size_t i = 0; i < frames; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    interleaved[2 * i] = 0.3f * std::sin(2.0f * 3.14159265f * 220.0f * t);
    interleaved[2 * i + 1] = 0.3f * std::sin(2.0f * 3.14159265f * 330.0f * t);
  }
  const std::string input = unique_temp_path("_morph_stereo_input.wav");
  save_wav_multichannel(input, interleaved.data(), frames, 2, ChannelLayout::Stereo, sample_rate);
  const std::string output = unique_temp_path("_morph_stereo_output.wav");
  const std::string room =
      " --length 6 --width 4 --height 3 --source-x 1.5 --source-y 1.5 --source-z 1.2"
      " --listener-x 4 --listener-y 2 --listener-z 1.7 --max-sec 0.3 ";

  auto [code, command_output] = exec_command(CLI + " room-morph " + input + " -o " + output + room +
                                             "--receiver-spacing 0.4 --json");
  REQUIRE(code == 0);
  // Nothing was dropped, so the downmix warning must not be printed.
  REQUIRE_THAT(command_output, !ContainsSubstring("downmixed to mono"));
  REQUIRE_THAT(command_output, ContainsSubstring("\"stereo\": true"));

  const auto [samples, rate, channels] = load_audio_interleaved(output);
  REQUIRE(channels == 2);
  REQUIRE(rate == sample_rate);
  double side = 0.0;
  for (size_t i = 0; i + 1 < samples.size(); i += 2) side += std::fabs(samples[i] - samples[i + 1]);
  REQUIRE(side > 0.0);

  // A spacing the room's receiver pair cannot take is refused before any work.
  auto [bad_code, bad_output] = exec_command(CLI + " room-morph " + input + " -o " + output + room +
                                             "--receiver-spacing 4.5");
  REQUIRE(bad_code != 0);
  REQUIRE_THAT(bad_output, ContainsSubstring("--receiver-spacing"));
  std::remove(input.c_str());
  std::remove(output.c_str());
}
#endif

TEST_CASE("CLI lufs --json stays valid JSON on a silent input", "[cli]") {
  // Silence has no level: -inf is written as null. A reading the input is too short for
  // (short-term over 1 s, the loudness range) is not measured, so its key is absent.
  create_test_wav(TEST_WAV, 1.0f, 0.0f);  // frequency 0 => all-zero samples
  auto [code, output] = exec_command(CLI + " lufs " + TEST_WAV + " --json -q");
  REQUIRE(code == 0);
  const auto payload = sonare::util::json::parse_strict(output);
  REQUIRE(payload["integrated_lufs"].is_null());
  REQUIRE(payload["momentary_lufs"].is_null());
  REQUIRE_FALSE(payload.contains("short_term_lufs"));
  REQUIRE_FALSE(payload.contains("loudness_range_lu"));
  // No non-finite tokens leaked into the JSON.
  REQUIRE_THAT(output, !ContainsSubstring("inf"));
  REQUIRE_THAT(output, !ContainsSubstring("nan"));
}

TEST_CASE("CLI rejects non-finite gain and RMS normalization targets", "[cli]") {
  create_test_wav(TEST_WAV);
  const std::string gain_output = unique_temp_path("_gain.wav");
  const std::string normalize_output = unique_temp_path("_normalize.wav");

  auto [gain_code, gain_message] =
      exec_command(CLI + " gain " + TEST_WAV + " -o " + gain_output + " --gain-db nan -q");
  REQUIRE(gain_code == 2);
  REQUIRE_THAT(gain_message, ContainsSubstring("must be finite"));

  auto [normalize_code, normalize_message] =
      exec_command(CLI + " normalize " + TEST_WAV + " -o " + normalize_output +
                   " --mode rms --target-db inf -q");
  REQUIRE(normalize_code == 2);
  REQUIRE_THAT(normalize_message, ContainsSubstring("must be finite"));

  REQUIRE_FALSE(std::ifstream(gain_output).good());
  REQUIRE_FALSE(std::ifstream(normalize_output).good());
}

TEST_CASE("CLI mix-strip preserves stereo input so --width changes the output", "[cli]") {
  const std::string input = unique_temp_path("_stereo.wav");
  const std::string narrow = unique_temp_path("_narrow.wav");
  const std::string wide = unique_temp_path("_wide.wav");
  std::vector<float> input_samples(256 * 2);
  for (size_t frame = 0; frame < 256; ++frame) {
    input_samples[2 * frame] = 0.5f;
    input_samples[2 * frame + 1] = -0.25f;
  }
  save_wav_multichannel(input, input_samples.data(), 256, 2, ChannelLayout::Stereo, 22050);

  auto [narrow_code, narrow_message] =
      exec_command(CLI + " mix-strip " + input + " -o " + narrow + " --width 0 -q");
  REQUIRE(narrow_code == 0);
  auto [wide_code, wide_message] =
      exec_command(CLI + " mix-strip " + input + " -o " + wide + " --width 2 -q");
  REQUIRE(wide_code == 0);

  auto [narrow_samples, narrow_rate, narrow_channels] = load_audio_interleaved(narrow);
  auto [wide_samples, wide_rate, wide_channels] = load_audio_interleaved(wide);
  REQUIRE(narrow_rate == 22050);
  REQUIRE(narrow_channels == 2);
  REQUIRE(wide_rate == 22050);
  REQUIRE(wide_channels == 2);
  REQUIRE(narrow_samples != wide_samples);

  const std::string mono = unique_temp_path("_mono.wav");
  const std::string rejected = unique_temp_path("_rejected.wav");
  create_test_wav(mono);
  auto [mono_code, mono_message] =
      exec_command(CLI + " mix-strip " + mono + " -o " + rejected + " --width 0.5 -q");
  REQUIRE(mono_code == 3);
  REQUIRE_THAT(mono_message, ContainsSubstring("requires a stereo input"));
  REQUIRE_FALSE(std::ifstream(rejected).good());

  // A mono input is carried on both sides of the strip.
  const std::string mono_out = unique_temp_path("_mono_out.wav");
  auto [mono_out_code, mono_out_message] =
      exec_command(CLI + " mix-strip " + mono + " -o " + mono_out + " -q");
  REQUIRE(mono_out_code == 0);
  auto [mono_samples, mono_rate, mono_channels] = load_audio_interleaved(mono_out);
  REQUIRE(mono_channels == 2);
  REQUIRE_FALSE(mono_samples.empty());
  for (size_t frame = 0; frame < mono_samples.size() / 2; ++frame) {
    REQUIRE(mono_samples[2 * frame] == mono_samples[2 * frame + 1]);
  }

  std::remove(input.c_str());
  std::remove(narrow.c_str());
  std::remove(wide.c_str());
  std::remove(mono.c_str());
  std::remove(mono_out.c_str());
}

TEST_CASE("CLI mix-strip --pan-mode takes the kebab spellings in any case", "[cli]") {
  const std::string input = unique_temp_path("_pan_mode.wav");
  std::vector<float> input_samples(256 * 2);
  for (size_t frame = 0; frame < 256; ++frame) {
    input_samples[2 * frame] = 0.5f;
    input_samples[2 * frame + 1] = -0.25f;
  }
  save_wav_multichannel(input, input_samples.data(), 256, 2, ChannelLayout::Stereo, 22050);
  const auto render = [&](const std::string& mode) {
    const std::string out = unique_temp_path("_pan_mode_out.wav");
    auto [code, message] = exec_command(CLI + " mix-strip " + input + " -o " + out +
                                        " --pan 0.5 --pan-mode " + mode + " -q");
    INFO(message);
    REQUIRE(code == 0);
    return std::get<0>(load_audio_interleaved(out));
  };
  const auto stereo_pan = render("stereo-pan");
  REQUIRE(render("STEREO-PAN") == stereo_pan);
  REQUIRE(render("dual-pan") != stereo_pan);
  REQUIRE(render("balance") != stereo_pan);

  for (const char* refused : {"stereopan", "stereoPan", "dualpan", "sideways"}) {
    const std::string out = unique_temp_path("_pan_mode_refused.wav");
    auto [code, message] =
        exec_command(CLI + " mix-strip " + input + " -o " + out + " --pan-mode " + refused + " -q");
    INFO(refused);
    REQUIRE(code == 2);
    REQUIRE_THAT(message, ContainsSubstring("balance, stereo-pan, dual-pan"));
    REQUIRE_FALSE(std::ifstream(out).good());
  }
}

#ifdef SONARE_WITH_MIXING
TEST_CASE("CLI mix renders inputs through a scene preset, tail included", "[cli]") {
  const std::string vocal = unique_temp_path("_mix_vocal.wav");
  const std::string other = unique_temp_path("_mix_other.wav");
  create_test_wav(vocal, 0.5f);
  create_test_wav(other, 0.5f);
  const std::string base = CLI + " mix --preset vocalReverbSend";

  SECTION("a scene alone is loaded and described") {
    auto [code, output] = exec_command(base + " --json -q");
    INFO(output);
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    CHECK(payload["strip_count"].as_number() == 2);
    CHECK(payload["sample_rate"].as_number() == 48000);
    CHECK(payload.find("rendered_samples") == nullptr);
  }

  SECTION("positional inputs are resampled to the mixer rate and rendered") {
    const std::string out = unique_temp_path("_mix_out.wav");
    auto [code, output] = exec_command(base + " --input " + vocal + " --input " + other + " -o " +
                                       out + " --json -q");
    INFO(output);
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    auto [samples, rate, channels] = load_audio_interleaved(out);
    REQUIRE(rate == 48000);
    REQUIRE(channels == 2);
    // The render runs past the inputs by the scene's tail.
    CHECK(payload["rendered_samples"].as_number() == static_cast<double>(samples.size() / 2));
    CHECK(samples.size() / 2 > 24000);
    std::remove(out.c_str());
  }

  SECTION("an input naming a strip the scene lacks is refused") {
    const std::string out = unique_temp_path("_mix_unknown.wav");
    auto [code, output] = exec_command(base + " --input nosuch=" + vocal + " -o " + out + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--input names strip 'nosuch', which the scene does "
                                           "not have"));
    REQUIRE_FALSE(std::ifstream(out).good());
  }

  SECTION("positional inputs must feed every strip") {
    const std::string out = unique_temp_path("_mix_short.wav");
    auto [code, output] = exec_command(base + " --input " + vocal + " -o " + out + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("scene has 2 strips but 1 inputs were given"));
    REQUIRE_FALSE(std::ifstream(out).good());
  }

  SECTION("a scene file that cannot be opened is a file error") {
    auto [code, output] = exec_command(CLI + " mix --scene /nonexistent/scene.json -q");
    REQUIRE(code == 4);
  }

  std::remove(vocal.c_str());
  std::remove(other.c_str());
}

TEST_CASE("CLI mix refuses an incomplete source or render before reading anything",
          "[cli][argument-contract]") {
  const std::string out = unique_temp_path("_mix_refused.wav");
  const std::string unreadable = "/nonexistent/mix-input.wav";
  const std::vector<std::pair<std::string, std::string>> rows = {
      {" mix -q", "mix needs --scene or --preset"},
      {" mix --scene " + unreadable + " --preset vocalReverbSend -q",
       "--scene is mutually exclusive with --preset"},
      {" mix --preset vocalReverbSend --input " + unreadable + " -q",
       "mix with --input requires --output"},
      {" mix --preset vocalReverbSend -o " + out + " -q",
       "mix with --output requires at least one --input"},
  };
  for (const auto& [arguments, refusal] : rows) {
    CAPTURE(arguments);
    auto [code, output] = exec_command(CLI + arguments);
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring(refusal));
  }
  REQUIRE_FALSE(std::ifstream(out).good());
}
#endif  // SONARE_WITH_MIXING

TEST_CASE("CLI filter applies zero phase to fourth-order filters only when requested", "[cli]") {
  const std::string input = unique_temp_path("_impulse.wav");
  const std::string causal_output = unique_temp_path("_causal.wav");
  const std::string zero_phase_output = unique_temp_path("_zero_phase.wav");
  std::vector<float> impulse(4096, 0.0f);
  impulse[2048] = 1.0f;
  save_wav(input, impulse, 22050);

  auto [causal_code, causal_message] =
      exec_command(CLI + " filter " + input + " -o " + causal_output +
                   " --type low-pass --cutoff-hz 1000 --order 4 -q");
  REQUIRE(causal_code == 0);
  auto [zero_phase_code, zero_phase_message] =
      exec_command(CLI + " filter " + input + " -o " + zero_phase_output +
                   " --type low-pass --cutoff-hz 1000 --order 4 --zero-phase -q");
  REQUIRE(zero_phase_code == 0);

  auto [causal, causal_rate] = load_wav(causal_output);
  auto [zero_phase, zero_phase_rate] = load_wav(zero_phase_output);
  REQUIRE(causal_rate == 22050);
  REQUIRE(zero_phase_rate == 22050);
  const auto peak_index = [](const std::vector<float>& signal) {
    size_t index = 0;
    for (size_t i = 1; i < signal.size(); ++i) {
      if (std::abs(signal[i]) > std::abs(signal[index])) index = i;
    }
    return index;
  };
  REQUIRE(peak_index(causal) > 2048);
  REQUIRE(std::abs(static_cast<int>(peak_index(zero_phase)) - 2048) < 5);

  std::remove(input.c_str());
  std::remove(causal_output.c_str());
  std::remove(zero_phase_output.c_str());
}

TEST_CASE("CLI filter takes the library's four response types and refuses any other spelling",
          "[cli]") {
  create_test_wav(TEST_WAV);
  const std::string out = unique_temp_path("_filter_refused.wav");
  for (const char* spelling : {"hp", "lp", "highpass", "lowPass"}) {
    const std::string type = spelling;
    CAPTURE(type);
    auto [code, output] = exec_command(CLI + " filter " + TEST_WAV + " -o " + out + " --type " +
                                       type + " --cutoff-hz 1000 -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(
        output,
        ContainsSubstring("argument --type: must be one of high-pass, low-pass, band-pass, notch"));
  }
  auto [order_code, order_output] = exec_command(CLI + " filter " + TEST_WAV + " -o " + out +
                                                 " --type low-pass --cutoff-hz 1000 --order 3 -q");
  REQUIRE(order_code == 2);
  REQUIRE_THAT(order_output, ContainsSubstring("argument --order: must be one of 2, 4"));
  auto [missing_code, missing_output] =
      exec_command(CLI + " filter " + TEST_WAV + " -o " + out + " --cutoff-hz 1000 -q");
  REQUIRE(missing_code == 2);
  REQUIRE_THAT(missing_output, ContainsSubstring("Missing required option '--type'"));
  REQUIRE_FALSE(std::ifstream(out).good());
}

TEST_CASE("CLI filter requires the frequencies its type reads and refuses the others", "[cli]") {
  create_test_wav(TEST_WAV);
  const std::string out = unique_temp_path("_filter_frequencies.wav");
  const std::pair<const char*, const char*> refusals[] = {
      {"--type high-pass", "--cutoff-hz is required when --type is high-pass or low-pass"},
      {"--type low-pass --cutoff-hz 1000 --center-hz 500",
       "--center-hz does not apply when --type is high-pass or low-pass"},
      {"--type band-pass --center-hz 1000",
       "--bandwidth-hz is required when --type is band-pass or notch"},
      {"--type notch --center-hz 1000 --bandwidth-hz 100 --cutoff-hz 500",
       "--cutoff-hz does not apply when --type is band-pass or notch"},
      {"--type band-pass --center-hz 1000 --bandwidth-hz 100 --order 4",
       "--order must be 2 when --type is band-pass or notch"},
      {"--type low-pass --cutoff-hz 0", "argument --cutoff-hz: must be finite and greater than 0"},
  };
  for (const auto& [arguments, message] : refusals) {
    CAPTURE(arguments);
    auto [code, output] =
        exec_command(CLI + " filter " + TEST_WAV + " -o " + out + " " + arguments + " -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring(message));
  }
  // A corner at or past Nyquist needs the input's rate, so the library refuses it after the read.
  auto [nyquist_code, nyquist_output] = exec_command(CLI + " filter " + TEST_WAV + " -o " + out +
                                                     " --type low-pass --cutoff-hz 30000 -q");
  REQUIRE(nyquist_code == 3);
  REQUIRE_THAT(nyquist_output, ContainsSubstring("cutoff_hz must be finite and in (0, "));
  REQUIRE_FALSE(std::ifstream(out).good());

  auto [code, output] = exec_command(CLI + " filter " + TEST_WAV + " -o " + out +
                                     " --type notch --center-hz 1000 --bandwidth-hz 100 --json -q");
  REQUIRE(code == 0);
  REQUIRE_THAT(output, !ContainsSubstring("cutoff_hz"));
  REQUIRE_THAT(output, ContainsSubstring("\"center_hz\": 1000"));
  std::remove(out.c_str());
}

TEST_CASE("CLI fade takes at least one side and refuses a negative length", "[cli]") {
  create_test_wav(TEST_WAV);
  const std::string out = unique_temp_path("_fade_refused.wav");
  auto [none_code, none_output] = exec_command(CLI + " fade " + TEST_WAV + " -o " + out + " -q");
  REQUIRE(none_code == 2);
  REQUIRE_THAT(none_output, ContainsSubstring("give --fade-in-sec or --fade-out-sec"));
  auto [negative_code, negative_output] =
      exec_command(CLI + " fade " + TEST_WAV + " -o " + out + " --fade-out-sec -1 -q");
  REQUIRE(negative_code == 2);
  REQUIRE_THAT(negative_output,
               ContainsSubstring("argument --fade-out-sec: must be finite and at least 0"));
  REQUIRE_FALSE(std::ifstream(out).good());
}

TEST_CASE("CLI presence flags do not swallow the audio file argument", "[cli]") {
  // Documented order is `<command> [options] <audio_file>`; a presence-only flag
  // like --ir must not consume the following path as its value and leave the
  // input empty (which produced a misleading "Missing audio file").
  create_test_wav(TEST_WAV);
  auto [code, output] = exec_command(CLI + " acoustic --ir " + TEST_WAV + " -q");
  REQUIRE_THAT(output, !ContainsSubstring("Missing audio file"));
  REQUIRE(code == 0);
}

TEST_CASE("CLI acoustic routes into IR analysis only through --ir", "[cli][acoustic]") {
  // The handler left AcousticConfig on its Auto default, so an impulse-like
  // file reached IR analysis with no --ir: the documented mode selector was a
  // no-op, and the command disagreed with sonare_detect_acoustic (which forces
  // blind) and therefore with the Python CLI and every binding.
  const std::string ir_path = unique_temp_path("_ir.wav");
  create_impulse_response_wav(ir_path);

  auto [blind_code, blind_output] = exec_command(CLI + " acoustic " + ir_path + " --json");
  REQUIRE(blind_code == 0);
  // Blind estimation cannot measure EDT or clarity, so it omits those keys
  // rather than reporting a value; the Auto route filled these in instead.
  // Their absence is the statement, so no flag beside them repeats it.
  for (const char* key : {"\"edt\"", "\"c50\"", "\"c80\"", "\"d50\"", "\"edt_bands\"",
                          "\"c50_bands\"", "\"c80_bands\"", "\"is_blind\""}) {
    REQUIRE_THAT(blind_output, !ContainsSubstring(key));
  }

  auto [ir_code, ir_output] = exec_command(CLI + " acoustic --ir " + ir_path + " --json");
  REQUIRE(ir_code == 0);
  REQUIRE_THAT(ir_output, ContainsSubstring("\"c50\": "));
  REQUIRE_THAT(ir_output, ContainsSubstring("\"c50_bands\": "));

  std::remove(ir_path.c_str());
}

TEST_CASE("CLI pitch-shift command", "[cli]") {
  create_test_wav(TEST_WAV);
  std::remove(TEST_OUT.c_str());

  SECTION("shift up") {
    auto [code, output] = exec_command(CLI + " pitch-shift --pitch-semitones 3 " + TEST_WAV +
                                       " -o " + TEST_OUT + " -q");
    REQUIRE(code == 0);

    // Verify output file exists
    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
  }

  SECTION("shift down") {
    auto [code, output] = exec_command(CLI + " pitch-shift --pitch-semitones -3 " + TEST_WAV +
                                       " -o " + TEST_OUT + " -q");
    REQUIRE(code == 0);

    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
  }

  SECTION("missing output file") {
    auto [code, output] =
        exec_command(CLI + " pitch-shift --pitch-semitones 3 " + TEST_WAV + " -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--output'"));
  }

  SECTION("missing semitones") {
    auto [code, output] =
        exec_command(CLI + " pitch-shift " + TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--pitch-semitones'"));
  }
}

TEST_CASE("CLI pitch-correct transposes by the whole requested interval", "[cli]") {
  // The handler fed a synthetic single-frame F0Track to the time-varying
  // correction path, so one hop of the retune IIR applied a fraction of the
  // interval -- about a fifth of it at 44.1 kHz with the default retune speed,
  // and a different fraction at every other sample rate. The command names the
  // interval outright, so the whole of it has to reach the output.
  const std::string input = unique_temp_path("_pitch_correct_in.wav");
  const std::string output_path = unique_temp_path("_pitch_correct_out.wav");
  const int sample_rate = 22050;
  const float source_hz = 220.0f;
  create_test_wav(input, 1.0f, source_hz, sample_rate);

  auto [code, output] =
      exec_command(CLI + " pitch-correct " + input + " --current-midi 60 --target-midi 72 -o " +
                   output_path + " -q");
  REQUIRE(code == 0);

  const auto [samples, rendered_rate] = load_wav(output_path);
  std::remove(input.c_str());
  std::remove(output_path.c_str());
  REQUIRE(rendered_rate == sample_rate);
  REQUIRE_FALSE(samples.empty());

  // Twelve semitones up doubles the frequency. The whole band between the
  // source and the octave is swept rather than the octave alone, because the
  // fraction the defect applied moves with the sample rate -- 297 Hz here, a
  // different tone at 44.1 kHz -- so an assertion naming one wrong frequency
  // would pass on the next rate.
  const float octave = tone_magnitude(samples, rendered_rate, 2.0f * source_hz);
  float strongest_partial = 0.0f;
  float strongest_partial_hz = 0.0f;
  for (float probe = source_hz; probe < 2.0f * source_hz - 20.0f; probe += 2.0f) {
    const float magnitude = tone_magnitude(samples, rendered_rate, probe);
    if (magnitude > strongest_partial) {
      strongest_partial = magnitude;
      strongest_partial_hz = probe;
    }
  }
  INFO("strongest partially corrected tone: " << strongest_partial_hz << " Hz");
  REQUIRE(octave > 4.0f * strongest_partial);
}

TEST_CASE("CLI time-stretch command", "[cli]") {
  create_test_wav(TEST_WAV);
  std::remove(TEST_OUT.c_str());

  SECTION("stretch slower") {
    auto [code, output] =
        exec_command(CLI + " time-stretch --rate 0.8 " + TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 0);

    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
  }

  SECTION("stretch faster") {
    auto [code, output] =
        exec_command(CLI + " time-stretch --rate 1.5 " + TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 0);

    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
  }

  SECTION("missing output file") {
    auto [code, output] = exec_command(CLI + " time-stretch --rate 0.8 " + TEST_WAV + " -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--output'"));
  }

  SECTION("missing rate") {
    auto [code, output] =
        exec_command(CLI + " time-stretch " + TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--rate'"));
  }
}

TEST_CASE("CLI vqt reports the gamma its bandwidths were built from", "[cli]") {
  // TEST_WAV is a per-process temp path, and ctest runs one case per process, so
  // a case that reads it has to write it: relying on a sibling leaves this green
  // under the whole-tag run and red under ctest.
  create_test_wav(TEST_WAV);
  // Left out, gamma is derived from the ERB bandwidths, which depend on bins-per-octave, so the
  // report carries the derived value.
  auto [auto_code, auto_output] = exec_command(CLI + " vqt " + TEST_WAV + " --json -q");
  REQUIRE(auto_code == 0);
  REQUIRE_THAT(auto_output, ContainsSubstring("\"gamma\": 13.19"));

  // A negative gamma is refused before the read rather than read as "derive it".
  auto [negative_code, negative_output] =
      exec_command(CLI + " vqt --gamma -1 " + TEST_WAV + " --json -q");
  REQUIRE(negative_code == 2);
  REQUIRE_THAT(negative_output, ContainsSubstring("argument --gamma:"));

  // An explicit gamma is not a sentinel and is reported unchanged.
  auto [explicit_code, explicit_output] =
      exec_command(CLI + " vqt --gamma 3.5 " + TEST_WAV + " --json -q");
  REQUIRE(explicit_code == 0);
  REQUIRE_THAT(explicit_output, ContainsSubstring("\"gamma\": 3.5"));
}

TEST_CASE("CLI voice-change compensates realtime chain latency", "[cli][voice-change]") {
  // Regression: the native voice-change realtime branch used to write
  // process_block() output directly into an audio.size()-length buffer with no
  // pre-roll/drop compensation, so the tail of the true signal was never
  // flushed and a file shorter than the chain latency came out entirely
  // silent. brightIdol has a nonzero retune+ISP-limiter latency (~1700
  // samples at 48 kHz), so a 20 ms / 960-sample clip is well inside the
  // previously-lost window.
  const std::string short_wav = unique_temp_path("_vc_short.wav");
  const std::string short_out = unique_temp_path("_vc_short_out.wav");
  create_test_wav(short_wav, 0.02f, 220.0f, 48000);

  auto [code, output] = exec_command(CLI + " voice-change --preset brightIdol " + short_wav +
                                     " -o " + short_out + " --json -q");
  REQUIRE(code == 0);
  REQUIRE_THAT(output, ContainsSubstring("\"latency_samples\""));

  auto [in_samples, in_rate] = load_wav(short_wav);
  auto [out_samples, out_rate] = load_wav(short_out);
  std::remove(short_wav.c_str());
  std::remove(short_out.c_str());

  // Output length equals input length: output sample k corresponds to input
  // sample k, not to a delayed/truncated window.
  REQUIRE(out_rate == in_rate);
  REQUIRE(out_samples.size() == in_samples.size());

  // Before the fix this buffer was all (near-)zero because the whole clip
  // fell inside the uncompensated chain latency.
  float peak = 0.0f;
  for (float sample : out_samples) peak = std::max(peak, std::fabs(sample));
  REQUIRE(peak > 0.05f);
}

TEST_CASE("CLI voice-change formant mode", "[cli][voice-change][argument-contract]") {
  const std::string wav = unique_temp_path("_vc_mode.wav");
  const std::string out = unique_temp_path("_vc_mode_out.wav");
  create_test_wav(wav, 0.2f, 220.0f, 22050);

  auto [relative_code, relative_output] =
      exec_command(CLI + " voice-change --pitch-semitones 5 --formant-factor 1.1 " + wav + " -o " +
                   out + " --json -q");
  REQUIRE(relative_code == 0);
  REQUIRE_THAT(relative_output, ContainsSubstring("\"formant_mode\": \"relative\""));

  auto [absolute_code, absolute_output] = exec_command(
      CLI + " voice-change --pitch-semitones 5 --formant-factor 1 --formant-mode absolute " + wav +
      " -o " + out + " --json -q");
  REQUIRE(absolute_code == 0);
  REQUIRE_THAT(absolute_output, ContainsSubstring("\"formant_mode\": \"absolute\""));
  REQUIRE_THAT(absolute_output, ContainsSubstring("\"formant_factor\": 1"));

  // The factor's own range holds in both modes, whatever the shift could carry.
  auto [refused_code, refused_output] = exec_command(
      CLI + " voice-change --pitch-semitones 5 --formant-factor 3 --formant-mode absolute " + wav +
      " -o " + out + " 2>&1");
  REQUIRE(refused_code == 3);
  REQUIRE_THAT(refused_output,
               ContainsSubstring("formant_factor must be finite and in [0.55, 1.65]"));
  // 2^(5/12) = 1.3348, so a warp in [0.55, 1.65] needs a factor in [0.7342, 1.65] at +5 semitones.
  auto [warp_code, warp_output] = exec_command(
      CLI + " voice-change --pitch-semitones 5 --formant-factor 0.6 --formant-mode absolute " +
      wav + " -o " + out + " 2>&1");
  REQUIRE(warp_code == 3);
  REQUIRE_THAT(warp_output, ContainsSubstring("[0.7342, 1.65]"));

  auto [unknown_code, unknown_output] =
      exec_command(CLI + " voice-change --formant-mode sideways " + wav + " -o " + out + " 2>&1");
  REQUIRE(unknown_code == 2);

  auto [preset_code, preset_output] =
      exec_command(CLI + " voice-change --preset brightIdol --formant-mode absolute " + wav +
                   " -o " + out + " 2>&1");
  REQUIRE(preset_code == 2);
  REQUIRE_THAT(preset_output, ContainsSubstring("cannot be combined with a realtime preset"));

  std::remove(wav.c_str());
  std::remove(out.c_str());
}

TEST_CASE("CLI voice-change rejects a realtime preset document with an unknown field",
          "[cli][voice-change][argument-contract]") {
  // A mistyped section name is a format error on voice-change, as on voice-preset-validate,
  // never a render with defaults.
  create_test_wav(TEST_WAV, 0.05f, 220.0f, 48000);
  std::remove(TEST_OUT.c_str());

  SECTION("typo'd dsp section name") {
    const std::string preset_path = unique_temp_path("_vc_typo_preset.json");
    {
      std::ofstream preset(preset_path);
      REQUIRE(preset.good());
      preset << R"json({"schemaVersion":4,"id":"typo-test","name":"Typo Test","category":"custom",
        "dsp":{"inputGainDb":0,"outputGainDb":0,"wetMix":1,
          "retune":{"pitchSemitones":0,"mix":0},
          "formnt":{"factor":1,"amount":0,"body":0,"brightness":0,"nasal":0},
          "eq":{"highPassHz":75,"bodyDb":0,"presenceDb":0,"airDb":0},
          "gate":{"thresholdDb":-55,"attackMs":2,"releaseMs":100,"rangeDb":18},
          "compressor":{"thresholdDb":-22,"ratio":2.5,"attackMs":6,"releaseMs":90,"makeupGainDb":1},
          "deesser":{"frequencyHz":7200,"thresholdDb":-28,"ratio":4,"rangeDb":8},
          "reverb":{"mix":0.04,"timeMs":320,"damping":0.55,"seed":0},
          "limiter":{"ceilingDb":-1,"releaseMs":50}}})json";
    }

    auto [code, output] = exec_command(CLI + " voice-change --preset-json " + preset_path + " " +
                                       TEST_WAV + " -o " + TEST_OUT + " -q");
    std::remove(preset_path.c_str());
    // A refused preset document is a format error, as the C ABI reports it.
    REQUIRE(code == 5);
    REQUIRE_THAT(output, ContainsSubstring("unknown key dsp.formnt"));
    std::ifstream f(TEST_OUT);
    REQUIRE_FALSE(f.good());
  }

  SECTION("a preset at an earlier schemaVersion") {
    const std::string preset_path = unique_temp_path("_vc_earlier_preset.json");
    {
      std::ofstream preset(preset_path);
      REQUIRE(preset.good());
      preset << R"json({"schemaVersion":2,"id":"earlier","name":"Earlier",
        "category":"custom","macros":{"brightness":0.2}})json";
    }
    auto [code, output] = exec_command(CLI + " voice-change --preset-json " + preset_path + " " +
                                       TEST_WAV + " -o " + TEST_OUT + " -q");
    std::remove(preset_path.c_str());
    REQUIRE(code == 5);
    REQUIRE_THAT(output,
                 ContainsSubstring("schemaVersion must be 4, the version this release reads; "
                                   "the document was written for an older schema"));
    std::ifstream f(TEST_OUT);
    REQUIRE_FALSE(f.good());
  }

  SECTION("typo'd --set macro key") {
    const std::string preset_path = unique_temp_path("_vc_typo_macro_preset.json");
    {
      std::ofstream preset(preset_path);
      REQUIRE(preset.good());
      preset << R"json({"schemaVersion":4,"id":"macro-typo-test","name":"Macro Typo Test",
        "category":"custom","macros":{"brightness":0.2}})json";
    }
    auto [code, output] =
        exec_command(CLI + " voice-change --preset-json " + preset_path +
                     " --set macros.brighness=0.8 " + TEST_WAV + " -o " + TEST_OUT + " -q");
    std::remove(preset_path.c_str());
    REQUIRE(code != 0);
    REQUIRE_THAT(output, ContainsSubstring("macros.brighness"));
    std::ifstream f(TEST_OUT);
    REQUIRE_FALSE(f.good());
  }
}

TEST_CASE("CLI hpss command", "[cli]") {
  create_test_wav(TEST_WAV);
  std::string out_base = unique_temp_path("_hpss");
  std::remove((out_base + "_harmonic.wav").c_str());
  std::remove((out_base + "_percussive.wav").c_str());

  SECTION("default separation") {
    auto [code, output] = exec_command(CLI + " hpss " + TEST_WAV + " -o " + out_base + " -q");
    REQUIRE(code == 0);

    std::ifstream h(out_base + "_harmonic.wav");
    std::ifstream p(out_base + "_percussive.wav");
    REQUIRE(h.good());
    REQUIRE(p.good());
  }

  SECTION("harmonic only") {
    std::string out = unique_temp_path("_hpss_h.wav");
    std::remove(out.c_str());
    auto [code, output] =
        exec_command(CLI + " hpss --harmonic-only " + TEST_WAV + " -o " + out + " -q");
    REQUIRE(code == 0);

    std::ifstream f(out);
    REQUIRE(f.good());
  }

  SECTION("percussive only") {
    std::string out = unique_temp_path("_hpss_p.wav");
    std::remove(out.c_str());
    auto [code, output] =
        exec_command(CLI + " hpss --percussive-only " + TEST_WAV + " -o " + out + " -q");
    REQUIRE(code == 0);

    std::ifstream f(out);
    REQUIRE(f.good());
  }

  SECTION("missing output file") {
    auto [code, output] = exec_command(CLI + " hpss " + TEST_WAV + " -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--output'"));
  }
}

TEST_CASE("CLI scale-quantize reads a mode name where it reads a mask", "[cli][scale-quantize]") {
  const auto quantize = [](const std::string& options) {
    return exec_command(CLI + " scale-quantize 63.4 " + options + " --json -q");
  };

  // Dorian is {0,2,3,5,7,9,10}: 1709. A name is another spelling of its mask, so
  // the two commands must answer identically, and not the way the default does.
  const auto [name_code, by_name] = quantize("--mode-mask dorian");
  const auto [number_code, by_number] = quantize("--mode-mask 1709");
  const auto [default_code, by_default] = quantize("");
  REQUIRE(name_code == 0);
  REQUIRE(number_code == 0);
  REQUIRE(default_code == 0);
  REQUIRE(by_name == by_number);
  REQUIRE(by_name != by_default);
  // Integer spellings are Python's int(text, 0): prefixes and underscores in,
  // a decimal with a leading zero out.
  for (const char* spelling : {"0x6AD", "0X6ad", "0o3255", "0b11010101101", "1_709", " 1709 "}) {
    INFO(spelling);
    REQUIRE(quantize(std::string("--mode-mask '") + spelling + "'").second == by_number);
  }
  REQUIRE(quantize("--mode-mask 010").first == 2);
  REQUIRE(quantize("--mode-mask 0x1000").first == 2);
  const auto [upper_code, by_upper_name] = quantize("--mode-mask DORIAN");
  REQUIRE(upper_code == 0);
  REQUIRE(by_upper_name == by_name);

  SECTION("an unknown name is a usage error that names the flag") {
    const auto [code, output] = quantize("--mode-mask bogus");
    REQUIRE(code == 2);
    REQUIRE_THAT(output,
                 ContainsSubstring("argument --mode-mask: must be an integer mask or one of "
                                   "major, maj, minor, min, m, dorian, phrygian, lydian, "
                                   "mixolydian, locrian"));
    REQUIRE_THAT(output, !ContainsSubstring("bogus"));
  }

  SECTION("an integer outside the twelve-bit range is a usage error") {
    REQUIRE(quantize("--mode-mask 0").first == 2);
    REQUIRE(quantize("--mode-mask 4096").first == 2);
  }

  SECTION("an explicit anchor of 0 is used as given and agrees with the default") {
    // The anchor only fixes which integers are candidates, so 0 and 69 are the
    // same grid. Neither is a sentinel: the flag is either given or absent.
    const auto [zero_code, zero] = quantize("--reference-midi 0");
    REQUIRE(zero_code == 0);
    REQUIRE(zero == by_default);
    // A fractional anchor moves the grid, which a swallowed 0 could not show.
    const auto [shifted_code, shifted] = quantize("--reference-midi 0.25");
    REQUIRE(shifted_code == 0);
    REQUIRE(shifted != by_default);
  }
}

TEST_CASE("CLI pitch-correct-timevarying exposes the correction parameters",
          "[cli][pitch-correct-timevarying]") {
  const std::string input = unique_temp_path("_tv_in.wav");
  create_test_wav(input, 1.5f, 440.0f);
  const auto run = [&](const std::string& options, const std::string& output) {
    return exec_command(CLI + " pitch-correct-timevarying " + input + " -o " + output + " " +
                        options + " --json -q");
  };
  const auto render = [&](const std::string& options) {
    const std::string output = unique_temp_path("_tv_out.wav");
    const auto [code, text] = run(options, output);
    INFO(text);
    REQUIRE(code == 0);
    auto samples = std::get<0>(load_wav(output));
    std::remove(output.c_str());
    return samples;
  };

  // A semitone up, so the correction is the whole of the difference.
  const std::string toward_c5 = "--mode midi --target-midi 70";
  const auto full = render(toward_c5);
  REQUIRE(render(toward_c5 + " --retune-amount 1 --max-correction-semitones 12 "
                             "--retune-speed-ms 50 --vibrato-threshold-cents 20") == full);

  SECTION("each parameter reaches the corrector") {
    REQUIRE(render(toward_c5 + " --retune-amount 0") != full);
    REQUIRE(render(toward_c5 + " --max-correction-semitones 0.25") != full);
    REQUIRE(render(toward_c5 + " --retune-speed-ms 400") != full);
    REQUIRE(render(toward_c5 + " --vibrato-threshold-cents 200") != full);
  }

  SECTION("values outside the library's ranges are invalid parameters") {
    const std::string output = unique_temp_path("_tv_bad.wav");
    for (const char* option :
         {"--retune-amount 1.5", "--retune-amount -0.1", "--max-correction-semitones -1",
          "--retune-speed-ms -1", "--vibrato-threshold-cents -1"}) {
      INFO(option);
      REQUIRE(run(option, output).first == 2);
    }
    std::remove(output.c_str());
  }

  SECTION("a mode name selects the scale") {
    // MIDI 63.4: E-flat is in dorian and not in major, so a name that selected
    // nothing would leave the two scales rendering alike.
    create_test_wav(input, 1.5f, 440.0f * std::pow(2.0f, (63.4f - 69.0f) / 12.0f));
    const std::string scale = "--mode scale --scale-root 0 ";
    const auto dorian = render(scale + "--scale-mode-mask dorian");
    REQUIRE(dorian == render(scale + "--scale-mode-mask 1709"));
    REQUIRE(dorian != render(scale));
    const std::string output = unique_temp_path("_tv_bad.wav");
    const auto [code, text] = run("--scale-mode-mask bogus", output);
    REQUIRE(code == 2);
    REQUIRE_THAT(text, ContainsSubstring("--scale-mode-mask"));
    std::remove(output.c_str());
  }

  SECTION("an anchor of 0 is the same grid as the default") {
    const std::string scale = "--mode scale ";
    REQUIRE(render(scale + "--reference-midi 0") == render(scale));
  }

  SECTION("the help says the scale target is chosen per frame") {
    const auto [code, text] = exec_command(CLI + " pitch-correct-timevarying --help");
    REQUIRE(code == 0);
    REQUIRE_THAT(text, ContainsSubstring("chosen per frame"));
    REQUIRE_THAT(text, ContainsSubstring("--retune-amount"));
  }

  std::remove(input.c_str());
}

TEST_CASE("CLI pitch-correct-timevarying corrects against the unrefined pYIN track",
          "[cli][pitch-correct-timevarying]") {
  // The Python front-end feeds the public pitch_pyin track to the corrector, so
  // the native one must not substitute the editing provider's refined contour.
  const std::string input = unique_temp_path("_tv_raw_in.wav");
  const std::string output = unique_temp_path("_tv_raw_out.wav");
  create_test_wav(input, 1.5f, 452.0f);
  const auto [code, text] = exec_command(CLI + " pitch-correct-timevarying " + input + " -o " +
                                         output + " --mode midi --target-midi 70 --json -q");
  INFO(text);
  REQUIRE(code == 0);
  const auto actual = std::get<0>(load_wav(output));
  auto [samples, sample_rate] = load_wav(input);
  std::remove(input.c_str());
  std::remove(output.c_str());
  const Audio audio = Audio::from_vector(std::move(samples), sample_rate);

  PitchConfig pitch_config;
  pitch_config.frame_length = window_length_at_rate(pitch_config.frame_length, sample_rate);
  const PitchResult pyin_result = pyin(audio, pitch_config);
  editing::pitch_editor::F0Track raw_track;
  raw_track.f0_hz = pyin_result.f0;
  raw_track.voiced_prob = pyin_result.voiced_prob;
  raw_track.voiced = pyin_result.voiced_flag;
  raw_track.hop_length = pitch_config.hop_length;
  raw_track.sample_rate = sample_rate;
  const editing::pitch_editor::F0Track refined_track =
      editing::pitch_editor::PyinF0Provider(pitch_config).detect(audio);

  const editing::pitch_editor::PitchCorrector corrector{
      editing::pitch_editor::PitchCorrectionConfig{}};
  const Audio from_raw = corrector.correct_to_midi_timevarying(audio, raw_track, 70.0f);
  const Audio from_refined = corrector.correct_to_midi_timevarying(audio, refined_track, 70.0f);
  REQUIRE(actual.size() == from_raw.size());
  REQUIRE(from_refined.size() == from_raw.size());

  // The output is 16-bit PCM, so it matches the in-process render to within one step.
  constexpr float kPcmStep = 1.0f / 32768.0f;
  float raw_error = 0.0f;
  float refined_gap = 0.0f;
  for (size_t i = 0; i < actual.size(); ++i) {
    raw_error = std::max(raw_error, std::abs(actual[i] - from_raw[i]));
    refined_gap = std::max(refined_gap, std::abs(from_refined[i] - from_raw[i]));
  }
  CAPTURE(raw_error, refined_gap);
  REQUIRE(refined_gap > 100.0f * kPcmStep);
  REQUIRE(raw_error <= 2.0f * kPcmStep);
}

TEST_CASE("CLI effect and note-editing refusals are decided from argv before the input is read",
          "[cli][argument-contract]") {
  // The input does not exist: a refusal reached only after the read would exit 4.
  const std::string missing = "/nonexistent/argv-refusal-input.wav";
  const std::string out = unique_temp_path("_argv_refusal.wav");
  struct Row {
    std::string argv;
    std::string text;
  };
  const std::vector<Row> rows = {
      {"hpss --harmonic-only --percussive-only " + missing + " -o " + out,
       "hpss output modes are mutually exclusive: choose one of --harmonic-only, "
       "--percussive-only, or --with-residual"},
      {"trim-silence --threshold-db -50 --top-db 30 " + missing,
       "--threshold-db and --top-db are mutually exclusive"},
      {"decompose-stems --hop-length 0 " + missing + " -o " + out,
       "argument --hop-length: must be an integer in [1, 2147483647]"},
      {"decompose-stems --n-components 0 " + missing + " -o " + out,
       "argument --n-components: must be an integer in [1, 2147483647]"},
      {"decompose-stems --n-iter 0 " + missing + " -o " + out,
       "argument --n-iter: must be an integer in [1, 2147483647]"},
      {"decompose-stems --n-fft 0 " + missing + " -o " + out,
       "argument --n-fft: must be an integer in [1, 2147483647]"},
      {"decompose-stems --mask-power 0.5 " + missing + " -o " + out,
       "argument --mask-power: must be finite and at least 1"},
      {"voice-change --preset neutralMonitor --preset-json x.json " + missing + " -o " + out,
       "voice-change preset selectors are mutually exclusive: choose one of --preset, "
       "--preset-json, or --preset-pack"},
      {"voice-change --preset-pack p.json --preset-json x.json " + missing + " -o " + out,
       "--preset-pack and --preset-json are mutually exclusive"},
      {"voice-change --preset-pack p.json " + missing + " -o " + out,
       "--preset-pack requires --preset to select an entry"},
      {"voice-change --preset neutralMonitor --pitch-semitones 2 " + missing + " -o " + out,
       "cannot be combined with a realtime preset"},
      {"voice-change --set a=1 " + missing + " -o " + out,
       "--set requires --preset, --preset-json, or --preset-pack"},
      {"voice-change --preset neutralMonitor --set nopath " + missing + " -o " + out,
       "invalid --set assignment: nopath"},
      {"voice-change --preset neutralMonitor --set =1 " + missing + " -o " + out,
       "invalid --set assignment: =1"},
      {"voice-change --preset neutralMonitor --set ..=1 " + missing + " -o " + out,
       "empty --set path"},
      {"voice-preset-validate --set nopath p.json", "invalid --set assignment: nopath"},
      {"voice-preset-validate --set ..=1 p.json", "empty --set path"},
#ifdef SONARE_WITH_VOICE_CHANGER
      {"voice-change --preset noSuchPreset " + missing + " -o " + out,
       "argument --preset: must be one of neutralMonitor"},
      {"voice-preset --preset noSuchPreset", "argument --preset: must be one of neutralMonitor"},
#endif
      {"polyphonic-render --edit nonsense " + missing + " -o " + out,
       "argument --edit: must be NOTE.FIELD=VALUE"},
      {"polyphonic-render --edit x.gain_db=1 " + missing + " -o " + out,
       "argument --edit: NOTE must be an integer"},
      {"polyphonic-render --edit 0.bogus=1 " + missing + " -o " + out,
       "argument --edit: FIELD must be one of pitch_shift_semitones, gain_db"},
      {"polyphonic-render --edit 0.gain_db=abc " + missing + " -o " + out,
       "argument --edit: gain_db must be a number"},
      {"polyphonic-render --edit 0.muted=maybe " + missing + " -o " + out,
       "argument --edit: muted must be one of true, 1, yes, on, false, 0, no, off"},
#ifdef SONARE_WITH_ARRANGEMENT
      {"transcribe --reattack-ratio 0.5 " + missing + " -o " + out,
       "--reattack-ratio must be 0 (no split) or greater than 1"},
      {"transcribe --min-note-division 8 --min-note-ms 50 " + missing + " -o " + out,
       "--min-note-division and --min-note-ms cannot both be given"},
      {"transcribe --polyphonic --monophonic-model singing " + missing + " -o " + out,
       "--monophonic-model singing cannot be combined with --polyphonic"},
      {"transcribe --f0-min-hz 100 --f0-max-hz 50 " + missing + " -o " + out,
       "--f0-max-hz must be greater than --f0-min-hz"},
      {"transcribe --reference-hz 0 " + missing + " -o " + out,
       "argument --reference-hz: must be finite and greater than 0"},
      {"transcribe --f0-min-hz 0 " + missing + " -o " + out,
       "argument --f0-min-hz: must be finite and greater than 0"},
      {"transcribe --min-note-ms 0 " + missing + " -o " + out,
       "argument --min-note-ms: must be finite and greater than 0"},
      {"transcribe --segmentation-threshold-cents 0 " + missing + " -o " + out,
       "argument --segmentation-threshold-cents: must be finite and greater than 0"},
      {"transcribe --velocity-floor-db 5 " + missing + " -o " + out,
       "argument --velocity-floor-db: must be finite and less than 0"},
      {"transcribe --fixed-velocity 200 " + missing + " -o " + out,
       "argument --fixed-velocity: must be an integer in [1, 127]"},
      {"transcribe --channel 16 " + missing + " -o " + out,
       "argument --channel: must be an integer in [0, 15]"},
#endif
  };
  for (const auto& row : rows) {
    CAPTURE(row.argv);
    auto [code, output] = exec_command(CLI + " " + row.argv + " -q 2>&1");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring(row.text));
  }
  REQUIRE_FALSE(std::ifstream(out).good());
}
