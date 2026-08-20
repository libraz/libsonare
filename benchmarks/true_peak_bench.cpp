/// @file true_peak_bench.cpp
/// @brief Cost of the realtime true-peak meter, and the block-size dependence of
///        the streaming measurement it publishes.
///
/// Every surface's live `truePeakDb` comes from MeterProcessor driving the
/// polyphase TruePeakFilter, which is the single most expensive loop in a
/// metered block. Three things are measured:
///
///  1. MeterProcessor::process() with true peak off vs on (2x / 4x / 8x), which
///     is the cost a host actually pays, stated as a share of the real-time
///     budget for the block.
///  2. TruePeakFilter::upsample_with_history() in isolation, which is where
///     essentially all of that cost sits.
///  3. The reading itself at several block sizes, because the streaming
///     reconstruction truncates its forward stencil at every block edge and so
///     under-reads slightly (documented on mixing::MeterSnapshot).
///
/// Usage: sonare_true_peak_bench [iterations]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "mixing/meter.h"
#include "rt/true_peak_filter.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPi;
using sonare::mixing::MeterConfig;
using sonare::mixing::MeterProcessor;
using sonare::rt::TruePeakFilter;

constexpr double kSampleRate = 48000.0;

double percentile(std::vector<double> samples, double q) {
  if (samples.empty()) return 0.0;
  std::sort(samples.begin(), samples.end());
  const size_t index = static_cast<size_t>(q * static_cast<double>(samples.size() - 1));
  return samples[index];
}

/// Broadband content with strong near-Nyquist energy: the region where the
/// reconstruction has the most work to do and where inter-sample peaks are
/// largest, so the measurement is neither best- nor worst-case trivial.
std::vector<float> make_source(int frames, int channel) {
  std::vector<float> out(static_cast<size_t>(frames), 0.0f);
  const double offset = 0.37 * static_cast<double>(channel);
  for (int i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    out[static_cast<size_t>(i)] =
        static_cast<float>(0.30 * std::sin(static_cast<double>(kTwoPi) * 11731.0 * t + offset) +
                           0.18 * std::sin(static_cast<double>(kTwoPi) * 997.0 * t) +
                           0.06 * std::sin(static_cast<double>(kTwoPi) * 19813.0 * t + 1.1));
  }
  return out;
}

struct Variant {
  std::string name;
  MeterConfig config;
};

/// Phased alternation rather than per-iteration interleaving: each variant runs
/// a run of consecutive blocks so its working set stays hot (alternating two
/// meters every block thrashes the cache and inflates both arms), while the
/// rounds still alternate often enough that background load and thermal drift
/// land on both.
void measure_meter(const std::vector<Variant>& variants, int channels, int block, int rounds,
                   int phase_blocks) {
  const int discard = phase_blocks / 10;
  // MeterProcessor holds a seqlock cell and so is neither copyable nor movable;
  // hold them indirectly rather than by value.
  std::vector<std::unique_ptr<MeterProcessor>> meters;
  std::vector<std::vector<double>> samples(variants.size());
  meters.reserve(variants.size());
  for (const auto& variant : variants) {
    meters.push_back(std::make_unique<MeterProcessor>(variant.config));
    meters.back()->prepare(kSampleRate, block);
  }

  std::vector<std::vector<float>> audio(static_cast<size_t>(channels));
  std::vector<float*> pointers(static_cast<size_t>(channels));
  for (int ch = 0; ch < channels; ++ch) {
    audio[static_cast<size_t>(ch)] = make_source(block, ch);
    pointers[static_cast<size_t>(ch)] = audio[static_cast<size_t>(ch)].data();
  }

  for (int round = 0; round < rounds; ++round) {
    for (size_t v = 0; v < meters.size(); ++v) {
      for (int k = 0; k < phase_blocks; ++k) {
        const auto start = std::chrono::steady_clock::now();
        meters[v]->process(pointers.data(), channels, block);
        const auto end = std::chrono::steady_clock::now();
        if (k >= discard) {
          samples[v].push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
      }
    }
  }

  const double budget_us = 1.0e6 * static_cast<double>(block) / kSampleRate;
  const double baseline_p50 = percentile(samples[0], 0.50);
  for (size_t v = 0; v < variants.size(); ++v) {
    const double p50 = percentile(samples[v], 0.50);
    const double p95 = percentile(samples[v], 0.95);
    const double delta = p50 - baseline_p50;
    std::printf("meter,%d,%d,%s,%.3f,%.3f,%.4f,%.4f,%.3f,%.2f\n", channels, block,
                variants[v].name.c_str(), p50, p95, 100.0 * p50 / budget_us,
                100.0 * delta / budget_us, 1000.0 * delta / static_cast<double>(block),
                static_cast<double>(meters[v]->snapshot().max_true_peak_db));
  }
  std::fflush(stdout);
}

/// The upsample call on its own, plus the peak scan the meter runs over its
/// output, so the share of the meter's cost that lives in the FIR is visible.
void measure_filter(int factor, int channels, int block, int iterations) {
  TruePeakFilter filter(channels, factor);
  filter.prepare(channels, block);

  std::vector<std::vector<float>> audio(static_cast<size_t>(channels));
  std::vector<std::vector<float>> oversampled(
      static_cast<size_t>(channels),
      std::vector<float>(static_cast<size_t>(block) * static_cast<size_t>(factor), 0.0f));
  std::vector<const float*> in(static_cast<size_t>(channels));
  std::vector<float*> out(static_cast<size_t>(channels));
  for (int ch = 0; ch < channels; ++ch) {
    audio[static_cast<size_t>(ch)] = make_source(block, ch);
    in[static_cast<size_t>(ch)] = audio[static_cast<size_t>(ch)].data();
    out[static_cast<size_t>(ch)] = oversampled[static_cast<size_t>(ch)].data();
  }

  std::vector<double> upsample_us;
  std::vector<double> total_us;
  upsample_us.reserve(static_cast<size_t>(iterations));
  total_us.reserve(static_cast<size_t>(iterations));
  float peak = 0.0f;
  for (int it = 0; it < iterations; ++it) {
    const auto start = std::chrono::steady_clock::now();
    filter.upsample_with_history(in.data(), out.data(), channels, block);
    const auto mid = std::chrono::steady_clock::now();
    float block_peak = 0.0f;
    for (int ch = 0; ch < channels; ++ch) {
      for (int i = 0; i < block * factor; ++i) {
        block_peak = std::max(
            block_peak, std::abs(oversampled[static_cast<size_t>(ch)][static_cast<size_t>(i)]));
      }
    }
    const auto end = std::chrono::steady_clock::now();
    upsample_us.push_back(std::chrono::duration<double, std::micro>(mid - start).count());
    total_us.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    peak = block_peak;
  }

  const double up_p50 = percentile(upsample_us, 0.50);
  const double all_p50 = percentile(total_us, 0.50);
  const double per_sample_ns =
      1000.0 * up_p50 / (static_cast<double>(block) * static_cast<double>(channels));
  std::printf("filter,%d,%d,%d,%.3f,%.3f,%.3f,%.6f\n", factor, channels, block, up_p50, all_p50,
              per_sample_ns, static_cast<double>(peak));
  std::fflush(stdout);
}

/// The documented block-edge approximation, as a number. The same signal metered
/// through the same filter at several block sizes must NOT give the same answer:
/// the centered stencil needs future base-rate samples a streaming path does not
/// have, so each block's tail interpolates against a truncated kernel.
void measure_block_size_dependence(int factor) {
  constexpr int kTotal = 8192;
  std::vector<float> signal(static_cast<size_t>(kTotal), 0.0f);
  for (int i = 0; i < kTotal; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    signal[static_cast<size_t>(i)] =
        static_cast<float>(0.70 * std::sin(static_cast<double>(kTwoPi) * 11997.0 * t + 0.25) +
                           0.25 * std::sin(static_cast<double>(kTwoPi) * 23994.0 * t));
  }
  float sample_peak = 0.0f;
  for (float value : signal) sample_peak = std::max(sample_peak, std::abs(value));
  std::printf("blocksize,%d,sample_peak,%.4f\n", factor,
              20.0 * std::log10(static_cast<double>(sample_peak)));

  for (const int block : {64, 128, 256, 512, 1024, 8192}) {
    TruePeakFilter filter(1, factor);
    filter.prepare(1, block);
    std::vector<float> oversampled(static_cast<size_t>(block) * static_cast<size_t>(factor), 0.0f);
    float peak = 0.0f;
    for (int offset = 0; offset + block <= kTotal; offset += block) {
      const float* in = signal.data() + offset;
      float* out = oversampled.data();
      filter.upsample_with_history(&in, &out, 1, block);
      for (int i = 0; i < block * factor; ++i) {
        peak = std::max(peak, std::abs(oversampled[static_cast<size_t>(i)]));
      }
    }
    std::printf("blocksize,%d,%d,%.4f\n", factor, block,
                20.0 * std::log10(static_cast<double>(peak)));
  }
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 4000;

  MeterConfig off{};
  off.measure_true_peak = false;
  MeterConfig tp2{};
  tp2.measure_true_peak = true;
  tp2.true_peak_oversample = 2;
  MeterConfig tp4{};
  tp4.measure_true_peak = true;
  tp4.true_peak_oversample = 4;
  MeterConfig tp8{};
  tp8.measure_true_peak = true;
  tp8.true_peak_oversample = 8;
  const std::vector<Variant> variants{
      {"lufs_only", off}, {"true_peak_2x", tp2}, {"true_peak_4x", tp4}, {"true_peak_8x", tp8}};

  std::printf(
      "# meter,channels,block,variant,p50_us,p95_us,p50_pct_realtime,delta_pct_realtime,"
      "delta_ns_per_frame,max_true_peak_db\n");
  const int phase_blocks = 500;
  const int rounds = std::max(2, iterations / phase_blocks);
  for (const int channels : {1, 2}) {
    for (const int block : {128, 256, 512}) {
      measure_meter(variants, channels, block, rounds, phase_blocks);
    }
  }

  std::printf(
      "# filter,factor,channels,block,upsample_p50_us,upsample_plus_scan_p50_us,"
      "ns_per_input_sample_per_channel,peak\n");
  for (const int factor : {2, 4, 8}) {
    for (const int channels : {1, 2}) {
      for (const int block : {128, 256, 512}) {
        measure_filter(factor, channels, block, iterations);
      }
    }
  }

  std::printf("# blocksize,factor,block,true_peak_db\n");
  measure_block_size_dependence(4);
  return 0;
}
