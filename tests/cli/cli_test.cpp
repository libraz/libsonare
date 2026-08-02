/// @file cli_test.cpp
/// @brief Tests for the sonare CLI tool.

#include <sonare/sonare_c_project.h>
#include <unistd.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/audio_io.h"
#include "sonare.h"
#include "util/constants.h"
#include "util/json.h"

using namespace sonare;
using Catch::Matchers::ContainsSubstring;

namespace {

/// @brief Creates a test WAV file with a sine wave.
/// @param path Output path
/// @param duration Duration in seconds
/// @param frequency Frequency in Hz
/// @param sample_rate Sample rate
void create_test_wav(const std::string& path, float duration = 3.0f, float frequency = 440.0f,
                     int sample_rate = 22050) {
  size_t n_samples = static_cast<size_t>(duration * sample_rate);
  std::vector<float> samples(n_samples);

  for (size_t i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / sample_rate;
    samples[i] =
        0.5f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * frequency * t);
  }

  save_wav(path, samples, sample_rate);
}

void create_test_stereo_wav(const std::string& path, int sample_rate = 22050) {
  std::vector<float> samples = {0.25f, -0.25f, 0.5f, -0.5f};
  save_wav_multichannel(path, samples.data(), 2, 2, ChannelLayout::Stereo, sample_rate);
}

/// @brief Reads the PCM WAV channel-count field from a RIFF header.
unsigned int wav_header_channel_count(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::array<unsigned char, 24> header{};
  if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) return 0;
  return static_cast<unsigned int>(header[22]) | (static_cast<unsigned int>(header[23]) << 8U);
}

/// @brief Custom deleter for FILE* using pclose.
struct PipeDeleter {
  void operator()(FILE* fp) const {
    if (fp) pclose(fp);
  }
};

/// @brief Executes a shell command and returns output.
/// @param cmd Command to execute
/// @return Pair of (exit_code, output)
std::pair<int, std::string> exec_command(const std::string& cmd) {
  std::array<char, 4096> buffer;
  std::string result;

  // Redirect stderr to stdout
  std::string full_cmd = cmd + " 2>&1";
  std::unique_ptr<FILE, PipeDeleter> pipe(popen(full_cmd.c_str(), "r"));
  if (!pipe) {
    return {-1, "popen failed"};
  }

  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }

  int status = pclose(pipe.release());
  int exit_code = WEXITSTATUS(status);
  return {exit_code, result};
}

/// @brief Gets the path to the sonare CLI executable.
std::string get_cli_path() {
  // Try common build paths
  std::vector<std::string> paths = {"./build/bin/sonare-cli",
                                    "./build-mastering-api/bin/sonare-cli", "./bin/sonare-cli",
                                    "../bin/sonare-cli"};

  for (const auto& path : paths) {
    std::ifstream f(path);
    if (f.good()) {
      return path;
    }
  }

  // Default to assuming it's in build/bin
  return "./build/bin/sonare-cli";
}

/// @brief Generates a unique temp file path for this test process.
std::string unique_temp_path(const std::string& suffix) {
  static int counter = 0;
  return "/tmp/sonare_cli_test_" + std::to_string(getpid()) + "_" + std::to_string(counter++) +
         suffix;
}

const std::string CLI = get_cli_path();
const std::string TEST_WAV = unique_temp_path(".wav");
const std::string TEST_OUT = unique_temp_path("_out.wav");

}  // namespace

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
  commands.insert(commands.end(), {"mix", "mix-strip", "mixing-presets", "mixing-preset"});
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
    auto [code, output] = exec_command(CLI + " project abi --synth --json");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("abi_version"));
  }
#endif
}

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
  REQUIRE(code != 0);
  REQUIRE_THAT(output, ContainsSubstring("does not produce a file output"));
  std::ifstream f(out);
  REQUIRE_FALSE(f.good());

  // The long-form spelling is rejected identically.
  auto [long_code, long_output] =
      exec_command(CLI + " lufs " + TEST_WAV + " --output " + out + " -q");
  REQUIRE(long_code != 0);
  REQUIRE_THAT(long_output, ContainsSubstring("does not produce a file output"));
}

TEST_CASE("CLI effect commands require an output destination", "[cli]") {
  // Offline-effect output contract: commands that render audio require -o and
  // report the same invalid-parameter exit code when it is missing.
  create_test_wav(TEST_WAV);

  SECTION("normalize") {
    auto [code, output] = exec_command(CLI + " normalize " + TEST_WAV + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("requires output file"));
  }

  SECTION("resample") {
    auto [code, output] = exec_command(CLI + " resample --target-rate 16000 " + TEST_WAV + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("requires output file"));
  }
}

#ifdef SONARE_WITH_ACOUSTIC_SIM
TEST_CASE("CLI estimate-room accepts both band-count spellings", "[cli][argument-contract]") {
  // Native historically spelled the flag --n-bands; the Python CLI uses
  // --n-octave-bands. Both must be recognized so scripts against either surface
  // keep working. Argument validation runs before audio loading, so a missing
  // input file (usage error) still proves the option itself was accepted.
  auto [native_code, native_output] = exec_command(CLI + " estimate-room --n-bands 6 -q");
  REQUIRE_THAT(native_output, !ContainsSubstring("Unknown option"));

  auto [alias_code, alias_output] = exec_command(CLI + " estimate-room --n-octave-bands 6 -q");
  REQUIRE_THAT(alias_output, !ContainsSubstring("Unknown option"));
}
#endif

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
    REQUIRE_THAT(output, ContainsSubstring("\"time_signature\""));
    REQUIRE_THAT(output, ContainsSubstring("\"groove_type\""));
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
  }
}

TEST_CASE("CLI lufs --json stays valid JSON on a silent input", "[cli]") {
  // A fully silent input drives LUFS/true-peak to -inf; the JSON builder must
  // emit `null` (not "-inf"/"nan", which no JSON parser accepts) for those fields.
  create_test_wav(TEST_WAV, 1.0f, 0.0f);  // frequency 0 => all-zero samples
  auto [code, output] = exec_command(CLI + " lufs " + TEST_WAV + " --json -q");
  REQUIRE(code == 0);
  REQUIRE_THAT(output, ContainsSubstring("\"integrated_lufs\""));
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
  REQUIRE(gain_code == 3);
  REQUIRE_THAT(gain_message, ContainsSubstring("must be finite"));

  auto [normalize_code, normalize_message] =
      exec_command(CLI + " normalize " + TEST_WAV + " -o " + normalize_output +
                   " --mode rms --target-db inf -q");
  REQUIRE(normalize_code == 3);
  REQUIRE_THAT(normalize_message, ContainsSubstring("must be finite"));

  REQUIRE_FALSE(std::ifstream(gain_output).good());
  REQUIRE_FALSE(std::ifstream(normalize_output).good());
}

TEST_CASE("CLI mix preserves stereo input so --width changes the output", "[cli]") {
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
  auto [compatibility_code, compatibility_message] =
      exec_command(CLI + " mix " + input + " -o " + narrow + " --width 0 -q");
  REQUIRE(compatibility_code == 0);
  auto [wide_code, wide_message] =
      exec_command(CLI + " mix " + input + " -o " + wide + " --width 2 -q");
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
      exec_command(CLI + " mix " + mono + " -o " + rejected + " --width 0.5 -q");
  REQUIRE(mono_code == 3);
  REQUIRE_THAT(mono_message, ContainsSubstring("requires a stereo input"));
  REQUIRE_FALSE(std::ifstream(rejected).good());

  std::remove(input.c_str());
  std::remove(narrow.c_str());
  std::remove(wide.c_str());
  std::remove(mono.c_str());
}

TEST_CASE("CLI filter applies zero phase to fourth-order filters only when requested", "[cli]") {
  const std::string input = unique_temp_path("_impulse.wav");
  const std::string causal_output = unique_temp_path("_causal.wav");
  const std::string zero_phase_output = unique_temp_path("_zero_phase.wav");
  std::vector<float> impulse(4096, 0.0f);
  impulse[2048] = 1.0f;
  save_wav(input, impulse, 22050);

  auto [causal_code, causal_message] = exec_command(
      CLI + " filter " + input + " -o " + causal_output + " --type lp --cutoff 1000 --order 4 -q");
  REQUIRE(causal_code == 0);
  auto [zero_phase_code, zero_phase_message] =
      exec_command(CLI + " filter " + input + " -o " + zero_phase_output +
                   " --type lp --cutoff 1000 --order 4 --zero-phase -q");
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

TEST_CASE("CLI presence flags do not swallow the audio file argument", "[cli]") {
  // Documented order is `<command> [options] <audio_file>`; a presence-only flag
  // like --ir must not consume the following path as its value and leave the
  // input empty (which produced a misleading "Missing audio file").
  create_test_wav(TEST_WAV);
  auto [code, output] = exec_command(CLI + " acoustic --ir " + TEST_WAV + " -q");
  REQUIRE_THAT(output, !ContainsSubstring("Missing audio file"));
  REQUIRE(code == 0);
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
    REQUIRE_THAT(output, ContainsSubstring("\"algorithm\""));
    REQUIRE_THAT(output, ContainsSubstring("\"n_frames\""));
  }

  SECTION("with yin algorithm") {
    auto [code, output] = exec_command(CLI + " pitch " + TEST_WAV + " --algorithm yin -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("yin"));
  }

  SECTION("rejects unknown algorithm") {
    auto [code, output] = exec_command(CLI + " pitch " + TEST_WAV + " --algorithm typo -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--algorithm must be 'yin' or 'pyin'"));
  }
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
  }
}

TEST_CASE("CLI pitch-shift command", "[cli]") {
  create_test_wav(TEST_WAV);
  std::remove(TEST_OUT.c_str());

  SECTION("shift up") {
    auto [code, output] =
        exec_command(CLI + " pitch-shift --semitones 3 " + TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 0);

    // Verify output file exists
    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
  }

  SECTION("shift down") {
    auto [code, output] =
        exec_command(CLI + " pitch-shift --semitones -3 " + TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 0);

    std::ifstream f(TEST_OUT);
    REQUIRE(f.good());
  }

  SECTION("missing output file") {
    auto [code, output] = exec_command(CLI + " pitch-shift --semitones 3 " + TEST_WAV + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("requires output file"));
  }

  SECTION("missing semitones") {
    auto [code, output] =
        exec_command(CLI + " pitch-shift " + TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--semitones required"));
  }
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
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("requires output file"));
  }

  SECTION("missing rate") {
    auto [code, output] =
        exec_command(CLI + " time-stretch " + TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--rate required"));
  }
}

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

  SECTION("voice-change preset warns about ignored simple knobs") {
    auto [code, output] = exec_command(
        CLI + " voice-change --preset neutral-monitor --pitch-semitones 5 --formant-factor 1.1 " +
        TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("warning: --pitch-semitones is ignored"));
    REQUIRE_THAT(output, ContainsSubstring("warning: --formant-factor is ignored"));
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

  SECTION("voice-preset-validate resolves a preset-pack entry and applies overrides") {
    const std::string pack = "schemas/realtime-voice-changer-presets.example.json";
    auto [code, output] =
        exec_command(CLI + " voice-preset-validate " + pack +
                     " --preset neutral-monitor --set dsp.outputGainDb=-2 --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"outputGainDb\":-2"));
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
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("requires output"));
  }
}

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
    REQUIRE_THAT(output, ContainsSubstring("\"processor\": \"dynamics.compressor\""));
    REQUIRE_THAT(output, ContainsSubstring("\"latency_samples\""));
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

    auto [params_code, params_output] = exec_command(
        CLI + " eq " + TEST_WAV +
        " --params band0.enabled=1 --auto-threshold --sidechain-freq-hz 1000 --sidechain-q 0.7 -q");
    REQUIRE(params_code == 0);
    REQUIRE_THAT(params_output, ContainsSubstring("--auto-threshold is ignored when --params"));
    REQUIRE_THAT(params_output, ContainsSubstring("--sidechain-freq-hz is ignored when --params"));
    REQUIRE_THAT(params_output, ContainsSubstring("--sidechain-q is ignored when --params"));
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
    REQUIRE_THAT(analysis_output, ContainsSubstring("\"sourceLufs\""));
    REQUIRE_THAT(analysis_output, ContainsSubstring("\"referenceLufs\""));
  }

  SECTION("pair analysis accepts independent source and reference lengths") {
    const std::string short_ref = unique_temp_path("_short_reference.wav");
    create_test_wav(short_ref, 1.0f, 660.0f);
    auto [analysis_code, analysis_output] =
        exec_command(CLI + " mastering-pair-analyze " + TEST_WAV + " --reference " + short_ref +
                     " --analysis match.referenceLoudness -q");
    REQUIRE(analysis_code == 0);
    REQUIRE_THAT(analysis_output, ContainsSubstring("\"sourceLufs\""));
    REQUIRE_THAT(analysis_output, ContainsSubstring("\"referenceLufs\""));
  }

  SECTION("runs stereo analysis") {
    auto [code, output] =
        exec_command(CLI + " mastering-stereo-analyze " + TEST_WAV + " --reference " + ref +
                     " --analysis stereo.monoCompatCheck -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"correlation\""));
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
    auto [code, output] = exec_command(CLI + " mix " + TEST_WAV + " -o " + out +
                                       " --input-trim-db 1 --fader-db -3 --pan 0.25 --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"meter\""));
    REQUIRE_THAT(output, ContainsSubstring("\"correlation\""));

    std::ifstream f(out);
    REQUIRE(f.good());
  }
}
#endif

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
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--candidates"));

    auto [global_code, global_output] =
        exec_command(CLI + " mel " + TEST_WAV + " --n-mels=64junk -q");
    REQUIRE(global_code == 3);
    REQUIRE_THAT(global_output, ContainsSubstring("--n-mels"));
  }

  SECTION("rejects global DSP options outside their command schema") {
    for (const char* option :
         {"--n-fft 4096", "--hop-length 256", "--n-mels 64", "--fmin 20", "--fmax 20000"}) {
      auto [code, output] = exec_command(CLI + " version " + option + " -q");
      REQUIRE(code == 3);
      REQUIRE_THAT(output, ContainsSubstring("Unknown option"));
    }
  }

  SECTION("global n-fft and hop-length reach frame conversion handlers") {
    auto [code, output] = exec_command(
        CLI + " frames-to-samples --frames 10 --n-fft 2048 --hop-length 512 --json -q");
    REQUIRE(code == 0);
    REQUIRE_THAT(output, ContainsSubstring("\"samples\": 6144"));
  }

  SECTION("project bounce rejects an unknown NativeSynth preset") {
    auto [code, output] = exec_command(
        CLI + " project bounce --in missing.json -o ignored.wav --synth not-a-preset -q");
    REQUIRE(code == 9);
    REQUIRE_THAT(output, ContainsSubstring("unknown synth preset"));
  }
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
      REQUIRE(code == 3);
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
    {
      std::ofstream file(project);
      file << R"({"version":1,"clips":[{"id":1,"track_id":99,"source_id":99,)"
              R"("length_ppq":1.0}]})";
    }

    auto [normal_code, normal_output] =
        exec_command(CLI + " project validate --in " + project + " --json");
    REQUIRE(normal_code == 0);
    REQUIRE_THAT(normal_output, ContainsSubstring("\"valid\": false"));
    REQUIRE_THAT(normal_output, ContainsSubstring("dangling_clip_source"));
    REQUIRE_THAT(normal_output, ContainsSubstring("dangling_clip_track"));

    auto [strict_code, strict_output] =
        exec_command(CLI + " project validate --in " + project + " --strict --json");
    REQUIRE(strict_code == 9);
    REQUIRE_THAT(strict_output, ContainsSubstring("\"valid\": false"));
    REQUIRE_THAT(strict_output, ContainsSubstring("dangling_clip_source"));
    REQUIRE_THAT(strict_output, ContainsSubstring("dangling_clip_track"));

    std::remove(project.c_str());
  }

  SECTION("bounce WAV header sample rate equals the render rate (default 48000)") {
    // Regression: the bounce used to tag the WAV with 44100 while the engine
    // rendered at the project rate (~2x pitch error). The reported sample_rate
    // must equal the rate the render actually used. With no --sample-rate the
    // CLI pins a definite 48000 render rate and tags the header to match.
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

  SECTION("bounce honors an explicit --sample-rate in the WAV header") {
    const std::string proj = unique_temp_path("_proj.json");
    const std::string wav = unique_temp_path("_bounce.wav");
    auto [nc, no] = exec_command(CLI + " project new -o " + proj + " --sample-rate 44100");
    REQUIRE(nc == 0);

    auto [bc, bo] = exec_command(CLI + " project bounce --in " + proj + " -o " + wav +
                                 " --frames 256 --sample-rate 44100 --json");
    REQUIRE(bc == 0);
    REQUIRE_THAT(bo, ContainsSubstring("\"sample_rate\": 44100"));

    std::remove(proj.c_str());
    std::remove(wav.c_str());
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
