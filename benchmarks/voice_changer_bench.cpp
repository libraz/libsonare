/// @file voice_changer_bench.cpp
/// @brief CPU benchmark of RealtimeVoiceChanger::process_block on the full-featured voice
///        transformer configuration, against a real-time budget (mean block time under 25% of the
///        block duration).
///
/// Mono, 48 kHz, 128-sample blocks. The default run enables the psola engine, three harmony
/// voices, a 20-band internal-carrier vocoder, four held notes, megaphone character and dub
/// echo. `--engine grain` times the default configuration with the grain engine, which refuses
/// harmony, the vocoder and notes. Prints the configuration, block count, block duration and
/// mean, p95 and p99 block times, then exits 0.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <vector>

#include "editing/voice_changer/realtime.h"
#include "util/constants.h"

namespace {

using sonare::constants::kCentsPerOctave;
using sonare::constants::kDefaultDawSampleRate;
using sonare::constants::kTwoPiD;
using sonare::editing::voice_changer::CharacterType;
using sonare::editing::voice_changer::PitchEngine;
using sonare::editing::voice_changer::RealtimeVoiceChanger;
using sonare::editing::voice_changer::RealtimeVoiceChangerConfig;
using sonare::editing::voice_changer::ReverbVariation;
using sonare::editing::voice_changer::VocoderCarrierSource;

constexpr int kBlockSize = 128;
constexpr int kDefaultBlocks = 20000;
constexpr int kWarmupBlocks = 2000;
/// Length of the synthetic input loop, in blocks (2 s at 48 kHz).
constexpr int kLoopBlocks = 750;
constexpr int kHarmonics = 12;
constexpr double kVoiceF0Hz = 220.0;
constexpr double kVibratoHz = 5.5;
constexpr double kVibratoCents = 10.0;
constexpr float kInputAmplitude = 0.2f;
constexpr float kNoteVelocity = 0.8f;
constexpr int kHarmonyVoices = 3;
constexpr int kVocoderBands = 20;
constexpr double kMicrosPerSecond = 1.0e6;
constexpr double kPercent = 100.0;
constexpr double kP95 = 0.95;
constexpr double kP99 = 0.99;

volatile float g_sink = 0.0f;

/// Harmonic tone with vibrato, rendered once and replayed as a loop.
std::vector<float> make_voice_input(int num_samples) {
  std::vector<float> x(static_cast<size_t>(num_samples));
  const double sr = kDefaultDawSampleRate;
  double phase = 0.0;
  for (int n = 0; n < num_samples; ++n) {
    const double t = static_cast<double>(n) / sr;
    const double cents = kVibratoCents * std::sin(kTwoPiD * kVibratoHz * t);
    const double f0 = kVoiceF0Hz * std::exp2(cents / kCentsPerOctave);
    phase += kTwoPiD * f0 / sr;
    double s = 0.0;
    for (int k = 1; k <= kHarmonics; ++k) s += std::sin(k * phase) / k;
    x[static_cast<size_t>(n)] = kInputAmplitude * static_cast<float>(s);
  }
  return x;
}

/// The fully loaded psola configuration, or the default configuration on the grain engine.
RealtimeVoiceChangerConfig make_config(bool grain) {
  RealtimeVoiceChangerConfig config;
  if (grain) {
    config.voice_engine.engine = PitchEngine::Grain;
    return config;
  }
  config.voice_engine.engine = PitchEngine::Psola;
  config.harmony.voices = kHarmonyVoices;
  config.vocoder.enabled = true;
  config.vocoder.bands = kVocoderBands;
  config.vocoder.carrier_source = VocoderCarrierSource::Internal;
  config.character.type = CharacterType::Megaphone;
  config.reverb_variation.variation = ReverbVariation::DubEcho;
  return config;
}

const char* engine_name(PitchEngine engine) {
  return engine == PitchEngine::Grain ? "grain" : "psola";
}

const char* character_name(CharacterType type) {
  return type == CharacterType::Megaphone ? "megaphone" : "none";
}

const char* variation_name(ReverbVariation variation) {
  return variation == ReverbVariation::DubEcho ? "dub_echo" : "reverb";
}

/// Value at the nearest-rank percentile @p q of an ascending-sortable sample set.
double percentile(std::vector<double>& samples, double q) {
  const size_t rank = static_cast<size_t>(std::ceil(q * static_cast<double>(samples.size())));
  const size_t index = rank == 0 ? 0 : rank - 1;
  std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(index),
                   samples.end());
  return samples[index];
}

void print_usage() {
  std::fprintf(stderr, "usage: sonare_voice_changer_bench [--blocks N] [--engine psola|grain]\n");
}

}  // namespace

int main(int argc, char** argv) {
  int blocks = kDefaultBlocks;
  bool grain = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) {
      blocks = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--engine") == 0 && i + 1 < argc) {
      const char* value = argv[++i];
      if (std::strcmp(value, "grain") == 0) {
        grain = true;
      } else if (std::strcmp(value, "psola") == 0) {
        grain = false;
      } else {
        print_usage();
        return 1;
      }
    } else {
      print_usage();
      return 1;
    }
  }
  if (blocks <= 0) {
    print_usage();
    return 1;
  }

  try {
    const RealtimeVoiceChangerConfig config = make_config(grain);
    RealtimeVoiceChanger changer(config);
    changer.prepare(kDefaultDawSampleRate, kBlockSize, 1);

    int held_notes = 0;
    if (!grain) {
      // Four held notes: the lead plus the three harmony voices.
      const int notes[] = {60, 64, 67, 72};
      for (int note : notes) {
        if (changer.note_on(note, kNoteVelocity)) ++held_notes;
      }
      if (held_notes != 4) {
        std::fprintf(stderr, "error: note_on refused %d of 4 notes\n", 4 - held_notes);
        return 1;
      }
    }

    const std::vector<float> input = make_voice_input(kLoopBlocks * kBlockSize);
    std::vector<float> output(static_cast<size_t>(kBlockSize), 0.0f);
    std::vector<double> block_us;
    block_us.reserve(static_cast<size_t>(blocks));

    const int total_blocks = kWarmupBlocks + blocks;
    for (int b = 0; b < total_blocks; ++b) {
      const float* in = input.data() + static_cast<size_t>(b % kLoopBlocks) * kBlockSize;
      const auto t0 = std::chrono::steady_clock::now();
      const bool ok = changer.process_block(in, output.data(), kBlockSize);
      const auto t1 = std::chrono::steady_clock::now();
      if (!ok) {
        std::fprintf(stderr, "error: process_block refused block %d\n", b);
        return 1;
      }
      g_sink = g_sink + output[static_cast<size_t>(kBlockSize - 1)];
      if (b >= kWarmupBlocks) {
        block_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
      }
    }

    const double block_duration_us =
        static_cast<double>(kBlockSize) / kDefaultDawSampleRate * kMicrosPerSecond;
    double sum = 0.0;
    for (double us : block_us) sum += us;
    const double mean_us = sum / static_cast<double>(block_us.size());
    const double p95_us = percentile(block_us, kP95);
    const double p99_us = percentile(block_us, kP99);

    std::printf("configuration\n");
    std::printf("  engine            %s\n", engine_name(config.voice_engine.engine));
    std::printf("  voices            %d (harmony)\n", config.harmony.voices);
    if (config.vocoder.enabled) {
      std::printf("  vocoder           %d bands, internal carrier\n", config.vocoder.bands);
    } else {
      std::printf("  vocoder           off\n");
    }
    std::printf("  held_notes        %d\n", held_notes);
    std::printf("  character         %s\n", character_name(config.character.type));
    std::printf("  reverb_variation  %s\n", variation_name(config.reverb_variation.variation));
    std::printf("  format            %.0f Hz, mono, %d-sample blocks\n", kDefaultDawSampleRate,
                kBlockSize);
    std::printf("blocks              %d (after %d warm-up blocks)\n", blocks, kWarmupBlocks);
    std::printf("block_duration_us   %.1f\n", block_duration_us);
    std::printf("mean_us             %.1f\n", mean_us);
    std::printf("p95_us              %.1f\n", p95_us);
    std::printf("p99_us              %.1f\n", p99_us);
    std::printf("mean_of_block       %.1f %%\n", kPercent * mean_us / block_duration_us);
    std::printf("sink                %f\n", static_cast<double>(g_sink));
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
