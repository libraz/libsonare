// C++ benchmark binary for libsonare per-feature performance.
//
// Mirrors run_bench.py but skips the Python cffi boundary, so the reported
// numbers reflect raw native execution time. Output is JSON on stdout so the
// driver script can stitch it together with the Python results.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "analysis/beat_analyzer.h"
#include "core/audio.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "effects/hpss.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "quick.h"

namespace {

constexpr int kResampledSr = 22050;
constexpr int kRuns = 3;

double median_ms(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  const size_t n = samples.size();
  if (n == 0) return 0.0;
  if (n % 2 == 1) return samples[n / 2];
  return (samples[n / 2 - 1] + samples[n / 2]) * 0.5;
}

template <typename F>
double bench(F&& fn, int runs = kRuns) {
  std::vector<double> times;
  times.reserve(runs);
  for (int i = 0; i < runs; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    fn();
    auto t1 = std::chrono::steady_clock::now();
    times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  return median_ms(std::move(times));
}

void print_row(const char* label, double ms) {
  std::fprintf(stderr, "%-22s %10.2f ms\n", label, ms);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <wav_file> [output.json]\n", argv[0]);
    return 1;
  }
  const std::string wav_path = argv[1];
  const std::string out_path = argc >= 3 ? argv[2] : "";

  auto audio = sonare::Audio::from_file(wav_path);
  auto resampled = sonare::resample(audio, kResampledSr);

  std::fprintf(stderr, "fixture: %s (%.2fs @ %d Hz -> resampled %d Hz, %zu samples)\n",
               wav_path.c_str(), audio.duration(), audio.sample_rate(), kResampledSr,
               resampled.size());
  std::fprintf(stderr, "runs per case: %d\n\n", kRuns);

  // Pre-computed intermediates used for "cached" measurements. These mirror
  // the zero-copy architecture inside analyze() where downstream features
  // reuse a single Spectrogram / MelSpectrogram instance.
  auto cached_spec = sonare::Spectrogram::compute(resampled);
  auto cached_mel = sonare::MelSpectrogram::compute(resampled, sonare::MelConfig{});

  // --- Full analysis (matches Python analyze() path) ---
  const double full_ms = bench(
      [&] { (void)sonare::quick::analyze(resampled.data(), resampled.size(), kResampledSr); });
  print_row("Full analyze", full_ms);

  std::fprintf(stderr, "\n-- Standalone (from raw audio) --\n");

  // Standalone timings: every call rebuilds STFT/Mel from raw audio.
  const double stft_ms = bench([&] {
    auto spec = sonare::Spectrogram::compute(resampled);
    (void)spec.n_frames();
  });
  print_row("STFT", stft_ms);

  const double mel_ms = bench([&] {
    auto m = sonare::MelSpectrogram::compute(resampled, sonare::MelConfig{});
    (void)m.n_frames();
  });
  print_row("Mel Spectrogram", mel_ms);

  const double hpss_ms = bench([&] {
    auto r = sonare::hpss(resampled);
    (void)r.harmonic.size();
  });
  print_row("HPSS", hpss_ms);

  const double onset_ms = bench([&] {
    auto v = sonare::compute_onset_strength(resampled);
    (void)v.size();
  });
  print_row("Onset Strength", onset_ms);

  const double chroma_ms = bench([&] {
    auto c = sonare::Chroma::compute(resampled);
    (void)c.n_frames();
  });
  print_row("Chroma", chroma_ms);

  const double beat_ms = bench([&] {
    auto b = sonare::detect_beats(resampled);
    (void)b.size();
  });
  print_row("Beat Track", beat_ms);

  const double mfcc_ms = bench([&] {
    auto m = sonare::MelSpectrogram::compute(resampled, sonare::MelConfig{});
    auto c = m.mfcc(13);
    (void)c.size();
  });
  print_row("MFCC", mfcc_ms);

  const double pyin_ms = bench([&] {
    auto p = sonare::pyin(resampled);
    (void)p.n_frames();
  });
  print_row("pYIN", pyin_ms);

  const double centroid_ms = bench([&] {
    auto spec = sonare::Spectrogram::compute(resampled);
    auto c = sonare::spectral_centroid(spec, kResampledSr);
    (void)c.size();
  });
  print_row("Spectral Centroid", centroid_ms);

  std::fprintf(stderr, "\n-- Cached (given pre-computed STFT / Mel) --\n");

  // Cached timings: STFT or Mel is built once outside the timed region. This
  // is the per-feature cost a multi-feature pipeline like analyze() actually
  // pays once it has the shared spectrogram in hand.
  const double mel_cached_ms = bench([&] {
    auto m = sonare::MelSpectrogram::from_spectrogram(cached_spec, kResampledSr);
    (void)m.n_frames();
  });
  print_row("Mel Spectrogram", mel_cached_ms);

  const double onset_cached_ms = bench([&] {
    auto v = sonare::compute_onset_strength(cached_mel);
    (void)v.size();
  });
  print_row("Onset Strength", onset_cached_ms);

  const double chroma_cached_ms = bench([&] {
    auto c = sonare::Chroma::from_spectrogram(cached_spec, kResampledSr);
    (void)c.n_frames();
  });
  print_row("Chroma", chroma_cached_ms);

  const double mfcc_cached_ms = bench([&] {
    auto c = cached_mel.mfcc(13);
    (void)c.size();
  });
  print_row("MFCC", mfcc_cached_ms);

  const double centroid_cached_ms = bench([&] {
    auto c = sonare::spectral_centroid(cached_spec, kResampledSr);
    (void)c.size();
  });
  print_row("Spectral Centroid", centroid_cached_ms);

  // --- JSON output ---
  auto write_json = [&](FILE* fp) {
    std::fprintf(fp, "{\n");
    std::fprintf(fp, "  \"fixture\": \"%s\",\n", wav_path.c_str());
    std::fprintf(fp, "  \"duration_sec\": %.3f,\n", audio.duration());
    std::fprintf(fp, "  \"source_sample_rate\": %d,\n", audio.sample_rate());
    std::fprintf(fp, "  \"resampled_sample_rate\": %d,\n", kResampledSr);
    std::fprintf(fp, "  \"runs_per_case\": %d,\n", kRuns);
    std::fprintf(fp, "  \"timing_source\": \"chrono::steady_clock (C++ internal)\",\n");
    std::fprintf(fp, "  \"full_analysis_ms\": %.2f,\n", full_ms);
    std::fprintf(fp, "  \"per_feature_standalone\": [\n");
    auto emit = [&](const char* name, double ms, bool last) {
      std::fprintf(fp, "    {\"feature\": \"%s\", \"libsonare_ms\": %.2f}%s\n", name, ms,
                   last ? "" : ",");
    };
    emit("STFT", stft_ms, false);
    emit("Mel Spectrogram", mel_ms, false);
    emit("HPSS", hpss_ms, false);
    emit("Onset Strength", onset_ms, false);
    emit("Chroma", chroma_ms, false);
    emit("Beat Track", beat_ms, false);
    emit("MFCC", mfcc_ms, false);
    emit("pYIN", pyin_ms, false);
    emit("Spectral Centroid", centroid_ms, true);
    std::fprintf(fp, "  ],\n");
    std::fprintf(fp, "  \"per_feature_cached\": [\n");
    emit("Mel Spectrogram", mel_cached_ms, false);
    emit("Onset Strength", onset_cached_ms, false);
    emit("Chroma", chroma_cached_ms, false);
    emit("MFCC", mfcc_cached_ms, false);
    emit("Spectral Centroid", centroid_cached_ms, true);
    std::fprintf(fp, "  ]\n");
    std::fprintf(fp, "}\n");
  };

  if (!out_path.empty()) {
    FILE* fp = std::fopen(out_path.c_str(), "w");
    if (!fp) {
      std::fprintf(stderr, "failed to open %s for writing\n", out_path.c_str());
      return 1;
    }
    write_json(fp);
    std::fclose(fp);
    std::fprintf(stderr, "\nwrote %s\n", out_path.c_str());
  } else {
    write_json(stdout);
  }

  return 0;
}
