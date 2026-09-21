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
        exec_command(CLI + " note-stretch --onset 100 --offset 2000 --ratio 1.2 " + TEST_WAV +
                     " -o " + TEST_OUT + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"ratio\""));
    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
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

  SECTION("voice-change reports the formant factor the warp applied") {
    // The warp resolves its factor into [0.55, 1.65] and 100 lands on the
    // ceiling, so echoing the request made two invocations that produce
    // byte-identical audio read as different settings.
    auto [code, output] = exec_command(CLI + " voice-change --formant-factor 100 " + TEST_WAV +
                                       " -o " + TEST_OUT + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"formant_factor\": 1.64"));
    REQUIRE_THAT(output, !ContainsSubstring("\"formant_factor\": 100"));
  }

  SECTION("voice-change preset rejects simple knob conflicts") {
    auto [code, output] = exec_command(
        CLI + " voice-change --preset neutral-monitor --pitch-semitones 5 --formant-factor 1.1 " +
        TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("cannot be combined with a realtime preset"));
  }

  SECTION("voice-change resolves a preset-pack entry and applies overrides") {
    const std::string pack = "schemas/realtime-voice-changer-presets.example.json";
    auto [code, output] = exec_command(CLI + " voice-change --preset-pack " + pack +
                                       " --preset neutral-monitor --set dsp.outputGainDb=-2 " +
                                       TEST_WAV + " -o " + TEST_OUT + " --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"preset\": \"neutral-monitor\""));
    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
  }

  SECTION("voice-change reports a pack without an entry ahead of the --set rule") {
    const std::string pack = "schemas/realtime-voice-changer-presets.example.json";
    auto [code, output] =
        exec_command(CLI + " voice-change --preset-pack " + pack + " --set dsp.outputGainDb=-2 " +
                     TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--preset-pack requires --preset"));
  }

  SECTION("voice-preset-validate resolves a preset-pack entry and applies overrides") {
    const std::string pack = "schemas/realtime-voice-changer-presets.example.json";
    auto [code, output] =
        exec_command(CLI + " voice-preset-validate " + pack +
                     " --preset neutral-monitor --set dsp.outputGainDb=-2 --json -q");
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

  SECTION("abi prints the project ABI version") {
    auto [code, output] = exec_command(CLI + " project abi");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring(std::to_string(SONARE_PROJECT_ABI_VERSION)));
  }

  SECTION("synth-presets lists the full NativeSynth catalog") {
    auto [code, output] = exec_command(CLI + " project synth-presets --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"presets\""));
    REQUIRE_THAT(output, ContainsSubstring("\"e-piano\""));
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

    auto [vc, vo] = exec_command(CLI + " project validate --in " + proj);
    REQUIRE(vc == 0);
    REQUIRE_THAT(vo, ContainsSubstring("valid"));

    auto [cc, co] = exec_command(CLI + " project compile --in " + proj);
    REQUIRE(cc == 0);

    std::remove(proj.c_str());
  }

  SECTION("compile JSON keeps one human message per diagnostic and always emits messages") {
    const std::string invalid = unique_temp_path("_compile_diagnostic.json");
    {
      std::ofstream file(invalid);
      file << R"json({
        "version": 1,
        "sample_rate": 48000,
        "sources": [{"kind": 0, "id": 1, "uri": "missing.wav", "channel_count": 1,
                     "sample_rate_hint": 48000, "storage_handle_id": 0}],
        "tracks": [{"id": 1, "name": "audio", "kind": 0, "gain": 1, "mute": false,
                    "solo": false, "pan": 0, "channel_strip_ref": "", "output_target": "",
                    "midi_destination_id": 0, "automation_lanes": []}],
        "clips": [{"id": 1, "track_id": 1, "source_id": 1, "start_ppq": 0,
                   "length_ppq": 1, "source_offset_ppq": 0, "gain": 1, "loop_mode": 0,
                   "loop_length_ppq": 0, "warp_ref_id": 0, "warp_mode": 0}]
      })json";
    }

    auto [diagnostic_code, diagnostic_output] =
        exec_command(CLI + " project compile --in " + invalid + " --json -q");
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
      REQUIRE(diagnostic.contains("message"));
      REQUIRE(diagnostic["message"].is_string());
      REQUIRE_FALSE(diagnostic["message"].as_string().empty());
      REQUIRE(diagnostic_payload["messages"].as_string().find(diagnostic["message"].as_string()) !=
              std::string::npos);
    }

    const std::string clean = unique_temp_path("_compile_clean.json");
    auto [new_code, new_output] = exec_command(CLI + " project new -o " + clean);
    REQUIRE(new_code == 0);
    auto [clean_code, clean_output] =
        exec_command(CLI + " project compile --in " + clean + " --json -q");
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

    auto [ec, eo] = exec_command(CLI + " project export-midi2 --in " + proj + " -o " + midi2);
    REQUIRE(ec == 0);
    REQUIRE_THAT(eo, ContainsSubstring("Exported MIDI2 Clip File"));

    auto [ic, io] =
        exec_command(CLI + " project import-midi2 --midi2 " + midi2 + " -o " + imported);
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
    auto [code, output] = exec_command(CLI + " project validate --in " + bad);
    REQUIRE(code != 0);
    std::remove(bad.c_str());
  }

  SECTION("validate reports loader diagnostics and --strict rejects them") {
    const std::string project = unique_temp_path("_warning.json");
    const std::string canonical = unique_temp_path("_warning_canonical.json");
    {
      std::ofstream file(project);
      file << R"({"version":1,"clips":[{"id":1,"track_id":99,"source_id":99,)"
              R"("length_ppq":1.0}]})";
    }

    auto [normal_code, normal_output] =
        exec_command(CLI + " project validate --in " + project + " --json");
    REQUIRE(normal_code == 0);
    REQUIRE_THAT(normal_output, ContainsSubstring("\"valid\": true"));
    REQUIRE_THAT(normal_output, ContainsSubstring("dangling_clip_source"));
    REQUIRE_THAT(normal_output, ContainsSubstring("dangling_clip_track"));

    auto [strict_code, strict_output] = exec_command(CLI + " project validate --in " + project +
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
        exec_command(CLI + " project validate --in " + project + " --frames 123 --json");
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
        exec_command(CLI + " project bounce --in " + proj + " -o " + wav + " --frames 256 --json");
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

    auto [bc, bo] = exec_command(CLI + " project bounce --in " + proj + " -o " + wav +
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

    auto [bc, bo] = exec_command(CLI + " project bounce --in " + proj + " -o " + wav +
                                 " --frames 256 --channels 1 --json");
    REQUIRE(bc == 0);
    REQUIRE_THAT(bo, ContainsSubstring("\"channels\": 1"));
    REQUIRE(wav_header_channel_count(wav) == 1);

    std::remove(proj.c_str());
    std::remove(wav.c_str());
  }

  SECTION("bounce refuses a width it does not render, naming the option") {
    // The bounce renders a stereo master and writes that pair or its mono
    // downmix. Any other width used to reach the C ABI and come back as a bare
    // "bounce project: Invalid parameter" after the project had been loaded,
    // with nothing pointing at the option that caused it.
    const std::string proj = unique_temp_path("_proj.json");
    const std::string wav = unique_temp_path("_bounce_bad_channels.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj);
    REQUIRE(nc == 0);

    for (const char* count : {"3", "6"}) {
      INFO(count);
      auto [bc, bo] = exec_command(CLI + " project bounce --in " + proj + " -o " + wav +
                                   " --frames 256 --channels " + count + " -q");
      REQUIRE(bc == 3);
      REQUIRE_THAT(bo, ContainsSubstring("--channels"));
      REQUIRE_THAT(bo, ContainsSubstring(count));
      std::ifstream out_file(wav);
      REQUIRE_FALSE(out_file.good());
    }

    // Refused before the input is opened: an unreadable project would otherwise
    // report the file failure first.
    auto [missing_code, missing_output] =
        exec_command(CLI + " project bounce --in no-such-project.json -o " + wav +
                     " --frames 256 --channels 3 -q");
    REQUIRE(missing_code == 3);
    REQUIRE_THAT(missing_output, ContainsSubstring("--channels"));

    std::remove(proj.c_str());
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
                                       CLI + " project compile --in " + fifo + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("byte limit"));

    std::remove(fifo.c_str());
  }

  SECTION("bounce honors an explicit --sample-rate in the WAV header") {
    const std::string proj = unique_temp_path("_proj.json");
    const std::string wav = unique_temp_path("_bounce.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj + " --sample-rate 44100");
    REQUIRE(nc == 0);

    auto [bc, bo] = exec_command(CLI + " project bounce --in " + proj + " -o " + wav +
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

      auto [bc, bo] = exec_command(CLI + " project bounce --in " + proj + " -o " + wav +
                                   " --frames 256 --json");
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

    auto [bc, bo] = exec_command(CLI + " project bounce --in " + proj + " -o " + wav +
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
    // routing through sonare_project_bounce_with_synth_instruments.
    const std::string proj = unique_temp_path("_proj.json");
    const std::string wav = unique_temp_path("_synth.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj);
    REQUIRE(nc == 0);

    auto [bc, bo] = exec_command(CLI + " project bounce --in " + proj + " -o " + wav +
                                 " --frames 256 --synth saw --json");
    REQUIRE(bc == 0);
    REQUIRE_THAT(bo, ContainsSubstring("\"synth\": true"));

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
    // A bare `project` with no subcommand prints usage and exits with the usage
    // code (2), not the project invalid-state code (9).
    auto [code, output] = exec_command(CLI + " project");
    REQUIRE(code == 2);
    REQUIRE_THAT(output, ContainsSubstring("PROJECT SUBCOMMANDS"));
  }

  SECTION("--help lists every subcommand instead of an empty option banner") {
    // `project` has no registry record of its own -- its contract lives in the
    // ten `project.<subcommand>` leaves -- so the generic per-command help
    // resolved to an empty option list and named none of them. The request goes
    // to the handler, which owns the full usage text, and the ten subcommands
    // and the four input options are what makes that observable.
    auto [code, output] = exec_command(CLI + " project --help");
    REQUIRE(code == 0);
    for (const char* subcommand : {"abi", "synth-presets", "new", "validate", "compile", "bounce",
                                   "export-smf", "import-smf", "export-midi2", "import-midi2"}) {
      CAPTURE(subcommand);
      REQUIRE_THAT(output, ContainsSubstring(std::string("  ") + subcommand));
    }
    REQUIRE_THAT(output, ContainsSubstring("--in"));
    REQUIRE_THAT(output, ContainsSubstring("--smf"));
    REQUIRE_THAT(output, ContainsSubstring("--midi2"));
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
    // `--smf` / `--midi2` name a user-supplied input file, so failing to open
    // one keeps the class it carries -- the same code `project validate --in`
    // reports for the same condition, and the same one the Python CLI reports.
    // A script that branches on "fetch the input again" versus "the arguments
    // are wrong" must not get a different answer per subcommand.
    const std::string missing = unique_temp_path("_absent.mid");
    const std::string out = unique_temp_path("_import.json");
    auto [smf_code, smf_output] =
        exec_command(CLI + " project import-smf --smf " + missing + " -o " + out);
    REQUIRE(smf_code == 4);
    REQUIRE_THAT(smf_output, ContainsSubstring("cannot open SMF file"));

    auto [midi2_code, midi2_output] =
        exec_command(CLI + " project import-midi2 --midi2 " + missing + " -o " + out);
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
    auto [code, output] = exec_command(CLI + " project validate --in " + big + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("exceeds"));
    std::remove(big.c_str());
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
    REQUIRE(code == 3);
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
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--track"));
  }

  SECTION("an overlap ratio outside [0, 1] is an invalid parameter") {
    for (const char* value : {"-0.1", "1.5"}) {
      CAPTURE(value);
      auto [code, output] = exec_command(base + " --min-overlap-ratio " + value);
      INFO(output);
      REQUIRE(code == 3);
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
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--max-correction-semitones"));
    // Zero is legal and means no note moves, so it cannot double as a sentinel.
    auto [zero_code, zero_output] = exec_command(base + " --max-correction-semitones 0 --json");
    INFO(zero_output);
    REQUIRE(zero_code == 0);
  }

  SECTION("an unknown policy name is an invalid parameter") {
    auto [code, output] = exec_command(base + " --unmatched-policy bogus");
    INFO(output);
    REQUIRE(code == 3);
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
