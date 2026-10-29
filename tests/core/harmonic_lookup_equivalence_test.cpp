#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "core/harmonic.h"

namespace {

float reference_interp_at(const float* spectrum, int n_bins, int n_frames,
                          const std::vector<float>& freqs, float target, int frame,
                          float fill_value) {
  if (n_bins == 1) return target == freqs.front() ? spectrum[frame] : fill_value;
  if (target < freqs.front() || target > freqs.back()) return fill_value;
  auto it = std::lower_bound(freqs.begin(), freqs.end(), target);
  int hi = static_cast<int>(it - freqs.begin());
  if (hi <= 0) hi = 1;
  if (hi >= n_bins) hi = n_bins - 1;
  const int lo = hi - 1;
  const float denom = freqs[hi] - freqs[lo];
  if (denom <= 0.0f) return spectrum[lo * n_frames + frame];
  const float t = (target - freqs[lo]) / denom;
  const float a = spectrum[lo * n_frames + frame];
  const float b = spectrum[hi * n_frames + frame];
  return a + t * (b - a);
}

std::vector<float> reference_interp_harmonics(const std::vector<float>& spectrum, int n_bins,
                                              int n_frames, const std::vector<float>& freqs,
                                              const std::vector<float>& harmonics) {
  std::vector<float> output(harmonics.size() * static_cast<size_t>(n_bins) * n_frames, 0.0f);
  for (size_t harmonic = 0; harmonic < harmonics.size(); ++harmonic) {
    for (int bin = 0; bin < n_bins; ++bin) {
      const float target = harmonics[harmonic] * freqs[bin];
      for (int frame = 0; frame < n_frames; ++frame) {
        output[(harmonic * n_bins + bin) * n_frames + frame] =
            reference_interp_at(spectrum.data(), n_bins, n_frames, freqs, target, frame, 0.0f);
      }
    }
  }
  return output;
}

std::vector<float> reference_salience(const std::vector<float>& spectrum, int n_bins, int n_frames,
                                      const std::vector<float>& freqs,
                                      const std::vector<float>& harmonics, float fill,
                                      const std::vector<float>& weights) {
  float weight_sum = 0.0f;
  for (size_t h = 0; h < harmonics.size(); ++h) {
    weight_sum += weights.empty() ? 1.0f : weights[h];
  }
  const float inv_sum = weight_sum > 0.0f ? 1.0f / weight_sum : 0.0f;
  std::vector<float> output(static_cast<size_t>(n_bins) * n_frames, 0.0f);
  for (int bin = 0; bin < n_bins; ++bin) {
    for (int frame = 0; frame < n_frames; ++frame) {
      float acc = 0.0f;
      for (size_t h = 0; h < harmonics.size(); ++h) {
        const float weight = weights.empty() ? 1.0f : weights[h];
        const float target = harmonics[h] * freqs[bin];
        acc += weight *
               reference_interp_at(spectrum.data(), n_bins, n_frames, freqs, target, frame, fill);
      }
      output[static_cast<size_t>(bin) * n_frames + frame] = acc * inv_sum;
    }
  }
  return output;
}

void require_same_values(const std::vector<float>& actual, const std::vector<float>& expected) {
  REQUIRE(actual.size() == expected.size());
  for (size_t i = 0; i < actual.size(); ++i) {
    if (std::isnan(expected[i])) {
      REQUIRE(std::isnan(actual[i]));
    } else {
      REQUIRE(std::memcmp(&actual[i], &expected[i], sizeof(float)) == 0);
    }
  }
}

void compare_case(const std::vector<float>& spectrum, const std::vector<float>& freqs,
                  const std::vector<float>& harmonics, int n_frames, float fill,
                  const std::vector<float>& weights) {
  const int n_bins = static_cast<int>(freqs.size());
  require_same_values(sonare::interp_harmonics(spectrum, n_bins, n_frames, freqs, harmonics),
                      reference_interp_harmonics(spectrum, n_bins, n_frames, freqs, harmonics));
  require_same_values(
      sonare::salience(spectrum, n_bins, n_frames, freqs, harmonics, fill, weights),
      reference_salience(spectrum, n_bins, n_frames, freqs, harmonics, fill, weights));
}

}  // namespace

TEST_CASE("Static harmonic lookups match repeated interpolation for regular spectra",
          "[core][harmonic]") {
  const std::vector<float> freqs{100.0f, 200.0f, 300.0f, 450.0f, 700.0f};
  std::vector<float> spectrum(freqs.size() * 4);
  for (size_t i = 0; i < spectrum.size(); ++i) {
    spectrum[i] = static_cast<float>(static_cast<int>(i % 13) - 6) * 0.125f;
  }
  compare_case(spectrum, freqs, {0.5f, 1.0f, 1.5f, 2.0f, 4.0f}, 4, -0.25f,
               {1.0f, 0.0f, 0.75f, 2.0f, 0.5f});
  compare_case(spectrum, freqs, {1.0f, 2.0f, 3.0f, 5.0f}, 4, 0.0f, {});
}

TEST_CASE("Static harmonic lookups preserve singleton and duplicate-bin edge behavior",
          "[core][harmonic]") {
  compare_case({-2.0f, 3.0f, std::numeric_limits<float>::infinity()}, {440.0f},
               {1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN()}, 3, -7.0f, {1.0f, 1.0f, 1.0f});

  const std::vector<float> duplicate_freqs{100.0f, 200.0f, 200.0f, 400.0f};
  const std::vector<float> duplicate_spectrum{
      1.0f,  std::numeric_limits<float>::infinity(),
      3.0f,  4.0f,
      5.0f,  6.0f,
      7.0f,  8.0f,
      9.0f,  10.0f,
      11.0f, 12.0f,
      13.0f, 14.0f,
      15.0f, 16.0f,
  };
  compare_case(duplicate_spectrum, duplicate_freqs,
               {1.0f, 1.5f, 2.0f, std::numeric_limits<float>::quiet_NaN()}, 4, 2.5f,
               {1.0f, 0.0f, 2.0f, -1.0f});

  const std::vector<float> all_equal_freqs{200.0f, 200.0f, 200.0f};
  const std::vector<float> all_equal_spectrum{
      std::numeric_limits<float>::infinity(), 1.0f, 3.0f, 4.0f, 5.0f, 6.0f,
  };
  compare_case(all_equal_spectrum, all_equal_freqs, {1.0f, 10.0f}, 2,
               std::numeric_limits<float>::infinity(), {1.0f, 1.0f});
}
