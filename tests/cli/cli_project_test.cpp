/// @file cli_project_test.cpp
/// @brief Tests for the sonare CLI project and DAW editing commands.

#include "cli/cli_test_helpers.h"

TEST_CASE("CLI DAW editing commands", "[cli]") {
  create_test_wav(TEST_WAV, 0.5f);
  std::remove(TEST_OUT.c_str());

  SECTION("pitch-correct") {
    auto [code, output] = exec_command(CLI + " pitch-correct --current-midi 69 --target-midi 70 " +
                                       TEST_WAV + " -o " + TEST_OUT + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"target_midi\""));
    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
  }

  SECTION("note-stretch") {
    auto [code, output] =
        exec_command(CLI + " note-stretch --onset-sample 100 --offset-sample 2000 --ratio 1.2 " +
                     TEST_WAV + " -o " + TEST_OUT + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"ratio\""));
    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
  }

  SECTION("note-stretch without an offset stretches to the end of the input") {
    auto [code, output] = exec_command(CLI + " note-stretch --onset-sample 100 --ratio 1.2 " +
                                       TEST_WAV + " -o " + TEST_OUT + " --json -q");
    REQUIRE(code == 0);
    // 0.5 s at 22050 Hz is 11025 samples; the resolved bound is reported.
    REQUIRE_THAT(output, ContainsSubstring("\"offset_sample\": 11025"));
  }

  SECTION("note-stretch rounds a bound in seconds to the nearest sample") {
    // 0.25 s is exactly 5512.5 samples at 22050 Hz; a half rounds up.
    auto [code, output] =
        exec_command(CLI + " note-stretch --onset-sec 0.1 --offset-sec 0.25 --ratio 1.2 " +
                     TEST_WAV + " -o " + TEST_OUT + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"onset_sample\": 2205"));
    REQUIRE_THAT(output, ContainsSubstring("\"offset_sample\": 5513"));
  }

  SECTION("note bounds refuse both spellings of one position") {
    auto [move_code, move_output] =
        exec_command(CLI + " note-move --target-onset-sample 0 --target-onset-sec 0 " + TEST_WAV +
                     " -o " + TEST_OUT + " -q");
    CHECK(move_code == 2);
    CHECK_THAT(move_output,
               ContainsSubstring("give --target-onset-sample or --target-onset-sec, not both"));
    auto [stretch_code, stretch_output] =
        exec_command(CLI + " note-stretch --offset-sample 100 --offset-sec 0.01 " + TEST_WAV +
                     " -o " + TEST_OUT + " -q");
    CHECK(stretch_code == 2);
    CHECK_THAT(stretch_output, ContainsSubstring("give --offset-sample or --offset-sec, not both"));
  }

  SECTION("note bounds refuse a negative position") {
    auto [code, output] =
        exec_command(CLI + " note-move --onset-sec -0.01 " + TEST_WAV + " -o " + TEST_OUT + " -q");
    CHECK(code == 2);
  }

  SECTION("voice-change reports the requested factor") {
    auto [code, output] = exec_command(CLI + " voice-change --formant-factor 1.5 " + TEST_WAV +
                                       " -o " + TEST_OUT + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"formant_factor\": 1.5"));
  }

  SECTION("voice-change") {
    auto [code, output] = exec_command(CLI +
                                       " voice-change --pitch-semitones 5 --formant-factor "
                                       "1.1 " +
                                       TEST_WAV + " -o " + TEST_OUT + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"formant_factor\""));
    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
  }

  SECTION("voice-change refuses a relative formant factor outside the warp's range") {
    auto [code, output] = exec_command(CLI + " voice-change --formant-factor 100 " + TEST_WAV +
                                       " -o " + TEST_OUT + " --json -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("formant_factor must be finite and in [0.55, 1.65]"));
  }

  SECTION("voice presets are named by their library ids in any letter case") {
    auto [list_code, list] = exec_command(CLI + " voice-presets --json");
    REQUIRE(list_code == 0);
    REQUIRE_THAT(list, ContainsSubstring("\"neutralMonitor\""));
    REQUIRE_THAT(list, ContainsSubstring("\"hardTune\""));
    REQUIRE_THAT(list, !ContainsSubstring("neutral-monitor"));

    auto [code, output] = exec_command(CLI + " voice-preset --preset HARDTUNE");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"id\":\"hardTune\""));

    auto [kebab_code, kebab] = exec_command(CLI + " voice-preset --preset hard-tune");
    REQUIRE(kebab_code == 2);
    REQUIRE_THAT(kebab, ContainsSubstring("argument --preset: must be one of neutralMonitor"));
  }

  SECTION("voice-change preset rejects simple knob conflicts") {
    auto [code, output] = exec_command(
        CLI + " voice-change --preset neutralMonitor --pitch-semitones 5 --formant-factor 1.1 " +
        TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("cannot be combined with a realtime preset"));
  }

  SECTION("voice-change resolves a preset-pack entry and applies overrides") {
    const std::string pack = "schemas/realtime-voice-changer-presets.example.json";
    auto [code, output] = exec_command(CLI + " voice-change --preset-pack " + pack +
                                       " --preset neutralMonitor --set dsp.outputGainDb=-2 " +
                                       TEST_WAV + " -o " + TEST_OUT + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"preset\": \"neutralMonitor\""));
    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
  }

  SECTION("voice-change reports a pack without an entry ahead of the --set rule") {
    const std::string pack = "schemas/realtime-voice-changer-presets.example.json";
    auto [code, output] =
        exec_command(CLI + " voice-change --preset-pack " + pack + " --set dsp.outputGainDb=-2 " +
                     TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("--preset-pack requires --preset"));
  }

  SECTION("voice-preset-validate resolves a preset-pack entry and applies overrides") {
    const std::string pack = "schemas/realtime-voice-changer-presets.example.json";
    auto [code, output] =
        exec_command(CLI + " voice-preset-validate " + pack +
                     " --preset neutralMonitor --set dsp.outputGainDb=-2 --json -q");
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE(payload["ok"].as_bool());
    const auto normalized =
        sonare::util::json::parse_strict(payload["normalized_json"].as_string());
    REQUIRE(normalized["dsp"]["outputGainDb"].as_number() == -2.0);
  }
}

#if defined(SONARE_WITH_ARRANGEMENT)
// project CLI parity: the `project` command group wraps the sonare_project_* C ABI
// (headless arrangement). These shell out to the same binary as the other CLI
// tests via get_cli_path().
TEST_CASE("CLI project command group", "[cli]") {
  SECTION("unknown subcommand is a usage error") {
    auto [code, output] = exec_command(CLI + " project not-a-subcommand -q");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("unknown project subcommand"));
  }

  SECTION("a fractional project rate is refused as a malformed document") {
    // The Python CLI pins the same exit code and diagnostic (test_cli_contract.py).
    const std::string project = unique_temp_path(".sonare");
    const std::string wav = unique_temp_path(".wav");
    {
      std::ofstream out(project);
      out << R"({"version":7,"sampleRate":44100.5,"tracks":[],"clips":[]})";
    }
    const SplitResult result =
        exec_command_split(CLI + " project bounce " + project + " -o " + wav + " --frames 64");
    CHECK(result.exit_code == 5);
    CHECK_THAT(result.err, ContainsSubstring("invalid_sample_rate: sampleRate must be an integer "
                                             "in [8000, 384000]"));
    CHECK_FALSE(std::ifstream(wav).good());
    std::remove(project.c_str());
  }

  SECTION("abi prints the project ABI version") {
    auto [code, output] = exec_command(CLI + " project abi");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring(std::to_string(SONARE_PROJECT_ABI_VERSION)));
  }

  SECTION("synth-presets lists the full NativeSynth catalog") {
    auto [code, output] = exec_command(CLI + " project synth-presets --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"presets\""));
    REQUIRE_THAT(output, ContainsSubstring("\"ePiano\""));
  }

  SECTION("stdout-only subcommands reject an output path") {
    const std::string out = unique_temp_path("_project_output.json");
    for (const char* subcommand : {"abi", "compile", "synth-presets"}) {
      auto [code, output] = exec_command(CLI + " project " + subcommand + " -o " + out + " -q");
      REQUIRE(code == 2);
      REQUIRE_THAT(output, ContainsSubstring("does not produce a file output"));
      std::ifstream file(out);
      REQUIRE_FALSE(file.good());
    }
  }

  SECTION("new -> validate -> compile round-trips through the C ABI") {
    const std::string proj = unique_temp_path("_proj.json");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj);
    REQUIRE(nc == 0);

    auto [vc, vo] = exec_command(CLI + " project validate " + proj);
    REQUIRE(vc == 0);
    REQUIRE_THAT(vo, ContainsSubstring("valid"));

    auto [cc, co] = exec_command(CLI + " project compile " + proj);
    REQUIRE(cc == 0);

    std::remove(proj.c_str());
  }

  SECTION("compile JSON keeps one human message per diagnostic and always emits messages") {
    const std::string invalid = unique_temp_path("_compile_diagnostic.json");
    {
      std::ofstream file(invalid);
      file << R"json({
        "version": 7,
        "sampleRate": 48000,
        "tempoSegments": [{"startPpq": 0, "bpm": 120, "startSample": 0, "endBpm": 90}],
        "sources": [{"kind": "audio", "id": 1, "uri": "missing.wav", "channelCount": 1,
                     "sampleRateHint": 48000, "storageHandleId": 0}],
        "tracks": [{"id": 1, "name": "audio", "kind": "audio", "gain": 1, "mute": false,
                    "solo": false, "pan": 0, "channelStripRef": "", "outputTarget": "",
                    "midiDestinationId": 0, "automationLanes": []}],
        "clips": [{"id": 1, "trackId": 1, "sourceId": 1, "startPpq": 0,
                   "lengthPpq": 1, "sourceOffsetPpq": 0, "gain": 1, "loopMode": "off",
                   "loopLengthPpq": 0, "warpRefId": 0, "warpMode": "off"}]
      })json";
    }

    auto [diagnostic_code, diagnostic_output] =
        exec_command(CLI + " project compile " + invalid + " --json -q");
    // A project that loads but compiles without a renderable timeline is a
    // project-state failure, the class `project validate --strict` already
    // reports for its own parsed-but-failing outcome, and the class the Python
    // CLI reports for this same input. It used to return a plain 1, which
    // normalizes to invalid-parameter and says the arguments were wrong.
    REQUIRE(diagnostic_code == 9);
    const auto diagnostic_payload = sonare::util::json::parse_strict(diagnostic_output);
    REQUIRE(diagnostic_payload["diagnostic_count"].as_int() > 0);
    REQUIRE(diagnostic_payload["messages"].is_string());
    const auto& diagnostics = diagnostic_payload["diagnostics"];
    REQUIRE(diagnostics.size() ==
            static_cast<size_t>(diagnostic_payload["diagnostic_count"].as_int()));
    for (const auto& diagnostic : diagnostics.as_array()) {
      // Code and severity are the library's tokens, as on the Python CLI.
      REQUIRE(diagnostic["code"].is_string());
      const std::string severity = diagnostic["severity"].as_string();
      REQUIRE((severity == "error" || severity == "warning"));
      REQUIRE(diagnostic.contains("message"));
      REQUIRE(diagnostic["message"].is_string());
      REQUIRE_FALSE(diagnostic["message"].as_string().empty());
      REQUIRE(diagnostic_payload["messages"].as_string().find(diagnostic["message"].as_string()) !=
              std::string::npos);
    }
    // The trailing-ramp warning is about the project as a whole, so it names no
    // target and omits the key; a clip diagnostic carries the clip's id.
    size_t targeted = 0;
    size_t project_wide = 0;
    for (const auto& diagnostic : diagnostics.as_array()) {
      if (diagnostic.contains("target_id")) {
        REQUIRE(diagnostic["target_id"].as_int() > 0);
        ++targeted;
      } else {
        REQUIRE_THAT(diagnostic["message"].as_string(), ContainsSubstring("trailing tempo"));
        ++project_wide;
      }
    }
    REQUIRE(targeted > 0);
    REQUIRE(project_wide == 1);

    const std::string clean = unique_temp_path("_compile_clean.json");
    auto [new_code, new_output] = exec_command(CLI + " project new -o " + clean);
    REQUIRE(new_code == 0);
    auto [clean_code, clean_output] =
        exec_command(CLI + " project compile " + clean + " --json -q");
    REQUIRE(clean_code == 0);
    const auto clean_payload = sonare::util::json::parse_strict(clean_output);
    REQUIRE(clean_payload.contains("messages"));
    REQUIRE(clean_payload["messages"].is_string());
    REQUIRE(clean_payload["messages"].as_string().empty());
    REQUIRE(clean_payload["diagnostics"].is_array());
    REQUIRE(clean_payload["diagnostics"].as_array().empty());

    std::remove(invalid.c_str());
    std::remove(clean.c_str());
  }

  SECTION("export-midi2 -> import-midi2 is wired through the C ABI") {
    const std::string proj = unique_temp_path("_proj.json");
    const std::string midi2 = unique_temp_path("_clip.midi2");
    const std::string imported = unique_temp_path("_imported.json");

    auto [nc, no] = exec_command(CLI + " project new -o " + proj);
    REQUIRE(nc == 0);

    auto [ec, eo] = exec_command(CLI + " project export-midi2 " + proj + " -o " + midi2);
    REQUIRE(ec == 0);
    REQUIRE_THAT(eo, ContainsSubstring("Exported MIDI2 Clip File"));

    auto [ic, io] = exec_command(CLI + " project import-midi2 " + midi2 + " -o " + imported);
    REQUIRE(ic == 0);
    REQUIRE_THAT(io, ContainsSubstring("Imported MIDI2 Clip File"));

    std::remove(proj.c_str());
    std::remove(midi2.c_str());
    std::remove(imported.c_str());
  }

  SECTION("malformed project json fails cleanly (non-zero, no crash)") {
    const std::string bad = unique_temp_path("_bad.json");
    {
      std::ofstream f(bad);
      f << "{ this is not valid json ";
    }
    auto [code, output] = exec_command(CLI + " project validate " + bad);
    REQUIRE(code != 0);
    std::remove(bad.c_str());
  }

  SECTION("validate reports loader diagnostics and --strict rejects them") {
    const std::string project = unique_temp_path("_warning.json");
    const std::string canonical = unique_temp_path("_warning_canonical.json");
    {
      std::ofstream file(project);
      file << R"({"version":7,"clips":[{"id":1,"trackId":99,"sourceId":99,)"
              R"("lengthPpq":1.0}]})";
    }

    auto [normal_code, normal_output] =
        exec_command(CLI + " project validate " + project + " --json");
    REQUIRE(normal_code == 0);
    REQUIRE_THAT(normal_output, ContainsSubstring("\"valid\": true"));
    REQUIRE_THAT(normal_output, ContainsSubstring("dangling_clip_source"));
    REQUIRE_THAT(normal_output, ContainsSubstring("dangling_clip_track"));

    auto [strict_code, strict_output] = exec_command(CLI + " project validate " + project +
                                                     " --strict -o " + canonical + " --json");
    REQUIRE(strict_code == 9);
    REQUIRE_THAT(strict_output, ContainsSubstring("\"valid\": true"));
    REQUIRE_THAT(strict_output, ContainsSubstring("dangling_clip_source"));
    REQUIRE_THAT(strict_output, ContainsSubstring("dangling_clip_track"));
    REQUIRE(std::ifstream(canonical).good());

    std::remove(project.c_str());
    std::remove(canonical.c_str());
  }

  SECTION("validate rejects options from another project subcommand") {
    const std::string project = unique_temp_path("_validate_option_scope.json");
    auto [new_code, new_output] = exec_command(CLI + " project new -o " + project);
    REQUIRE(new_code == 0);
    auto [code, output] =
        exec_command(CLI + " project validate " + project + " --frames 123 --json");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("Unknown option '--frames'"));
    std::remove(project.c_str());
  }

  SECTION("bounce WAV header sample rate equals the render rate (default 48000)") {
    // Regression: the bounce used to tag the WAV with 44100 while the engine
    // rendered at the project rate (~2x pitch error). The reported sample_rate
    // must equal the rate the render actually used. With no --sample-rate the
    // CLI defaults to the project's own rate, which for a fresh `project new`
    // project (no --sample-rate given at creation) is 48000.
    const std::string proj = unique_temp_path("_proj.json");
    const std::string wav = unique_temp_path("_bounce.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj);
    REQUIRE(nc == 0);

    auto [bc, bo] =
        exec_command(CLI + " project bounce " + proj + " -o " + wav + " --frames 256 --json");
    REQUIRE(bc == 0);
    REQUIRE_THAT(bo, ContainsSubstring("\"sample_rate\": 48000"));

    std::remove(proj.c_str());
    std::remove(wav.c_str());
  }

  SECTION("bounce preserves the requested stereo channel count in the WAV header") {
    const std::string proj = unique_temp_path("_proj.json");
    const std::string wav = unique_temp_path("_bounce.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj);
    REQUIRE(nc == 0);

    auto [bc, bo] = exec_command(CLI + " project bounce " + proj + " -o " + wav +
                                 " --frames 256 --channels 2 --json");
    REQUIRE(bc == 0);
    REQUIRE_THAT(bo, ContainsSubstring("\"channels\": 2"));
    REQUIRE(wav_header_channel_count(wav) == 2);

    std::remove(proj.c_str());
    std::remove(wav.c_str());
  }

  SECTION("bounce writes a mono WAV header for the mono downmix") {
    // The only other width the bounce renders, and the branch the layout is
    // derived through. A layout that disagreed with the count would be refused
    // by the WAV writer rather than producing this header.
    const std::string proj = unique_temp_path("_proj.json");
    const std::string wav = unique_temp_path("_bounce_mono.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj);
    REQUIRE(nc == 0);

    auto [bc, bo] = exec_command(CLI + " project bounce " + proj + " -o " + wav +
                                 " --frames 256 --channels 1 --json");
    REQUIRE(bc == 0);
    REQUIRE_THAT(bo, ContainsSubstring("\"channels\": 1"));
    REQUIRE(wav_header_channel_count(wav) == 1);

    std::remove(proj.c_str());
    std::remove(wav.c_str());
  }

  SECTION("bounce refuses a width no speaker layout has, naming the option") {
    // The bounce renders its master at 1, 2, 6 or 8 channels. Any other width
    // used to reach the C ABI and come back as a bare
    // "bounce project: Invalid parameter" after the project had been loaded,
    // with nothing pointing at the option that caused it.
    const std::string proj = unique_temp_path("_proj.json");
    const std::string wav = unique_temp_path("_bounce_bad_channels.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj);
    REQUIRE(nc == 0);

    for (const char* count : {"3", "4"}) {
      INFO(count);
      auto [bc, bo] = exec_command(CLI + " project bounce " + proj + " -o " + wav +
                                   " --frames 256 --channels " + count + " -q");
      REQUIRE(bc == 2);
      REQUIRE_THAT(bo, ContainsSubstring("argument --channels: must be one of 1, 2, 6, 8; leave it "
                                         "out for the scene master's width"));
      std::ifstream out_file(wav);
      REQUIRE_FALSE(out_file.good());
    }

    // Refused before the input is opened: an unreadable project would otherwise
    // report the file failure first.
    auto [missing_code, missing_output] = exec_command(
        CLI + " project bounce no-such-project.json -o " + wav + " --frames 256 --channels 3 -q");
    REQUIRE(missing_code == 2);
    REQUIRE_THAT(missing_output, ContainsSubstring("--channels"));

    // The render length feeds an int64 count, so a value past INT_MAX reaches the input read.
    for (const char* command : {" project bounce", " midi-render"}) {
      INFO(command);
      auto [long_code, long_output] = exec_command(CLI + command + " no-such-project.json -o " +
                                                   wav + " --frames 3000000000 -q");
      REQUIRE(long_code == 4);
      REQUIRE_THAT(long_output, !ContainsSubstring("--frames"));
    }

    std::remove(proj.c_str());
  }

  SECTION("bounce refuses 6/8 that outgrow a stereo master, without naming the option") {
    // 6 and 8 are speaker-layout widths, so validate_project_bounce_channels
    // lets them through; a fresh `project new` project has no scene, which
    // caps its master at stereo, so the request still fails, but only once it
    // reaches the C ABI's own master-width check -- the generic error the
    // render always had, with nothing option-specific in it.
    const std::string proj = unique_temp_path("_proj.json");
    const std::string wav = unique_temp_path("_bounce_narrow_master.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj);
    REQUIRE(nc == 0);

    for (const char* count : {"6", "8"}) {
      INFO(count);
      auto [bc, bo] = exec_command(CLI + " project bounce " + proj + " -o " + wav +
                                   " --frames 256 --channels " + count + " -q");
      REQUIRE(bc == 3);
      REQUIRE_THAT(bo, ContainsSubstring("Invalid parameter"));
      REQUIRE_THAT(bo, !ContainsSubstring("--channels"));
      std::ifstream out_file(wav);
      REQUIRE_FALSE(out_file.good());
    }

    std::remove(proj.c_str());
  }

  SECTION("bounce renders 6/8 channels up to a surround master's own width") {
    const std::string proj51 = unique_temp_path("_proj_51.json");
    const std::string proj71 = unique_temp_path("_proj_71.json");
    const std::string wav = unique_temp_path("_bounce_surround.wav");
    create_surround_master_project(proj51, "5.1");
    create_surround_master_project(proj71, "7.1");

    {
      auto [bc, bo] = exec_command(CLI + " project bounce " + proj51 + " -o " + wav +
                                   " --frames 256 --channels 6 --json");
      REQUIRE(bc == 0);
      REQUIRE_THAT(bo, ContainsSubstring("\"channels\": 6"));
      REQUIRE(wav_header_channel_count(wav) == 6);
    }
    {
      auto [bc, bo] = exec_command(CLI + " project bounce " + proj71 + " -o " + wav +
                                   " --frames 256 --channels 8 --json");
      REQUIRE(bc == 0);
      REQUIRE_THAT(bo, ContainsSubstring("\"channels\": 8"));
      REQUIRE(wav_header_channel_count(wav) == 8);
    }
    // 8 still exceeds a 5.1 master's own 6-channel width: the value is a valid
    // speaker layout, but this project's master is narrower than it.
    {
      auto [bc, bo] = exec_command(CLI + " project bounce " + proj51 + " -o " + wav +
                                   " --frames 256 --channels 8 -q");
      REQUIRE(bc == 3);
      REQUIRE_THAT(bo, !ContainsSubstring("--channels"));
    }

    std::remove(proj51.c_str());
    std::remove(proj71.c_str());
    std::remove(wav.c_str());
  }

  SECTION("an input that outgrows its size probe is refused instead of buffered") {
    // The size cap used to be checked with a seek/tell probe and then ignored
    // by a read-to-EOF, so any input the probe could not size -- one that grows
    // after the check, or a stream with no size at all -- was loaded whole. A
    // FIFO is the deterministic form of that: it reports no size and then
    // delivers more bytes than the cap allows.
    const std::string fifo = unique_temp_path("_project.fifo");
    std::remove(fifo.c_str());
    REQUIRE(mkfifo(fifo.c_str(), S_IRUSR | S_IWUSR) == 0);

    const std::string over_cap = std::to_string(64ull * 1024ull * 1024ull + 1ull);
    auto [code, output] = exec_command("head -c " + over_cap + " /dev/zero > " + fifo + " & " +
                                       CLI + " project compile " + fifo + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("byte limit"));

    std::remove(fifo.c_str());
  }

  SECTION("bounce honors an explicit --sample-rate in the WAV header") {
    const std::string proj = unique_temp_path("_proj.json");
    const std::string wav = unique_temp_path("_bounce.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj + " --sample-rate 44100");
    REQUIRE(nc == 0);

    auto [bc, bo] = exec_command(CLI + " project bounce " + proj + " -o " + wav +
                                 " --frames 256 --sample-rate 44100 --json");
    REQUIRE(bc == 0);
    REQUIRE_THAT(bo, ContainsSubstring("\"sample_rate\": 44100"));
    REQUIRE(wav_header_sample_rate(wav) == 44100u);

    std::remove(proj.c_str());
    std::remove(wav.c_str());
  }

  SECTION("bounce renders at the project's own non-48000 sample rate with no --sample-rate flag") {
    // Regression: project bounce used to unconditionally pass 48000 to the C
    // ABI, so a non-48000 project failed with a spurious invalid-parameter
    // error. The default must come from the project's stored rate.
    for (const int rate : {44100, 96000}) {
      const std::string proj = unique_temp_path("_proj_rate.json");
      const std::string wav = unique_temp_path("_bounce_rate.wav");
      auto [nc, no] =
          exec_command(CLI + " project new -o " + proj + " --sample-rate " + std::to_string(rate));
      REQUIRE(nc == 0);

      auto [bc, bo] =
          exec_command(CLI + " project bounce " + proj + " -o " + wav + " --frames 256 --json");
      REQUIRE(bc == 0);
      REQUIRE_THAT(bo, ContainsSubstring("\"sample_rate\": " + std::to_string(rate)));
      REQUIRE(wav_header_sample_rate(wav) == static_cast<unsigned int>(rate));

      std::remove(proj.c_str());
      std::remove(wav.c_str());
    }
  }

  SECTION("bounce rejects an explicit --sample-rate that disagrees with the project's own rate") {
    const std::string proj = unique_temp_path("_proj_mismatch.json");
    const std::string wav = unique_temp_path("_bounce_mismatch.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj + " --sample-rate 44100");
    REQUIRE(nc == 0);

    auto [bc, bo] = exec_command(CLI + " project bounce " + proj + " -o " + wav +
                                 " --frames 256 --sample-rate 48000 --json");
    REQUIRE(bc == 3);
    REQUIRE_THAT(bo, ContainsSubstring("44100"));
    REQUIRE_THAT(bo, ContainsSubstring("48000"));
    std::ifstream out_file(wav);
    REQUIRE_FALSE(out_file.good());

    std::remove(proj.c_str());
  }

  SECTION("--synth routes MIDI through the built-in instrument bounce") {
    // Without --synth a MIDI bounce is silent; --synth makes it audible by
    // routing through the synth_instruments list of sonare_project_bounce.
    const std::string proj = unique_temp_path("_proj.json");
    const std::string wav = unique_temp_path("_synth.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj);
    REQUIRE(nc == 0);

    auto [bc, bo] = exec_command(CLI + " project bounce " + proj + " -o " + wav +
                                 " --frames 256 --synth saw --json");
    REQUIRE(bc == 0);
    REQUIRE_THAT(bo, ContainsSubstring("\"synth\": true"));

    std::remove(proj.c_str());
    std::remove(wav.c_str());
  }

  SECTION("--synth takes the library preset id in any letter case and refuses kebab-case") {
    const std::string proj = unique_temp_path("_proj.json");
    const std::string wav = unique_temp_path("_synth.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj);
    REQUIRE(nc == 0);

    auto [kc, ko] = exec_command(CLI + " project bounce " + proj + " -o " + wav +
                                 " --frames 256 --synth SAWLEAD --json");
    REQUIRE(kc == 0);
    REQUIRE_THAT(ko, ContainsSubstring("\"synth\": true"));

    auto [cc, co] = exec_command(CLI + " project bounce " + proj + " -o " + wav +
                                 " --frames 256 --synth saw-lead -q");
    REQUIRE(cc == 2);
    REQUIRE_THAT(co, ContainsSubstring("unknown synth preset 'saw-lead'"));

    std::remove(proj.c_str());
    std::remove(wav.c_str());
  }

  SECTION("project help documents CLI SF2 and synth-json limitations") {
    auto [code, output] = exec_command(CLI + " project help");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("--synth"));
    REQUIRE_THAT(output, ContainsSubstring("--sf2"));
    REQUIRE_THAT(output, ContainsSubstring("--synth-json"));
    REQUIRE_THAT(output, ContainsSubstring("SoundFont-backed bounces"));
  }

  SECTION("missing subcommand is a usage error, not an invalid state") {
    // A bare `project` with no subcommand is a one-line usage error with the
    // usage code (2), not the project invalid-state code (9).
    auto [code, output] = exec_command(CLI + " project");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("No project subcommand specified"));
    REQUIRE_THAT(output, ContainsSubstring("Try 'sonare-cli project --help'."));
  }

  SECTION("--help lists every subcommand instead of an empty option banner") {
    // `project` has no registry record of its own -- its contract lives in the
    // ten `project.<subcommand>` leaves -- so the generic per-command help
    // resolved to an empty option list and named none of them. The request goes
    // to the handler, which owns the full usage text, and the ten subcommands
    // and the four input positionals are what makes that observable.
    auto [code, output] = exec_command(CLI + " project --help");
    REQUIRE(code == 0);
    for (const char* subcommand : {"abi", "synth-presets", "new", "validate", "compile", "bounce",
                                   "export-smf", "import-smf", "export-midi2", "import-midi2"}) {
      CAPTURE(subcommand);
      REQUIRE_THAT(output, ContainsSubstring(std::string("  ") + subcommand));
    }
    REQUIRE_THAT(output, ContainsSubstring("in.json"));
    REQUIRE_THAT(output, ContainsSubstring("in.mid"));
    REQUIRE_THAT(output, ContainsSubstring("in.midi2"));
    REQUIRE_THAT(output, ContainsSubstring("--synth"));
  }

  SECTION("a subcommand's --help names the subcommand in its usage line") {
    // The usage line has to name the leaf whose options it goes on to list:
    // printing `project [options]` above the bounce option list gives a reader
    // an invocation that exits 2 and an option list belonging to nothing.
    auto [code, output] = exec_command(CLI + " project bounce --help");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("project bounce [options]"));
    REQUIRE_THAT(output, ContainsSubstring("--frames"));
  }

  SECTION("a missing import input is a file-not-found exit, not an invalid parameter") {
    // The SMF / MIDI2 positional names a user-supplied input file, so failing to open
    // one keeps the class it carries -- the same code `project validate`
    // reports for the same condition, and the same one the Python CLI reports.
    // A script that branches on "fetch the input again" versus "the arguments
    // are wrong" must not get a different answer per subcommand.
    const std::string missing = unique_temp_path("_absent.mid");
    const std::string out = unique_temp_path("_import.json");
    auto [smf_code, smf_output] =
        exec_command(CLI + " project import-smf " + missing + " -o " + out);
    REQUIRE(smf_code == 4);
    REQUIRE_THAT(smf_output, ContainsSubstring("cannot open SMF file"));

    auto [midi2_code, midi2_output] =
        exec_command(CLI + " project import-midi2 " + missing + " -o " + out);
    REQUIRE(midi2_code == 4);
    REQUIRE_THAT(midi2_output, ContainsSubstring("cannot open MIDI2 file"));
  }

  SECTION("oversized import is rejected before allocation") {
    // A project/MIDI file above the byte cap is refused with a clear diagnostic
    // (invalid-parameter exit) instead of an unbounded allocation.
    const std::string big = unique_temp_path("_oversized.json");
    {
      std::ofstream f(big, std::ios::binary);
      f.seekp((64LL * 1024 * 1024) + 1);
      f.put('\0');
    }
    auto [code, output] = exec_command(CLI + " project validate " + big + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("exceeds"));
    std::remove(big.c_str());
  }
}
#endif  // SONARE_WITH_ARRANGEMENT

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("CLI project bounce renders and reports count-in bars", "[cli][count-in]") {
  const std::string project = unique_temp_path("_count_in_project.json");
  const std::string default_wav = unique_temp_path("_count_in_default.wav");
  const std::string zero_wav = unique_temp_path("_count_in_zero.wav");
  const std::string silent_wav = unique_temp_path("_count_in_silent.wav");
  const std::string off_wav = unique_temp_path("_count_in_off.wav");
  const std::string click_wav = unique_temp_path("_count_in_click.wav");
  const std::string on_wav = unique_temp_path("_count_in_on.wav");
  const std::string no_prefix_click_wav = unique_temp_path("_count_in_no_prefix_click.wav");
  const std::string full_prefix_wav = unique_temp_path("_count_in_full_prefix.wav");
  const std::string missing_project = unique_temp_path("_count_in_missing_project.json");
  const std::string refused_wav = unique_temp_path("_count_in_refused.wav");

  auto [new_code, new_output] = exec_command(CLI + " project new -o " + project);
  REQUIRE(new_code == 0);

  const auto bounce = [&](const std::string& wav, const std::string& options) {
    return exec_command(CLI + " project bounce " + project + " -o " + wav + " " + options +
                        " --json");
  };
  const auto require_reported_frames = [](const std::string& output, size_t frames,
                                          size_t count_in_frames) {
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE(payload["frames"].as_number() == static_cast<double>(frames));
    REQUIRE(payload["count_in_frames"].as_number() == static_cast<double>(count_in_frames));
  };
  const auto read_pcm = [](const std::string& path) {
    const auto [samples, sample_rate, channels] = load_audio_interleaved(path);
    REQUIRE(sample_rate == 48000);
    REQUIRE(channels == 2);
    return samples;
  };

  SECTION("omitted and explicit zero count-in write the requested length") {
    auto [default_code, default_output] = bounce(default_wav, "--frames 10000");
    REQUIRE(default_code == 0);
    require_reported_frames(default_output, 10000, 0);
    const std::vector<float> default_samples = read_pcm(default_wav);
    REQUIRE(default_samples.size() == 20000);
    CHECK(std::all_of(default_samples.begin(), default_samples.end(),
                      [](float sample) { return sample == 0.0f; }));

    auto [zero_code, zero_output] = bounce(zero_wav, "--frames 10000 --count-in-bars 0");
    REQUIRE(zero_code == 0);
    require_reported_frames(zero_output, 10000, 0);
    CHECK(read_pcm(zero_wav) == default_samples);
  }

  SECTION("a short explicit render reports only the pre-roll frames it writes") {
    auto [silent_code, silent_output] = bounce(silent_wav, "--frames 10000 --count-in-bars 1");
    REQUIRE(silent_code == 0);
    require_reported_frames(silent_output, 10000, 10000);
    const std::vector<float> silent_samples = read_pcm(silent_wav);
    REQUIRE(silent_samples.size() == 20000);
    CHECK(std::all_of(silent_samples.begin(), silent_samples.end(),
                      [](float sample) { return sample == 0.0f; }));

    auto [off_code, off_output] =
        bounce(off_wav, "--frames 10000 --count-in-bars 1 --metronome=off");
    REQUIRE(off_code == 0);
    require_reported_frames(off_output, 10000, 10000);
    CHECK(read_pcm(off_wav) == silent_samples);

    auto [click_code, click_output] =
        bounce(click_wav, "--frames 10000 --count-in-bars 1 --metronome");
    REQUIRE(click_code == 0);
    require_reported_frames(click_output, 10000, 10000);
    const std::vector<float> click_samples = read_pcm(click_wav);
    REQUIRE(click_samples.size() == 20000);
    CHECK(std::any_of(click_samples.begin(), click_samples.end(),
                      [](float sample) { return sample != 0.0f; }));

    auto [on_code, on_output] = bounce(on_wav, "--frames 10000 --count-in-bars 1 --metronome=on");
    REQUIRE(on_code == 0);
    require_reported_frames(on_output, 10000, 10000);
    CHECK(read_pcm(on_wav) == click_samples);

    auto [no_prefix_code, no_prefix_output] =
        bounce(no_prefix_click_wav, "--frames 10000 --count-in-bars 0 --metronome");
    REQUIRE(no_prefix_code == 0);
    require_reported_frames(no_prefix_output, 10000, 0);
    const std::vector<float> no_prefix_samples = read_pcm(no_prefix_click_wav);
    CHECK(std::any_of(no_prefix_samples.begin(), no_prefix_samples.end(),
                      [](float sample) { return sample != 0.0f; }));
  }

  SECTION("an arrangement-length render writes and reports the full bar") {
    auto [code, output] = bounce(full_prefix_wav, "--count-in-bars 1");
    REQUIRE(code == 0);
    require_reported_frames(output, 96000, 96000);
    const std::vector<float> samples = read_pcm(full_prefix_wav);
    CHECK(samples.size() == 192000);
  }

  SECTION("count-in domains are refused before the project is opened") {
    std::remove(missing_project.c_str());
    for (const char* value : {"-1", "17", "1.5"}) {
      CAPTURE(value);
      auto [code, output] = exec_command(CLI + " project bounce " + missing_project + " -o " +
                                         refused_wav + " --count-in-bars " + value + " --json");
      CHECK(code == 2);
      CHECK_THAT(output, ContainsSubstring("argument --count-in-bars"));
      CHECK_FALSE(std::ifstream(refused_wav).good());
    }
  }

  for (const auto& path : {project, default_wav, zero_wav, silent_wav, off_wav, click_wav, on_wav,
                           no_prefix_click_wav, full_prefix_wav, refused_wav}) {
    std::remove(path.c_str());
  }
}
#endif  // SONARE_WITH_ARRANGEMENT

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
// `tune-to-midi` needs both subsystems -- the SMF reader and the assignment rule
// -- so a build missing either does not register it and these cases are absent
// with it.
//
// Every case below writes its own take and its own reference. Catch2 discovery
// gives one process per case, and the temp paths are per process, so a case
// reading an input a sibling wrote is green in a whole-tag run and red on its
// own.
namespace {

// The take is A4, which is a whole MIDI number, so every shift the reference asks
// for is exact and the expected output pitch is midi_to_hz of the target.
constexpr float kTakeToneHz = sonare::constants::kA4Hz;
constexpr int kTakeSampleRate = 48000;
constexpr float kTakeNoteSec = 0.4f;
constexpr float kTakeGapSec = 0.1f;

// The grid the measured pitches are searched on: wide enough for a whole octave
// down from the take's own tone and for the upward reach the reference asks for.
constexpr float kProbeFminHz = 150.0f;
constexpr float kProbeFmaxHz = 700.0f;

}  // namespace

TEST_CASE("CLI tune-to-midi moves each note onto the pitch the reference names",
          "[cli][tune-to-midi]") {
  const std::string take = unique_temp_path("_take.wav");
  const std::string reference = unique_temp_path("_reference.mid");
  const std::string tuned = unique_temp_path("_tuned.wav");
  // Three notes at 440 Hz, each inside its own half-second reference interval.
  create_note_sequence_wav(take, {kTakeToneHz, kTakeToneHz, kTakeToneHz}, kTakeNoteSec, kTakeGapSec,
                           kTakeSampleRate);
  create_reference_smf(reference, {{0.0, 1.0, 60}, {1.0, 2.0, 67}, {2.0, 3.0, 64}}, 4.0);

  auto [code, output] = exec_command(CLI + " tune-to-midi " + take + " --reference-smf " +
                                     reference + " -o " + tuned + " --json");
  INFO(output);
  REQUIRE(code == 0);

  const auto payload = sonare::util::json::parse_strict(output);
  REQUIRE(payload["output"].as_string() == tuned);
  REQUIRE(payload["assigned_count"].as_int() == 3);
  REQUIRE(payload["note_count"].as_int() == 3);
  REQUIRE(payload["sample_rate"].as_int() == kTakeSampleRate);

  const Audio before = Audio::from_file(take);
  const Audio after = Audio::from_file(tuned);
  // The render keeps the input's length, which is what makes the per-note spans
  // below line up on both sides.
  REQUIRE(payload["length"].as_int() == static_cast<int>(before.size()));
  REQUIRE(after.size() == before.size());

  const std::vector<float> source(before.data(), before.data() + before.size());
  const std::vector<float> result(after.data(), after.data() + after.size());
  const std::array<float, 3> expected{midi_to_hz(60.0f), midi_to_hz(67.0f), midi_to_hz(64.0f)};
  for (size_t note = 0; note < expected.size(); ++note) {
    const float source_hz =
        dominant_frequency(note_span(source, note, kTakeNoteSec, kTakeGapSec, kTakeSampleRate),
                           kTakeSampleRate, kProbeFminHz, kProbeFmaxHz);
    const float tuned_hz =
        dominant_frequency(note_span(result, note, kTakeNoteSec, kTakeGapSec, kTakeSampleRate),
                           kTakeSampleRate, kProbeFminHz, kProbeFmaxHz);
    CAPTURE(note, source_hz, tuned_hz, expected[note]);
    REQUIRE(std::abs(source_hz - kTakeToneHz) < 3.0f);
    // A semitone is 6% here, so 2% keeps the tolerance well inside the interval
    // the reference asked for rather than accepting its neighbour.
    REQUIRE(std::abs(tuned_hz - expected[note]) / expected[note] < 0.02f);
  }
}

TEST_CASE("CLI tune-to-midi answers an unreached note by the policy it was given",
          "[cli][tune-to-midi]") {
  const std::string take = unique_temp_path("_take.wav");
  const std::string reference = unique_temp_path("_reference.mid");
  create_note_sequence_wav(take, {kTakeToneHz, kTakeToneHz}, kTakeNoteSec, kTakeGapSec,
                           kTakeSampleRate);
  // One interval, covering the first note only, so the second note has a
  // measured pitch and no target -- which is exactly what the policy governs.
  create_reference_smf(reference, {{0.0, 1.0, 60}}, 4.0);

  // The count and the second note's span, which is where the three answers part.
  const auto tune = [&](const std::string& policy) {
    const std::string tuned = unique_temp_path("_tuned_" + policy + ".wav");
    auto [code, output] =
        exec_command(CLI + " tune-to-midi " + take + " --reference-smf " + reference + " -o " +
                     tuned + " --unmatched-policy " + policy + " --json");
    INFO(output);
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE(payload["note_count"].as_int() == 2);
    const Audio after = Audio::from_file(tuned);
    const std::vector<float> result(after.data(), after.data() + after.size());
    return std::make_pair(static_cast<int>(payload["assigned_count"].as_int()),
                          note_span(result, 1, kTakeNoteSec, kTakeGapSec, kTakeSampleRate));
  };

  const auto [leave_assigned, leave] = tune("leave");
  const auto [mute_assigned, mute] = tune("mute");
  const auto [nearest_assigned, nearest] = tune("nearest");

  const float leave_hz = dominant_frequency(leave, kTakeSampleRate, kProbeFminHz, kProbeFmaxHz);
  const float nearest_hz = dominant_frequency(nearest, kTakeSampleRate, kProbeFminHz, kProbeFmaxHz);
  const float mute_peak =
      mute.empty() ? 0.0f : *std::max_element(mute.begin(), mute.end(), [](float a, float b) {
        return std::abs(a) < std::abs(b);
      });
  CAPTURE(leave_assigned, mute_assigned, nearest_assigned, leave_hz, nearest_hz, mute_peak);

  // Left alone the note renders as recorded, and only the one overlapping note
  // counts as assigned.
  REQUIRE(leave_assigned == 1);
  REQUIRE(std::abs(leave_hz - kTakeToneHz) < 3.0f);
  // Muted it is silent, which no pitch can be read off at all. Muting is not
  // assigning, so the count is the one `leave` reports.
  REQUIRE(mute_assigned == 1);
  REQUIRE(std::abs(mute_peak) < 0.01f);
  // `nearest` does assign, so its count covers both notes, and the unreached one
  // lands on the only target there is -- the pitch the first note was given.
  REQUIRE(nearest_assigned == 2);
  REQUIRE(std::abs(nearest_hz - midi_to_hz(60.0f)) / midi_to_hz(60.0f) < 0.02f);
}

TEST_CASE("CLI tune-to-midi reports a reference that reaches nothing as zero assignments",
          "[cli][tune-to-midi]") {
  const std::string take = unique_temp_path("_take.wav");
  const std::string reference = unique_temp_path("_reference.mid");
  const std::string tuned = unique_temp_path("_tuned.wav");
  create_note_sequence_wav(take, {kTakeToneHz, kTakeToneHz}, kTakeNoteSec, kTakeGapSec,
                           kTakeSampleRate);
  // Ten seconds past the end of a one-second take: every note has a pitch and no
  // target, so nothing is assigned. That is an answer about the reference, not a
  // failure, and the exit code has to say so.
  create_reference_smf(reference, {{20.0, 21.0, 60}}, 22.0);

  auto [code, output] = exec_command(CLI + " tune-to-midi " + take + " --reference-smf " +
                                     reference + " -o " + tuned + " --json");
  INFO(output);
  REQUIRE(code == 0);

  const auto payload = sonare::util::json::parse_strict(output);
  REQUIRE(payload["assigned_count"].as_int() == 0);
  REQUIRE(payload["note_count"].as_int() > 0);

  // With the default policy nothing moves, so the written take is the one that
  // came in -- the property the whole editing model rests on.
  const Audio before = Audio::from_file(take);
  const Audio after = Audio::from_file(tuned);
  REQUIRE(after.size() == before.size());
  float largest = 0.0f;
  for (size_t i = 0; i < before.size(); ++i) {
    largest = std::max(largest, std::abs(after.data()[i] - before.data()[i]));
  }
  CAPTURE(largest);
  // Exactly zero, not a tolerance: a note set whose edits are all identity is
  // reproduced bit for bit, and the 16-bit round trip on both sides is the same
  // quantization applied to the same samples.
  REQUIRE(largest == 0.0f);
}

TEST_CASE("CLI tune-to-midi refuses each rejected argument in its own exit class",
          "[cli][tune-to-midi]") {
  const std::string take = unique_temp_path("_take.wav");
  const std::string reference = unique_temp_path("_reference.mid");
  const std::string tuned = unique_temp_path("_tuned.wav");
  create_note_sequence_wav(take, {kTakeToneHz}, kTakeNoteSec, kTakeGapSec, kTakeSampleRate);
  create_reference_smf(reference, {{0.0, 1.0, 60}}, 4.0);

  const std::string base =
      CLI + " tune-to-midi " + take + " --reference-smf " + reference + " -o " + tuned;

  SECTION("a missing output is an invalid parameter, not a usage error") {
    // The render would be computed and thrown away, which is the offline-effect
    // contract's one hard error rather than a mis-spelled command line.
    auto [code, output] =
        exec_command(CLI + " tune-to-midi " + take + " --reference-smf " + reference);
    INFO(output);
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("--output"));
  }

  SECTION("a missing reference is a usage error") {
    // Required in the parser on both front-ends, so it keeps the usage class
    // every other parser-required option carries.
    auto [code, output] = exec_command(CLI + " tune-to-midi " + take + " -o " + tuned);
    INFO(output);
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("--reference-smf"));
  }

  SECTION("a negative track index is an invalid parameter") {
    auto [code, output] = exec_command(base + " --track -1");
    INFO(output);
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("--track"));
  }

  SECTION("an overlap ratio outside [0, 1] is an invalid parameter") {
    for (const char* value : {"-0.1", "1.5"}) {
      CAPTURE(value);
      auto [code, output] = exec_command(base + " --min-overlap-ratio " + value);
      INFO(output);
      REQUIRE(code == 2);
      REQUIRE_THAT(output, ContainsSubstring("--min-overlap-ratio"));
    }
    // Both bounds are inclusive, so neither end is refused.
    for (const char* value : {"0", "1"}) {
      CAPTURE(value);
      auto [code, output] = exec_command(base + " --min-overlap-ratio " + value + " --json");
      INFO(output);
      REQUIRE(code == 0);
    }
  }

  SECTION("a negative correction bound is an invalid parameter") {
    auto [code, output] = exec_command(base + " --max-correction-semitones -1");
    INFO(output);
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("--max-correction-semitones"));
    // Zero is legal and means no note moves, so it cannot double as a sentinel.
    auto [zero_code, zero_output] = exec_command(base + " --max-correction-semitones 0 --json");
    INFO(zero_output);
    REQUIRE(zero_code == 0);
  }

  SECTION("an unknown policy name is an invalid parameter") {
    auto [code, output] = exec_command(base + " --unmatched-policy bogus");
    INFO(output);
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("--unmatched-policy"));
  }
}

TEST_CASE("CLI tune-to-midi bounds reach the assignment rule", "[cli][tune-to-midi]") {
  // A domain check only proves the value was parsed. These two run the same
  // command twice, differing in one argument, and require the answers to differ:
  // a bound dropped on the floor produces the same answer both times.
  const std::string take = unique_temp_path("_take.wav");
  const std::string reference = unique_temp_path("_reference.mid");
  create_note_sequence_wav(take, {kTakeToneHz, kTakeToneHz}, kTakeNoteSec, kTakeGapSec,
                           kTakeSampleRate);
  create_reference_smf(reference, {{0.0, 1.0, 60}, {1.0, 2.0, 67}}, 4.0);

  const auto run = [&](const std::string& extra) {
    const std::string tuned = unique_temp_path("_tuned.wav");
    auto [code, output] = exec_command(CLI + " tune-to-midi " + take + " --reference-smf " +
                                       reference + " -o " + tuned + " --json " + extra);
    INFO(output);
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    const Audio after = Audio::from_file(tuned);
    const std::vector<float> result(after.data(), after.data() + after.size());
    return std::make_pair(
        static_cast<int>(payload["assigned_count"].as_int()),
        dominant_frequency(note_span(result, 0, kTakeNoteSec, kTakeGapSec, kTakeSampleRate),
                           kTakeSampleRate, kProbeFminHz, kProbeFmaxHz));
  };

  const auto [default_assigned, default_hz] = run("");
  // The correction saturates at the bound, so 0 leaves every assigned note where
  // it was recorded while the count is unchanged.
  const auto [clamped_assigned, clamped_hz] = run("--max-correction-semitones 0");
  CAPTURE(default_assigned, default_hz, clamped_assigned, clamped_hz);
  REQUIRE(clamped_assigned == default_assigned);
  REQUIRE(std::abs(default_hz - midi_to_hz(60.0f)) / midi_to_hz(60.0f) < 0.02f);
  REQUIRE(std::abs(clamped_hz - kTakeToneHz) < 3.0f);

  // Nothing overlaps a whole note exactly, so demanding the whole of it assigns
  // fewer notes than the default half does.
  const auto [strict_assigned, strict_hz] = run("--min-overlap-ratio 1");
  CAPTURE(strict_assigned, strict_hz);
  REQUIRE(strict_assigned < default_assigned);
}
#endif  // SONARE_WITH_ARRANGEMENT && SONARE_WITH_PITCH_EDITOR

TEST_CASE("CLI transcribe polyphony options reach the transcriber", "[cli][transcribe]") {
  const std::string chord = unique_temp_path("_chord.wav");
  const std::string out = unique_temp_path("_chord.mid");
  constexpr int kRate = 22050;
  const std::array<float, 4> tones{261.63f, 329.63f, 392.0f, 493.88f};
  std::vector<float> samples(static_cast<size_t>(2.0f * kRate));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / kRate;
    float sum = 0.0f;
    for (float hz : tones) {
      sum += std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * hz * t);
    }
    samples[i] = 0.2f * sum;
  }
  save_wav(chord, samples, kRate);

  const std::string base =
      CLI + " transcribe " + chord + " --polyphonic --tempo-bpm 120 --json -o " + out;

  SECTION("a polyphony cap of one yields fewer notes than the default") {
    auto [default_code, default_output] = exec_command(base);
    INFO(default_output);
    REQUIRE(default_code == 0);
    auto [capped_code, capped_output] = exec_command(base + " --max-polyphony 1");
    INFO(capped_output);
    REQUIRE(capped_code == 0);
    const auto default_payload = sonare::util::json::parse_strict(default_output);
    const auto capped_payload = sonare::util::json::parse_strict(capped_output);
    REQUIRE(default_payload["note_count"].as_int() > capped_payload["note_count"].as_int());
  }

  SECTION("each value outside a declared domain is a usage error") {
    for (const char* bad : {"--max-polyphony 0", "--max-polyphony 65",
                            "--min-frame-peak-ratio -0.1", "--min-ridge-peak-ratio -0.1",
                            "--min-frame-peak-ratio 1.5", "--min-ridge-peak-ratio 1.5"}) {
      auto [code, output] = exec_command(base + " " + bad);
      INFO(bad << ": " << output);
      REQUIRE(code == 2);
    }
  }

  SECTION("a reattack ratio between 0 and 1 is a usage error") {
    for (const char* bad : {"--reattack-ratio 0.5", "--reattack-ratio -1"}) {
      auto [code, output] = exec_command(base + " " + bad);
      INFO(bad << ": " << output);
      REQUIRE(code == 2);
    }
  }

  SECTION("a polyphonic-only option on monophonic transcription names the refusal") {
    const std::string mono = CLI + " transcribe " + chord + " --tempo-bpm 120 -o " + out;
    auto [code, output] = exec_command(mono + " --max-polyphony 4");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("max_polyphony requires polyphonic transcription"));
  }

  SECTION("a coarser note division keeps fewer notes") {
    auto [fine_code, fine_output] = exec_command(base + " --min-note-division 32");
    INFO(fine_output);
    REQUIRE(fine_code == 0);
    auto [whole_code, whole_output] = exec_command(base + " --min-note-division 1");
    INFO(whole_output);
    REQUIRE(whole_code == 0);
    const auto fine_payload = sonare::util::json::parse_strict(fine_output);
    const auto whole_payload = sonare::util::json::parse_strict(whole_output);
    REQUIRE(fine_payload["note_count"].as_int() > whole_payload["note_count"].as_int());
  }

  SECTION("a note division outside its domain is a usage error") {
    for (const char* bad : {"--min-note-division 0", "--min-note-division 129"}) {
      auto [code, output] = exec_command(base + " " + bad);
      INFO(bad << ": " << output);
      REQUIRE(code == 2);
    }
    auto [both_code, both_output] = exec_command(base + " --min-note-division 32 --min-note-ms 30");
    INFO(both_output);
    REQUIRE(both_code == 2);
  }

  SECTION("the monophonic model is accepted by name and an unknown name is refused") {
    const std::string mono = CLI + " transcribe " + chord + " --tempo-bpm 120 --json -o " + out;
    for (const char* model : {"segments", "singing"}) {
      auto [code, output] = exec_command(mono + " --monophonic-model " + model);
      INFO(model << ": " << output);
      REQUIRE(code == 0);
    }
    auto [bad_code, bad_output] = exec_command(mono + " --monophonic-model choir");
    INFO(bad_output);
    REQUIRE(bad_code == 2);
    REQUIRE_THAT(bad_output, ContainsSubstring("monophonic-model"));
  }

  SECTION("zero ratios and no-split are accepted") {
    auto [code, output] = exec_command(base +
                                       " --reattack-ratio 0 --min-frame-peak-ratio 0"
                                       " --min-ridge-peak-ratio 0 --max-polyphony 4");
    INFO(output);
    REQUIRE(code == 0);
  }

  std::remove(chord.c_str());
  std::remove(out.c_str());
}

TEST_CASE("CLI transcribe reports detected tempo confidence and meter", "[cli][transcribe]") {
  const std::string clicks = unique_temp_path("_transcribe_clicks.wav");
  const std::string output_midi = unique_temp_path("_transcribe_clicks.mid");
  create_click_track_wav(clicks);

  const auto run = [&](const std::string& tempo_option) {
    auto [code, output] = exec_command(CLI + " transcribe " + clicks + " " + tempo_option +
                                       " --json -o " + output_midi);
    INFO(output);
    REQUIRE(code == 0);
    return sonare::util::json::parse_strict(output);
  };

  const auto detected = run("");
  REQUIRE(detected.contains("tempo_confidence"));
  REQUIRE(detected["tempo_confidence"].is_number());
  REQUIRE(std::isfinite(detected["tempo_confidence"].as_number()));
  REQUIRE(detected.contains("time_signature"));
  const auto& time_signature = detected["time_signature"];
  REQUIRE(time_signature.size() == 3);
  for (const char* key : {"numerator", "denominator", "confidence"}) {
    REQUIRE(time_signature.contains(key));
    REQUIRE(time_signature[key].is_number());
    REQUIRE(std::isfinite(time_signature[key].as_number()));
  }
  CHECK(time_signature["numerator"].as_number() > 0.0);
  CHECK(time_signature["denominator"].as_number() > 0.0);
  CHECK(time_signature["confidence"].as_number() >= 0.0);
  CHECK(time_signature["confidence"].as_number() <= 1.0);

  const auto explicit_tempo = run("--tempo-bpm 120");
  CHECK_FALSE(explicit_tempo.contains("tempo_confidence"));
  CHECK_FALSE(explicit_tempo.contains("time_signature"));

  std::remove(clicks.c_str());
  std::remove(output_midi.c_str());
}

TEST_CASE("CLI pitch-correct-timevarying holds the pYIN frame as a duration",
          "[cli][pitch-correct-timevarying]") {
  for (const int rate : {44100, 48000, 192000}) {
    create_test_wav(TEST_WAV, 0.5f, 220.0f, rate);
    std::remove(TEST_OUT.c_str());
    const auto [code, output] =
        exec_command(CLI + " pitch-correct-timevarying " + TEST_WAV + " -o " + TEST_OUT + " -q");
    INFO("sample rate " << rate << ": " << output);
    REQUIRE(code == 0);
    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
  }
}

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("CLI project commands refuse synth, audio bindings and outputs from argv",
          "[cli][argument-contract][project]") {
  // The project does not exist: a refusal reached only after the read would exit 4.
  const std::string missing = "/nonexistent/argv-refusal-project.json";
  const std::string out = unique_temp_path("_project_refusal.wav");
  struct Row {
    std::string argv;
    std::string text;
  };
  const std::vector<Row> rows = {
      {"project bounce " + missing + " -o " + out + " --synth nosuchsynth",
       "argument --synth: unknown synth preset 'nosuchsynth'"},
      {"midi-render " + missing + " -o " + out + " --synth nosuchsynth",
       "argument --synth: unknown synth preset 'nosuchsynth'"},
      {"project bounce " + missing + " -o " + out + " --audio nofile",
       "argument --audio: must be <source_id>=FILE"},
      {"project bounce " + missing + " -o " + out + " --audio 0=a.wav",
       "argument --audio: <source_id> must be an integer in [1, 4294967295]"},
      {"project align-takes " + missing + " -o " + out + " --reference-source 1 --audio nofile",
       "argument --audio: must be <source_id>=FILE"},
      {"project new -o " + out + " --sample-rate 100",
       "argument --sample-rate: must be an integer in [8000, 384000]"},
      {"synthesize-rir -o " + out + " --sample-rate 100",
       "argument --sample-rate: must be an integer in [8000, 384000]"},
  };
  for (const Row& row : rows) {
    const auto [code, output] = exec_command(CLI + " " + row.argv + " -q");
    INFO(row.argv << ": " << output);
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring(row.text));
  }
}
#endif

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("CLI project align-takes reads source kinds through the loaded project",
          "[cli][project]") {
  const std::string takes = unique_temp_path("_kinds_takes.json");
  const std::string project = unique_temp_path("_kinds_project.json");
  const std::string wav = unique_temp_path("_kinds_take.wav");
  const std::string out = unique_temp_path("_kinds_out.json");
  create_takes_project(takes);
  create_test_wav(wav, 2.0f, 22050);
  std::string document;
  {
    std::ifstream in(takes);
    document.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const std::string sources = R"("sources":[)";
  const size_t at = document.find(sources);
  REQUIRE(at != std::string::npos);
  document.insert(at + sources.size(), R"({"kind":"midi","id":3,"name":"","channelHint":0},)");
  {
    std::ofstream file(project);
    file << document;
  }
  const std::string audio = " --audio 1=" + wav + " --audio 2=" + wav + " -o " + out + " -q";

  SECTION("an audio source in its token spelling is a usable reference") {
    auto [code, output] =
        exec_command(CLI + " project align-takes " + project + " --reference-source 1" + audio);
    INFO(output);
    CHECK(code == 0);
  }
  SECTION("a MIDI source is refused as a reference") {
    auto [code, output] =
        exec_command(CLI + " project align-takes " + project + " --reference-source 3" + audio);
    CHECK(code == 3);
    CHECK_THAT(output, ContainsSubstring("that source is not an audio source"));
  }
  for (const std::string& path : {takes, project, wav, out}) std::remove(path.c_str());
}
#endif
