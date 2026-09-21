/// @file cli_test_helpers.h
/// @brief Shared fixtures and the shell harness the native CLI tests run
///        the built `sonare` binary through.

#pragma once

#include <sonare/sonare_c_project.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cli/sonare_cli_args.h"
#include "cli/sonare_cli_json.h"
#include "cli/sonare_cli_registry.h"
#include "core/audio.h"
#include "core/audio_io.h"
#include "effects/silence.h"
#include "sonare.h"
#include "util/constants.h"
#include "util/json.h"
#include "util/types.h"

using namespace sonare;
using Catch::Matchers::ContainsSubstring;

namespace sonare::test::cli {

/// @brief Creates a test WAV file with a sine wave.
/// @param path Output path
/// @param duration Duration in seconds
/// @param frequency Frequency in Hz
/// @param sample_rate Sample rate
inline void create_test_wav(const std::string& path, float duration = 3.0f,
                            float frequency = 440.0f, int sample_rate = 22050) {
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
inline void create_two_segment_wav(const std::string& path, int sample_rate = 22050) {
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
inline void create_stepped_level_wav(const std::string& path, int sample_rate = 22050) {
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

/// @brief Creates a WAV from consecutive (amplitude, seconds) blocks.
/// @param path Output path
/// @param blocks Amplitude and duration of each block, in order
/// @param sample_rate Sample rate
///
/// split-silence reports where a signal sounds, so its fixture needs silence
/// somewhere other than the ends; the union across takes only differs from the
/// intersection when two takes fall quiet at different times, which needs one
/// schedule per take.
inline void create_blocked_level_wav(const std::string& path,
                                     const std::vector<std::pair<float, float>>& blocks,
                                     int sample_rate = 22050) {
  std::vector<float> samples;
  const float two_pi = 2.0f * static_cast<float>(sonare::constants::kPiD);
  for (const auto& block : blocks) {
    const size_t count = static_cast<size_t>(block.second * static_cast<float>(sample_rate));
    for (size_t i = 0; i < count; ++i) {
      const float t = static_cast<float>(samples.size()) / static_cast<float>(sample_rate);
      samples.push_back(block.first * std::sin(two_pi * 220.0f * t));
    }
  }
  save_wav(path, samples, sample_rate);
}

/// @brief Reads the intervals a `split-silence --json` run printed.
inline std::vector<std::pair<int, int>> parse_split_silence_json(const std::string& text) {
  // Named, because as_array() hands back a reference into the document.
  const sonare::util::json::Value document = sonare::util::json::parse_strict(text);
  std::vector<std::pair<int, int>> ranges;
  for (const auto& entry : document.as_array()) {
    ranges.emplace_back(entry["start_sample"].as_int(), entry["end_sample"].as_int());
  }
  return ranges;
}

/// @brief Whether any reported interval covers @p sample.
inline bool split_silence_covers(const std::vector<std::pair<int, int>>& ranges, int sample) {
  return std::any_of(ranges.begin(), ranges.end(), [sample](const std::pair<int, int>& range) {
    return range.first <= sample && sample < range.second;
  });
}

inline void create_test_stereo_wav(const std::string& path, int sample_rate = 22050) {
  std::vector<float> samples = {0.25f, -0.25f, 0.5f, -0.5f};
  save_wav_multichannel(path, samples.data(), 2, 2, ChannelLayout::Stereo, sample_rate);
}

// Both header readers exist for the project-bounce cases, which are gated on
// the arrangement subsystem, so they carry the same guard. Ungated they would
// have no callers under -DBUILD_ARRANGEMENT=OFF, which the build rejects as
// unused functions.
#if defined(SONARE_WITH_ARRANGEMENT)
/// @brief Reads the PCM WAV channel-count field from a RIFF header.
inline unsigned int wav_header_channel_count(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::array<unsigned char, 24> header{};
  if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) return 0;
  return static_cast<unsigned int>(header[22]) | (static_cast<unsigned int>(header[23]) << 8U);
}

/// @brief Reads the PCM WAV sample-rate field from a RIFF header.
inline unsigned int wav_header_sample_rate(const std::string& path) {
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
inline unsigned int wav_header_bits_per_sample(const std::string& path) {
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
inline std::string to_lowercase(const std::string& value) {
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
inline std::pair<int, std::string> exec_command(const std::string& cmd) {
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
inline std::string get_cli_path() {
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
inline std::string unique_temp_path(const std::string& suffix) {
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
inline void create_impulse_response_wav(const std::string& path, float rt60 = 0.6f,
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

inline void create_clipped_wav(const std::string& path, int sample_rate = 22050) {
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

/// @brief Creates a WAV clipped by driving a tone past full scale.
/// @details Distinct from @ref create_clipped_wav, whose flat run interrupts a
///   quiet sine: a declipper reconstructs from the neighbourhood it is given,
///   and a flat top surrounded by a quarter-scale signal is reconstructed back
///   inside full scale. Here the clipped run IS the top of a loud sine, so the
///   reconstruction extrapolates above the ceiling -- which is the only case
///   where the writer's clamp can discard the repair.
inline void create_overdriven_wav(const std::string& path, int sample_rate = 22050) {
  const size_t n_samples = static_cast<size_t>(sample_rate);
  std::vector<float> samples(n_samples);
  const float two_pi = 2.0f * static_cast<float>(sonare::constants::kPiD);
  for (size_t i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / sample_rate;
    // 1.06x drive clips roughly a fifth of the samples: enough runs for the
    // detector, short enough runs for the LPC fit to have something to read.
    const float driven = 1.06f * std::sin(two_pi * 180.0f * t) +
                         0.2f * std::sin(two_pi * 430.0f * t) * std::sin(two_pi * 3.0f * t);
    samples[i] = std::clamp(driven, -1.0f, 1.0f);
  }
  save_wav(path, samples, sample_rate);
}

/// @brief Creates a WAV whose noise floor sits close under its programme.
/// @details The assistant selects repair from measurement, so a clean tone
///   selects nothing and cannot show whether a flag reached the suggester. The
///   noise is loud enough that the floor lands well inside the rule rather than
///   at its edge, so the fixture does not become a threshold test by accident.
inline void create_noisy_wav(const std::string& path, int sample_rate = 22050) {
  const size_t n_samples = static_cast<size_t>(sample_rate * 3);
  std::vector<float> samples(n_samples);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
  const float two_pi = 2.0f * static_cast<float>(sonare::constants::kPiD);
  for (size_t i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / sample_rate;
    samples[i] = 0.5f * std::sin(two_pi * 440.0f * t) + noise(rng);
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
inline float tone_magnitude(const std::vector<float>& samples, int sample_rate, float frequency) {
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

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
/// One note of a reference melody, in quarter notes. The project's default tempo
/// is 120 BPM, so one PPQ is half a second of reference time.
struct ReferenceNote {
  double on_ppq;
  double off_ppq;
  uint8_t midi;
};

/// @brief Writes @p melody at @p path as a Standard MIDI File.
/// @details Through the project exporter rather than hand-built bytes: the
///   reader under test is this writer's counterpart, so a byte layout spelled
///   here would be a second SMF writer to keep in step with it.
inline void create_reference_smf(const std::string& path, const std::vector<ReferenceNote>& melody,
                                 double clip_length_ppq) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  uint32_t track_id = 0;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, clip_length_ppq, &track_id, &clip_id) ==
          SONARE_OK);

  std::vector<SonareMidiEventPod> events;
  for (const ReferenceNote& note : melody) {
    SonareMidiEventPod on{};
    SonareMidiEventPod off{};
    REQUIRE(sonare_midi_note_on(note.on_ppq, 0, 0, note.midi, 100, &on) == SONARE_OK);
    REQUIRE(sonare_midi_note_off(note.off_ppq, 0, 0, note.midi, 0, &off) == SONARE_OK);
    events.push_back(on);
    events.push_back(off);
  }
  REQUIRE(sonare_project_set_midi_events(project, clip_id, events.data(), events.size()) ==
          SONARE_OK);

  uint8_t* bytes = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_export_smf(project, &bytes, &len) == SONARE_OK);
  {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(len));
  }
  sonare_free_bytes(bytes);
  sonare_project_destroy(project);
}

/// @brief Creates a take of equal-length tones separated by silence.
/// @param path Output path
/// @param frequencies One tone per note, in order
/// @param note_sec Sounding length of each note
/// @param gap_sec Silence after each note
/// @param sample_rate Sample rate
///
/// The silence is what makes the take segment: the note extractor breaks on the
/// unvoiced frames, so each tone becomes one note with a span the reference can
/// be written against. A single sustained tone yields one note covering the whole
/// take, which no per-note assignment can be read off.
inline void create_note_sequence_wav(const std::string& path, const std::vector<float>& frequencies,
                                     float note_sec, float gap_sec, int sample_rate) {
  const float two_pi = 2.0f * static_cast<float>(sonare::constants::kPiD);
  const size_t note_samples = static_cast<size_t>(note_sec * static_cast<float>(sample_rate));
  const size_t gap_samples = static_cast<size_t>(gap_sec * static_cast<float>(sample_rate));
  std::vector<float> samples;
  for (float frequency : frequencies) {
    for (size_t i = 0; i < note_samples; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
      samples.push_back(0.5f * std::sin(two_pi * frequency * t));
    }
    samples.insert(samples.end(), gap_samples, 0.0f);
  }
  save_wav(path, samples, sample_rate);
}

/// @brief The strongest frequency in @p samples on a 1 Hz grid over [@p fmin,
///        @p fmax].
/// @details Reported rather than compared against one expectation, so a case
///   states the pitch it measured instead of only whether a probe frequency was
///   present. The grid is coarse on purpose: the intervals under test are
///   semitones, which are tens of Hz apart here.
inline float dominant_frequency(const std::vector<float>& samples, int sample_rate, float fmin,
                                float fmax) {
  float best_frequency = 0.0f;
  float best_magnitude = 0.0f;
  for (float frequency = fmin; frequency <= fmax; frequency += 1.0f) {
    const float magnitude = tone_magnitude(samples, sample_rate, frequency);
    if (magnitude > best_magnitude) {
      best_magnitude = magnitude;
      best_frequency = frequency;
    }
  }
  return best_frequency;
}

/// @brief The samples of one note's sounding span, as the take lays them out.
inline std::vector<float> note_span(const std::vector<float>& samples, size_t note, float note_sec,
                                    float gap_sec, int sample_rate) {
  const size_t note_samples = static_cast<size_t>(note_sec * static_cast<float>(sample_rate));
  const size_t gap_samples = static_cast<size_t>(gap_sec * static_cast<float>(sample_rate));
  const size_t begin = note * (note_samples + gap_samples);
  if (begin + note_samples > samples.size()) return {};
  return std::vector<float>(samples.begin() + static_cast<std::ptrdiff_t>(begin),
                            samples.begin() + static_cast<std::ptrdiff_t>(begin + note_samples));
}
#endif  // SONARE_WITH_ARRANGEMENT && SONARE_WITH_PITCH_EDITOR

inline const std::string CLI = get_cli_path();
inline const std::string TEST_WAV = unique_temp_path(".wav");
inline const std::string TEST_OUT = unique_temp_path("_out.wav");

/// Restores the global C++ locale, so that a locale a test installs cannot
/// change how a later case formats numbers.
struct GlobalLocaleGuard {
  std::locale previous = std::locale();

  ~GlobalLocaleGuard() { std::locale::global(previous); }
};

}  // namespace sonare::test::cli

using namespace sonare::test::cli;
