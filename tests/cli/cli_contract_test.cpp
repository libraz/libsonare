/// @file cli_contract_test.cpp
/// @brief Tests for the sonare CLI argument contract: parsing, the option
///        registry, option domains and exit codes.

#include "cli/cli_test_helpers.h"

TEST_CASE("CLI JSON numbers keep a dot decimal separator under any host locale", "[cli][json]") {
  // std::ostringstream formats through the global C++ locale rather than
  // through LC_NUMERIC, so std::setlocale does not reproduce this and only
  // std::locale::global does. On a host whose global locale marks decimals with
  // a comma, an un-imbued builder emits `"lufs": -14,5` -- a payload no JSON
  // parser accepts, from a command that still exits 0.
  std::locale comma_locale;
  try {
    comma_locale = std::locale("de_DE.UTF-8");
  } catch (const std::runtime_error&) {
    SKIP("de_DE.UTF-8 is not installed on this host");
  }

  GlobalLocaleGuard guard;
  std::locale::global(comma_locale);

  const std::string from_double =
      JsonBuilder().begin_object().kv("lufs", -14.5).end_object().build();
  REQUIRE_THAT(from_double, ContainsSubstring("\"lufs\": -14.5"));
  REQUIRE(from_double.find(',') == std::string::npos);

  const std::string from_float =
      JsonBuilder().begin_object().kv("peak", -1.25f).end_object().build();
  REQUIRE_THAT(from_float, ContainsSubstring("\"peak\": -1.25"));
  REQUIRE(from_float.find(',') == std::string::npos);
}

// Native-only commands have no cross-surface contract, so the conformance
// harness (which runs every case on both surfaces) has no place for them by
// design. Their exit codes and message shapes are locked here instead.
TEST_CASE("CLI numeric option validation", "[cli]") {
  SECTION("a negative size is a rejected parameter, not an internal error") {
    // The value used to be narrowed to size_t before any range check, so -1
    // became a huge allocation bound and surfaced as a generic failure.
    for (const std::string command : {"pad-center", "fix-length"}) {
      auto [code, output] = exec_command(CLI + " " + command + " --values 1,2,3 --size -1");
      INFO(command);
      REQUIRE(code == 3);
      REQUIRE_THAT(output, ContainsSubstring("--size"));
      REQUIRE_THAT(output, ContainsSubstring("-1"));
    }
  }

  SECTION("a size option with no usable default is required, not silently empty") {
    auto [fix_code, fix_output] = exec_command(CLI + " fix-length --values 1,2,3");
    REQUIRE(fix_code == 3);
    REQUIRE_THAT(fix_output, ContainsSubstring("--size"));

    auto [pcen_code, pcen_output] = exec_command(CLI + " pcen --values 1,2,3,4");
    REQUIRE(pcen_code == 3);
    REQUIRE_THAT(pcen_output, ContainsSubstring("--n-bins"));
  }

  SECTION("a negative region length is rejected instead of yielding contradictory output") {
    const std::string clipped = unique_temp_path("_clipped.wav");
    create_clipped_wav(clipped);
    auto [code, output] = exec_command(CLI + " clipping " + clipped + " --min-region -1 --json");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--min-region"));
    std::remove(clipped.c_str());
  }

  SECTION("the same option rejects every unparsable value the same way") {
    // A value with a numeric prefix and a value with none take different paths
    // through the standard library's conversion, and the second used to escape
    // as "stoi: no conversion" with no option name and no rejected value.
    auto [partial_code, partial_output] =
        exec_command(CLI + " pad-center --values 1,2,3 --size 1.5x");
    auto [none_code, none_output] = exec_command(CLI + " pad-center --values 1,2,3 --size abc");
    REQUIRE(partial_code == none_code);
    REQUIRE_THAT(partial_output, ContainsSubstring("invalid integer value for --size: 1.5x"));
    REQUIRE_THAT(none_output, ContainsSubstring("invalid integer value for --size: abc"));
  }

  SECTION("a list element names the option and the element that failed") {
    auto [code, output] = exec_command(CLI + " fix-frames --values a,b,c");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--values"));
    REQUIRE_THAT(output, ContainsSubstring("a"));
  }
}

TEST_CASE("CLI version command", "[cli]") {
  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " version");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("sonare-cli"));
    REQUIRE_THAT(output, ContainsSubstring("libsonare"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " version --json");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"cli\": \"native\""));
    REQUIRE_THAT(output, ContainsSubstring("\"cli_version\""));
    REQUIRE_THAT(output, ContainsSubstring("\"lib_version\""));
  }

  SECTION("cli_version tracks the compiled library version") {
    // The CLI version must be derived from the build, not a stale literal, so
    // it always matches the library version it ships with.
    const std::string expected_cli = std::string("\"cli_version\": \"") + version() + "\"";
    const std::string expected_lib = std::string("\"lib_version\": \"") + version() + "\"";
    auto [code, output] = exec_command(CLI + " version --json");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring(expected_cli));
    REQUIRE_THAT(output, ContainsSubstring(expected_lib));
    REQUIRE_THAT(output, !ContainsSubstring("1.0.0"));
  }
}

TEST_CASE("CLI doctor command", "[cli]") {
  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " doctor");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("libsonare Doctor"));
    REQUIRE_THAT(output, ContainsSubstring("Platform"));
  }

  SECTION("json output matches the C API schema") {
    auto [code, output] = exec_command(CLI + " doctor --json");
    REQUIRE(code == 0);
    const auto capabilities = sonare::util::json::parse_strict(output);
    REQUIRE(capabilities["version"].as_string() == version());
    REQUIRE(capabilities["abi"]["project"].as_number() == SONARE_PROJECT_ABI_VERSION);
    REQUIRE(capabilities["abi"]["engine"].as_number() == sonare_engine_abi_version());
    REQUIRE(capabilities["features"]["ffmpeg"].as_bool() == (sonare_has_ffmpeg_support() != 0));
  }
}

TEST_CASE("CLI system-info command", "[cli]") {
  SECTION("text output") {
    auto [code, output] = exec_command(CLI + " system-info");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("CPU Cores"));
    REQUIRE_THAT(output, ContainsSubstring("Memory"));
    REQUIRE_THAT(output, ContainsSubstring("Parallel"));
  }

  SECTION("json output") {
    auto [code, output] = exec_command(CLI + " system-info --json");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"logical_cores\""));
    REQUIRE_THAT(output, ContainsSubstring("\"strategy\""));
  }
}

TEST_CASE("CLI help command", "[cli]") {
  auto [code, output] = exec_command(CLI + " --help");
  REQUIRE(code == 0);
  REQUIRE_THAT(output, ContainsSubstring("ANALYSIS COMMANDS"));
  REQUIRE_THAT(output, ContainsSubstring("PROCESSING COMMANDS"));
  REQUIRE_THAT(output, ContainsSubstring("FEATURE COMMANDS"));
  REQUIRE_THAT(output, ContainsSubstring("UTILITY COMMANDS"));
  REQUIRE(output.find("FEATURE COMMANDS") < output.find("\n  mel            "));
  REQUIRE(output.find("  mfcc-to-audio") < output.find("UTILITY COMMANDS"));
  REQUIRE(output.find("UTILITY COMMANDS") < output.find("\n  frames-to-samples"));
}

TEST_CASE("CLI hidden contract inventory is machine-readable", "[cli][contract]") {
  auto [code, output] = exec_command(CLI + " --dump-cli-contract");
  REQUIRE(code == 0);
  const auto inventory = sonare::util::json::parse_strict(output);
  REQUIRE(inventory["schema_version"].as_int() == 2);
  REQUIRE(inventory["surface"].as_string() == "native");
  REQUIRE(inventory["commands"].is_array());

  bool found_version = false;
  bool found_chroma = false;
  bool found_pitch_shift = false;
  bool found_resample = false;
#ifdef SONARE_WITH_ARRANGEMENT
  bool found_project_validate = false;
#endif
  bool found_voice_validate = false;
  for (const auto& command : inventory["commands"].as_array()) {
    const auto path = command["path"].as_string();
    if (path == "version") {
      found_version = true;
      REQUIRE(command["options"].size() == 1);
      REQUIRE(command["options"][0]["name"].as_string() == "json");
    } else if (path == "chroma") {
      found_chroma = true;
      REQUIRE(command["options"].size() == 3);
      REQUIRE(command["options"][1]["name"].as_string() == "n-fft");
      REQUIRE(command["options"][1]["default"].as_int() == 2048);
      REQUIRE(command["options"][2]["default"].as_int() == 512);
    } else if (path == "pitch-shift") {
      found_pitch_shift = true;
      REQUIRE(command["options"][0]["name"].as_string() == "json");
      REQUIRE(command["options"][1]["name"].as_string() == "semitones");
      REQUIRE_FALSE(command["options"][1]["required"].as_bool());
      REQUIRE(command["options"][1]["default"].is_null());
    } else if (path == "resample") {
      found_resample = true;
      REQUIRE(command["options"].size() == 3);
      REQUIRE(command["options"][1]["name"].as_string() == "target-rate");
      REQUIRE(command["options"][1]["aliases"][0].as_string() == "target-sr");
      REQUIRE(command["options"][1]["required"].as_bool());
      REQUIRE(command["options"][1]["default"].is_null());
#ifdef SONARE_WITH_ARRANGEMENT
    } else if (path == "project.validate") {
      found_project_validate = true;
      REQUIRE(command["options"].size() == 4);
      REQUIRE(command["options"][2]["name"].as_string() == "in");
      REQUIRE(command["options"][2]["required"].as_bool());
      REQUIRE(command["options"][2]["default"].is_null());
      REQUIRE(command["options"][3]["aliases"][0].as_string() == "o");
      REQUIRE_FALSE(command["options"][3]["required"].as_bool());
#endif
    } else if (path == "voice-preset-validate") {
      found_voice_validate = true;
      REQUIRE(command["options"].size() == 4);
      REQUIRE(command["options"][3]["repeatable"].as_bool());
      REQUIRE(command["options"][3]["default"].is_array());
      REQUIRE(command["options"][3]["default"].size() == 0);
    }
  }
  REQUIRE(found_version);
  REQUIRE(found_chroma);
  REQUIRE(found_pitch_shift);
  REQUIRE(found_resample);
#ifdef SONARE_WITH_ARRANGEMENT
  REQUIRE(found_project_validate);
#endif
  REQUIRE(found_voice_validate);
}

TEST_CASE("CLI contract inventory follows path-scoped parser metadata", "[cli][contract]") {
  const auto inventory =
      sonare::util::json::parse_strict(exec_command(CLI + " --dump-cli-contract").second);
  const auto command = [&](const std::string& path) {
    for (const auto& item : inventory["commands"].as_array()) {
      if (item["path"].as_string() == path) return item;
    }
    return sonare::util::json::Value();
  };
  const auto has_option = [&](const sonare::util::json::Value& item, const std::string& name) {
    for (const auto& option : item["options"].as_array()) {
      if (option["name"].as_string() == name) return true;
    }
    return false;
  };

  REQUIRE_FALSE(has_option(command("beats"), "hop-length"));
  REQUIRE_FALSE(has_option(command("downbeats"), "hop-length"));
  REQUIRE_FALSE(has_option(command("onsets"), "hop-length"));
  REQUIRE_FALSE(has_option(command("pitch-correct"), "n-fft"));
  REQUIRE_FALSE(has_option(command("pitch-correct"), "hop-length"));
  REQUIRE_FALSE(has_option(command("note-stretch"), "n-fft"));
  REQUIRE_FALSE(has_option(command("note-stretch"), "hop-length"));
  REQUIRE_FALSE(has_option(command("spectral"), "output"));
  REQUIRE(has_option(command("pitch-shift"), "output"));

  // A handler that reads a global DSP field must have the matching option on
  // its own path, or the parser rejects the spelling and the read can only
  // ever observe the built-in default.
  REQUIRE(has_option(command("melody"), "hop-length"));
  REQUIRE(has_option(command("melody"), "fmin"));
  REQUIRE(has_option(command("melody"), "fmax"));
  REQUIRE(has_option(command("boundaries"), "n-fft"));
  REQUIRE(has_option(command("boundaries"), "hop-length"));
  REQUIRE(has_option(command("pcen"), "hop-length"));
  REQUIRE(has_option(command("dynamics"), "hop-length"));
  REQUIRE(has_option(command("rhythm"), "n-fft"));
  REQUIRE(has_option(command("rhythm"), "hop-length"));
  // dynamics windows a loudness series but runs no FFT of its own.
  REQUIRE_FALSE(has_option(command("dynamics"), "n-fft"));

#ifdef SONARE_WITH_ARRANGEMENT
  REQUIRE_FALSE(has_option(command("project.abi"), "frames"));
  REQUIRE_FALSE(has_option(command("project.validate"), "frames"));
  REQUIRE_FALSE(has_option(command("project.compile"), "output"));
  REQUIRE(has_option(command("project.validate"), "output"));
  REQUIRE(has_option(command("project.bounce"), "output"));

  SECTION("the parser rejects options absent from each path") {
    for (const std::string invocation : {"project abi --frames 1", "project validate --frames 1",
                                         "project compile -o ignored.wav"}) {
      auto [code, output] = exec_command(CLI + " " + invocation);
      REQUIRE(code == 2);
      REQUIRE_THAT(output, ContainsSubstring("option"));
    }
  }
#endif
}

TEST_CASE("CLI global DSP options reach the analysis on every path that reads them",
          "[cli][argument-contract]") {
  const auto payload_of = [](const std::string& invocation) {
    auto [code, output] = exec_command(invocation);
    REQUIRE(code == 0);
    return sonare::util::json::parse_strict(output);
  };

  SECTION("melody") {
    create_test_wav(TEST_WAV);
    const std::string base_command = CLI + " melody " + TEST_WAV + " --json -q";
    const auto base = payload_of(base_command);

    const auto finer_hop = payload_of(base_command + " --hop-length 256");
    REQUIRE(finer_hop["pitch_count"].as_int() != base["pitch_count"].as_int());

    const auto lowered_ceiling = payload_of(base_command + " --fmax 300");
    REQUIRE(lowered_ceiling["mean_frequency"].as_float() != base["mean_frequency"].as_float());

    const auto raised_floor = payload_of(base_command + " --fmin 500");
    REQUIRE(raised_floor["has_melody"].as_bool() != base["has_melody"].as_bool());
  }

  SECTION("boundaries") {
    const std::string segmented = unique_temp_path("_segmented.wav");
    create_two_segment_wav(segmented);
    const std::string base_command = CLI + " boundaries " + segmented + " --json -q";
    const auto base = payload_of(base_command);
    REQUIRE(base["count"].as_int() > 0);

    const auto finer_hop = payload_of(base_command + " --hop-length 256");
    REQUIRE(finer_hop["boundaries"][0]["frame"].as_int() !=
            base["boundaries"][0]["frame"].as_int());

    const auto wider_window = payload_of(base_command + " --n-fft 8192");
    REQUIRE(wider_window["boundaries"][0]["time"].as_float() !=
            base["boundaries"][0]["time"].as_float());
  }

  SECTION("dynamics") {
    const std::string stepped = unique_temp_path("_stepped.wav");
    create_stepped_level_wav(stepped);
    const std::string base_command = CLI + " dynamics " + stepped + " --json -q";
    const auto base = payload_of(base_command);

    // dynamic_range_db is the only reading the hop reaches: it is the
    // percentile spread of the windowed RMS series.
    const auto coarser_hop = payload_of(base_command + " --hop-length 4096");
    REQUIRE(coarser_hop["dynamic_range_db"].as_float() != base["dynamic_range_db"].as_float());
  }

  SECTION("rhythm") {
    create_test_wav(TEST_WAV);
    const std::string base_command = CLI + " rhythm " + TEST_WAV + " --json -q";
    const auto base = payload_of(base_command);

    const auto finer_hop = payload_of(base_command + " --hop-length 256");
    REQUIRE(finer_hop["bpm"].as_float() != base["bpm"].as_float());

    const auto wider_window = payload_of(base_command + " --n-fft 8192");
    REQUIRE(wider_window["bpm"].as_float() != base["bpm"].as_float());
  }

  SECTION("pcen") {
    const std::string base_command =
        CLI + " pcen --values 1,2,3,4,5,6 --n-bins 1 --n-frames 6 --json";
    const auto base = payload_of(base_command);
    const auto finer_hop = payload_of(base_command + " --hop-length 256");
    REQUIRE(base.size() == finer_hop.size());
    REQUIRE(base[base.size() - 1].as_float() != finer_hop[finer_hop.size() - 1].as_float());
  }

  SECTION("pitch-correct corrects to one constant pitch and takes no hop control") {
    create_test_wav(TEST_WAV);
    auto [code, output] = exec_command(CLI + " pitch-correct --current-midi 69 --target-midi 70 " +
                                       TEST_WAV + " -o " + TEST_OUT + " -q --hop-length 256");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Unknown option '--hop-length'"));
  }
}

TEST_CASE("CLI registry exposes immutable leaf contracts", "[cli][registry]") {
  const auto& registry = cli_command_registry();
  REQUIRE_FALSE(registry.empty());

  const auto* chroma = cli_command_spec_for_path("chroma");
  REQUIRE(chroma != nullptr);
  const auto* fft = cli_option_spec_for_command("chroma", "n-fft");
  REQUIRE(fft != nullptr);
  REQUIRE(fft->scalar_type == CliOptionScalarType::Integer);
  REQUIRE(fft->arity == CliOptionArity::RequiredValue);
  REQUIRE(fft->default_value.kind == CliOptionDefaultKind::Integer);
  REQUIRE(fft->default_value.integer_value == 2048);
  REQUIRE(fft->global_lexical);

  const auto* output = cli_option_spec_for_command("pitch-shift", "o");
  REQUIRE(output != nullptr);
  REQUIRE(output->name == "output");
  REQUIRE(output->scalar_type == CliOptionScalarType::Path);
  REQUIRE(output->aliases == std::vector<std::string>{"o"});
  REQUIRE(output->global_lexical);
  // A command that cannot run without writing a file declares that in the
  // registry, together with the exit class its absence reports.
  REQUIRE(output->required);
  REQUIRE(output->required_stage == CliOptionDomainStage::Parameter);
  REQUIRE(output->default_value.kind == CliOptionDefaultKind::Null);
  // A command that only writes when asked keeps it optional.
  const auto* optional_output = cli_option_spec_for_command("trim-silence", "output");
  REQUIRE(optional_output != nullptr);
  REQUIRE_FALSE(optional_output->required);

  const auto* semitones = cli_option_spec_for_command("pitch-shift", "semitones");
  REQUIRE(semitones != nullptr);
  REQUIRE_FALSE(semitones->required);
  REQUIRE(semitones->default_value.kind == CliOptionDefaultKind::Null);

  const auto* target_rate = cli_option_spec_for_command("resample", "target-rate");
  REQUIRE(target_rate != nullptr);
  REQUIRE(target_rate->required);
  REQUIRE(target_rate->aliases == std::vector<std::string>{"target-sr"});
  REQUIRE(target_rate->default_value.kind == CliOptionDefaultKind::Null);
  REQUIRE(cli_option_spec_for_command("resample", "target-sr") == target_rate);

  const auto* key_hpss = cli_option_spec_for_command("key", "use-hpss");
  REQUIRE(key_hpss != nullptr);
  REQUIRE(key_hpss->name == "use-hpss");
  REQUIRE(key_hpss->aliases == std::vector<std::string>{"hpss"});
  REQUIRE(key_hpss->scalar_type == CliOptionScalarType::Boolean);
  REQUIRE(key_hpss->default_value.kind == CliOptionDefaultKind::Boolean);
  REQUIRE_FALSE(key_hpss->default_value.boolean_value);
  REQUIRE(cli_option_spec_for_command("key", "hpss") == key_hpss);

  const auto* candidates = cli_option_spec_for_command("key", "candidates");
  REQUIRE(candidates != nullptr);
  REQUIRE(candidates->scalar_type == CliOptionScalarType::Integer);
  REQUIRE(candidates->default_value.kind == CliOptionDefaultKind::Null);

  const auto* smoothing_window = cli_option_spec_for_command("chords", "smoothing-window");
  REQUIRE(smoothing_window != nullptr);
  REQUIRE(smoothing_window->scalar_type == CliOptionScalarType::Number);
  REQUIRE(smoothing_window->default_value.kind == CliOptionDefaultKind::Number);
  REQUIRE(smoothing_window->default_value.number_value == 2.0);
  const auto* no_beat_sync = cli_option_spec_for_command("chords", "no-beat-sync");
  REQUIRE(no_beat_sync != nullptr);
  REQUIRE(no_beat_sync->scalar_type == CliOptionScalarType::Boolean);
  REQUIRE(no_beat_sync->default_value.kind == CliOptionDefaultKind::Boolean);
  REQUIRE_FALSE(no_beat_sync->default_value.boolean_value);

  for (const std::string command : {"onset-env", "onset-envelope", "tempogram", "plp"}) {
    const auto* n_mels = cli_option_spec_for_command(command, "n-mels");
    REQUIRE(n_mels != nullptr);
    REQUIRE(n_mels->scalar_type == CliOptionScalarType::Integer);
    REQUIRE(n_mels->default_value.kind == CliOptionDefaultKind::Integer);
    REQUIRE(n_mels->default_value.integer_value == 128);
  }
  for (const std::string command : {"fourier-tempogram", "tempogram-ratio"})
    REQUIRE(cli_option_spec_for_command(command, "n-fft") == nullptr);

  const auto* pitch_threshold = cli_option_spec_for_command("pitch", "threshold");
  REQUIRE(pitch_threshold != nullptr);
  REQUIRE(pitch_threshold->default_value.kind == CliOptionDefaultKind::Number);
  REQUIRE(pitch_threshold->default_value.number_value == 0.1);
  const auto* pitch_hop = cli_option_spec_for_command("pitch", "hop-length");
  REQUIRE(pitch_hop != nullptr);
  REQUIRE(pitch_hop->default_value.kind == CliOptionDefaultKind::Integer);
  REQUIRE(pitch_hop->default_value.integer_value == 512);
  const auto* pitch_fmin = cli_option_spec_for_command("pitch", "fmin");
  REQUIRE(pitch_fmin != nullptr);
  REQUIRE(pitch_fmin->default_value.kind == CliOptionDefaultKind::Number);
  REQUIRE(pitch_fmin->default_value.number_value == 65.0);
  const auto* pitch_fmax = cli_option_spec_for_command("pitch", "fmax");
  REQUIRE(pitch_fmax != nullptr);
  REQUIRE(pitch_fmax->default_value.kind == CliOptionDefaultKind::Number);
  REQUIRE(pitch_fmax->default_value.number_value == 2093.0);

  const auto* mel_htk = cli_option_spec_for_command("mel", "htk");
  REQUIRE(mel_htk != nullptr);
  REQUIRE(mel_htk->scalar_type == CliOptionScalarType::Boolean);
  REQUIRE(mel_htk->default_value.kind == CliOptionDefaultKind::Boolean);
  REQUIRE_FALSE(mel_htk->default_value.boolean_value);

  const auto* trim_top_db = cli_option_spec_for_command("trim-silence", "top-db");
  REQUIRE(trim_top_db != nullptr);
  REQUIRE(trim_top_db->scalar_type == CliOptionScalarType::Number);
  REQUIRE(trim_top_db->default_value.kind == CliOptionDefaultKind::Null);
  const auto* trim_threshold_db = cli_option_spec_for_command("trim-silence", "threshold-db");
  REQUIRE(trim_threshold_db != nullptr);
  REQUIRE(trim_threshold_db->scalar_type == CliOptionScalarType::Number);
  REQUIRE(trim_threshold_db->default_value.kind == CliOptionDefaultKind::Null);

  const auto* voice_preset = cli_option_spec_for_command("voice-change", "preset");
  REQUIRE(voice_preset != nullptr);
  REQUIRE(voice_preset->default_value.kind == CliOptionDefaultKind::String);
  REQUIRE(voice_preset->default_value.string_value.empty());
  const auto* voice_formant = cli_option_spec_for_command("voice-change", "formant-factor");
  REQUIRE(voice_formant != nullptr);
  REQUIRE(voice_formant->scalar_type == CliOptionScalarType::Number);
  REQUIRE(voice_formant->default_value.kind == CliOptionDefaultKind::Null);

#ifdef SONARE_WITH_MASTERING
  const auto* processor = cli_option_spec_for_command("mastering-processor", "processor");
  REQUIRE(processor != nullptr);
  REQUIRE(processor->scalar_type == CliOptionScalarType::String);
  REQUIRE(processor->required);
  REQUIRE(processor->default_value.kind == CliOptionDefaultKind::Null);
  const auto* pair_analysis = cli_option_spec_for_command("mastering-pair-analyze", "analysis");
  REQUIRE(pair_analysis != nullptr);
  REQUIRE(pair_analysis->required);
  REQUIRE(pair_analysis->default_value.kind == CliOptionDefaultKind::Null);
  const auto* pair_reference = cli_option_spec_for_command("mastering-pair-analyze", "reference");
  REQUIRE(pair_reference != nullptr);
  REQUIRE(pair_reference->required);
  REQUIRE(pair_reference->default_value.kind == CliOptionDefaultKind::Null);
#endif

#ifdef SONARE_WITH_ACOUSTIC_SIM
  const auto* octave_bands = cli_option_spec_for_command("estimate-room", "n-octave-bands");
  REQUIRE(octave_bands != nullptr);
  REQUIRE(octave_bands->aliases == std::vector<std::string>{"n-bands"});
  REQUIRE(octave_bands->default_value.kind == CliOptionDefaultKind::Null);
  REQUIRE(cli_option_spec_for_command("estimate-room", "n-bands") == octave_bands);
#endif

  const auto* set = cli_option_spec_for_command("voice-preset-validate", "set");
  REQUIRE(set != nullptr);
  REQUIRE(set->repeatable);
  REQUIRE(set->default_value.kind == CliOptionDefaultKind::StringArray);
  REQUIRE(set->default_value.string_array_value.empty());

#ifdef SONARE_WITH_ARRANGEMENT
  const auto* synth = cli_option_spec_for_command("project.bounce", "synth");
  REQUIRE(synth != nullptr);
  REQUIRE(synth->arity == CliOptionArity::OptionalValue);
  REQUIRE(synth->implicit_optional_default.kind == CliOptionDefaultKind::String);
  REQUIRE(synth->implicit_optional_default.string_value == "true");

  size_t project_leaf_count = 0;
  for (const auto& command : registry) {
    if (command.path.rfind("project.", 0) == 0) ++project_leaf_count;
  }
  REQUIRE(project_leaf_count == 11);
  REQUIRE(cli_command_spec_for_path("project") == nullptr);
  const auto* project_input = cli_option_spec_for_command("project.validate", "in");
  REQUIRE(project_input != nullptr);
  REQUIRE(project_input->required);
  REQUIRE(project_input->default_value.kind == CliOptionDefaultKind::Null);
#endif
}

TEST_CASE("CLI registry defaults and hidden controls project through parser", "[cli][registry]") {
  auto parse = [](std::initializer_list<const char*> words) {
    std::vector<std::string> storage;
    storage.reserve(words.size());
    for (const char* word : words) storage.emplace_back(word);
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& word : storage) argv.push_back(word.data());
    return ArgParser::parse(static_cast<int>(argv.size()), argv.data());
  };

  const CliArgs chroma = parse({"sonare-cli", "chroma"});
  REQUIRE(chroma.get_int("n-fft", -1) == 2048);
  REQUIRE(chroma.get_int("hop-length", -1) == 512);
  REQUIRE(chroma.get_int("n-fft", -1) != -1);

  const CliArgs key = parse({"sonare-cli", "key", "--hpss", "--candidates", "3"});
  REQUIRE(key.has("use-hpss"));
  REQUIRE(key.has("hpss"));
  REQUIRE(key.get_int("candidates", -1) == 3);
  REQUIRE(validate_cli_arguments(key, true).empty());

  const CliArgs chords =
      parse({"sonare-cli", "chords", "--smoothing-window", "1.25", "--no-beat-sync"});
  REQUIRE(chords.get_float("smoothing-window", -1.0f) == 1.25f);
  REQUIRE(chords.has("no-beat-sync"));
  REQUIRE(validate_cli_arguments(chords, true).empty());

  const CliArgs pitch = parse({"sonare-cli", "pitch"});
  REQUIRE(pitch.get_float("threshold", -1.0f) == 0.1f);
  REQUIRE(pitch.get_int("hop-length", -1) == 512);
  REQUIRE(pitch.get_float("fmin", -1.0f) == 65.0f);
  REQUIRE(pitch.get_float("fmax", -1.0f) == 2093.0f);
  REQUIRE(validate_cli_arguments(pitch, true).empty());

  const CliArgs trim = parse({"sonare-cli", "trim-silence"});
  REQUIRE(trim.get_float("threshold-db", -60.0f) == -60.0f);
  REQUIRE(trim.get_float("top-db", 60.0f) == 60.0f);
  REQUIRE(validate_cli_arguments(trim, true).empty());

  const CliArgs voice = parse({"sonare-cli", "voice-change", "-o", "out.wav"});
  REQUIRE(voice.get_string("preset", "sentinel") == "");
  REQUIRE(voice.get_float("formant-factor", 1.0f) == 1.0f);
  REQUIRE(validate_cli_arguments(voice, true).empty());

#ifdef SONARE_WITH_ACOUSTIC_SIM
  const CliArgs estimate = parse({"sonare-cli", "estimate-room", "--n-bands", "8"});
  REQUIRE(estimate.get_int("n-octave-bands", -1) == 8);
  REQUIRE(validate_cli_arguments(estimate, true).empty());
#endif

  const CliArgs tempogram = parse({"sonare-cli", "tempogram", "--n-mels", "64"});
  REQUIRE(tempogram.n_mels == 64);
  REQUIRE(validate_cli_arguments(tempogram, true).empty());

  const CliArgs fourier = parse({"sonare-cli", "fourier-tempogram", "--n-fft", "1024"});
  REQUIRE_FALSE(validate_cli_arguments(fourier, true).empty());

  CliArgs required;
  required.command = "pitch-shift";
  REQUIRE(required.get_float("semitones", 17.0f) == 17.0f);

  const auto metadata = cli_option_metadata_for_command("chroma");
  for (const auto& option : metadata) {
    REQUIRE(option.name != "quiet");
    REQUIRE(option.name != "help");
  }
  const auto* quiet = cli_option_spec_for_command("chroma", "q");
  REQUIRE(quiet != nullptr);
  REQUIRE_FALSE(quiet->inventory);
  const auto* help = cli_option_spec_for_command("chroma", "h");
  REQUIRE(help != nullptr);
  REQUIRE_FALSE(help->inventory);

#ifdef SONARE_WITH_ARRANGEMENT
  const CliArgs project =
      parse({"sonare-cli", "project", "validate", "--strict", "--in", "project.json"});
  REQUIRE(project.command == "project");
  REQUIRE(project.input_file == "validate");
  REQUIRE(project.has("strict"));
  REQUIRE(project.get_string("in") == "project.json");
  REQUIRE(validate_cli_arguments(project, false).empty());

  const CliArgs bounce = parse(
      {"sonare-cli", "project", "bounce", "--in", "project.json", "-o", "out.wav", "--synth"});
  REQUIRE(bounce.command == "project");
  REQUIRE(bounce.input_file == "bounce");
  REQUIRE(bounce.get_string("synth") == "true");
  REQUIRE(validate_cli_arguments(bounce, false).empty());
#endif
}

TEST_CASE("CLI option domains match the cross-surface declaration", "[cli][argument-contract]") {
  // The registry is the only place a domain is enforced, and this fixture is
  // the only place it is declared for review. Pinning them against each other
  // in both directions is what keeps a new option's domain from being added on
  // one surface alone: the same values are driven through both executables by
  // tests/conformance/cli_contract_v2.json, and a domain that never reaches
  // this file never reaches that comparison either.
  std::ifstream input("tests/conformance/cli_option_domains.json");
  REQUIRE(input.good());
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  const auto fixture = sonare::util::json::parse_strict(text);

  const auto stage_name = [](CliOptionDomainStage stage) {
    return stage == CliOptionDomainStage::Parameter ? std::string("invalid_parameter")
                                                    : std::string("usage");
  };
  // "<command>\t<option>" keys, so a mismatch names both halves.
  std::set<std::string> declared_domains;
  std::set<std::string> declared_required;
  for (const auto& record : fixture["domains"].as_array()) {
    const std::string command = record["command"].as_string();
    const std::string option = record["option"].as_string();
    CAPTURE(command, option);
    declared_domains.insert(command + "\t" + option);
    const CliOptionSpec* spec = cli_option_spec_for_command(command, option);
    REQUIRE(spec != nullptr);
    REQUIRE_FALSE(spec->domain.empty());
    REQUIRE(stage_name(spec->domain.stage) == record["rejectExit"].as_string());
    std::vector<std::string> choices;
    for (const auto& choice : record["choices"].as_array()) choices.push_back(choice.as_string());
    REQUIRE(spec->domain.choices == choices);
    REQUIRE(spec->domain.has_minimum == !record["minimum"].is_null());
    if (spec->domain.has_minimum) {
      REQUIRE(spec->domain.minimum == record["minimum"].as_number());
      REQUIRE(spec->domain.exclusive_minimum == record["exclusiveMinimum"].as_bool());
    }
    REQUIRE(spec->domain.has_maximum == !record["maximum"].is_null());
    if (spec->domain.has_maximum) {
      REQUIRE(spec->domain.maximum == record["maximum"].as_number());
      REQUIRE(spec->domain.exclusive_maximum == record["exclusiveMaximum"].as_bool());
    }
  }
  for (const auto& record : fixture["requiredInvalidParameter"].as_array()) {
    const std::string command = record["command"].as_string();
    const std::string option = record["option"].as_string();
    CAPTURE(command, option);
    declared_required.insert(command + "\t" + option);
    const CliOptionSpec* spec = cli_option_spec_for_command(command, option);
    REQUIRE(spec != nullptr);
    REQUIRE(spec->required);
    REQUIRE(spec->required_stage == CliOptionDomainStage::Parameter);
  }

  // The other direction: a domain added to the registry without a line here.
  for (const auto& command : cli_command_registry()) {
    for (const auto& option : command.options) {
      const std::string key = command.path + "\t" + option.name;
      if (!option.domain.empty()) {
        CAPTURE(command.path, option.name);
        REQUIRE(declared_domains.count(key) == 1);
      }
      if (option.required && option.required_stage == CliOptionDomainStage::Parameter) {
        CAPTURE(command.path, option.name);
        REQUIRE(declared_required.count(key) == 1);
      }
    }
  }
}

TEST_CASE("CLI option domains are declared once and enforced before dispatch",
          "[cli][argument-contract]") {
  const auto parse = [](std::initializer_list<const char*> words) {
    std::vector<std::string> storage(words.begin(), words.end());
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& word : storage) argv.push_back(word.data());
    return ArgParser::parse(static_cast<int>(argv.size()), argv.data());
  };
  const auto reject = [&](std::initializer_list<const char*> words, bool requires_audio) {
    return validate_cli_arguments(parse(words), requires_audio);
  };

  SECTION("a global option reaches the option map, so its value is used and checked") {
    // The parser used to write the dedicated field only. get_int() then missed
    // the value, fell back to the registry default, and `--hop-length 1`
    // silently framed at 512.
    const CliArgs framed =
        parse({"sonare-cli", "frame-signal", "--values", "1,2,3,4", "--hop-length", "1"});
    REQUIRE(framed.has("hop-length"));
    REQUIRE(framed.hop_length == 1);
    REQUIRE(framed.get_int("hop-length", framed.hop_length) == 1);
    REQUIRE(validate_cli_arguments(framed, false).empty());

    const CliArgs defaulted = parse({"sonare-cli", "frame-signal", "--values", "1,2,3,4"});
    REQUIRE_FALSE(defaulted.has("hop-length"));
    REQUIRE(defaulted.get_int("hop-length", defaulted.hop_length) == 512);
  }

  SECTION("pitch domains are refused in the class the Python CLI reports") {
    // Parse-time domains on the Python side (`type=` callables): usage.
    for (const auto* value : {"-50", "0"}) {
      CAPTURE(value);
      const CliValidationError error = reject({"sonare-cli", "pitch", "--fmin", value}, true);
      REQUIRE_FALSE(error.empty());
      REQUIRE_FALSE(error.invalid_parameter);
    }
    REQUIRE_FALSE(reject({"sonare-cli", "pitch", "--threshold", "0"}, true).empty());
    REQUIRE_FALSE(reject({"sonare-cli", "pitch", "--threshold", "1.5"}, true).empty());
    REQUIRE(reject({"sonare-cli", "pitch", "--threshold", "1"}, true).empty());
    REQUIRE_FALSE(reject({"sonare-cli", "pitch", "--hop-length", "0"}, true).empty());

    // The cross-option constraint neither domain can express. It used to be
    // checked by pyin and ignored by yin, so the same arguments gave a
    // different answer per algorithm.
    for (const auto* algorithm : {"pyin", "yin"}) {
      CAPTURE(algorithm);
      const CliValidationError error = reject(
          {"sonare-cli", "pitch", "--fmin", "3000", "--fmax", "500", "--algorithm", algorithm},
          true);
      REQUIRE_FALSE(error.empty());
      REQUIRE_FALSE(error.invalid_parameter);
    }
    REQUIRE(reject({"sonare-cli", "pitch", "--fmin", "100", "--fmax", "1000"}, true).empty());

    // A handler-level domain on the Python side: invalid parameter.
    const CliValidationError algorithm =
        reject({"sonare-cli", "pitch", "--algorithm", "bogus"}, true);
    REQUIRE_FALSE(algorithm.empty());
    REQUIRE(algorithm.invalid_parameter);
    REQUIRE(reject({"sonare-cli", "pitch", "--algorithm", "yin"}, true).empty());
  }

#ifdef SONARE_WITH_MASTERING
  SECTION("the write-path domains do not depend on an output file being present") {
    // --bits used to be checked inside save_wav, which only ran with -o, so the
    // same value was refused or accepted depending on an unrelated option.
    for (const auto* command : {"mastering", "eq"}) {
      CAPTURE(command);
      const CliValidationError error = reject({"sonare-cli", command, "--bits", "8"}, true);
      REQUIRE_FALSE(error.empty());
      REQUIRE(error.invalid_parameter);
    }
    // The same domain on a command that also has a required option: the missing
    // option is reported first, so supply it to reach the value check.
    const CliValidationError processor_bits =
        reject({"sonare-cli", "mastering-processor", "--processor", "gain", "--bits", "8"}, true);
    REQUIRE_FALSE(processor_bits.empty());
    REQUIRE(processor_bits.invalid_parameter);
    REQUIRE(reject({"sonare-cli", "eq", "--bits", "24"}, true).empty());

    // An argparse `choices=` tuple on the Python side: usage.
    const CliValidationError oversample =
        reject({"sonare-cli", "mastering", "--true-peak-oversample", "3"}, true);
    REQUIRE_FALSE(oversample.empty());
    REQUIRE_FALSE(oversample.invalid_parameter);
    REQUIRE(reject({"sonare-cli", "mastering", "--true-peak-oversample", "8"}, true).empty());
  }
#endif

  SECTION("a required output is refused as an invalid parameter, not a usage error") {
    const CliValidationError error = reject({"sonare-cli", "gain", "--gain-db", "3"}, true);
    REQUIRE_FALSE(error.empty());
    REQUIRE(error.invalid_parameter);
    REQUIRE(reject({"sonare-cli", "gain", "--gain-db", "3", "-o", "out.wav"}, true).empty());
    // A registry-required option with no Python handler counterpart keeps the
    // usage class it always had.
    const CliValidationError usage = reject({"sonare-cli", "resample", "-o", "out.wav"}, true);
    REQUIRE_FALSE(usage.empty());
    REQUIRE_FALSE(usage.invalid_parameter);
  }
}

TEST_CASE("CLI rejects option typos and terminal required options before dispatch",
          "[cli][argument-contract]") {
  std::vector<std::string> commands = {"analyze",
                                       "bpm",
                                       "key",
                                       "beats",
                                       "downbeats",
                                       "onsets",
                                       "chords",
                                       "sections",
                                       "timbre",
                                       "dynamics",
                                       "rhythm",
                                       "melody",
                                       "boundaries",
                                       "acoustic",
                                       "lufs",
                                       "meter",
                                       "clipping",
                                       "dynamic-range",
                                       "stereo",
                                       "phase",
                                       "pitch-shift",
                                       "time-stretch",
                                       "pitch-correct",
                                       "note-stretch",
                                       "voice-change",
                                       "voice-presets",
                                       "voice-preset",
                                       "voice-preset-validate",
                                       "hpss",
                                       "preemphasis",
                                       "deemphasis",
                                       "trim-silence",
                                       "split-silence",
                                       "normalize",
                                       "gain",
                                       "fade",
                                       "filter",
                                       "resample",
                                       "tone",
                                       "chirp",
                                       "clicks",
                                       "mel",
                                       "chroma",
                                       "tonnetz",
                                       "spectral",
                                       "pitch",
                                       "onset-env",
                                       "onset-envelope",
                                       "tempogram",
                                       "fourier-tempogram",
                                       "tempogram-ratio",
                                       "plp",
                                       "nnls-chroma",
                                       "cqt",
                                       "vqt",
                                       "mel-to-audio",
                                       "mfcc-to-audio",
                                       "frames-to-samples",
                                       "samples-to-frames",
                                       "power-to-db",
                                       "amplitude-to-db",
                                       "db-to-power",
                                       "db-to-amplitude",
                                       "frame-signal",
                                       "pad-center",
                                       "fix-length",
                                       "fix-frames",
                                       "peak-pick",
                                       "vector-normalize",
                                       "pcen",
                                       "info",
                                       "version",
                                       "doctor",
                                       "system-info"};
#ifdef SONARE_WITH_ACOUSTIC_SIM
  commands.insert(commands.end(), {"estimate-room", "synthesize-rir", "room-morph"});
#endif
#ifdef SONARE_WITH_MASTERING
  commands.insert(
      commands.end(),
      {"mastering", "eq", "mastering-processor", "mastering-pair-processor",
       "mastering-pair-analyze", "mastering-stereo-analyze", "mastering-processors",
       "mastering-pair-processors", "mastering-pair-analyses", "mastering-stereo-analyses"});
#endif
#ifdef SONARE_WITH_MIXING
  commands.insert(commands.end(), {"mix-strip", "mixing-presets", "mixing-preset"});
#endif
#ifdef SONARE_WITH_ARRANGEMENT
  commands.push_back("project");
#endif

  for (const std::string& command : commands) {
    CAPTURE(command);
    auto [typo_code, typo_output] =
        exec_command(CLI + " " + command + " --definitely-unknown-option");
    REQUIRE(typo_code != 0);
    REQUIRE_THAT(typo_output, ContainsSubstring("Unknown option"));

    auto [terminal_code, terminal_output] = exec_command(CLI + " " + command + " --n-fft");
    REQUIRE(terminal_code != 0);
    REQUIRE_THAT(terminal_output, ContainsSubstring("Missing value for option '--n-fft'"));
  }
}

TEST_CASE("CLI rejects extra positionals and preserves flag and negative-value parsing",
          "[cli][argument-contract]") {
  SECTION("unconsumed positional") {
    auto [code, output] = exec_command(CLI + " version unexpected");
    REQUIRE(code != 0);
    REQUIRE_THAT(output, ContainsSubstring("Unexpected positional argument 'unexpected'"));
  }

  SECTION("negative numeric list remains an option value") {
    auto [code, output] = exec_command(CLI + " power-to-db --values -6,-3 --json");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("["));
  }

  SECTION("terminal boolean flag remains presence-only") {
    auto [code, output] = exec_command(CLI + " fix-frames --values 1,2 --no-pad --json");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("["));
  }

#ifdef SONARE_WITH_ARRANGEMENT
  SECTION("terminal optional-value flag remains valid") {
    // `--synth` belongs to the bounce subcommand.  The old broad project
    // schema accidentally accepted it for `project abi`; path-scoped schemas
    // must keep the optional-value parser behavior while rejecting it on
    // unrelated project routes.
    auto [code, output] = exec_command(CLI + " project bounce --synth");
    REQUIRE(code != 0);
    REQUIRE_THAT(output, !ContainsSubstring("Unknown option"));
  }
#endif
}

TEST_CASE("CLI enforces registry requirements and canonical option aliases",
          "[cli][argument-contract]") {
  SECTION("missing project input is a usage error before dispatch") {
#ifdef SONARE_WITH_ARRANGEMENT
    auto [code, output] = exec_command(CLI + " project validate --json");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--in'"));

    auto [legacy_code, legacy_output] =
        exec_command("SONARE_LEGACY_EXIT=1 " + CLI + " project validate --json");
    REQUIRE(legacy_code == 1);
    REQUIRE_THAT(legacy_output, ContainsSubstring("Missing required option '--in'"));
#endif
  }

  SECTION("foreign project options remain path-scoped") {
#ifdef SONARE_WITH_ARRANGEMENT
    auto [code, output] = exec_command(CLI + " project validate --frames 1 --json");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Unknown option '--frames'"));
    REQUIRE_THAT(output, !ContainsSubstring("Missing required option '--in'"));
#endif
  }

  SECTION("resample alias satisfies the one required target option") {
    create_test_wav(TEST_WAV);
    const std::string output_path = unique_temp_path("_resample_alias.wav");
    auto [code, output] = exec_command(CLI + " resample --target-sr 16000 " + TEST_WAV + " -o " +
                                       output_path + " -q");
    REQUIRE(code == 0);
    REQUIRE(output_path != TEST_WAV);
    const auto [samples, sample_rate] = load_wav(output_path);
    REQUIRE_FALSE(samples.empty());
    REQUIRE(sample_rate == 16000);
    std::remove(output_path.c_str());
  }
}

TEST_CASE("CLI generic parser failures use the usage exit code", "[cli][argument-contract]") {
  SECTION("unknown option") {
    auto [code, output] = exec_command(CLI + " chroma --no-such-option");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Unknown option"));
  }

  SECTION("missing option value") {
    auto [code, output] = exec_command(CLI + " chroma --n-fft");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing value for option '--n-fft'"));
  }

  SECTION("invalid numeric value") {
    auto [code, output] = exec_command(CLI + " chroma --n-fft not-a-number");
    REQUIRE(code == 2);
  }

  SECTION("legacy mode folds parser failures to one") {
    auto [code, output] = exec_command("SONARE_LEGACY_EXIT=1 " + CLI + " chroma --n-fft");
    REQUIRE(code == 1);
    REQUIRE_THAT(output, ContainsSubstring("Missing value for option '--n-fft'"));
  }

  SECTION("legacy mode folds top-level and command early failures to one") {
    auto [no_args_code, no_args_output] = exec_command("SONARE_LEGACY_EXIT=1 " + CLI);
    REQUIRE(no_args_code == 1);
    REQUIRE_THAT(no_args_output, ContainsSubstring("Usage:"));

    auto [unknown_code, unknown_output] =
        exec_command("SONARE_LEGACY_EXIT=1 " + CLI + " no-such-command");
    REQUIRE(unknown_code == 1);
    REQUIRE_THAT(unknown_output, ContainsSubstring("Unknown command"));

    auto [missing_audio_code, missing_audio_output] =
        exec_command("SONARE_LEGACY_EXIT=1 " + CLI + " chroma");
    REQUIRE(missing_audio_code == 1);
    REQUIRE_THAT(missing_audio_output, ContainsSubstring("Missing audio file"));
  }
}

TEST_CASE("CLI exception exit codes include cancellation", "[cli][contract]") {
  // This table is intentionally exercised through the same exception-to-exit
  // mapping used by main(), rather than making cancellation a synthetic CLI
  // command.  Cancellation is emitted by cooperative core operations.
  REQUIRE(cli_exit_code_for_error(sonare::ErrorCode::Cancelled, false) == 11);
  REQUIRE(cli_exit_code_for_error(sonare::ErrorCode::Cancelled, true) == 1);
}

#if defined(SONARE_WITH_VOICE_CHANGER)
TEST_CASE("CLI voice-preset-validate emits the Batch-0 JSON envelope", "[cli][contract]") {
  const std::string valid_path = unique_temp_path("_contract_valid_preset.json");
  const std::string invalid_path = unique_temp_path("_contract_invalid_preset.json");
  {
    std::ofstream valid(valid_path);
    valid << R"({"schemaVersion":1,"id":"contract-fixture","name":"Contract Fixture",)"
             R"("category":"custom","macros":{"pitch":0,"formant":1,"brightness":0,)"
             R"("space":0,"intensity":0.5,"noiseControl":0,"sibilance":0}})";
    std::ofstream invalid(invalid_path);
    invalid << R"({"schemaVersion":1,"id":"contract-invalid","name":"Invalid",)"
               R"("category":"custom","dsp":{}})";
  }

  auto [valid_code, valid_output] =
      exec_command(CLI + " voice-preset-validate " + valid_path + " --json");
  REQUIRE(valid_code == 0);
  const auto valid_payload = sonare::util::json::parse_strict(valid_output);
  REQUIRE(valid_payload["ok"].as_bool());
  REQUIRE(valid_payload["normalized_json"].is_string());

  auto [invalid_code, invalid_output] =
      exec_command(CLI + " voice-preset-validate " + invalid_path + " --json");
  REQUIRE(invalid_code == 3);
  const auto invalid_payload = sonare::util::json::parse_strict(invalid_output);
  REQUIRE_FALSE(invalid_payload["ok"].as_bool());
  REQUIRE(invalid_payload["error"].is_string());

  std::remove(valid_path.c_str());
  std::remove(invalid_path.c_str());
}
#endif

TEST_CASE("CLI honors --flag=false to disable a boolean flag", "[cli][argument-contract]") {
  // A presence-only flag given `=false`/`=0`/`=no`/`=off` must be treated as
  // absent, not enabled. fix-frames pads by default; `--no-pad` disables padding.
  auto [on_code, on_output] = exec_command(CLI + " fix-frames --values 1,2,3,4,5 --no-pad --json");
  REQUIRE(on_code == 0);
  REQUIRE_THAT(on_output, ContainsSubstring("[1, 2, 3, 4, 5]"));

  // `--no-pad=false` disables the flag, so padding is applied (leading 0), the
  // same as omitting the flag entirely.
  auto [off_code, off_output] =
      exec_command(CLI + " fix-frames --values 1,2,3,4,5 --no-pad=false --json");
  REQUIRE(off_code == 0);
  REQUIRE_THAT(off_output, ContainsSubstring("[0, 1, 2, 3, 4, 5]"));

  // `--no-pad=true` enables it, matching the bare flag.
  auto [true_code, true_output] =
      exec_command(CLI + " fix-frames --values 1,2,3,4,5 --no-pad=true --json");
  REQUIRE(true_code == 0);
  REQUIRE_THAT(true_output, ContainsSubstring("[1, 2, 3, 4, 5]"));
}

TEST_CASE("CLI applies every repeated --set assignment, not just the last",
          "[cli][argument-contract]") {
  // Regression: the native --set was stored in a std::map keyed on the option
  // name, so `--set a --set b` silently kept only b. A repeated --set must apply
  // every assignment, matching the Python CLI's argparse action="append".
  const std::string preset_path = unique_temp_path("_preset.json");
  auto [gen_code, gen_output] = exec_command(CLI + " voice-preset > " + preset_path);
  REQUIRE(gen_code == 0);

  auto [code, output] =
      exec_command(CLI + " voice-preset-validate " + preset_path +
                   " --set dsp.retune.semitones=-9.375 --set dsp.formant.factor=1.5");
  std::remove(preset_path.c_str());
  REQUIRE(code == 0);
  // Both assignments must land in the normalized config. The first (-9.375) is the
  // discriminator: before the fix it was dropped and semitones kept its default.
  REQUIRE_THAT(output, ContainsSubstring("-9.375"));
  REQUIRE_THAT(output, ContainsSubstring("1.5"));
}

TEST_CASE("CLI option values do not depend on the option's side of the command word",
          "[cli][argument-contract]") {
  const auto parse = [](std::initializer_list<const char*> words) {
    std::vector<std::string> storage(words.begin(), words.end());
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& word : storage) argv.push_back(word.data());
    return ArgParser::parse(static_cast<int>(argv.size()), argv.data());
  };

  SECTION("a repeatable option written before the command keeps every occurrence") {
    // The parser used to classify an occurrence the moment it read it, so one
    // written before the command token had no registry entry to consult: --set
    // reached `options` as a last-one-wins scalar and never reached
    // `repeated_options`. get_string_list() then reported zero assignments and
    // the handler returned success on an unedited config.
    const CliArgs leading = parse({"sonare-cli", "--set", "dsp.retune.semitones=-9.375", "--set",
                                   "dsp.formant.factor=1.5", "voice-preset-validate", "p.json"});
    const CliArgs trailing =
        parse({"sonare-cli", "voice-preset-validate", "p.json", "--set",
               "dsp.retune.semitones=-9.375", "--set", "dsp.formant.factor=1.5"});
    const std::vector<std::string> expected = {"dsp.retune.semitones=-9.375",
                                               "dsp.formant.factor=1.5"};
    REQUIRE(leading.get_string_list("set") == expected);
    REQUIRE(trailing.get_string_list("set") == expected);
    REQUIRE(leading.options == trailing.options);
    REQUIRE(leading.repeated_options == trailing.repeated_options);
    REQUIRE(validate_cli_arguments(leading, false).empty());
  }

  SECTION("a leading alias lands under the same canonical key as a trailing one") {
    // Reading through an alias always worked, but the key the alias was stored
    // under depended on whether the command was known yet, which is the same
    // position dependence seen from the other end.
    const CliArgs leading =
        parse({"sonare-cli", "--target-sr", "8000", "resample", "in.wav", "-o", "out.wav"});
    const CliArgs trailing =
        parse({"sonare-cli", "resample", "in.wav", "--target-sr", "8000", "-o", "out.wav"});
    REQUIRE(leading.get_int("target-rate", -1) == 8000);
    REQUIRE(leading.options == trailing.options);
    REQUIRE(validate_cli_arguments(leading, true).empty());
  }
}

TEST_CASE("CLI --set applies identically on either side of the command word",
          "[cli][argument-contract]") {
  const std::string preset_path = unique_temp_path("_leading_set_preset.json");
  auto [gen_code, gen_output] = exec_command(CLI + " voice-preset > " + preset_path);
  REQUIRE(gen_code == 0);

  const std::string assignments = " --set dsp.retune.semitones=-9.375 --set dsp.formant.factor=1.5";
  auto [trailing_code, trailing_output] =
      exec_command(CLI + " voice-preset-validate " + preset_path + assignments + " --json");
  auto [leading_code, leading_output] =
      exec_command(CLI + assignments + " voice-preset-validate " + preset_path + " --json");
  std::remove(preset_path.c_str());

  REQUIRE(trailing_code == 0);
  REQUIRE(leading_code == trailing_code);
  // Byte-identical, not merely both successful: the leading form used to exit 0
  // with "ok": true and every assignment dropped, so an unedited render shipped
  // as a success and only the value told the two apart.
  REQUIRE(leading_output == trailing_output);
  REQUIRE_THAT(leading_output, ContainsSubstring("-9.375"));
}

TEST_CASE("CLI --set delivers a JSON value that contains commas intact",
          "[cli][argument-contract]") {
  // Repeated --set was folded into one comma-joined string and split back
  // apart, so every value carrying a comma of its own -- a JSON object, an
  // array, or ordinary free text -- was torn into fragments, with no escape
  // available. One occurrence is one assignment, and the value reaches the JSON
  // parser byte for byte.
  const std::string preset_path = unique_temp_path("_macro_preset.json");
  {
    std::ofstream preset(preset_path);
    preset << R"({"schemaVersion":1,"id":"set-fixture","name":"Set Fixture","category":"custom",)"
           << R"("macros":{"pitch":0,"formant":1,"brightness":0,"space":0,"intensity":0.5,)"
           << R"("noiseControl":0,"sibilance":0}})";
  }

  SECTION("an object value and a comma-bearing string both survive") {
    auto [code, output] = exec_command(CLI + " voice-preset-validate " + preset_path +
                                       " --set 'description=Adds warmth, presence, and air'"
                                       " --set 'macros={\"pitch\":3,\"brightness\":0.75}' --json");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Adds warmth, presence, and air"));
    // Both members of the object have to arrive: pitch drives retune.semitones
    // and brightness 0.75 drives presenceDb +3, so either one alone would leave
    // the other at its fixture value.
    REQUIRE_THAT(output, ContainsSubstring("\\\"semitones\\\":3"));
    REQUIRE_THAT(output, ContainsSubstring("\\\"presenceDb\\\":3"));
  }

  SECTION("an array value reaches the preset validator as an array") {
    auto [code, output] = exec_command(CLI + " voice-preset-validate " + preset_path +
                                       " --set 'macros.pitch=[1,2]' --json");
    REQUIRE(code == 3);
    // The preset schema rejects the array on its own terms. The splitter used
    // to fail first, on the orphaned "2]" fragment, which never reached the
    // schema at all.
    REQUIRE_THAT(output, ContainsSubstring("field must be numeric: macros.pitch"));
    REQUIRE_THAT(output, !ContainsSubstring("invalid --set assignment"));
  }

  std::remove(preset_path.c_str());
}

TEST_CASE("CLI voice-preset-validate rejects an invalid preset document",
          "[cli][argument-contract]") {
  const std::string preset_path = unique_temp_path("_invalid_preset.json");
  auto [generate_code, generate_output] = exec_command(CLI + " voice-preset > " + preset_path);
  REQUIRE(generate_code == 0);
  (void)generate_output;

  std::ifstream input(preset_path);
  REQUIRE(input.good());
  const std::string baseline((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  const std::string needle = "\"schemaVersion\":1";
  const auto version = baseline.find(needle);
  REQUIRE(version != std::string::npos);
  {
    std::ofstream preset(preset_path);
    REQUIRE(preset.good());
    std::string invalid = baseline;
    invalid.replace(version, needle.size(), "\"schemaVersion\":9");
    preset << invalid;
  }

  auto [code, output] = exec_command(CLI + " voice-preset-validate " + preset_path);
  std::remove(preset_path.c_str());
  REQUIRE(code != 0);
  REQUIRE_THAT(output, ContainsSubstring("schemaVersion"));
}

TEST_CASE("CLI rejects -o for commands that produce no file output", "[cli][argument-contract]") {
  // A pure-analysis command has no artifact to write, so accepting -o would
  // exit 0 while silently discarding the requested destination. It must fail
  // before dispatch and never create the file.
  create_test_wav(TEST_WAV);
  const std::string out = unique_temp_path("_rejected.json");
  std::remove(out.c_str());

  auto [code, output] = exec_command(CLI + " bpm " + TEST_WAV + " -o " + out + " -q");
  REQUIRE(code == 2);
  REQUIRE_THAT(output, ContainsSubstring("does not produce a file output"));
  std::ifstream f(out);
  REQUIRE_FALSE(f.good());

  // The long-form spelling is rejected identically.
  auto [long_code, long_output] =
      exec_command(CLI + " lufs " + TEST_WAV + " --output " + out + " -q");
  REQUIRE(long_code == 2);
  REQUIRE_THAT(long_output, ContainsSubstring("does not produce a file output"));

  // Promoted analysis paths must reject both spellings at the parser/schema
  // boundary, before loading the audio file.  Their contract exit is usage=2
  // (and the compatibility mode still folds it to legacy=1).
  for (const std::string command : {"analyze", "spectral"}) {
    auto [long_analysis_code, long_analysis_output] =
        exec_command(CLI + " " + command + " " + TEST_WAV + " --output " + out + " -q");
    REQUIRE(long_analysis_code == 2);
    REQUIRE_THAT(long_analysis_output, ContainsSubstring("does not produce a file output"));

    auto [short_analysis_code, short_analysis_output] =
        exec_command(CLI + " " + command + " " + TEST_WAV + " -o " + out + " -q");
    REQUIRE(short_analysis_code == 2);
    REQUIRE_THAT(short_analysis_output, ContainsSubstring("does not produce a file output"));

    auto [legacy_long_code, legacy_long_output] =
        exec_command("SONARE_LEGACY_EXIT=1 " + CLI + " " + command + " " + TEST_WAV + " --output " +
                     out + " -q");
    REQUIRE(legacy_long_code == 1);
    REQUIRE_THAT(legacy_long_output, ContainsSubstring("does not produce a file output"));

    auto [legacy_short_code, legacy_short_output] = exec_command(
        "SONARE_LEGACY_EXIT=1 " + CLI + " " + command + " " + TEST_WAV + " -o " + out + " -q");
    REQUIRE(legacy_short_code == 1);
    REQUIRE_THAT(legacy_short_output, ContainsSubstring("does not produce a file output"));
  }
}

TEST_CASE("CLI refuses an enumerator index outside its enumeration", "[cli][argument-contract]") {
  // The switches that map one of these indices answer every unrecognized value
  // with their first enumerator, so an unchecked `--type 999` applied a peak
  // filter, wrote a normal --json payload and exited 0 -- a caller who mistyped
  // a high-pass got a bell curve with nothing to say so. The refusal is declared
  // in the registry, next to the option, and enforced once before dispatch, so
  // it needs no audio file to happen.
  create_test_wav(TEST_WAV);
  const std::string eq_output = unique_temp_path("_enum_eq.wav");

  struct EnumOption {
    const char* command;
    const char* option;
    const char* first_rejected;
    const char* last_accepted;
    // False when the last enumerator is inside the domain but unreachable
    // through this command for an unrelated reason, so only the domain verdict
    // can be asserted. `--placement` is the one case: mid and side ask for a
    // stereo backend, and the CLI loads every input as mono.
    bool last_accepted_runs;
    std::string rest;
  };
  const std::vector<EnumOption> options = {
#ifdef SONARE_WITH_MASTERING
      {"eq", "type", "9", "8", true, TEST_WAV + " -o " + eq_output + " -q"},
      {"eq", "coeff-mode", "2", "1", true, TEST_WAV + " -o " + eq_output + " -q"},
      {"eq", "placement", "5", "4", false, TEST_WAV + " -o " + eq_output + " -q"},
      {"eq", "phase-mode", "4", "3", true, TEST_WAV + " -o " + eq_output + " -q"},
      {"eq", "resolution", "6", "5", true, TEST_WAV + " -o " + eq_output + " -q"},
#endif
      {"vector-normalize", "norm-type", "4", "3", true, "--values 1,2,3"},
  };

  for (const EnumOption& option : options) {
    CAPTURE(option.command, option.option);
    const std::string flag = std::string(" --") + option.option + " ";
    const std::string expected = std::string("invalid value for --") + option.option;

    auto [rejected_code, rejected_output] =
        exec_command(CLI + " " + option.command + flag + option.first_rejected + " " + option.rest);
    REQUIRE(rejected_code == 3);
    REQUIRE_THAT(rejected_output, ContainsSubstring(expected));

    auto [negative_code, negative_output] =
        exec_command(CLI + " " + option.command + flag + "-1 " + option.rest);
    REQUIRE(negative_code == 3);
    REQUIRE_THAT(negative_output, ContainsSubstring(expected));

    // The last enumerator is inside the domain, so it must get past validation
    // rather than be refused by an off-by-one bound.
    auto [accepted_code, accepted_output] =
        exec_command(CLI + " " + option.command + flag + option.last_accepted + " " + option.rest);
    REQUIRE_THAT(accepted_output, !ContainsSubstring(expected));
    if (option.last_accepted_runs) REQUIRE(accepted_code == 0);
  }
  std::remove(eq_output.c_str());
}

TEST_CASE("CLI checks --fmin against the effective --fmax, not only a supplied one",
          "[cli][argument-contract]") {
  // `--fmin 3000` inverts the range against the 2093 Hz default `--fmax` just as
  // surely as an explicit pair does. Gating the cross-option check on both being
  // present let this invocation reach a core SONARE_CHECK that names neither
  // option and reports the invalid-parameter class, so the same mistake had two
  // exit codes and two messages depending on how it was typed.
  create_test_wav(TEST_WAV);

  auto [code, output] = exec_command(CLI + " pitch " + TEST_WAV + " --fmin 3000 -q");
  REQUIRE(code == 2);
  REQUIRE_THAT(output, ContainsSubstring("--fmax must be greater than --fmin"));
  // The effective value of the half the caller never typed is what explains the
  // refusal.
  REQUIRE_THAT(output, ContainsSubstring("2093"));

  // The symmetric case: a lone --fmax below the 65 Hz default --fmin.
  auto [reverse_code, reverse_output] = exec_command(CLI + " pitch " + TEST_WAV + " --fmax 40 -q");
  REQUIRE(reverse_code == 2);
  REQUIRE_THAT(reverse_output, ContainsSubstring("--fmax must be greater than --fmin"));

  // A valid lone --fmin still runs.
  auto [ok_code, ok_output] = exec_command(CLI + " pitch " + TEST_WAV + " --fmin 100 --json -q");
  REQUIRE(ok_code == 0);
  REQUIRE_THAT(ok_output, !ContainsSubstring("--fmax must be greater"));
}

#ifdef SONARE_WITH_MIXING
TEST_CASE("CLI answers the channel strip under one spelling only",
          "[cli][registry][argument-contract]") {
  // `mix` was this command's deprecated alias and named a different capability
  // on the Python CLI -- a scene mixer -- so one command line meant two things
  // depending on which front-end ran it. The spelling is gone rather than
  // documented, which is only true while nothing reintroduces it: a second row
  // naming the other as an alias never uses the alias path (path lookup wins),
  // so each name would again be validated against its own copy of the option
  // list and an option added to one would become unknown under the other.
  const CliCommandSpec* canonical = cli_command_spec_for_path("mix-strip");
  REQUIRE(canonical != nullptr);
  REQUIRE(cli_command_spec_for_path("mix") == nullptr);
  REQUIRE(std::find(canonical->aliases.begin(), canonical->aliases.end(), "mix") ==
          canonical->aliases.end());

  size_t rows = 0;
  for (const auto& command : cli_command_registry()) {
    if (command.path == "mix" || command.path == "mix-strip") ++rows;
  }
  REQUIRE(rows == 1);

  // The removed spelling is refused as a command rather than as a bad option,
  // which is what tells a caller the name is gone instead of mistyped.
  auto [removed_code, removed_output] = exec_command(CLI + " mix --width 1.5");
  REQUIRE(removed_code != 0);
  REQUIRE_THAT(removed_output, !ContainsSubstring("Unknown option"));

  // The surviving spelling still accepts its own options and rejects others.
  auto [code, output] = exec_command(CLI + " mix-strip --definitely-unknown-option");
  REQUIRE(code != 0);
  REQUIRE_THAT(output, ContainsSubstring("Unknown option"));

  auto [width_code, width_output] = exec_command(CLI + " mix-strip --width 1.5");
  REQUIRE_THAT(width_output, !ContainsSubstring("Unknown option"));
  (void)width_code;
}
#endif  // SONARE_WITH_MIXING

TEST_CASE("CLI reports a failed output write as an encode failure", "[cli][argument-contract]") {
  // A `-o` that resolves to a directory is an ordinary shell mistake, and it
  // fails at the atomic rename and nowhere else. Reporting it as a decode
  // failure told the caller their *input* could not be read; every other stage
  // of the same write already reported the encode class.
  create_test_wav(TEST_WAV);
  const std::string directory = unique_temp_path("_outdir");
  REQUIRE(::mkdir(directory.c_str(), 0700) == 0);

  auto [code, output] = exec_command(CLI + " normalize " + TEST_WAV + " -o " + directory + " -q");
  REQUIRE(code == 12);
  REQUIRE_THAT(output, ContainsSubstring("finalize"));

  ::rmdir(directory.c_str());
}

TEST_CASE("CLI error handling", "[cli]") {
  SECTION("unknown command") {
    auto [code, output] = exec_command(CLI + " unknown-command");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Unknown command"));
  }

  SECTION("missing audio file") {
    auto [code, output] = exec_command(CLI + " bpm");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing audio file"));
  }

  SECTION("nonexistent file") {
    auto [code, output] = exec_command(CLI + " bpm /nonexistent/file.wav -q");
    REQUIRE(code == 4);
    REQUIRE_THAT(output, ContainsSubstring("Error"));
  }

  SECTION("handler parameter failure") {
    create_test_wav(TEST_WAV);
    auto [code, output] =
        exec_command(CLI + " pitch-shift " + TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--semitones required"));
  }

  SECTION("legacy mode folds runtime failures") {
    auto [code, output] =
        exec_command("SONARE_LEGACY_EXIT=1 " + CLI + " bpm /nonexistent/file.wav -q");
    REQUIRE(code == 1);
    REQUIRE_THAT(output, ContainsSubstring("Error"));
  }
}

TEST_CASE("CLI global options", "[cli]") {
  create_test_wav(TEST_WAV);

  SECTION("custom n-fft") {
    auto [code, output] = exec_command(CLI + " mel " + TEST_WAV + " --n-fft 4096 -q");
    REQUIRE(code == 0);
  }

  SECTION("key respects an explicitly requested default n-fft") {
    auto [code, output] = exec_command(CLI + " key " + TEST_WAV + " --n-fft 2048 --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"root\""));
  }

  SECTION("custom hop-length") {
    auto [code, output] = exec_command(CLI + " mel " + TEST_WAV + " --hop-length 256 -q");
    REQUIRE(code == 0);
  }

  SECTION("custom n-mels") {
    auto [code, output] = exec_command(CLI + " mel " + TEST_WAV + " --n-mels 64 --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"n_mels\": 64"));
  }

  SECTION("equals syntax for global and command options") {
    auto [code, output] =
        exec_command(CLI + " mel " + TEST_WAV + " --n-mels=64 --fmin=20 --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"n_mels\": 64"));
  }

  SECTION("rejects numeric suffixes with the option name") {
    auto [code, output] = exec_command(CLI + " key " + TEST_WAV + " --candidates 3junk -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("--candidates"));

    auto [global_code, global_output] =
        exec_command(CLI + " mel " + TEST_WAV + " --n-mels=64junk -q");
    REQUIRE(global_code == 2);
    REQUIRE_THAT(global_output, ContainsSubstring("--n-mels"));
  }

  SECTION("rejects global DSP options outside their command schema") {
    for (const char* option :
         {"--n-fft 4096", "--hop-length 256", "--n-mels 64", "--fmin 20", "--fmax 20000"}) {
      auto [code, output] = exec_command(CLI + " version " + option + " -q");
      REQUIRE(code == 2);
      REQUIRE_THAT(output, ContainsSubstring("Unknown option"));
    }
  }

  SECTION("global n-fft and hop-length reach frame conversion handlers") {
    auto [code, output] = exec_command(
        CLI + " frames-to-samples --frames 10 --n-fft 2048 --hop-length 512 --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"samples\": 6144"));
  }

#ifdef SONARE_WITH_ARRANGEMENT
  SECTION("project bounce rejects an unknown NativeSynth preset") {
    // The rejected preset is an invalid parameter, and it stays one: the family
    // rule that rewrote every project failure to the invalid-state code is gone,
    // so this reports the class the C ABI actually returned.
    auto [code, output] = exec_command(
        CLI + " project bounce --in missing.json -o ignored.wav --synth not-a-preset -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("unknown synth preset"));
  }
#endif
}

TEST_CASE("CLI command help", "[cli]") {
  auto [code, output] = exec_command(CLI + " mel --help");
  REQUIRE(code == 0);
  REQUIRE_THAT(output, ContainsSubstring("Usage:"));
  REQUIRE_THAT(output, ContainsSubstring("mel [options] <audio_file>"));
  REQUIRE_THAT(output, ContainsSubstring("--n-mels <value>"));
  REQUIRE_THAT(output, ContainsSubstring("--fmin <value>"));

  auto [global_code, global_output] = exec_command(CLI + " --help");
  REQUIRE(global_code == 0);
  REQUIRE_THAT(global_output, ContainsSubstring("--n-mels <int>"));
  REQUIRE_THAT(global_output, ContainsSubstring("--fmin <hz>"));
  REQUIRE_THAT(global_output, ContainsSubstring("--fmax <hz>"));

#ifdef SONARE_WITH_ARRANGEMENT
  auto [project_code, project_output] = exec_command(CLI + " project validate --help");
  REQUIRE(project_code == 0);
  REQUIRE_THAT(project_output, ContainsSubstring("--strict"));
  REQUIRE_THAT(project_output, ContainsSubstring("--in <value>"));
  REQUIRE_THAT(project_output, ContainsSubstring("--output <value>"));
#endif
}
