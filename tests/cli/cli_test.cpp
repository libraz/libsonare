/// @file cli_test.cpp
/// @brief Tests for the sonare CLI tool.

#include <sonare/sonare_c_project.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <locale>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cli_support.h"
#include "core/audio.h"
#include "core/audio_io.h"
#include "sonare.h"
#include "util/constants.h"
#include "util/json.h"
#include "util/types.h"

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

/// @brief Creates a WAV whose second half gains an upper partial.
/// @param path Output path
/// @param sample_rate Sample rate
///
/// Structural analysis only reacts to the FFT size when the material has a
/// timbral change to resolve; a single steady tone yields the same boundaries
/// at every --n-fft, so it cannot show that the option reaches the analysis.
void create_two_segment_wav(const std::string& path, int sample_rate = 22050) {
  const size_t segment = static_cast<size_t>(1.5f * sample_rate);
  std::vector<float> samples(2 * segment);
  const float two_pi = 2.0f * static_cast<float>(sonare::constants::kPiD);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / sample_rate;
    samples[i] = i < segment
                     ? 0.5f * std::sin(two_pi * 220.0f * t)
                     : 0.4f * std::sin(two_pi * 880.0f * t) + 0.2f * std::sin(two_pi * 2640.0f * t);
  }
  save_wav(path, samples, sample_rate);
}

/// @brief Creates a WAV whose level steps between loud and quiet blocks.
/// @param path Output path
/// @param sample_rate Sample rate
///
/// Only the windowed RMS series reacts to the dynamics hop length; peak, RMS
/// and crest are whole-signal, and the loudness range runs EBU R128 on its own
/// fixed windows. A stepped envelope is what gives that series a percentile
/// spread to move.
void create_stepped_level_wav(const std::string& path, int sample_rate = 22050) {
  const std::array<float, 6> levels{0.9f, 0.08f, 0.6f, 0.15f, 0.8f, 0.05f};
  const size_t block = static_cast<size_t>(0.5f * sample_rate);
  std::vector<float> samples(levels.size() * block);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / sample_rate;
    samples[i] = levels[i / block] *
                 std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 220.0f * t);
  }
  save_wav(path, samples, sample_rate);
}

void create_test_stereo_wav(const std::string& path, int sample_rate = 22050) {
  std::vector<float> samples = {0.25f, -0.25f, 0.5f, -0.5f};
  save_wav_multichannel(path, samples.data(), 2, 2, ChannelLayout::Stereo, sample_rate);
}

// Both header readers exist for the project-bounce cases, which are gated on
// the arrangement subsystem, so they carry the same guard. Ungated they would
// have no callers under -DBUILD_ARRANGEMENT=OFF, which the build rejects as
// unused functions.
#if defined(SONARE_WITH_ARRANGEMENT)
/// @brief Reads the PCM WAV channel-count field from a RIFF header.
unsigned int wav_header_channel_count(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::array<unsigned char, 24> header{};
  if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) return 0;
  return static_cast<unsigned int>(header[22]) | (static_cast<unsigned int>(header[23]) << 8U);
}

/// @brief Reads the PCM WAV sample-rate field from a RIFF header.
unsigned int wav_header_sample_rate(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::array<unsigned char, 28> header{};
  if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) return 0;
  return static_cast<unsigned int>(header[24]) | (static_cast<unsigned int>(header[25]) << 8U) |
         (static_cast<unsigned int>(header[26]) << 16U) |
         (static_cast<unsigned int>(header[27]) << 24U);
}
#endif  // SONARE_WITH_ARRANGEMENT

#if defined(SONARE_WITH_ACOUSTIC_SIM)
/// @brief Reads the PCM WAV bits-per-sample field from a RIFF header.
///
/// load_wav() returns floats, so the only way to observe the width a command
/// chose to write is to read the header field itself.
unsigned int wav_header_bits_per_sample(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::array<unsigned char, 36> header{};
  if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) return 0;
  return static_cast<unsigned int>(header[34]) | (static_cast<unsigned int>(header[35]) << 8U);
}
#endif  // SONARE_WITH_ACOUSTIC_SIM

/// @brief Custom deleter for FILE* using pclose.
struct PipeDeleter {
  void operator()(FILE* fp) const {
    if (fp) pclose(fp);
  }
};

/// @brief ASCII-lowercases a string, for comparing a serialized enum spelling
///        against its own canonical form.
std::string to_lowercase(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

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
#ifdef SONARE_TEST_CLI
  return SONARE_TEST_CLI;
#else
  if (const char* configured = std::getenv("SONARE_TEST_CLI");
      configured != nullptr && *configured != '\0') {
    return configured;
  }

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
#endif
}

/// @brief Generates a unique temp file path for this test process.
std::string unique_temp_path(const std::string& suffix) {
  static int counter = 0;
  return "/tmp/sonare_cli_test_" + std::to_string(getpid()) + "_" + std::to_string(counter++) +
         suffix;
}

/// @brief Creates a WAV with a sustained full-scale region.
/// @param path Output path
/// @param sample_rate Sample rate
///
/// The shared analysis tone never reaches full scale, so it cannot exercise a
/// clipping option at all: `--min-region` only selects among detected regions,
/// and a signal with no clipped samples has none to select from.
/// @brief Creates a WAV holding a synthetic room impulse response.
/// @param path Output path
/// @param rt60 Reverberation time in seconds
/// @param sample_rate Sample rate
///
/// The acoustic command routes on --ir alone, so proving the flag is not a
/// no-op needs input the analyzer's own impulse-response heuristic accepts: a
/// full-scale onset followed by exponentially decaying noise.
void create_impulse_response_wav(const std::string& path, float rt60 = 0.6f,
                                 int sample_rate = 48000) {
  const size_t n_samples = static_cast<size_t>(1.5f * static_cast<float>(sample_rate));
  std::vector<float> samples(n_samples);
  const float decay = std::log(1000.0f) / rt60;
  uint32_t state = 0x1234567u;
  for (size_t i = 0; i < n_samples; ++i) {
    state = state * 1664525u + 1013904223u;
    const float noise = static_cast<float>((state >> 8) & 0xffffu) / 32768.0f - 1.0f;
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] = noise * std::exp(-decay * t);
  }
  samples[0] = 1.0f;
  save_wav(path, samples, sample_rate);
}

void create_clipped_wav(const std::string& path, int sample_rate = 22050) {
  const size_t n_samples = static_cast<size_t>(sample_rate / 2);
  const size_t clip_begin = n_samples / 4;
  const size_t clip_end = clip_begin + static_cast<size_t>(sample_rate / 20);
  std::vector<float> samples(n_samples);
  const float two_pi = 2.0f * static_cast<float>(sonare::constants::kPiD);
  for (size_t i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / sample_rate;
    samples[i] = (i >= clip_begin && i < clip_end) ? 1.0f : 0.25f * std::sin(two_pi * 220.0f * t);
  }
  save_wav(path, samples, sample_rate);
}

/// @brief Measures the amplitude of one frequency in a buffer.
/// @param samples Signal to probe
/// @param sample_rate Sample rate of @p samples
/// @param frequency Probe frequency in Hz
///
/// A single-bin correlation rather than an FFT, so the probe frequency does not
/// have to fall on a bin center: a transposition test compares magnitudes at
/// frequencies chosen by the interval under test, not by the transform size.
float tone_magnitude(const std::vector<float>& samples, int sample_rate, float frequency) {
  if (samples.empty()) return 0.0f;
  const double omega = 2.0 * sonare::constants::kPiD * frequency / sample_rate;
  double real = 0.0;
  double imag = 0.0;
  for (size_t i = 0; i < samples.size(); ++i) {
    const double phase = omega * static_cast<double>(i);
    real += samples[i] * std::cos(phase);
    imag += samples[i] * std::sin(phase);
  }
  return static_cast<float>(2.0 * std::sqrt(real * real + imag * imag) /
                            static_cast<double>(samples.size()));
}

const std::string CLI = get_cli_path();
const std::string TEST_WAV = unique_temp_path(".wav");
const std::string TEST_OUT = unique_temp_path("_out.wav");

/// Restores the global C++ locale, so that a locale a test installs cannot
/// change how a later case formats numbers.
struct GlobalLocaleGuard {
  std::locale previous = std::locale();

  ~GlobalLocaleGuard() { std::locale::global(previous); }
};

}  // namespace

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
  REQUIRE(project_leaf_count == 10);
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
TEST_CASE("CLI resolves the deprecated mix spelling through the alias path",
          "[cli][registry][argument-contract]") {
  // One registry row, reached under both names. Two rows that each named the
  // other as an alias never used the alias path -- path lookup wins -- so each
  // name was validated against its own copy of the option list and an option
  // added to one became an unknown option under the other.
  const CliCommandSpec* canonical = cli_command_spec_for_path("mix-strip");
  const CliCommandSpec* alias = cli_command_spec_for_path("mix");
  REQUIRE(canonical != nullptr);
  REQUIRE(alias == canonical);
  REQUIRE(std::find(canonical->aliases.begin(), canonical->aliases.end(), "mix") !=
          canonical->aliases.end());

  size_t rows = 0;
  for (const auto& command : cli_command_registry()) {
    if (command.path == "mix" || command.path == "mix-strip") ++rows;
  }
  REQUIRE(rows == 1);

  // Both spellings therefore accept the same options and reject the same ones.
  for (const std::string name : {"mix", "mix-strip"}) {
    CAPTURE(name);
    auto [code, output] = exec_command(CLI + " " + name + " --definitely-unknown-option");
    REQUIRE(code != 0);
    REQUIRE_THAT(output, ContainsSubstring("Unknown option"));

    auto [width_code, width_output] = exec_command(CLI + " " + name + " --width 1.5");
    REQUIRE_THAT(width_output, !ContainsSubstring("Unknown option"));
    (void)width_code;
  }
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

TEST_CASE("CLI effect commands require an output destination", "[cli]") {
  // Offline-effect output contract: commands that render audio require -o and
  // report the same invalid-parameter exit code when it is missing.
  create_test_wav(TEST_WAV);

  SECTION("normalize") {
    auto [code, output] = exec_command(CLI + " normalize " + TEST_WAV + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--output'"));
  }

  SECTION("resample") {
    auto [code, output] = exec_command(CLI + " resample --target-rate 16000 " + TEST_WAV + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--output'"));
  }
}

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

TEST_CASE("CLI acoustic commands preserve the default seed for non-positive values",
          "[cli][acoustic]") {
  const std::string synth_options =
      " --length 7 --width 5 --height 3 --absorption 0.2 --ism-order 0"
      " --max-seconds 0.3 --sample-rate 16000 --json -q";
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
  const auto synth_negative = run_synthesize("negative", " --seed -1");
  const auto synth_one = run_synthesize("one", " --seed 1");
  const auto synth_other = run_synthesize("other", " --seed 7");
  REQUIRE(synth_default == synth_zero);
  REQUIRE(synth_default == synth_negative);
  REQUIRE(synth_default == synth_one);
  REQUIRE(synth_default != synth_other);

  const std::string input = unique_temp_path("_seed_morph_input.wav");
  std::vector<float> impulse(256, 0.0f);
  impulse[0] = 1.0f;
  save_wav(input, impulse, 16000);
  const std::string morph_options =
      " --length 7 --width 5 --height 3 --absorption 0.2 --ism-order 0"
      " --max-seconds 0.3 --wet 1 --suppression 0 --json -q";
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
  const auto morph_negative = run_morph("negative", " --seed -1");
  const auto morph_one = run_morph("one", " --seed 1");
  const auto morph_other = run_morph("other", " --seed 7");
  REQUIRE(morph_default == morph_zero);
  REQUIRE(morph_default == morph_negative);
  REQUIRE(morph_default == morph_one);
  REQUIRE(morph_default != morph_other);
  std::remove(input.c_str());
}

TEST_CASE("CLI synthesize-rir reports its warning diagnostics and keeps the tail's headroom",
          "[cli][acoustic]") {
  // A max_seconds below the direct sound's own arrival raises three warnings at
  // once, none of which sets has_error: the length was clamped, the cap was
  // raised to fit the direct sound, and the request came back as early
  // reflections with no diffuse tail. Discarding them made a truncated RIR
  // indistinguishable from a complete one under a green exit.
  const std::string truncated = unique_temp_path("_rir_truncated.wav");
  auto [code, output] = exec_command(CLI + " synthesize-rir --max-seconds 0.005 -o " + truncated +
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
  auto [full_code, full_output] = exec_command(CLI + " synthesize-rir --max-seconds 2 -o " +
                                               complete + " --sample-rate 22050 --json");
  REQUIRE(full_code == 0);
  REQUIRE_THAT(full_output, !ContainsSubstring("warning:"));
  REQUIRE(wav_header_bits_per_sample(complete) == 24);
  std::remove(complete.c_str());
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

TEST_CASE("CLI acoustic routes into IR analysis only through --ir", "[cli][acoustic]") {
  // The handler left AcousticConfig on its Auto default, so an impulse-like
  // file reached IR analysis with no --ir: the documented mode selector was a
  // no-op, and the command disagreed with sonare_detect_acoustic (which forces
  // blind) and therefore with the Python CLI and every binding.
  const std::string ir_path = unique_temp_path("_ir.wav");
  create_impulse_response_wav(ir_path);

  auto [blind_code, blind_output] = exec_command(CLI + " acoustic " + ir_path + " --json");
  REQUIRE(blind_code == 0);
  REQUIRE_THAT(blind_output, ContainsSubstring("\"is_blind\": true"));
  // Blind estimation cannot measure clarity, and reports that as null rather
  // than as a value; the Auto route filled these in instead.
  REQUIRE_THAT(blind_output, ContainsSubstring("\"c50\": null"));
  REQUIRE_THAT(blind_output, ContainsSubstring("\"c50_bands\": []"));

  auto [ir_code, ir_output] = exec_command(CLI + " acoustic --ir " + ir_path + " --json");
  REQUIRE(ir_code == 0);
  REQUIRE_THAT(ir_output, ContainsSubstring("\"is_blind\": false"));
  REQUIRE_THAT(ir_output, !ContainsSubstring("\"c50\": null"));

  std::remove(ir_path.c_str());
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

  SECTION("zero-frame frequency range emits a nullable voiced ratio") {
    const std::string short_wav = unique_temp_path("_pitch_zero_frames.wav");
    create_test_wav(short_wav, 0.02f, 440.0f);
    auto [code, output] =
        exec_command(CLI + " pitch " + short_wav + " --fmin 5 --fmax 10 --json -q");
    REQUIRE(code == 0);
    const auto payload = sonare::util::json::parse_strict(output);
    REQUIRE(payload["n_frames"].as_int() == 0);
    REQUIRE(payload["voiced_count"].as_int() == 0);
    REQUIRE(payload["voiced_ratio"].is_null());
    std::remove(short_wav.c_str());
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
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--output'"));
  }

  SECTION("missing semitones") {
    auto [code, output] =
        exec_command(CLI + " pitch-shift " + TEST_WAV + " -o " + TEST_OUT + " -q");
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("--semitones required"));
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
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--output'"));
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

TEST_CASE("CLI voice-change compensates realtime chain latency", "[cli][voice-change]") {
  // Regression: the native voice-change realtime branch used to write
  // process_block() output directly into an audio.size()-length buffer with no
  // pre-roll/drop compensation, so the tail of the true signal was never
  // flushed and a file shorter than the chain latency came out entirely
  // silent. bright-idol has a nonzero retune+ISP-limiter latency (~1700
  // samples at 48 kHz), so a 20 ms / 960-sample clip is well inside the
  // previously-lost window.
  const std::string short_wav = unique_temp_path("_vc_short.wav");
  const std::string short_out = unique_temp_path("_vc_short_out.wav");
  create_test_wav(short_wav, 0.02f, 220.0f, 48000);

  auto [code, output] = exec_command(CLI + " voice-change --preset bright-idol " + short_wav +
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

TEST_CASE("CLI voice-change rejects a realtime preset document with an unknown field",
          "[cli][voice-change][argument-contract]") {
  // Regression: the native voice-change realtime branch parsed the
  // resolved config through the tolerant realtime_voice_changer_config_from_json
  // instead of the strict validator, so a mistyped section name silently
  // rendered with unrelated defaults and exited 0. voice-preset-validate
  // already used the strict validator, so the two entry points disagreed on
  // the same malformed document.
  create_test_wav(TEST_WAV, 0.05f, 220.0f, 48000);
  std::remove(TEST_OUT.c_str());

  SECTION("typo'd dsp section name") {
    const std::string preset_path = unique_temp_path("_vc_typo_preset.json");
    {
      std::ofstream preset(preset_path);
      REQUIRE(preset.good());
      preset << R"json({"schemaVersion":1,"id":"typo-test","name":"Typo Test","category":"custom",
        "dsp":{"inputGainDb":0,"outputGainDb":0,"wetMix":1,
          "retune":{"semitones":0,"mix":0,"grainSize":0},
          "formnt":{"factor":1,"amount":0,"body":0,"brightness":0,"nasal":0},
          "eq":{"highpassHz":75,"bodyDb":0,"presenceDb":0,"airDb":0},
          "gate":{"thresholdDb":-55,"attackMs":2,"releaseMs":100,"rangeDb":18},
          "compressor":{"thresholdDb":-22,"ratio":2.5,"attackMs":6,"releaseMs":90,"makeupGainDb":1},
          "deesser":{"frequencyHz":7200,"thresholdDb":-28,"ratio":4,"rangeDb":8},
          "reverb":{"mix":0.04,"timeMs":320,"damping":0.55,"seed":0},
          "limiter":{"ceilingDb":-1,"releaseMs":50}}})json";
    }

    auto [code, output] = exec_command(CLI + " voice-change --preset-json " + preset_path + " " +
                                       TEST_WAV + " -o " + TEST_OUT + " -q");
    std::remove(preset_path.c_str());
    REQUIRE(code != 0);
    REQUIRE_THAT(output, ContainsSubstring("dsp.formnt"));
    std::ifstream f(TEST_OUT);
    REQUIRE_FALSE(f.good());
  }

  SECTION("typo'd --set macro key") {
    const std::string preset_path = unique_temp_path("_vc_typo_macro_preset.json");
    {
      std::ofstream preset(preset_path);
      REQUIRE(preset.good());
      preset << R"json({"schemaVersion":1,"id":"macro-typo-test","name":"Macro Typo Test",
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
    REQUIRE(code == 3);
    REQUIRE_THAT(output, ContainsSubstring("Missing required option '--output'"));
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
    // prefer_streaming_safe defaults to true, so the reachable control is the
    // one that turns it off, and the repair explanation is where the suggester
    // reports which of the two it applied.
    auto [safe_code, safe_output] = exec_command(
        CLI + " mastering " + TEST_WAV + " --assistant --enable-repair --explain --json -q");
    REQUIRE(safe_code == 0);
    REQUIRE_THAT(safe_output, ContainsSubstring("streaming-safe repair enabled"));

    auto [open_code, open_output] =
        exec_command(CLI + " mastering " + TEST_WAV +
                     " --assistant --enable-repair --no-streaming-safe --explain --json -q");
    REQUIRE(open_code == 0);
    REQUIRE_THAT(open_output, !ContainsSubstring("streaming-safe repair enabled"));
    REQUIRE_THAT(open_output, ContainsSubstring("repair stages enabled"));
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
    // A project that loads but does not compile is a handler failure with no
    // error code of its own, so it lands on the same invalid-parameter code
    // every other command uses for that state.
    REQUIRE(diagnostic_code == 3);
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
