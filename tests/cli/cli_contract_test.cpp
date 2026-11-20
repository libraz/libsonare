/// @file cli_contract_test.cpp
/// @brief Tests for the sonare CLI argument contract: parsing, the option
///        registry, option domains and exit codes.

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <functional>
#include <sstream>

#include "analysis/section_analyzer.h"
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

TEST_CASE("CLI sections threshold registry follows the core default", "[cli][sections]") {
  const auto* threshold = cli_option_spec_for_command("sections", "threshold");
  REQUIRE(threshold != nullptr);
  // Left to the library when absent, so the help states the core value rather than passing it.
  REQUIRE(threshold->default_value.kind == CliOptionDefaultKind::Null);
  CHECK(threshold->stated_default == "0.22");
  CHECK(std::stof(threshold->stated_default) == kSectionBoundaryThreshold);
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
      REQUIRE_THAT(output, ContainsSubstring("argument --size: must be an integer of at least 1"));
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
    // through the standard library's conversion; both read as the requirement,
    // naming the option and not echoing the text.
    auto [partial_code, partial_output] =
        exec_command(CLI + " pad-center --values 1,2,3 --size 1.5x");
    auto [none_code, none_output] = exec_command(CLI + " pad-center --values 1,2,3 --size abc");
    REQUIRE(partial_code == none_code);
    REQUIRE_THAT(partial_output, ContainsSubstring("argument --size: must be an integer"));
    REQUIRE_THAT(none_output, ContainsSubstring("argument --size: must be an integer"));
    REQUIRE_THAT(none_output, !ContainsSubstring("abc"));
    // A number past the C int range names the range it has to fit.
    auto [wide_code, wide_output] =
        exec_command(CLI + " pad-center --values 1,2,3 --size 99999999999");
    REQUIRE(wide_code == none_code);
    REQUIRE_THAT(
        wide_output,
        ContainsSubstring("argument --size: must be an integer in [-2147483648, 2147483647]"));
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
    REQUIRE(capabilities["features"]["ffmpeg"].as_bool() == (sonare_has_feature("ffmpeg") != 0));
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

TEST_CASE("CLI help goes to stdout and usage errors stay one line on stderr", "[cli]") {
  const auto count_lines = [](const std::string& text) {
    return std::count(text.begin(), text.end(), '\n');
  };

  SECTION("--help prints usage on stdout with nothing on stderr") {
    const auto r = exec_command_split(CLI + " --help");
    REQUIRE(r.exit_code == 0);
    REQUIRE_THAT(r.out, ContainsSubstring("Usage:"));
    REQUIRE_THAT(r.out, ContainsSubstring("ANALYSIS COMMANDS"));
    REQUIRE(r.err.empty());
  }

  SECTION("a command's --help prints on stdout") {
    const auto r = exec_command_split(CLI + " mel --help");
    REQUIRE(r.exit_code == 0);
    REQUIRE_THAT(r.out, ContainsSubstring("mel [options] <audio_file>"));
    REQUIRE(r.err.empty());
  }

  SECTION("an unknown command is one error line plus a hint on stderr") {
    const auto r = exec_command_split(CLI + " no-such-command");
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.out.empty());
    REQUIRE_THAT(r.err, ContainsSubstring("Unknown command 'no-such-command'"));
    REQUIRE_THAT(r.err, ContainsSubstring("Try 'sonare-cli --help'."));
    REQUIRE(count_lines(r.err) == 2);
    REQUIRE_THAT(r.err, !ContainsSubstring("COMMANDS:"));
  }

  SECTION("no arguments is a one-line usage error") {
    const auto r = exec_command_split(CLI);
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.out.empty());
    REQUIRE(count_lines(r.err) == 2);
    REQUIRE_THAT(r.err, ContainsSubstring("Try 'sonare-cli --help'."));
  }

  SECTION("a missing audio argument is a one-line usage error") {
    const auto r = exec_command_split(CLI + " chroma --json");
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.out.empty());
    REQUIRE(count_lines(r.err) == 2);
    REQUIRE_THAT(r.err, ContainsSubstring("Try 'sonare-cli --help'."));
  }
}

TEST_CASE("CLI --version prints the library version and exits 0", "[cli]") {
  const auto r = exec_command_split(CLI + " --version");
  REQUIRE(r.exit_code == 0);
  REQUIRE_THAT(r.out, ContainsSubstring(std::string("libsonare version ") + sonare::version()));
  REQUIRE(r.err.empty());
}

TEST_CASE("CLI hidden contract inventory is machine-readable", "[cli][contract]") {
  auto [code, output] = exec_command(CLI + " --dump-cli-contract");
  REQUIRE(code == 0);
  const auto inventory = sonare::util::json::parse_strict(output);
  REQUIRE(inventory["schema_version"].as_int() == 4);
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
      REQUIRE(command["options"][1]["default"].is_null());
      REQUIRE(command["options"][2]["default"].is_null());
    } else if (path == "pitch-shift") {
      found_pitch_shift = true;
      REQUIRE(command["options"][0]["name"].as_string() == "json");
      REQUIRE(command["options"][1]["name"].as_string() == "pitch-semitones");
      REQUIRE(command["options"][1]["required"].as_bool());
      REQUIRE(command["options"][1]["default"].is_null());
    } else if (path == "resample") {
      found_resample = true;
      REQUIRE(command["options"].size() == 3);
      REQUIRE(command["options"][1]["name"].as_string() == "target-sample-rate");
      REQUIRE(command["options"][1]["aliases"].as_array().empty());
      REQUIRE(command["options"][1]["required"].as_bool());
      REQUIRE(command["options"][1]["default"].is_null());
#ifdef SONARE_WITH_ARRANGEMENT
    } else if (path == "project.validate") {
      found_project_validate = true;
      REQUIRE(command["options"].size() == 3);
      for (const auto& option : command["options"].as_array()) {
        REQUIRE(option["name"].as_string() != "in");
      }
      REQUIRE(command["options"][2]["aliases"][0].as_string() == "o");
      REQUIRE_FALSE(command["options"][2]["required"].as_bool());
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
  REQUIRE(has_option(command("melody"), "f0-min-hz"));
  REQUIRE(has_option(command("melody"), "f0-max-hz"));
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
      REQUIRE_THAT(output, ContainsSubstring("Error:"));
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

    const auto coarser_hop = payload_of(base_command + " --hop-length 512");
    REQUIRE(coarser_hop["pitch_count"].as_int() != base["pitch_count"].as_int());

    // A measure read off voiced frames is absent when none is voiced.
    const auto mean_hz = [](const auto& payload) {
      return payload.contains("mean_frequency_hz") ? payload["mean_frequency_hz"].as_float()
                                                   : -1.0f;
    };
    const auto lowered_ceiling = payload_of(base_command + " --f0-max-hz 300");
    REQUIRE(mean_hz(lowered_ceiling) != mean_hz(base));

    const auto raised_floor = payload_of(base_command + " --f0-min-hz 500");
    REQUIRE(raised_floor.contains("mean_frequency_hz") != base.contains("mean_frequency_hz"));
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
    const std::string clicks = unique_temp_path("_clicks.wav");
    create_click_track_wav(clicks);
    const std::string base_command = CLI + " rhythm " + clicks + " --json -q";
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
  REQUIRE(fft->default_value.kind == CliOptionDefaultKind::Null);
  REQUIRE(fft->stated_default == "2048");
  REQUIRE(fft->global_lexical);

  const auto* output = cli_option_spec_for_command("pitch-shift", "o");
  REQUIRE(output != nullptr);
  REQUIRE(output->name == "output");
  REQUIRE(output->scalar_type == CliOptionScalarType::Path);
  REQUIRE(output->aliases == std::vector<std::string>{"o"});
  REQUIRE(output->global_lexical);
  // A command that cannot run without writing a file declares that in the
  // registry; its absence is refused before dispatch.
  REQUIRE(output->required);
  REQUIRE(output->default_value.kind == CliOptionDefaultKind::Null);
  // A command that only writes when asked keeps it optional.
  const auto* optional_output = cli_option_spec_for_command("trim-silence", "output");
  REQUIRE(optional_output != nullptr);
  REQUIRE_FALSE(optional_output->required);

  const auto* semitones = cli_option_spec_for_command("pitch-shift", "pitch-semitones");
  REQUIRE(semitones != nullptr);
  REQUIRE(semitones->required);
  REQUIRE(semitones->default_value.kind == CliOptionDefaultKind::Null);

  const auto* target_rate = cli_option_spec_for_command("resample", "target-sample-rate");
  REQUIRE(target_rate != nullptr);
  REQUIRE(target_rate->required);
  REQUIRE(target_rate->aliases.empty());
  REQUIRE(target_rate->default_value.kind == CliOptionDefaultKind::Null);
  REQUIRE(cli_option_spec_for_command("resample", "target-rate") == nullptr);
  REQUIRE(cli_option_spec_for_command("resample", "target-sr") == nullptr);

  const auto* key_hpss = cli_option_spec_for_command("key", "hpss");
  REQUIRE(key_hpss != nullptr);
  REQUIRE(key_hpss->name == "hpss");
  REQUIRE(key_hpss->aliases.empty());
  REQUIRE(key_hpss->scalar_type == CliOptionScalarType::Boolean);
  REQUIRE(key_hpss->default_value.kind == CliOptionDefaultKind::Boolean);
  REQUIRE_FALSE(key_hpss->default_value.boolean_value);
  REQUIRE(cli_option_spec_for_command("key", "use-hpss") == nullptr);

  const auto* candidates = cli_option_spec_for_command("key", "candidates");
  REQUIRE(candidates != nullptr);
  REQUIRE(candidates->scalar_type == CliOptionScalarType::Integer);
  REQUIRE(candidates->default_value.kind == CliOptionDefaultKind::Null);

  const auto* smoothing_window = cli_option_spec_for_command("chords", "smoothing-window");
  REQUIRE(smoothing_window != nullptr);
  REQUIRE(smoothing_window->scalar_type == CliOptionScalarType::Number);
  REQUIRE(smoothing_window->default_value.kind == CliOptionDefaultKind::Null);
  REQUIRE(smoothing_window->stated_default == "2.0");
  const auto* no_beat_sync = cli_option_spec_for_command("chords", "no-beat-sync");
  REQUIRE(no_beat_sync != nullptr);
  REQUIRE(no_beat_sync->scalar_type == CliOptionScalarType::Boolean);
  REQUIRE(no_beat_sync->default_value.kind == CliOptionDefaultKind::Boolean);
  REQUIRE_FALSE(no_beat_sync->default_value.boolean_value);
  const auto* chroma_method = cli_option_spec_for_command("chords", "chroma-method");
  REQUIRE(chroma_method != nullptr);
  REQUIRE(chroma_method->scalar_type == CliOptionScalarType::String);
  REQUIRE(chroma_method->default_value.kind == CliOptionDefaultKind::Null);
  REQUIRE(chroma_method->stated_default == "cqt");
  REQUIRE(chroma_method->domain.choices == std::vector<std::string>{"cqt", "stft", "nnls"});
  REQUIRE(cli_option_spec_for_command("chords", "no-hmm") != nullptr);
  REQUIRE(cli_option_spec_for_command("chords", "with-seventh") != nullptr);
  REQUIRE(cli_option_spec_for_command("chords", "triads-only") == nullptr);
  REQUIRE(cli_option_spec_for_command("chords", "nnls") == nullptr);
  REQUIRE(cli_option_spec_for_command("chords", "use-hmm") == nullptr);
  const auto* analyze_hpss = cli_option_spec_for_command("analyze", "hpss");
  REQUIRE(analyze_hpss != nullptr);
  REQUIRE_FALSE(analyze_hpss->default_value.boolean_value);
  REQUIRE(cli_option_spec_for_command("analyze", "no-hpss") == nullptr);

  for (const std::string command : {"onset-strength", "tempogram", "plp"}) {
    const auto* n_mels = cli_option_spec_for_command(command, "n-mels");
    REQUIRE(n_mels != nullptr);
    REQUIRE(n_mels->scalar_type == CliOptionScalarType::Integer);
    REQUIRE(n_mels->default_value.kind == CliOptionDefaultKind::Null);
    REQUIRE(n_mels->stated_default == "128");
  }
  for (const std::string command : {"fourier-tempogram", "tempogram-ratio"})
    REQUIRE(cli_option_spec_for_command(command, "n-fft") == nullptr);

  const auto* pitch_threshold = cli_option_spec_for_command("pitch", "threshold");
  REQUIRE(pitch_threshold != nullptr);
  REQUIRE(pitch_threshold->default_value.kind == CliOptionDefaultKind::Null);
  REQUIRE(pitch_threshold->stated_default == "0.1");
  const auto* pitch_hop = cli_option_spec_for_command("pitch", "hop-length");
  REQUIRE(pitch_hop != nullptr);
  REQUIRE(pitch_hop->default_value.kind == CliOptionDefaultKind::Null);
  REQUIRE(pitch_hop->stated_default == "512");
  const auto* pitch_fmin = cli_option_spec_for_command("pitch", "f0-min-hz");
  REQUIRE(pitch_fmin != nullptr);
  REQUIRE(pitch_fmin->default_value.kind == CliOptionDefaultKind::Null);
  REQUIRE(pitch_fmin->stated_default == "65.0");
  const auto* pitch_fmax = cli_option_spec_for_command("pitch", "f0-max-hz");
  REQUIRE(pitch_fmax != nullptr);
  REQUIRE(pitch_fmax->default_value.kind == CliOptionDefaultKind::Null);
  REQUIRE(pitch_fmax->stated_default == "2093.0");

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
  REQUIRE(voice_preset->default_value.kind == CliOptionDefaultKind::Null);
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
  REQUIRE(octave_bands->aliases.empty());
  REQUIRE(octave_bands->default_value.kind == CliOptionDefaultKind::Null);
  REQUIRE(cli_option_spec_for_command("estimate-room", "n-bands") == nullptr);
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
  REQUIRE(cli_option_spec_for_command("project.validate", "in") == nullptr);
  const auto* project_validate = cli_command_spec_for_path("project.validate");
  REQUIRE(project_validate != nullptr);
  REQUIRE(project_validate->positional_count == 2);
  REQUIRE(project_validate->positional_name == "project.json");
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

  // The spectral geometry is the library's, so the parse leaves it unset.
  const CliArgs chroma = parse({"sonare-cli", "chroma"});
  REQUIRE_FALSE(chroma.has("n-fft"));
  REQUIRE_FALSE(chroma.has("hop-length"));

  const CliArgs key = parse({"sonare-cli", "key", "--hpss", "--candidates", "3"});
  REQUIRE(key.has("hpss"));
  REQUIRE(key.get_int("candidates", -1) == 3);
  REQUIRE(validate_cli_arguments(key, true).empty());

  const CliArgs chords =
      parse({"sonare-cli", "chords", "--smoothing-window", "1.25", "--no-beat-sync"});
  REQUIRE(chords.get_float("smoothing-window", -1.0f) == 1.25f);
  REQUIRE(chords.has("no-beat-sync"));
  REQUIRE(validate_cli_arguments(chords, true).empty());

  // The pYIN settings are the library's, so the parse leaves them unset.
  const CliArgs pitch = parse({"sonare-cli", "pitch"});
  REQUIRE(pitch.get_float("threshold", -1.0f) == -1.0f);
  REQUIRE_FALSE(pitch.has("hop-length"));
  REQUIRE(pitch.get_float("f0-min-hz", -1.0f) == -1.0f);
  REQUIRE(pitch.get_float("f0-max-hz", -1.0f) == -1.0f);
  REQUIRE(validate_cli_arguments(pitch, true).empty());

  const CliArgs trim = parse({"sonare-cli", "trim-silence"});
  REQUIRE(trim.get_float("threshold-db", -60.0f) == -60.0f);
  REQUIRE(trim.get_float("top-db", 60.0f) == 60.0f);
  REQUIRE(validate_cli_arguments(trim, true).empty());

  const CliArgs voice = parse({"sonare-cli", "voice-change", "-o", "out.wav"});
  // No preset is a preset left out, not an empty id.
  REQUIRE_FALSE(voice.has("preset"));
  REQUIRE(voice.get_float("formant-factor", 1.0f) == 1.0f);
  REQUIRE(validate_cli_arguments(voice, true).empty());

#ifdef SONARE_WITH_ACOUSTIC_SIM
  const CliArgs estimate = parse({"sonare-cli", "estimate-room", "--n-octave-bands", "8"});
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
  REQUIRE(required.get_float("pitch-semitones", 17.0f) == 17.0f);

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
  const CliArgs project = parse({"sonare-cli", "project", "validate", "--strict", "project.json"});
  REQUIRE(project.command == "project");
  REQUIRE(project.input_file == "validate");
  REQUIRE(project.has("strict"));
  REQUIRE(project.positionals.size() == 2);
  REQUIRE(project.positionals[1] == "project.json");
  REQUIRE(validate_cli_arguments(project, false).empty());

  const CliArgs bounce =
      parse({"sonare-cli", "project", "bounce", "project.json", "-o", "out.wav", "--synth"});
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

  // A command absent from the registry was compiled out by whichever feature
  // gate governs it (arrangement, playback, ...); this checks registry
  // membership rather than repeating one `#ifndef SONARE_WITH_<FEATURE>` per
  // gate, so a new feature-gated command needs no edit here.
  const auto compiled_out = [](const std::string& command) {
    const auto& registry = cli_command_registry();
    return std::none_of(registry.begin(), registry.end(),
                        [&](const auto& entry) { return entry.path == command; });
  };
  // "<command>\t<option>" keys, so a mismatch names both halves.
  std::set<std::string> declared_domains;
  for (const auto& record : fixture["domains"].as_array()) {
    const std::string command = record["command"].as_string();
    if (compiled_out(command)) continue;
    const std::string option = record["option"].as_string();
    CAPTURE(command, option);
    declared_domains.insert(command + "\t" + option);
    const CliOptionSpec* spec = cli_option_spec_for_command(command, option);
    REQUIRE(spec != nullptr);
    REQUIRE_FALSE(spec->domain.empty());
    std::vector<std::string> choices;
    for (const auto& choice : record["choices"].as_array()) choices.push_back(choice.as_string());
    REQUIRE(spec->domain.choices == choices);
    REQUIRE(spec->domain.has_minimum == !record["minimum"].is_null());
    if (spec->domain.has_minimum) {
      REQUIRE(spec->domain.minimum == record["minimum"].as_number());
      REQUIRE(spec->domain.exclusive_minimum == record["exclusive_minimum"].as_bool());
    }
    REQUIRE(spec->domain.has_maximum == !record["maximum"].is_null());
    if (spec->domain.has_maximum) {
      REQUIRE(spec->domain.maximum == record["maximum"].as_number());
      REQUIRE(spec->domain.exclusive_maximum == record["exclusive_maximum"].as_bool());
    }
  }
  // The other direction: a domain added to the registry without a line here.
  for (const auto& command : cli_command_registry()) {
    for (const auto& option : command.options) {
      const std::string key = command.path + "\t" + option.name;
      if (!option.domain.empty()) {
        CAPTURE(command.path, option.name);
        REQUIRE(declared_domains.count(key) == 1);
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
  const auto parse_words = [](std::vector<std::string> storage) {
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& word : storage) argv.push_back(word.data());
    return ArgParser::parse(static_cast<int>(argv.size()), argv.data());
  };

  SECTION("a global option reaches the option map, so its value is used and checked") {
    // The parser used to write the dedicated field only. get_int() then missed
    // the value, fell back to the registry default, and `--hop-length 1`
    // silently framed at 512.
    const CliArgs framed = parse({"sonare-cli", "frame-signal", "--values", "1,2,3,4",
                                  "--frame-length", "2", "--hop-length", "1"});
    REQUIRE(framed.has("hop-length"));
    REQUIRE(framed.hop_length == 1);
    REQUIRE(framed.get_int("hop-length", 0) == 1);
    REQUIRE(validate_cli_arguments(framed, false).empty());
  }

  SECTION("an option the library takes without a default is required, never filled in") {
    REQUIRE(
        reject({"sonare-cli", "frame-signal", "--values", "1,2,3,4", "--frame-length", "2"}, false)
            .message == "Missing required option '--hop-length'");
    REQUIRE(
        reject({"sonare-cli", "frame-signal", "--values", "1,2,3,4", "--hop-length", "1"}, false)
            .message == "Missing required option '--frame-length'");
    REQUIRE_THAT(reject({"sonare-cli", "frame-signal", "--values", "1,2,3,4", "--frame-length", "2",
                         "--hop-length", "1", "--n-fft", "4"},
                        false)
                     .message,
                 ContainsSubstring("Unknown option '--n-fft'"));
    for (const char* omitted : {"pre-max", "post-max", "pre-avg", "post-avg", "delta", "wait"}) {
      CAPTURE(omitted);
      std::vector<std::string> words = {"sonare-cli", "peak-pick", "--values", "0,1,0"};
      for (const char* option : {"pre-max", "post-max", "pre-avg", "post-avg", "delta", "wait"}) {
        if (std::string(option) == omitted) continue;
        words.push_back(std::string("--") + option);
        words.push_back("1");
      }
      REQUIRE(validate_cli_arguments(parse_words(words), false).message ==
              "Missing required option '--" + std::string(omitted) + "'");
    }
    REQUIRE(reject({"sonare-cli", "frames-to-samples"}, false).message ==
            "Missing required option '--frames'");
    REQUIRE(reject({"sonare-cli", "samples-to-frames"}, false).message ==
            "Missing required option '--samples'");
  }

  SECTION("pitch domains are refused before dispatch") {
    for (const auto* value : {"-50", "0"}) {
      CAPTURE(value);
      const CliValidationError error = reject({"sonare-cli", "pitch", "--f0-min-hz", value}, true);
      REQUIRE_FALSE(error.empty());
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
      const CliValidationError error = reject({"sonare-cli", "pitch", "--f0-min-hz", "3000",
                                               "--f0-max-hz", "500", "--algorithm", algorithm},
                                              true);
      REQUIRE_FALSE(error.empty());
    }
    REQUIRE(
        reject({"sonare-cli", "pitch", "--f0-min-hz", "100", "--f0-max-hz", "1000"}, true).empty());

    REQUIRE_FALSE(reject({"sonare-cli", "pitch", "--algorithm", "bogus"}, true).empty());
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
    }
    // The same domain on a command that also has a required option: the missing
    // option is reported first, so supply it to reach the value check.
    const CliValidationError processor_bits =
        reject({"sonare-cli", "mastering-processor", "--processor", "gain", "--bits", "8"}, true);
    REQUIRE_FALSE(processor_bits.empty());
    REQUIRE(reject({"sonare-cli", "eq", "--bits", "24"}, true).empty());

    REQUIRE_FALSE(
        reject({"sonare-cli", "mastering-normalize-loudness", "--true-peak-oversample", "3"}, true)
            .empty());
    REQUIRE(
        reject({"sonare-cli", "mastering-normalize-loudness", "--true-peak-oversample", "8"}, true)
            .empty());
  }
#endif

  SECTION("a missing required option is refused before dispatch") {
    REQUIRE(reject({"sonare-cli", "gain", "--gain-db", "3"}, true).message ==
            "Missing required option '--output'");
    REQUIRE(reject({"sonare-cli", "gain", "--gain-db", "3", "-o", "out.wav"}, true).empty());
    REQUIRE(reject({"sonare-cli", "resample", "-o", "out.wav"}, true).message ==
            "Missing required option '--target-sample-rate'");
    REQUIRE(reject({"sonare-cli", "pitch-shift", "-o", "out.wav"}, true).message ==
            "Missing required option '--pitch-semitones'");
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
                                       "onset-strength",
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
  commands.insert(commands.end(),
                  {"mastering", "mastering-normalize-loudness", "eq", "mastering-processor",
                   "mastering-pair-processor", "mastering-pair-analyze", "mastering-stereo-analyze",
                   "mastering-processors", "mastering-pair-processors", "mastering-pair-analyses",
                   "mastering-stereo-analyses"});
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
    REQUIRE_THAT(output, ContainsSubstring("Missing required positional argument <project.json>"));
#endif
  }

  SECTION("a retired input option is an unknown option") {
#ifdef SONARE_WITH_ARRANGEMENT
    for (const char* command :
         {"project compile --in p.json", "project import-smf --smf s.mid -o o.json",
          "project import-midi2 --midi2 m.midi2 -o o.json", "midi-render --in p.json -o o.wav"}) {
      CAPTURE(command);
      auto [code, output] = exec_command(CLI + " " + command);
      REQUIRE(code == 2);
      REQUIRE_THAT(output, ContainsSubstring("Unknown option"));
    }
    auto [code, output] = exec_command(CLI + " midi-render -o o.wav");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing required positional argument <project.json>"));
#endif
  }

  SECTION("foreign project options remain path-scoped") {
#ifdef SONARE_WITH_ARRANGEMENT
    auto [code, output] = exec_command(CLI + " project validate --frames 1 --json");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Unknown option '--frames'"));
    REQUIRE_THAT(output, !ContainsSubstring("Missing required positional"));
#endif
  }

  SECTION("retired long-option spellings are unknown options") {
    create_test_wav(TEST_WAV);
    const std::string output_path = unique_temp_path("_retired_spelling.wav");
    for (const std::string& invocation :
         {" resample --target-sr 16000 " + TEST_WAV + " -o " + output_path,
          " resample --target-rate 16000 " + TEST_WAV + " -o " + output_path,
          " eq " + TEST_WAV + " --lookahead-ms 1 -o " + output_path,
          " mastering " + TEST_WAV + " --config " + output_path + ".json -o " + output_path,
          " mastering " + TEST_WAV + " --preset pop --true-peak-oversample 8 -o " + output_path,
          " estimate-room " + TEST_WAV + " --n-bands 6",
          " acoustic " + TEST_WAV + " --n-bands 6"}) {
      auto [code, output] = exec_command(CLI + invocation + " -q");
      INFO(invocation);
      REQUIRE(code == 2);
      REQUIRE_THAT(output, ContainsSubstring("Unknown option"));
    }
  }
}

TEST_CASE("CLI refuses every registry domain before the input is read",
          "[cli][argument-contract]") {
  // A value outside a declared domain is decided from the command line alone, so
  // validation refuses it by name; main() turns any validation error into the
  // usage exit before an input is opened.
  const auto number_text = [](double value) {
    std::ostringstream text;
    text << value;
    return text.str();
  };
  const auto outside = [&](const CliOptionDomain& domain) -> std::string {
    if (!domain.choices.empty()) {
      double highest = 0.0;
      for (const auto& choice : domain.choices) {
        char* end = nullptr;
        const double value = std::strtod(choice.c_str(), &end);
        if (end == choice.c_str() || *end != '\0') return "not-a-choice";
        highest = std::max(highest, value);
      }
      return number_text(highest + 1.0);
    }
    if (domain.has_minimum) {
      return number_text(domain.exclusive_minimum ? domain.minimum : domain.minimum - 1.0);
    }
    return number_text(domain.exclusive_maximum ? domain.maximum : domain.maximum + 1.0);
  };
  const auto inside = [&](const CliOptionSpec& option) -> std::string {
    if (!option.domain.choices.empty()) return option.domain.choices.front();
    if (option.domain.has_minimum) {
      return number_text(option.domain.exclusive_minimum ? option.domain.minimum + 1.0
                                                         : option.domain.minimum);
    }
    return option.scalar_type == CliOptionScalarType::Path ? "out.wav" : "1";
  };

  size_t compared = 0;
  for (const auto& command : cli_command_registry()) {
    for (const auto& option : command.options) {
      if (option.domain.empty()) continue;
      std::vector<std::string> words{"sonare-cli"};
      std::istringstream path(command.path);
      for (std::string word; std::getline(path, word, '.');) words.push_back(word);
      words.push_back("--" + option.name);
      words.push_back(outside(option.domain));
      for (const auto& other : command.options) {
        if (!other.required || other.name == option.name) continue;
        words.push_back("--" + other.name);
        words.push_back(inside(other));
      }
      std::vector<char*> argv;
      for (auto& word : words) argv.push_back(word.data());
      const CliArgs args = ArgParser::parse(static_cast<int>(argv.size()), argv.data());
      const CliValidationError error = validate_cli_arguments(args, false);
      CAPTURE(command.path, option.name, words);
      REQUIRE(error.message.rfind("argument --" + option.name + ": must ", 0) == 0);
      ++compared;
    }
  }
  REQUIRE(compared > 100);
}

TEST_CASE("CLI integer domains end at the width of the field they feed",
          "[cli][argument-contract]") {
  // A C int field ends at INT_MAX, so only an int64 option is unbounded above;
  // the library states the two as "in [N, 2147483647]" and "of at least N".
  size_t bounded = 0;
  for (const auto& command : cli_command_registry()) {
    for (const auto& option : command.options) {
      if (option.scalar_type != CliOptionScalarType::Integer || !option.domain.has_minimum ||
          !option.domain.choices.empty()) {
        continue;
      }
      CAPTURE(command.path, option.name);
      REQUIRE(option.domain.has_maximum != option.int64_width);
      if (option.domain.has_maximum) ++bounded;
    }
  }
  REQUIRE(bounded > 25);
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

  SECTION("SONARE_LEGACY_EXIT no longer changes the exit status") {
    auto [code, output] = exec_command("SONARE_LEGACY_EXIT=1 " + CLI + " chroma --n-fft");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing value for option '--n-fft'"));
  }
}

TEST_CASE("CLI exception exit codes include cancellation", "[cli][contract]") {
  // This table is intentionally exercised through the same exception-to-exit
  // mapping used by main(), rather than making cancellation a synthetic CLI
  // command.  Cancellation is emitted by cooperative core operations.
  REQUIRE(cli_exit_code_for_error(sonare::ErrorCode::Cancelled) == 11);
}

TEST_CASE("CLI exit code for StaleResult is 14, not its error code", "[cli][contract]") {
  REQUIRE(cli_exit_code_for_error(sonare::ErrorCode::StaleResult) == 14);
}

#if defined(SONARE_WITH_VOICE_CHANGER)
TEST_CASE("CLI voice-preset-validate emits the Batch-0 JSON envelope", "[cli][contract]") {
  const std::string valid_path = unique_temp_path("_contract_valid_preset.json");
  const std::string invalid_path = unique_temp_path("_contract_invalid_preset.json");
  {
    std::ofstream valid(valid_path);
    valid << R"({"schemaVersion":4,"id":"contract-fixture","name":"Contract Fixture",)"
             R"("category":"custom","macros":{"pitch":0,"formant":1,"brightness":0,)"
             R"("space":0,"intensity":0.5,"noiseControl":0,"sibilance":0}})";
    std::ofstream invalid(invalid_path);
    invalid << R"({"schemaVersion":4,"id":"contract-invalid","name":"Invalid",)"
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
                   " --set dsp.retune.pitchSemitones=-9.375 --set dsp.formant.factor=1.5");
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
    const CliArgs leading = parse(
        {"sonare-cli", "-o", "out.wav", "resample", "in.wav", "--target-sample-rate", "8000"});
    const CliArgs trailing = parse(
        {"sonare-cli", "resample", "in.wav", "--target-sample-rate", "8000", "-o", "out.wav"});
    REQUIRE(leading.output_file == "out.wav");
    REQUIRE(leading.output_file == trailing.output_file);
    REQUIRE(leading.options == trailing.options);
    REQUIRE(validate_cli_arguments(leading, true).empty());
  }
}

TEST_CASE("CLI --set applies identically on either side of the command word",
          "[cli][argument-contract]") {
  const std::string preset_path = unique_temp_path("_leading_set_preset.json");
  auto [gen_code, gen_output] = exec_command(CLI + " voice-preset > " + preset_path);
  REQUIRE(gen_code == 0);

  const std::string assignments =
      " --set dsp.retune.pitchSemitones=-9.375 --set dsp.formant.factor=1.5";
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
    preset << R"({"schemaVersion":4,"id":"set-fixture","name":"Set Fixture","category":"custom",)"
           << R"("macros":{"pitch":0,"formant":1,"brightness":0,"space":0,"intensity":0.5,)"
           << R"("noiseControl":0,"sibilance":0}})";
  }

  SECTION("an object value and a comma-bearing string both survive") {
    auto [code, output] = exec_command(CLI + " voice-preset-validate " + preset_path +
                                       " --set 'description=Adds warmth, presence, and air'"
                                       " --set 'macros={\"pitch\":3,\"brightness\":0.75}' --json");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("Adds warmth, presence, and air"));
    // Both members of the object have to arrive: pitch drives retune.pitchSemitones
    // and brightness 0.75 drives presenceDb +3, so either one alone would leave
    // the other at its fixture value.
    REQUIRE_THAT(output, ContainsSubstring("\\\"pitchSemitones\\\":3"));
    REQUIRE_THAT(output, ContainsSubstring("\\\"presenceDb\\\":3"));
  }

  SECTION("an array value reaches the preset validator as an array") {
    auto [code, output] = exec_command(CLI + " voice-preset-validate " + preset_path +
                                       " --set 'macros.pitch=[1,2]' --json");
    REQUIRE(code == 3);
    // The preset schema rejects the array on its own terms. The splitter used
    // to fail first, on the orphaned "2]" fragment, which never reached the
    // schema at all.
    REQUIRE_THAT(output, ContainsSubstring("macros.pitch must be a number"));
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
  const std::string needle = "\"schemaVersion\":4";
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
  // boundary, before loading the audio file.  Their contract exit is usage=2.
  for (const std::string command : {"analyze", "spectral"}) {
    auto [long_analysis_code, long_analysis_output] =
        exec_command(CLI + " " + command + " " + TEST_WAV + " --output " + out + " -q");
    REQUIRE(long_analysis_code == 2);
    REQUIRE_THAT(long_analysis_output, ContainsSubstring("does not produce a file output"));

    auto [short_analysis_code, short_analysis_output] =
        exec_command(CLI + " " + command + " " + TEST_WAV + " -o " + out + " -q");
    REQUIRE(short_analysis_code == 2);
    REQUIRE_THAT(short_analysis_output, ContainsSubstring("does not produce a file output"));
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
      {"eq", "type", "10", "9", true, TEST_WAV + " -o " + eq_output + " -q"},
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
    const std::string expected = std::string("argument --") + option.option + ": must be one of ";

    auto [rejected_code, rejected_output] =
        exec_command(CLI + " " + option.command + flag + option.first_rejected + " " + option.rest);
    REQUIRE(rejected_code == 2);
    REQUIRE_THAT(rejected_output, ContainsSubstring(expected));

    auto [negative_code, negative_output] =
        exec_command(CLI + " " + option.command + flag + "-1 " + option.rest);
    REQUIRE(negative_code == 2);
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

TEST_CASE("CLI checks --f0-min-hz against the effective --f0-max-hz, not only a supplied one",
          "[cli][argument-contract]") {
  // `--f0-min-hz 3000` inverts the range against the 2093 Hz default `--f0-max-hz` just as
  // surely as an explicit pair does. Gating the cross-option check on both being
  // present let this invocation reach a core SONARE_CHECK that names neither
  // option and reports the invalid-parameter class, so the same mistake had two
  // exit codes and two messages depending on how it was typed.
  create_test_wav(TEST_WAV);

  auto [code, output] = exec_command(CLI + " pitch " + TEST_WAV + " --f0-min-hz 3000 -q");
  REQUIRE(code == 2);
  REQUIRE_THAT(output, ContainsSubstring("--f0-max-hz must be greater than --f0-min-hz"));
  // The effective value of the half the caller never typed is what explains the
  // refusal.
  REQUIRE_THAT(output, ContainsSubstring("2093"));

  // The symmetric case: a lone --f0-max-hz below the 65 Hz default --f0-min-hz.
  auto [reverse_code, reverse_output] =
      exec_command(CLI + " pitch " + TEST_WAV + " --f0-max-hz 40 -q");
  REQUIRE(reverse_code == 2);
  REQUIRE_THAT(reverse_output, ContainsSubstring("--f0-max-hz must be greater than --f0-min-hz"));

  // A valid lone --f0-min-hz still runs.
  auto [ok_code, ok_output] =
      exec_command(CLI + " pitch " + TEST_WAV + " --f0-min-hz 100 --json -q");
  REQUIRE(ok_code == 0);
  REQUIRE_THAT(ok_output, !ContainsSubstring("--f0-max-hz must be greater"));
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

  SECTION("missing required transform amount") {
    create_test_wav(TEST_WAV);
    auto [code, output] =
        exec_command(CLI + " pitch-shift " + TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--pitch-semitones'"));
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
        exec_command(CLI + " mel " + TEST_WAV + " --n-mels=64 --fmin-hz=20 --json -q");
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
         {"--n-fft 4096", "--hop-length 256", "--n-mels 64", "--fmin-hz 20", "--fmax-hz 20000"}) {
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
    auto [code, output] =
        exec_command(CLI + " project bounce missing.json -o ignored.wav --synth not-a-preset -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("unknown synth preset 'not-a-preset'"));
  }
#endif
}

TEST_CASE("CLI command help", "[cli]") {
  auto [code, output] = exec_command(CLI + " mel --help");
  REQUIRE(code == 0);
  REQUIRE_THAT(output, ContainsSubstring("Usage:"));
  REQUIRE_THAT(output, ContainsSubstring("mel [options] <audio_file>"));
  REQUIRE_THAT(output, ContainsSubstring("--n-mels <value>"));
  REQUIRE_THAT(output, ContainsSubstring("--fmin-hz <value>"));

  auto [global_code, global_output] = exec_command(CLI + " --help");
  REQUIRE(global_code == 0);
  REQUIRE_THAT(global_output, ContainsSubstring("--n-mels <int>"));
  REQUIRE_THAT(global_output, ContainsSubstring("--fmin-hz <hz>"));
  REQUIRE_THAT(global_output, ContainsSubstring("--fmax-hz <hz>"));

#ifdef SONARE_WITH_ARRANGEMENT
  auto [project_code, project_output] = exec_command(CLI + " project validate --help");
  REQUIRE(project_code == 0);
  REQUIRE_THAT(project_output, ContainsSubstring("--strict"));
  REQUIRE_THAT(project_output, !ContainsSubstring("--in <value>"));
  REQUIRE_THAT(project_output, ContainsSubstring("<project.json>"));
  REQUIRE_THAT(project_output, ContainsSubstring("--output <value>"));
#endif
}

// An option the handler leaves to the library states the library's value in `--help`. Leaving
// it out must render what passing that stated value renders, and a different value must render
// something else, so the row's input reaches the option at all.
namespace {

// Every case below carries this name, so the migration ledger finds them by one substring.
constexpr const char* kStatedDefaultCase =
    "CLI options left to the library reproduce their stated default";

// The files a row names. The tables are built once, at registration, with a placeholder token
// in each member; a case swaps in its own per-process paths before it runs.
struct StatedDefaultPaths {
  std::string tone_wav, short_tone_wav, clipped_wav, stepped_wav, noisy_wav, out, project_json,
      reference_smf, rir_wav, click_wav, sine_chords_wav, sine_chords_fast_wav, harmonic_chords_wav,
      sections_wav, silent_wav, glide_wav, stereo_wav, gapped_wav, takes_project, takes_out,
      short_notes_wav;
};

struct StatedRow {
  // As typed on the command line; a project leaf is spelled `project bounce`.
  std::string command;
  std::string arguments;
  std::string option;
  // Empty only where the command prints nothing the option affects.
  std::string other_value;
  // Set where the case runs over about 2 s, so the row runs in the slow tier.
  bool slow = false;
};

// A default derived from another option: each case sets that option and names the value the
// library then takes, and two cases keep the row from passing on one setting alone.
struct DerivedRow {
  std::string command;
  std::string arguments;
  std::string option;
  std::vector<std::pair<std::string, std::string>> cases;
};

// A default stated in words: what the library does then has no value the CLI could pass, so
// no run can compare it, and the reason says what the words describe.
struct ProseRow {
  std::string command;
  std::string option;
  std::string reason;
};

struct StatedDefaultTables {
  std::vector<StatedRow> rows;
  std::vector<DerivedRow> derived;
  std::vector<ProseRow> prose;
};

StatedDefaultTables build_stated_default_tables(const StatedDefaultPaths& p) {
  const std::string to_out = " -o " + p.out + " --json -q";
  // Two recordings of one progression at different tempi, so the alignment has a path to find.
  const std::string takes_arguments =
      p.takes_project + " --reference-source 1 --audio 1=" + p.sine_chords_fast_wav +
      " --audio 2=" + p.sine_chords_wav + " -o " + p.takes_out + " --json -q";
  StatedDefaultTables tables;
  tables.rows = {
      {"preemphasis", p.tone_wav + to_out, "coef", "0.5"},
      {"deemphasis", p.tone_wav + to_out, "coef", "0.5"},
      {"power-to-db", "--values 0.5,0.25", "ref", "2"},
      {"power-to-db", "--values 1e-3,1e-12", "amin", "1e-9"},
      {"power-to-db", "--values 1,1e-9", "top-db", "100"},
      {"amplitude-to-db", "--values 0.5", "ref", "2"},
      {"amplitude-to-db", "--values 1e-2,1e-7", "amin", "1e-4"},
      {"amplitude-to-db", "--values 1,1e-5", "top-db", "120"},
      {"db-to-power", "--values -3", "ref", "2"},
      {"db-to-amplitude", "--values -3", "ref", "2"},
      {"pad-center", "--values 1,2 --size 4", "pad-value", "9"},
      {"fix-length", "--values 1,2 --size 4", "pad-value", "9"},
      {"fix-frames", "--values 3,5", "x-min", "1"},
      {"fix-frames", "--values 3,5", "x-max", "10"},
      {"vector-normalize", "--values 3,4", "norm-type", "2"},
      {"vector-normalize", "--values 0.3,0.4", "threshold", "1"},
      {"frames-to-samples", "--frames 10", "n-fft", "2048"},
      {"samples-to-frames", "--samples 5000", "n-fft", "2048"},
      {"frames-to-samples", "--frames 10", "hop-length", "256"},
      {"samples-to-frames", "--samples 5000", "hop-length", "256"},
      {"pcen", "--values 1,2,3,4 --n-bins 2 --n-frames 2", "sample-rate", "44100"},
      {"pcen", "--values 1,2,3,4 --n-bins 2 --n-frames 2", "time-constant", "0.1"},
      {"pcen", "--values 1,2,3,4 --n-bins 2 --n-frames 2", "gain", "0.5"},
      {"pcen", "--values 1,2,3,4 --n-bins 2 --n-frames 2", "bias", "1"},
      {"pcen", "--values 1,2,3,4 --n-bins 2 --n-frames 2", "power", "0.25"},
      {"pcen", "--values 1,2,3,4 --n-bins 2 --n-frames 2", "eps", "10"},
      {"pcen", "--values 1,2,3,4 --n-bins 2 --n-frames 2", "hop-length", "128"},
      {"tone", "--frequency-hz 440" + to_out, "sample-rate", "16000"},
      {"tone", "--frequency-hz 440" + to_out, "duration-sec", "0.5"},
      {"tone", "--frequency-hz 440" + to_out, "phase", "1"},
      {"tone", "--frequency-hz 440" + to_out, "amplitude", "0.5"},
      {"chirp", "--fmin-hz 100 --fmax-hz 1000" + to_out, "sample-rate", "16000"},
      {"chirp", "--fmin-hz 100 --fmax-hz 1000" + to_out, "duration-sec", "0.5"},
      {"clicks", "--times 0.1" + to_out, "sample-rate", "16000"},
      {"clicks", "--times 0.1" + to_out, "length", "44100"},
      {"clicks", "--times 0.1" + to_out, "frequency-hz", "500"},
      {"clicks", "--times 0.1" + to_out, "click-duration-sec", "0.05"},
      {"melody", p.noisy_wav + " --json -q", "threshold", "0.9"},
      {"melody", p.tone_wav + " --json -q", "hop-length", "512"},
      {"melody", p.tone_wav + " --json -q", "f0-min-hz", "500"},
      {"melody", p.tone_wav + " --json -q", "f0-max-hz", "300"},
      {"cqt", p.tone_wav + " --json -q", "n-bins", "48"},
      {"cqt", p.tone_wav + " --json -q", "bins-per-octave", "24"},
      {"vqt", p.tone_wav + " --json -q", "n-bins", "48"},
      {"vqt", p.tone_wav + " --json -q", "bins-per-octave", "24"},
      {"vqt", p.tone_wav + " --json -q", "gamma", "0"},
      // The vqt JSON prints only shape, fmin, bins per octave and gamma, none of which depend on
      // it.
      {"vqt", p.tone_wav + " --json -q", "filter-scale", ""},
      {"meter", p.clipped_wav + " --json -q", "clip-threshold", "0.2"},
      {"meter", p.clipped_wav + " --json -q", "oversample", "1"},
      {"clipping", p.clipped_wav + " --json -q", "threshold", "0.2"},
      {"clipping", p.clipped_wav + " --json -q", "min-region", "100000"},
      {"dynamic-range", p.stepped_wav + " --json -q", "window-sec", "1"},
      {"dynamic-range", p.stepped_wav + " --window-sec 0.5 --json -q", "hop-sec", "0.25"},
      {"dynamic-range", p.stepped_wav + " --window-sec 0.5 --json -q", "low-percentile", "0.5"},
      {"dynamic-range", p.stepped_wav + " --window-sec 0.5 --json -q", "high-percentile", "0.5"},
      // Options a steady tone cannot move read a chord, section or glide input built for them.
      {"analyze", p.tone_wav + " --json -q", "chroma-high-pass-hz", "4000"},
      {"analyze", p.tone_wav + " --json -q", "meter-denominator", "8"},
      {"key", p.tone_wav + " --json -q", "high-pass-hz", "4000"},
      {"key", p.tone_wav + " --json -q", "modes", "all"},
      {"key", p.tone_wav + " --json -q", "genre-hint", "edm"},
      // A steady tone has no meter to choose; clicks every 0.4 s do. Three full analyses of the
      // 6 s track take about 4 s.
      {"analyze", p.click_wav + " --json -q", "meter-candidates", "5,7", true},
      // Options read past the chroma stage take the cheaper STFT chroma.
      {"chords", p.sine_chords_wav + " --chroma-method stft --json -q", "min-duration-sec", "1.5"},
      {"chords", p.tone_wav + " --chroma-method stft --json -q", "smoothing-window", "0.5"},
      {"chords", p.tone_wav + " --chroma-method stft --json -q", "threshold", "0.99"},
      {"chords", p.tone_wav + " --json -q", "chroma-method", "stft"},
      {"chords", p.harmonic_chords_wav + " --chroma-method stft --json -q", "hmm-beam-width", "1"},
      {"chords", p.sine_chords_fast_wav + " --key-context --json -q", "key-root", "D"},
      {"chords", p.sine_chords_fast_wav + " --key-context --json -q", "key-mode", "minor"},
      {"sections", p.sections_wav + " --json -q", "min-duration-sec", "1"},
      {"sections", p.sections_wav + " --min-duration-sec 1 --json -q", "threshold", "0.9"},
      {"dynamics", p.tone_wav + " --json -q", "window-sec", "0.1"},
      // start-bpm answers only when the estimator finds no tempo, which silence guarantees.
      {"rhythm", p.silent_wav + " --json -q", "start-bpm", "80"},
      {"rhythm", p.click_wav + " --json -q", "bpm-min", "160"},
      {"rhythm", p.click_wav + " --json -q", "bpm-max", "100"},
      {"pitch", p.noisy_wav + " --json -q", "threshold", "0.9"},
      {"pitch", p.tone_wav + " --json -q", "f0-min-hz", "500"},
      {"pitch", p.tone_wav + " --json -q", "f0-max-hz", "300"},
      {"tempogram", p.tone_wav + " --json -q", "win-length", "128"},
      {"plp", p.tone_wav + " --json -q", "win-length", "128"},
      {"acoustic", p.tone_wav + " --json -q", "n-octave-bands", "3"},
      {"hpss", p.tone_wav + " --harmonic-only" + to_out, "kernel-harmonic", "7"},
      {"hpss", p.tone_wav + " --harmonic-only" + to_out, "kernel-percussive", "7"},
      {"decompose-stems", p.short_tone_wav + to_out, "n-components", "2"},
      {"decompose-stems", p.short_tone_wav + to_out, "n-iter", "5"},
      {"decompose-stems", p.short_tone_wav + to_out, "beta", "1"},
      {"decompose-stems", p.short_tone_wav + to_out, "init", "nndsvd"},
      {"decompose-stems", p.short_tone_wav + to_out, "mask-power", "2"},
#ifdef SONARE_WITH_ACOUSTIC_SIM
      {"acoustic", p.rir_wav + " --json -q", "min-decay-db", "20"},
      {"acoustic", p.rir_wav + " --json -q", "noise-floor-margin-db", "60"},
      {"estimate-room", p.rir_wav + " --json -q", "aspect-lw", "2"},
      {"estimate-room", p.rir_wav + " --json -q", "aspect-lh", "2"},
      {"estimate-room", p.rir_wav + " --json -q", "reference-absorption", "0.5"},
      {"synthesize-rir", "--max-sec 0.3" + to_out, "length", "10"},
      {"synthesize-rir", "--max-sec 0.3" + to_out, "width", "8"},
      {"synthesize-rir", "--max-sec 0.3" + to_out, "height", "4"},
      {"synthesize-rir", "--max-sec 0.3" + to_out, "absorption", "0.5"},
      {"synthesize-rir", "--max-sec 0.3" + to_out, "source-x", "2"},
      {"synthesize-rir", "--max-sec 0.3" + to_out, "source-y", "2"},
      {"synthesize-rir", "--max-sec 0.3" + to_out, "source-z", "1.5"},
      {"synthesize-rir", "--max-sec 0.3" + to_out, "listener-x", "4"},
      {"synthesize-rir", "--max-sec 0.3" + to_out, "listener-y", "3"},
      {"synthesize-rir", "--max-sec 0.3" + to_out, "listener-z", "1.2"},
      {"synthesize-rir", "--max-sec 0.3" + to_out, "ism-order", "1"},
      {"synthesize-rir", "--max-sec 0.3" + to_out, "seed", "7"},
      {"room-morph", p.tone_wav + " --max-sec 0.3" + to_out, "absorption", "0.5"},
      {"room-morph", p.tone_wav + " --max-sec 0.3" + to_out, "source-x", "2"},
      {"room-morph", p.tone_wav + " --max-sec 0.3" + to_out, "source-y", "2"},
      {"room-morph", p.tone_wav + " --max-sec 0.3" + to_out, "source-z", "1.5"},
      {"room-morph", p.tone_wav + " --max-sec 0.3" + to_out, "listener-x", "4"},
      {"room-morph", p.tone_wav + " --max-sec 0.3" + to_out, "listener-y", "3"},
      {"room-morph", p.tone_wav + " --max-sec 0.3" + to_out, "listener-z", "1.2"},
      {"room-morph", p.tone_wav + " --max-sec 0.3" + to_out, "suppression", "1"},
      {"room-morph", p.tone_wav + " --max-sec 0.3" + to_out, "wet", "0"},
      {"room-morph", p.tone_wav + " --max-sec 0.3" + to_out, "ism-order", "1"},
      {"room-morph", p.tone_wav + " --max-sec 0.3" + to_out, "seed", "7"},
      // Only a stereo input is heard through the receiver pair; it shows in the written WAV.
      {"room-morph", p.stereo_wav + " --max-sec 0.3" + to_out, "receiver-spacing", "2.0"},
#endif
      // The spectral geometry. key reads the FFT size only with --hpss, chords only with STFT
      // chroma, and sections moves only when the minimum lets more than one section survive.
      {"timbre", p.tone_wav + " --json -q", "n-fft", "1024"},
      {"timbre", p.tone_wav + " --json -q", "hop-length", "256"},
      {"timbre", p.tone_wav + " --json -q", "n-mels", "64"},
      {"mel", p.tone_wav + " --json -q", "n-fft", "1024"},
      {"mel", p.tone_wav + " --json -q", "hop-length", "256"},
      {"mel", p.tone_wav + " --json -q", "n-mels", "64"},
      {"mel", p.tone_wav + " --json -q", "fmin-hz", "100"},
      {"chroma", p.tone_wav + " --json -q", "n-fft", "1024"},
      {"chroma", p.tone_wav + " --json -q", "hop-length", "256"},
      {"tonnetz", p.tone_wav + " --json -q", "n-fft", "1024"},
      {"tonnetz", p.tone_wav + " --json -q", "hop-length", "256"},
      {"spectral", p.tone_wav + " --json -q", "n-fft", "1024"},
      {"spectral", p.tone_wav + " --json -q", "hop-length", "256"},
      {"onset-strength", p.tone_wav + " --json -q", "n-fft", "1024"},
      {"onset-strength", p.tone_wav + " --json -q", "hop-length", "256"},
      {"onset-strength", p.tone_wav + " --json -q", "n-mels", "64"},
      {"fourier-tempogram", p.tone_wav + " --json -q", "hop-length", "256"},
      {"tempogram-ratio", p.tone_wav + " --json -q", "hop-length", "256"},
      {"fourier-tempogram", p.tone_wav + " --json -q", "win-length", "192"},
      {"tempogram-ratio", p.tone_wav + " --json -q", "win-length", "192"},
      {"tempogram", p.tone_wav + " --json -q", "n-fft", "1024"},
      {"tempogram", p.tone_wav + " --json -q", "hop-length", "256"},
      {"tempogram", p.tone_wav + " --json -q", "n-mels", "64"},
      {"plp", p.tone_wav + " --json -q", "n-fft", "1024"},
      {"plp", p.tone_wav + " --json -q", "hop-length", "256"},
      {"plp", p.tone_wav + " --json -q", "n-mels", "64"},
      {"nnls-chroma", p.tone_wav + " --json -q", "hop-length", "256"},
      {"cqt", p.tone_wav + " --json -q", "hop-length", "256"},
      {"vqt", p.tone_wav + " --json -q", "hop-length", "256"},
      {"pitch", p.tone_wav + " --json -q", "hop-length", "256"},
      {"key", p.tone_wav + " --hpss --json -q", "n-fft", "1024"},
      {"key", p.tone_wav + " --json -q", "hop-length", "256"},
      {"chords", p.sine_chords_wav + " --chroma-method stft --json -q", "n-fft", "1024"},
      {"chords", p.sine_chords_wav + " --chroma-method stft --json -q", "hop-length", "256"},
      {"sections", p.sections_wav + " --min-duration-sec 1 --json -q", "n-fft", "1024"},
      {"sections", p.sections_wav + " --min-duration-sec 1 --json -q", "hop-length", "256"},
      {"dynamics", p.tone_wav + " --json -q", "hop-length", "256"},
      {"rhythm", p.click_wav + " --json -q", "n-fft", "1024"},
      {"rhythm", p.click_wav + " --json -q", "hop-length", "256"},
      {"boundaries", p.sections_wav + " --json -q", "n-fft", "1024"},
      {"boundaries", p.sections_wav + " --json -q", "hop-length", "256"},
      {"boundaries", p.sections_wav + " --json -q", "threshold", "0.9"},
      {"boundaries", p.sections_wav + " --json -q", "absolute-threshold", "0.5"},
      {"boundaries", p.sections_wav + " --json -q", "kernel-size", "16"},
      {"boundaries", p.sections_wav + " --json -q", "n-mfcc", "4"},
      {"boundaries", p.sections_wav + " --no-mfcc --json -q", "n-chroma", "4"},
      {"boundaries", p.sections_wav + " --json -q", "peak-distance", "20"},
      // A 0.5 s window lets the weaker of the two boundaries clear --threshold 0.7 on its own.
      {"boundaries", p.sections_wav + " --threshold 0.7 --json -q", "reference-window", "0.5"},
      {"hpss", p.tone_wav + " --harmonic-only" + to_out, "n-fft", "1024"},
      {"decompose-stems", p.short_tone_wav + to_out, "n-fft", "1024"},
      {"trim-silence", p.gapped_wav + to_out, "threshold-db", "-20"},
      {"trim-silence", p.gapped_wav + to_out, "n-fft", "1024"},
      {"trim-silence", p.gapped_wav + to_out, "hop-length", "256"},
      {"split-silence", p.gapped_wav + " --json -q", "top-db", "30"},
      {"split-silence", p.gapped_wav + " --json -q", "n-fft", "1024"},
      {"split-silence", p.gapped_wav + " --json -q", "hop-length", "256"},
      {"pitch-shift", p.tone_wav + " --pitch-semitones 3" + to_out, "n-fft", "1024"},
      {"time-stretch", p.tone_wav + " --rate 1.5" + to_out, "n-fft", "1024"},
      {"mel-to-audio", p.short_tone_wav + to_out, "n-fft", "1024"},
      {"mel-to-audio", p.short_tone_wav + to_out, "hop-length", "256"},
      {"mel-to-audio", p.short_tone_wav + to_out, "n-mels", "64"},
      {"mel-to-audio", p.short_tone_wav + to_out, "fmin-hz", "100"},
      {"mel-to-audio", p.short_tone_wav + to_out, "n-iter", "8"},
      {"mfcc-to-audio", p.short_tone_wav + to_out, "n-fft", "1024"},
      {"mfcc-to-audio", p.short_tone_wav + to_out, "hop-length", "256"},
      {"mfcc-to-audio", p.short_tone_wav + to_out, "n-mels", "64"},
      {"mfcc-to-audio", p.short_tone_wav + to_out, "fmin-hz", "100"},
      {"mfcc-to-audio", p.short_tone_wav + to_out, "n-iter", "8"},
      {"mfcc-to-audio", p.short_tone_wav + to_out, "n-mfcc", "13"},
#ifdef SONARE_WITH_PITCH_EDITOR
      // A target three semitones up makes every correction setting audible; the scale
      // options act only in scale mode, where a root or mask off the tone puts it off-grid.
      {"pitch-correct-timevarying", p.tone_wav + " --target-midi 72" + to_out, "hop-length", "256"},
      {"pitch-correct-timevarying", p.tone_wav + " --target-midi 72" + to_out, "retune-amount",
       "0.5"},
      {"pitch-correct-timevarying", p.tone_wav + " --target-midi 72" + to_out,
       "max-correction-semitones", "1"},
      {"pitch-correct-timevarying", p.tone_wav + " --target-midi 72" + to_out, "retune-speed-ms",
       "500"},
      {"pitch-correct-timevarying", p.tone_wav + " --target-midi 72" + to_out,
       "vibrato-threshold-cents", ""},
      {"pitch-correct-timevarying", p.tone_wav + " --mode scale" + to_out, "scale-root", "1"},
      {"pitch-correct-timevarying", p.tone_wav + " --mode scale" + to_out, "scale-mode-mask", "1"},
      {"pitch-correct-timevarying", p.tone_wav + " --mode scale" + to_out, "reference-midi",
       "69.5"},
#ifdef SONARE_WITH_ARRANGEMENT
      {"tune-to-midi", p.tone_wav + " --reference-smf " + p.reference_smf + to_out,
       "unmatched-policy", ""},
#endif
#endif
#ifdef SONARE_WITH_ARRANGEMENT
      // Bounce does not read the option today, so nothing it prints or writes can differ.
      {"project bounce", p.project_json + " --frames 128" + to_out, "instrument-latency-samples",
       ""},
      {"midi-render", p.project_json + " --frames 128" + to_out, "instrument-latency-samples", ""},
      // The channel shows only in the written .mid status bytes.
      {"transcribe", p.tone_wav + to_out, "channel", "5"},
      {"transcribe", p.glide_wav + to_out, "monophonic-model", "singing"},
      {"transcribe", p.tone_wav + to_out, "reference-hz", "415"},
      {"transcribe", p.tone_wav + to_out, "velocity-floor-db", "-12"},
      {"transcribe", p.glide_wav + to_out, "segmentation-threshold-cents", "20"},
      {"transcribe", p.harmonic_chords_wav + " --polyphonic" + to_out, "max-polyphony", "1"},
      {"transcribe", p.harmonic_chords_wav + " --polyphonic" + to_out, "min-frame-peak-ratio",
       "0.95"},
      {"transcribe", p.harmonic_chords_wav + " --polyphonic" + to_out, "min-ridge-peak-ratio",
       "0.95"},
      {"transcribe", p.harmonic_chords_wav + " --polyphonic" + to_out, "reattack-ratio", "50"},
      {"project align-takes", takes_arguments, "hop-length", "256"},
      {"project align-takes", takes_arguments, "bins-per-octave", "24"},
#endif
#ifdef SONARE_WITH_PLAYBACK
      {"playback", p.stereo_wav + to_out, "target", "5.1"},
      // Auto takes the layout from the channel count; a fixed layout that does not match is
      // refused.
      {"playback", p.tone_wav + to_out, "input-layout", "stereo"},
      {"playback", p.stereo_wav + to_out, "night", "0.8"},
      // A stereo input has no centre channel for the dialogue gain to reach.
      {"playback", p.stereo_wav + to_out, "dialogue-db", ""},
      {"playback", p.stereo_wav + to_out, "target-lufs", "-14"},
#endif
  };
  tables.derived = {
      {"decompose-stems",
       p.short_tone_wav + to_out,
       "hop-length",
       {{"--n-fft 2048", "512"}, {"--n-fft 1024", "256"}}},
      {"hpss",
       p.tone_wav + " --harmonic-only" + to_out,
       "hop-length",
       {{"--n-fft 2048", "512"}, {"--n-fft 1024", "256"}}},
      {"pitch-shift",
       p.tone_wav + " --pitch-semitones 3" + to_out,
       "hop-length",
       {{"--n-fft 2048", "512"}, {"--n-fft 1024", "256"}}},
      {"time-stretch",
       p.tone_wav + " --rate 1.5" + to_out,
       "hop-length",
       {{"--n-fft 2048", "512"}, {"--n-fft 1024", "256"}}},
      {"normalize",
       p.gapped_wav + to_out,
       "target-db",
       {{"--mode peak", "0.0"}, {"--mode rms", "-20.0"}}},
#ifdef SONARE_WITH_ARRANGEMENT
      {"transcribe",
       p.short_notes_wav + to_out,
       "min-note-ms",
       {{"--monophonic-model segments", "30"},
        {"--monophonic-model singing", "45"},
        {"--polyphonic", "60"}}},
#endif
#ifdef SONARE_WITH_PLAYBACK
      // Auto renders the input in its own layout: mono on a mono input, stereo on a stereo one,
      // and each of those fixed layouts is refused on the other input.
      {"playback", to_out, "input-layout", {{p.tone_wav, "mono"}, {p.stereo_wav, "stereo"}}},
#endif
  };
  tables.prose = {
      {"key", "profile", "the genre hint picks the profile family"},
      {"mel", "fmax-hz", "half the input's sample rate"},
      {"voice-change", "preset", "no preset is applied"},
#ifdef SONARE_WITH_ACOUSTIC_SIM
      {"synthesize-rir", "max-sec", "the synthesized tail runs to its own end"},
      {"room-morph", "max-sec", "the synthesized tail runs to its own end"},
#endif
#ifdef SONARE_WITH_ARRANGEMENT
      {"project new", "sample-rate", "the new project's own rate"},
      {"project bounce", "frames", "the project's last event plus its tail"},
      {"project bounce", "channels", "the scene master's channel count"},
      {"midi-render", "frames", "the project's last event plus its tail"},
      {"midi-render", "channels", "the scene master's channel count"},
      {"midi-render", "synth", "each channel's GM program picks its instrument"},
      {"transcribe", "tempo-bpm", "the tempo detected in the input"},
#endif
#ifdef SONARE_WITH_PLAYBACK
      {"playback", "program-lufs", "the loudness measured over the whole input"},
#endif
  };
  return tables;
}

const StatedDefaultTables& stated_default_tables() {
  static const StatedDefaultTables tables = build_stated_default_tables(
      {"@tone@",        "@short_tone@",       "@clipped@",         "@stepped@",  "@noisy@",
       "@out@",         "@project@",          "@reference_smf@",   "@rir@",      "@clicks@",
       "@sine_chords@", "@sine_chords_fast@", "@harmonic_chords@", "@sections@", "@silent@",
       "@glide@",       "@stereo@",           "@gapped@",          "@takes@",    "@takes_out@",
       "@short_notes@"});
  return tables;
}

// The registry spells a project leaf `project.bounce`.
std::string stated_registry_path(std::string command) {
  std::replace(command.begin(), command.end(), ' ', '.');
  return command;
}

// The per-process files behind the placeholder tokens: each fixture is written the first time an
// invocation names it, and every path is removed when the case ends.
class StatedDefaultFixtures {
 public:
  StatedDefaultFixtures() {
    add("@tone@", "_stated_tone.wav", [](const std::string& path) { create_test_wav(path, 1.0f); });
    add("@short_tone@", "_stated_short_tone.wav",
        [](const std::string& path) { create_test_wav(path, 0.1f); });
    add("@clipped@", "_stated_clipped.wav",
        [](const std::string& path) { create_clipped_wav(path); });
    add("@stepped@", "_stated_stepped.wav",
        [](const std::string& path) { create_stepped_level_wav(path); });
    add("@noisy@", "_stated_noisy.wav", [](const std::string& path) { create_noisy_wav(path); });
    add("@out@", "_stated_out.wav", nullptr);
    add("@project@", "_stated_project.json", [](const std::string& path) {
      REQUIRE(exec_command(CLI + " project new -o " + path + " -q").first == 0);
    });
    add("@reference_smf@", "_stated_reference.mid",
        [](const std::string& path) { create_reference_smf(path, {{0.0, 1.0, 60}}, 4.0); });
    // A tone has no decay to measure, so the room analyses read a synthesized impulse response.
    add("@rir@", "_stated_rir.wav", [](const std::string& path) {
      REQUIRE(exec_command(CLI + " synthesize-rir -o " + path + " --max-sec 0.5 -q").first == 0);
    });
    // A click every 0.4 s, so a narrowed tempo search moves the reported tempo.
    add("@clicks@", "_stated_clicks.wav", [](const std::string& path) {
      std::string click_times = "0";
      for (int i = 1; i < 20; ++i) click_times += "," + std::to_string(0.4 * i);
      REQUIRE(exec_command(CLI + " clicks --times " + click_times +
                           " --click-duration-sec 0.03 -o " + path + " -q")
                  .first == 0);
    });
    add("@sine_chords@", "_stated_sine_chords.wav",
        [](const std::string& path) { create_chord_progression_wav(path, 0.5f, false); });
    add("@sine_chords_fast@", "_stated_sine_chords_fast.wav",
        [](const std::string& path) { create_chord_progression_wav(path, 0.25f, false); });
    add("@harmonic_chords@", "_stated_harmonic_chords.wav",
        [](const std::string& path) { create_chord_progression_wav(path, 0.25f, true); });
    add("@sections@", "_stated_sections.wav",
        [](const std::string& path) { create_section_triads_wav(path); });
    add("@silent@", "_stated_silent.wav", [](const std::string& path) { create_silent_wav(path); });
    add("@glide@", "_stated_glide.wav", [](const std::string& path) { create_glide_wav(path); });
    add("@stereo@", "_stated_stereo.wav",
        [](const std::string& path) { create_stereo_delayed_wav(path); });
    // Two tones around a near-silent gap, with silence at both ends to trim.
    add("@gapped@", "_stated_gapped.wav", [](const std::string& path) {
      create_blocked_level_wav(
          path, {{0.0f, 0.3f}, {0.5f, 0.4f}, {0.003f, 0.3f}, {0.5f, 0.4f}, {0.0f, 0.3f}});
    });
    add("@takes@", "_stated_takes.json",
        [](const std::string& path) { create_takes_project(path); });
    add("@takes_out@", "_stated_takes_out.json", nullptr);
    add("@short_notes@", "_stated_short_notes.wav",
        [](const std::string& path) { create_short_notes_wav(path); });
  }

  ~StatedDefaultFixtures() {
    for (const Fixture& fixture : fixtures_) std::remove(fixture.path.c_str());
  }

  StatedDefaultFixtures(const StatedDefaultFixtures&) = delete;
  StatedDefaultFixtures& operator=(const StatedDefaultFixtures&) = delete;

  // The invocation with every token replaced by its path, each named fixture written first.
  std::string resolve(std::string invocation) {
    for (Fixture& fixture : fixtures_) {
      bool named = false;
      for (size_t at = invocation.find(fixture.token); at != std::string::npos;
           at = invocation.find(fixture.token, at + fixture.path.size())) {
        invocation.replace(at, fixture.token.size(), fixture.path);
        named = true;
      }
      if (named && fixture.create && !fixture.written) {
        fixture.create(fixture.path);
        fixture.written = true;
      }
    }
    return invocation;
  }

  // stdout plus the written file, if any, so a generator is compared by what it rendered.
  std::string rendered(const std::string& invocation) {
    auto [code, text] = outcome(invocation);
    REQUIRE(code == 0);
    return text;
  }

  // The exit code and what the run produced; a value the library refuses moves the result too.
  std::pair<int, std::string> outcome(const std::string& invocation) {
    const std::string& out = path_of("@out@");
    std::remove(out.c_str());
    auto [code, output] = exec_command(CLI + " " + invocation);
    std::ifstream file(out, std::ios::binary);
    return {code, output + std::string(std::istreambuf_iterator<char>(file), {})};
  }

 private:
  struct Fixture {
    std::string token;
    std::string path;
    std::function<void(const std::string&)> create;
    bool written = false;
  };

  void add(const char* token, const char* suffix, std::function<void(const std::string&)> create) {
    fixtures_.push_back({token, unique_temp_path(suffix), std::move(create)});
  }

  const std::string& path_of(const std::string& token) const {
    for (const Fixture& fixture : fixtures_)
      if (fixture.token == token) return fixture.path;
    FAIL("no fixture for " << token);
    return fixtures_.front().path;
  }

  std::vector<Fixture> fixtures_;
};

void run_stated_row(std::size_t index) {
  const StatedRow& row = stated_default_tables().rows[index];
  CAPTURE(row.command, row.option);
  const auto* spec = cli_option_spec_for_command(stated_registry_path(row.command), row.option);
  REQUIRE(spec != nullptr);

  StatedDefaultFixtures fixtures;
  const std::string base = fixtures.resolve(row.command + " " + row.arguments);
  const std::string omitted = fixtures.rendered(base);
  CHECK(omitted == fixtures.rendered(base + " --" + row.option + " " + spec->stated_default));
  if (!row.other_value.empty()) {
    CHECK(std::make_pair(0, omitted) !=
          fixtures.outcome(base + " --" + row.option + " " + row.other_value));
  }
}

void run_derived_row(std::size_t index) {
  const DerivedRow& row = stated_default_tables().derived[index];
  CAPTURE(row.command, row.option);
  StatedDefaultFixtures fixtures;
  for (const auto& [setting, value] : row.cases) {
    CAPTURE(setting, value);
    const std::string base = fixtures.resolve(row.command + " " + row.arguments + " " + setting);
    const std::string omitted = fixtures.rendered(base);
    CHECK(omitted == fixtures.rendered(base + " --" + row.option + " " + value));
    // Another case's value has to move this one, or the equality above could hold on any value.
    for (const auto& [other_setting, other_value] : row.cases) {
      if (other_value == value) continue;
      CAPTURE(other_value);
      CHECK(std::make_pair(0, omitted) !=
            fixtures.outcome(base + " --" + row.option + " " + other_value));
    }
  }
}

class StatedDefaultInvoker final : public Catch::ITestInvoker {
 public:
  StatedDefaultInvoker(void (*run)(std::size_t), std::size_t index) : run_(run), index_(index) {}
  void invoke() const override { run_(index_); }

 private:
  void (*run_)(std::size_t);
  std::size_t index_;
};

// One ctest case per row, so each case spawns the CLI only two or three times and stays short
// under a parallel run; a duplicated command and option pair fails registration by name.
[[maybe_unused]] const bool kStatedDefaultCasesRegistered = [] {
  static std::deque<std::string> names;
  const auto register_case = [](std::string name, const char* tags, void (*run)(std::size_t),
                                std::size_t index) {
    names.push_back(std::move(name));
    Catch::AutoReg(Catch::Detail::make_unique<StatedDefaultInvoker>(run, index),
                   CATCH_INTERNAL_LINEINFO, Catch::StringRef(),
                   Catch::NameAndTags{names.back(), tags});
  };
  const StatedDefaultTables& tables = stated_default_tables();
  for (std::size_t i = 0; i < tables.rows.size(); ++i) {
    const StatedRow& row = tables.rows[i];
    register_case(std::string(kStatedDefaultCase) + ": " + row.command + " --" + row.option,
                  row.slow ? "[cli][defaults][.][slow]" : "[cli][defaults]", &run_stated_row, i);
  }
  for (std::size_t i = 0; i < tables.derived.size(); ++i) {
    const DerivedRow& row = tables.derived[i];
    register_case(
        std::string(kStatedDefaultCase) + ": " + row.command + " --" + row.option + " derived",
        "[cli][defaults]", &run_derived_row, i);
  }
  return true;
}();

}  // namespace

TEST_CASE("CLI options left to the library each have a stated-default row", "[cli][defaults]") {
  const StatedDefaultTables& tables = stated_default_tables();

  // Every option that states a library default is in one of the two tables.
  for (const CliCommandSpec& command : cli_command_registry()) {
    for (const CliOptionSpec& option : command.options) {
      if (option.stated_default.empty()) continue;
      CAPTURE(command.path, option.name);
      const auto names = [&](const auto& row) {
        return stated_registry_path(row.command) == command.path && row.option == option.name;
      };
      CHECK((std::any_of(tables.rows.begin(), tables.rows.end(), names) ||
             std::any_of(tables.derived.begin(), tables.derived.end(), names) ||
             std::any_of(tables.prose.begin(), tables.prose.end(), names)));
    }
  }

  // And every row names an option the library leaves unset and states a default for.
  const auto check_spec = [](const std::string& command, const std::string& option) {
    CAPTURE(command, option);
    const auto* spec = cli_option_spec_for_command(stated_registry_path(command), option);
    REQUIRE(spec != nullptr);
    CHECK(spec->default_value.kind == CliOptionDefaultKind::Null);
    CHECK_FALSE(spec->stated_default.empty());
  };
  for (const StatedRow& row : tables.rows) check_spec(row.command, row.option);
  for (const DerivedRow& row : tables.derived) {
    check_spec(row.command, row.option);
    REQUIRE(row.cases.size() >= 2);
    CHECK(row.cases[0].second != row.cases[1].second);
  }
  for (const ProseRow& row : tables.prose) {
    check_spec(row.command, row.option);
    CHECK_FALSE(row.reason.empty());
  }
}

TEST_CASE("CLI loudness range keys carry the LU unit", "[cli][json]") {
  create_test_wav(TEST_WAV);
  for (const std::string command : {"dynamics", "analyze", "lufs"}) {
    CAPTURE(command);
    auto [code, output] = exec_command(CLI + " " + command + " " + TEST_WAV + " --json -q");
    REQUIRE(code == 0);
    CHECK_THAT(output, ContainsSubstring("\"loudness_range_lu\""));
    CHECK_THAT(output, !ContainsSubstring("\"loudness_range_db\""));
    CHECK_THAT(output, !ContainsSubstring("\"loudness_range\""));
  }
}
