#include <Eigen/Core>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "util/constants.h"

namespace {

constexpr int kNMels = 128;
constexpr int kNChroma = 12;
constexpr int kNBins = 1025;

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

// GEMV: out[M] = filterbank[M, N] * power[N], filterbank is row-major.
void apply_gemv_eigen(const float* filterbank, const float* power, float* out, int n_rows,
                      int n_cols) {
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> fb_map(
      filterbank, n_rows, n_cols);
  Eigen::Map<const Eigen::VectorXf> power_map(power, n_cols);
  Eigen::Map<Eigen::VectorXf> out_map(out, n_rows);
  out_map.noalias() = fb_map * power_map;
}

void apply_gemv_scalar(const float* filterbank, const float* power, float* out, int n_rows,
                       int n_cols) {
  for (int m = 0; m < n_rows; ++m) {
    float sum = 0.0f;
    const float* filter_row = filterbank + m * n_cols;
    for (int k = 0; k < n_cols; ++k) {
      sum += filter_row[k] * power[k];
    }
    out[m] = sum;
  }
}

// Build a sparse triangle-shaped filterbank similar to filterbank_bench.cpp,
// scaled so peak position varies with row count.
void build_filterbank(std::vector<float>& fb, int n_rows, int n_cols) {
  fb.assign(static_cast<size_t>(n_rows) * static_cast<size_t>(n_cols), 0.0f);
  // Distribute centers across the bin range.
  const float stride = static_cast<float>(n_cols) / static_cast<float>(n_rows + 1);
  for (int m = 0; m < n_rows; ++m) {
    const float center = stride * static_cast<float>(m + 1);
    const float half_width = std::max(4.0f, stride);
    for (int k = 0; k < n_cols; ++k) {
      const float dist = std::abs(static_cast<float>(k) - center);
      const float val = 1.0f - dist / half_width;
      fb[static_cast<size_t>(m * n_cols + k)] = std::max(0.0f, std::min(1.0f, val));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 5000;
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 7;

  // Build dummy filterbanks for mel (128 x 1025) and chroma (12 x 1025).
  std::vector<float> mel_fb;
  build_filterbank(mel_fb, kNMels, kNBins);
  std::vector<float> chroma_fb;
  build_filterbank(chroma_fb, kNChroma, kNBins);

  // Single power vector of length n_bins.
  std::vector<float> power(static_cast<size_t>(kNBins), 0.0f);
  for (int k = 0; k < kNBins; ++k) {
    const float phase = 0.01f * sonare::constants::kPi * static_cast<float>(k);
    power[static_cast<size_t>(k)] = std::abs(std::sin(phase));
  }

  std::vector<float> mel_out_eigen(static_cast<size_t>(kNMels), 0.0f);
  std::vector<float> mel_out_scalar(static_cast<size_t>(kNMels), 0.0f);
  std::vector<float> chroma_out_eigen(static_cast<size_t>(kNChroma), 0.0f);
  std::vector<float> chroma_out_scalar(static_cast<size_t>(kNChroma), 0.0f);

  // Warm-up + correctness check.
  apply_gemv_eigen(mel_fb.data(), power.data(), mel_out_eigen.data(), kNMels, kNBins);
  apply_gemv_scalar(mel_fb.data(), power.data(), mel_out_scalar.data(), kNMels, kNBins);
  apply_gemv_eigen(chroma_fb.data(), power.data(), chroma_out_eigen.data(), kNChroma, kNBins);
  apply_gemv_scalar(chroma_fb.data(), power.data(), chroma_out_scalar.data(), kNChroma, kNBins);

  float max_abs_diff_mel = 0.0f;
  for (size_t i = 0; i < mel_out_eigen.size(); ++i) {
    max_abs_diff_mel =
        std::max(max_abs_diff_mel, std::abs(mel_out_eigen[i] - mel_out_scalar[i]));
  }
  const bool correctness_mel_pass = max_abs_diff_mel <= 1e-3f;

  float max_abs_diff_chroma = 0.0f;
  for (size_t i = 0; i < chroma_out_eigen.size(); ++i) {
    max_abs_diff_chroma =
        std::max(max_abs_diff_chroma, std::abs(chroma_out_eigen[i] - chroma_out_scalar[i]));
  }
  const bool correctness_chroma_pass = max_abs_diff_chroma <= 1e-3f;

  const double mel_eigen_ms = bench(
      [&] {
        apply_gemv_eigen(mel_fb.data(), power.data(), mel_out_eigen.data(), kNMels, kNBins);
        g_sink += mel_out_eigen[0];
      },
      runs, iterations);

  const double mel_scalar_ms = bench(
      [&] {
        apply_gemv_scalar(mel_fb.data(), power.data(), mel_out_scalar.data(), kNMels, kNBins);
        g_sink += mel_out_scalar[0];
      },
      runs, iterations);

  const double chroma_eigen_ms = bench(
      [&] {
        apply_gemv_eigen(chroma_fb.data(), power.data(), chroma_out_eigen.data(), kNChroma,
                         kNBins);
        g_sink += chroma_out_eigen[0];
      },
      runs, iterations);

  const double chroma_scalar_ms = bench(
      [&] {
        apply_gemv_scalar(chroma_fb.data(), power.data(), chroma_out_scalar.data(), kNChroma,
                          kNBins);
        g_sink += chroma_out_scalar[0];
      },
      runs, iterations);

  const double mel_speedup = mel_scalar_ms / std::max(mel_eigen_ms, 1.0e-9);
  const double chroma_speedup = chroma_scalar_ms / std::max(chroma_eigen_ms, 1.0e-9);

  std::printf("{\n");
  std::printf("  \"benchmark\": \"streaming_mel_chroma_gemv_eigen_vs_scalar\",\n");
  std::printf("  \"n_mels\": %d,\n", kNMels);
  std::printf("  \"n_chroma\": %d,\n", kNChroma);
  std::printf("  \"n_bins\": %d,\n", kNBins);
  std::printf("  \"runs\": %d,\n", runs);
  std::printf("  \"iterations_per_run\": %d,\n", iterations);
  std::printf("  \"mel_gemv_eigen_ms\": %.6f,\n", mel_eigen_ms);
  std::printf("  \"mel_gemv_scalar_ms\": %.6f,\n", mel_scalar_ms);
  std::printf("  \"mel_speedup_ratio\": %.6f,\n", mel_speedup);
  std::printf("  \"max_abs_diff_mel\": %.6e,\n", static_cast<double>(max_abs_diff_mel));
  std::printf("  \"correctness_mel\": \"%s\",\n", correctness_mel_pass ? "PASS" : "FAILED");
  std::printf("  \"chroma_gemv_eigen_ms\": %.6f,\n", chroma_eigen_ms);
  std::printf("  \"chroma_gemv_scalar_ms\": %.6f,\n", chroma_scalar_ms);
  std::printf("  \"chroma_speedup_ratio\": %.6f,\n", chroma_speedup);
  std::printf("  \"max_abs_diff_chroma\": %.6e,\n", static_cast<double>(max_abs_diff_chroma));
  std::printf("  \"correctness_chroma\": \"%s\"\n", correctness_chroma_pass ? "PASS" : "FAILED");
  std::printf("}\n");

  return 0;
}
