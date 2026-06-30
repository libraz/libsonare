/// @file piano_voice_bench.cpp
/// @brief CPU microbenchmark for the extended-waveguide piano (kPiano /
///        midi/synth/piano_voice). Measures the real-time factor (CPU time
///        per second of rendered audio) across voice counts and block sizes,
///        so the per-voice cost and polyphony headroom can be projected onto
///        a single-threaded WebAssembly AudioWorklet budget.
///
/// Reports CPU-ms per 1 s of audio and the real-time factor RTF = cpu/audio
/// (RTF < 1 => renders faster than real time; 1/RTF is the theoretical voice
/// headroom at that operating point). The block sizes 128 and 256 bracket the
/// AudioWorklet render quantum.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"

namespace {

using sonare::midi::make_midi1_note_on;
using sonare::midi::MidiEvent;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;

constexpr double kSampleRate = 48000.0;
constexpr double kAudioSeconds = 2.0;

volatile float g_sink = 0.0f;

MidiEvent note_on(uint8_t note, uint8_t velocity) {
  MidiEvent e;
  e.ump = make_midi1_note_on(0, 0, note, velocity);
  return e;
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  if (n == 0) return 0.0;
  return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// Render @p polyphony held piano notes for kAudioSeconds at @p block_size and
/// return the median CPU-ms spent per 1 s of audio over a few runs.
double measure(int polyphony, int block_size, int* sounding) {
  NativeSynthConfig cfg;
  cfg.patch = gm_fallback_patch(0, 0);  // GM program 0: acoustic grand (kPiano)
  cfg.polyphony = polyphony;

  const int total = static_cast<int>(kAudioSeconds * kSampleRate);
  const int blocks = total / block_size;

  std::vector<float> left(static_cast<size_t>(block_size));
  std::vector<float> right(static_cast<size_t>(block_size));
  float* chans[2] = {left.data(), right.data()};

  std::vector<double> per_run_ms_per_s;
  for (int run = 0; run < 5; ++run) {
    NativeSynth synth(cfg);
    synth.prepare(kSampleRate, block_size);
    // Spread the chord across the keyboard (A0=21 .. C8=108).
    for (int v = 0; v < polyphony; ++v) {
      const int note = 21 + (87 * v) / std::max(1, polyphony - 1);
      synth.on_event(0, note_on(static_cast<uint8_t>(note), 100));
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) {
      synth.process(chans, 2, block_size);
      g_sink += left[0] + right[block_size - 1];
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double cpu_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double audio_s = static_cast<double>(blocks * block_size) / kSampleRate;
    per_run_ms_per_s.push_back(cpu_ms / audio_s);
    if (sounding) *sounding = synth.active_voice_count();
  }
  return median(std::move(per_run_ms_per_s));
}

}  // namespace

int main() {
  std::printf("kPiano (extended-waveguide) CPU microbench @ %.0f Hz, %.1f s/render\n", kSampleRate,
              kAudioSeconds);
  std::printf("RTF = CPU/audio (<1 = real-time capable); headroom = 1/RTF voices\n\n");
  std::printf("%-10s %-7s %-10s %-14s %-8s %-10s\n", "voices", "block", "sounding", "CPU-ms/1s",
              "RTF", "headroom");
  std::printf("%s\n", "------------------------------------------------------------------");

  const int voice_counts[] = {1, 8, 16, 32, 64};
  const int block_sizes[] = {128, 256};
  for (int bs : block_sizes) {
    for (int vc : voice_counts) {
      int sounding = 0;
      const double ms_per_s = measure(vc, bs, &sounding);
      const double rtf = ms_per_s / 1000.0;
      const double headroom = rtf > 0.0 ? 1.0 / rtf : 0.0;
      std::printf("%-10d %-7d %-10d %-14.3f %-8.4f %-10.1f\n", vc, bs, sounding, ms_per_s, rtf,
                  headroom);
    }
    std::printf("\n");
  }
  std::printf("note: NativeSynth caps polyphony at kMaxSynthVoices=64\n");
  std::printf("sink=%f\n", static_cast<double>(g_sink));
  return 0;
}
