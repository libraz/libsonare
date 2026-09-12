#include <Eigen/Core>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "analysis/music_analyzer.h"
#include "core/audio.h"
#include "core/spectrum.h"
#include "feature/mel_spectrogram.h"
#include "util/constants.h"

namespace {

constexpr int kSize1 = 1025;
constexpr int kSize2 = 4097;

/// Signal length for the mel-pipeline stages, long enough that the per-call
/// allocation is a small share of the measurement.
constexpr int kBenchSeconds = 30;
constexpr float kBenchFundamentalHz = 110.0f;
constexpr int kBenchHarmonics = 8;

/// Enough repetitions that an interquartile range is an interquartile range.
constexpr int kMelPipelineSamples = 31;

/// analyze() runs HPSS, two CQTs, onset and beat tracking on top of the mel
/// path, so it gets its own far smaller count; it exists to give the stage
/// fraction a pipeline-level denominator, not to be characterised itself.
constexpr int kAnalyzeSamples = 3;

volatile float g_sink = 0.0f;

double median_ms(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  const size_t n = samples.size();
  if (n == 0) return 0.0;
  if ((n % 2) == 1) return samples[n / 2];
  return (samples[n / 2 - 1] + samples[n / 2]) * 0.5;
}

template <typename Fn>
double bench(Fn&& fn, int runs, int iterations) {
  std::vector<double> times;
  times.reserve(static_cast<size_t>(runs));
  for (int run = 0; run < runs; ++run) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) fn();
    const auto t1 = std::chrono::steady_clock::now();
    const double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    times.push_back(total_ms / static_cast<double>(iterations));
  }
  return median_ms(std::move(times));
}

void power_eigen(const std::complex<float>* data, float* out, int n) {
  Eigen::Map<const Eigen::ArrayXcf> in_map(data, n);
  Eigen::Map<Eigen::ArrayXf> out_map(out, n);
  out_map = in_map.abs2();
}

void power_scalar(const std::complex<float>* data, float* out, int n) {
  for (int i = 0; i < n; ++i) {
    const float re = data[i].real();
    const float im = data[i].imag();
    out[i] = re * re + im * im;
  }
}

std::vector<std::complex<float>> make_complex_signal(int n) {
  std::vector<std::complex<float>> data(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const float phase = 0.01f * sonare::constants::kPi * static_cast<float>(i);
    data[static_cast<size_t>(i)] = std::complex<float>(std::cos(phase), std::sin(phase));
  }
  return data;
}

struct SizeResult {
  double eigen_ms;
  double scalar_ms;
  double speedup;
  float max_abs_diff;
};

SizeResult run_size(int n, int iterations, int runs) {
  const std::vector<std::complex<float>> data = make_complex_signal(n);
  std::vector<float> out_eigen(static_cast<size_t>(n), 0.0f);
  std::vector<float> out_scalar(static_cast<size_t>(n), 0.0f);

  power_eigen(data.data(), out_eigen.data(), n);
  power_scalar(data.data(), out_scalar.data(), n);

  float max_abs_diff = 0.0f;
  for (int i = 0; i < n; ++i) {
    max_abs_diff = std::max(max_abs_diff, std::abs(out_eigen[static_cast<size_t>(i)] -
                                                   out_scalar[static_cast<size_t>(i)]));
  }

  const double eigen_ms = bench(
      [&] {
        power_eigen(data.data(), out_eigen.data(), n);
        g_sink += out_eigen[0];
      },
      runs, iterations);

  const double scalar_ms = bench(
      [&] {
        power_scalar(data.data(), out_scalar.data(), n);
        g_sink += out_scalar[0];
      },
      runs, iterations);

  return SizeResult{eigen_ms, scalar_ms, scalar_ms / std::max(eigen_ms, 1.0e-12), max_abs_diff};
}

/// @brief Linearly interpolated quantile of an already-sorted sample vector.
double quantile(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) return 0.0;
  const double pos = q * static_cast<double>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(pos);
  const size_t hi = std::min(lo + 1, sorted.size() - 1);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - static_cast<double>(lo));
}

/// @brief Order statistics of one timed quantity.
/// @details The interquartile range is the reported spread. A min-to-max range
///          over timing samples estimates the worst scheduling artifact rather
///          than the distribution, and one artifact should not characterise the
///          measurement.
struct Stats {
  double min_ms;
  double q1_ms;
  double median_ms;
  double q3_ms;
  double max_ms;

  double iqr_ms() const { return q3_ms - q1_ms; }
};

Stats summarize(std::vector<double> samples) {
  if (samples.empty()) return Stats{0.0, 0.0, 0.0, 0.0, 0.0};
  std::sort(samples.begin(), samples.end());
  return Stats{samples.front(), quantile(samples, 0.25), quantile(samples, 0.5),
               quantile(samples, 0.75), samples.back()};
}

template <typename Fn>
double time_once_ms(Fn&& fn) {
  const auto t0 = std::chrono::steady_clock::now();
  fn();
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

/// @brief Deterministic broadband signal for the mel-pipeline stages.
/// @details Broadband on purpose: a sparse spectrum leaves near-zero bins whose
///          squares are subnormal, which times the hardware's subnormal handling
///          rather than the power fill.
sonare::Audio make_bench_audio(int sample_rate, int seconds) {
  const size_t n = static_cast<size_t>(sample_rate) * static_cast<size_t>(seconds);
  std::vector<float> samples(n, 0.0f);
  uint32_t state = 22695477u;
  for (size_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    float value = 0.0f;
    for (int h = 1; h <= kBenchHarmonics; ++h) {
      const float freq = kBenchFundamentalHz * static_cast<float>(h);
      value += std::cos(sonare::constants::kTwoPi * freq * t) / static_cast<float>(h);
    }
    state = state * 1664525u + 1013904223u;
    const float unit = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    samples[i] = 0.25f * value + 0.05f * (2.0f * unit - 1.0f);
  }
  return sonare::Audio::from_vector(std::move(samples), sample_rate);
}

struct MelPipelineResult {
  Stats stft;
  Stats fill_current;
  Stats fill_hypot;
  Stats fill_sqrt;
  Stats filterbank;
  Stats end_to_end;
  Stats analyze;
};

/// @brief Times the mel path and both candidate power formulas in one binary.
/// @details The fill is far smaller than the STFT, so recovering it by
///          subtracting two STFT-sized timings puts it inside their spread.
///          Each region is timed on its own instead. One Spectrogram serves the
///          first five: compute() leaves both caches empty, complex_data() does
///          not fill them, and by the filterbank region the power cache is warm.
///          The two candidate formulas run over that same complex data so the
///          comparison never crosses a binary — hypotf does not vectorise, so a
///          ratio measured outside this target does not transfer into it. The
///          last region repeats the pipeline on a fresh Spectrogram as a
///          cross-check on the others' sum.
MelPipelineResult run_mel_pipeline(int samples) {
  const sonare::Audio audio =
      make_bench_audio(sonare::constants::kDefaultSampleRate, kBenchSeconds);
  const int sample_rate = audio.sample_rate();

  sonare::StftConfig stft_config;
  stft_config.n_fft = sonare::constants::kDefaultNFft;
  stft_config.hop_length = sonare::constants::kDefaultHopLength;
  const sonare::MelFilterConfig mel_config;

  std::vector<double> stft;
  std::vector<double> fill_current;
  std::vector<double> fill_hypot;
  std::vector<double> fill_sqrt;
  std::vector<double> filterbank;
  std::vector<double> end_to_end;
  std::vector<double> analyze;

  for (int i = 0; i < kAnalyzeSamples; ++i) {
    analyze.push_back(time_once_ms([&] {
      sonare::MusicAnalyzer analyzer(audio);
      const sonare::AnalysisResult result = analyzer.analyze();
      g_sink += result.bpm;
    }));
  }

  for (int i = 0; i < samples; ++i) {
    sonare::Spectrogram spec;
    stft.push_back(time_once_ms([&] { spec = sonare::Spectrogram::compute(audio, stft_config); }));

    const std::complex<float>* data = spec.complex_data();
    const size_t cells = static_cast<size_t>(spec.n_bins()) * static_cast<size_t>(spec.n_frames());
    g_sink += data[0].real();

    fill_current.push_back(time_once_ms([&] { g_sink += spec.power()[0]; }));

    // Option A's fill: magnitude via std::abs (hypotf), power by squaring it.
    fill_hypot.push_back(time_once_ms([&] {
      std::vector<float> out(cells);
      for (size_t c = 0; c < cells; ++c) {
        const float m = std::abs(data[c]);
        out[c] = m * m;
      }
      g_sink += out[0];
    }));

    // The cheaper magnitude: same squaring, sqrt instead of hypotf.
    fill_sqrt.push_back(time_once_ms([&] {
      std::vector<float> out(cells);
      for (size_t c = 0; c < cells; ++c) {
        const float re = data[c].real();
        const float im = data[c].imag();
        const float m = std::sqrt(re * re + im * im);
        out[c] = m * m;
      }
      g_sink += out[0];
    }));

    filterbank.push_back(time_once_ms([&] {
      const sonare::MelSpectrogram mel =
          sonare::MelSpectrogram::from_spectrogram(spec, sample_rate, mel_config);
      g_sink += mel.power_data()[0];
    }));

    end_to_end.push_back(time_once_ms([&] {
      const sonare::Spectrogram fresh = sonare::Spectrogram::compute(audio, stft_config);
      const sonare::MelSpectrogram mel =
          sonare::MelSpectrogram::from_spectrogram(fresh, sample_rate, mel_config);
      g_sink += mel.power_data()[0];
    }));
  }

  return MelPipelineResult{summarize(stft),      summarize(fill_current), summarize(fill_hypot),
                           summarize(fill_sqrt), summarize(filterbank),   summarize(end_to_end),
                           summarize(analyze)};
}

void print_stats(const char* name, const Stats& stats) {
  std::printf("  \"%s_min_ms\": %.6f,\n", name, stats.min_ms);
  std::printf("  \"%s_q1_ms\": %.6f,\n", name, stats.q1_ms);
  std::printf("  \"%s_median_ms\": %.6f,\n", name, stats.median_ms);
  std::printf("  \"%s_q3_ms\": %.6f,\n", name, stats.q3_ms);
  std::printf("  \"%s_max_ms\": %.6f,\n", name, stats.max_ms);
  std::printf("  \"%s_iqr_ms\": %.6f,\n", name, stats.iqr_ms());
}

/// @brief What a candidate formula adds, as ms and as a share of one mel call.
/// @details Interval by propagating each quantity's IQR to the widening end. It
///          is crude and it is stated: a crude interval beats a withheld number.
void print_candidate_cost(const char* name, const Stats& candidate, const Stats& current,
                          const Stats& end_to_end) {
  const double added = candidate.median_ms - current.median_ms;
  const double added_low = (candidate.median_ms - candidate.iqr_ms() * 0.5) -
                           (current.median_ms + current.iqr_ms() * 0.5);
  const double added_high = (candidate.median_ms + candidate.iqr_ms() * 0.5) -
                            (current.median_ms - current.iqr_ms() * 0.5);
  const double denom = std::max(end_to_end.median_ms, 1.0e-12);
  const double denom_wide = std::max(end_to_end.median_ms + end_to_end.iqr_ms() * 0.5, 1.0e-12);
  const double denom_narrow = std::max(end_to_end.median_ms - end_to_end.iqr_ms() * 0.5, 1.0e-12);
  std::printf("  \"%s_added_ms\": %.6f,\n", name, added);
  std::printf("  \"%s_added_ms_low\": %.6f,\n", name, added_low);
  std::printf("  \"%s_added_ms_high\": %.6f,\n", name, added_high);
  std::printf("  \"%s_pct_of_end_to_end\": %.4f,\n", name, 100.0 * added / denom);
  std::printf("  \"%s_pct_of_end_to_end_low\": %.4f,\n", name, 100.0 * added_low / denom_wide);
  std::printf("  \"%s_pct_of_end_to_end_high\": %.4f,\n", name, 100.0 * added_high / denom_narrow);
}

}  // namespace

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 50000;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 7;

  const SizeResult r1 = run_size(kSize1, iterations, runs);
  const SizeResult r2 = run_size(kSize2, std::max(1, iterations / 4), runs);

  std::printf("{\n");
  std::printf("  \"benchmark\": \"spectrum_power_eigen_vs_scalar\",\n");
  std::printf("  \"size_1\": %d,\n", kSize1);
  std::printf("  \"size_2\": %d,\n", kSize2);
  std::printf("  \"runs\": %d,\n", runs);
  std::printf("  \"iterations_per_run\": %d,\n", iterations);
  std::printf("  \"iterations_per_run_size_2\": %d,\n", std::max(1, iterations / 4));
  std::printf("  \"power_1025_eigen_ms\": %.6f,\n", r1.eigen_ms);
  std::printf("  \"power_1025_scalar_ms\": %.6f,\n", r1.scalar_ms);
  std::printf("  \"power_1025_speedup_ratio\": %.6f,\n", r1.speedup);
  std::printf("  \"max_abs_diff_1025\": %.6e,\n", static_cast<double>(r1.max_abs_diff));
  std::printf("  \"power_4097_eigen_ms\": %.6f,\n", r2.eigen_ms);
  std::printf("  \"power_4097_scalar_ms\": %.6f,\n", r2.scalar_ms);
  std::printf("  \"power_4097_speedup_ratio\": %.6f,\n", r2.speedup);
  std::printf("  \"max_abs_diff_4097\": %.6e,\n", static_cast<double>(r2.max_abs_diff));

  const MelPipelineResult mel = run_mel_pipeline(kMelPipelineSamples);
  const double total_ms =
      mel.stft.median_ms + mel.fill_current.median_ms + mel.filterbank.median_ms;
  const double cross_check_delta_ms = total_ms - mel.end_to_end.median_ms;
  const double cross_check_tolerance_ms = mel.stft.iqr_ms() + mel.fill_current.iqr_ms() +
                                          mel.filterbank.iqr_ms() + mel.end_to_end.iqr_ms();

  std::printf("  \"mel_pipeline_seconds\": %d,\n", kBenchSeconds);
  std::printf("  \"mel_pipeline_sample_rate\": %d,\n", sonare::constants::kDefaultSampleRate);
  std::printf("  \"mel_pipeline_n_fft\": %d,\n", sonare::constants::kDefaultNFft);
  std::printf("  \"mel_pipeline_hop_length\": %d,\n", sonare::constants::kDefaultHopLength);
  std::printf("  \"n_reps\": %d,\n", kMelPipelineSamples);
  print_stats("stft", mel.stft);
  print_stats("power_fill_current", mel.fill_current);
  print_stats("power_fill_hypot", mel.fill_hypot);
  print_stats("power_fill_sqrt", mel.fill_sqrt);
  print_stats("mel_filterbank", mel.filterbank);
  print_stats("end_to_end", mel.end_to_end);
  print_stats("analyze", mel.analyze);
  std::printf("  \"analyze_n_reps\": %d,\n", kAnalyzeSamples);
  // The stage percentages divide by the mel call; this converts them to the
  // pipeline a caller actually runs.
  std::printf("  \"mel_share_of_analyze\": %.6f,\n",
              mel.end_to_end.median_ms / std::max(mel.analyze.median_ms, 1.0e-12));
  std::printf("  \"power_fill_fraction_of_analyze\": %.6f,\n",
              mel.fill_current.median_ms / std::max(mel.analyze.median_ms, 1.0e-12));
  std::printf("  \"stage_sum_ms\": %.6f,\n", total_ms);
  std::printf("  \"cross_check_delta_ms\": %.6f,\n", cross_check_delta_ms);
  std::printf("  \"cross_check_tolerance_ms\": %.6f,\n", cross_check_tolerance_ms);
  std::printf("  \"cross_check_agrees\": %s,\n",
              std::fabs(cross_check_delta_ms) <= cross_check_tolerance_ms ? "true" : "false");

  // Every percentage below divides by this, named so it cannot travel without it.
  std::printf("  \"cost_percent_denominator\": \"end_to_end_median_ms\",\n");
  print_candidate_cost("option_a_hypot", mel.fill_hypot, mel.fill_current, mel.end_to_end);
  print_candidate_cost("option_a_prime_sqrt", mel.fill_sqrt, mel.fill_current, mel.end_to_end);

  // The fraction the fill already costs, reported with its interval rather than
  // gated on a threshold: the threshold is the reader's to apply, not this
  // harness's to enforce by withholding the number.
  const double e2e = std::max(mel.end_to_end.median_ms, 1.0e-12);
  const double e2e_wide =
      std::max(mel.end_to_end.median_ms + mel.end_to_end.iqr_ms() * 0.5, 1.0e-12);
  const double e2e_narrow =
      std::max(mel.end_to_end.median_ms - mel.end_to_end.iqr_ms() * 0.5, 1.0e-12);
  std::printf("  \"power_fill_fraction_of_mel_pipeline\": %.6f,\n",
              mel.fill_current.median_ms / e2e);
  std::printf("  \"power_fill_fraction_low\": %.6f,\n",
              (mel.fill_current.median_ms - mel.fill_current.iqr_ms() * 0.5) / e2e_wide);
  std::printf("  \"power_fill_fraction_high\": %.6f\n",
              (mel.fill_current.median_ms + mel.fill_current.iqr_ms() * 0.5) / e2e_narrow);
  std::printf("}\n");

  return 0;
}
