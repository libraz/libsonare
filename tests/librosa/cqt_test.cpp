/// @file cqt_test.cpp
/// @brief Reference compatibility tests for Constant-Q Transform.
/// @details Reference values from: tests/librosa/reference/cqt.json, icqt.json and
///          cqt_family.json

#include "feature/cqt.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "feature/vqt.h"
#include "util/json_reader.h"
#include "util/math_utils.h"

using namespace sonare;
using namespace sonare::constants;
using namespace sonare::test;
using Catch::Matchers::WithinRel;

TEST_CASE("CQT reference compatibility", "[.][slow][cqt][reference]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/cqt.json");
  const auto& data = json["data"];

  int sr = data["sr"].as_int();
  float ref_fmin = data["fmin"].as_float();
  int ref_n_bins = data["n_bins"].as_int();
  int ref_bins_per_octave = data["bins_per_octave"].as_int();
  int hop_length = data["hop_length"].as_int();

  // Create 440Hz tone, 1.0s
  size_t n_samples = static_cast<size_t>(sr);
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(kTwoPi * 440.0f * t);
  }
  Audio audio = Audio::from_buffer(samples.data(), n_samples, sr);

  SECTION("shape and statistics") {
    CqtConfig config;
    config.fmin = ref_fmin;
    config.n_bins = ref_n_bins;
    config.bins_per_octave = ref_bins_per_octave;
    config.hop_length = hop_length;

    CqtResult result = cqt(audio, config);

    // Verify frequency bins match
    const auto& shape = data["shape"].as_array();
    REQUIRE(result.n_bins() == shape[0].as_int());

    // With center padding, frame counts should match the reference
    int ref_n_frames = shape[1].as_int();
    int our_n_frames = result.n_frames();
    CAPTURE(our_n_frames, ref_n_frames);
    REQUIRE(our_n_frames == ref_n_frames);

    const auto& mag = result.magnitude();
    float mag_sum = 0.0f;
    float mag_max = 0.0f;
    for (float v : mag) {
      mag_sum += v;
      mag_max = std::max(mag_max, v);
    }

    // librosa runs CQT iteratively per octave with downsampling + sparsification;
    // we use a single-pass full-rate FFT instead. The residual gap (~0.85%) is
    // dominated by the long low-frequency filters at native sr. The sparse
    // kernel's 1% cumulative-L1 pruning shifts the peak by another ~1.3e-4;
    // only the spread of energy across many low-frequency bins differs slightly.
    REQUIRE_THAT(static_cast<double>(mag_sum),
                 WithinRel(static_cast<double>(data["magnitude_sum"].as_float()), 9e-3));
    REQUIRE_THAT(static_cast<double>(mag_max),
                 WithinRel(static_cast<double>(data["magnitude_max"].as_float()), 2e-4));
  }

  SECTION("stored frames") {
    CqtConfig config;
    config.fmin = ref_fmin;
    config.n_bins = ref_n_bins;
    config.bins_per_octave = ref_bins_per_octave;
    config.hop_length = hop_length;

    CqtResult result = cqt(audio, config);

    const auto& mag = result.magnitude();
    const auto& ref_frames = data["frames"].as_array();
    int stored_frames = data["n_stored_frames"].as_int();
    int n_frames = result.n_frames();

    REQUIRE(static_cast<int>(ref_frames.size()) == result.n_bins() * stored_frames);

    float mean_abs_diff = 0.0f;
    float max_abs_diff = 0.0f;
    size_t ref_idx = 0;
    for (int bin = 0; bin < result.n_bins(); ++bin) {
      for (int frame = 0; frame < stored_frames; ++frame) {
        float diff = std::abs(mag[bin * n_frames + frame] - ref_frames[ref_idx++].as_float());
        mean_abs_diff += diff;
        max_abs_diff = std::max(max_abs_diff, diff);
      }
    }
    mean_abs_diff /= static_cast<float>(ref_frames.size());

    REQUIRE(mean_abs_diff < 1.5e-2f);
    REQUIRE(max_abs_diff < 0.15f);
  }

  SECTION("frequencies") {
    const auto& ref_freqs = data["frequencies"].as_array();
    auto freqs = cqt_frequencies(ref_fmin, ref_n_bins, ref_bins_per_octave);
    REQUIRE(freqs.size() == ref_freqs.size());

    for (size_t i = 0; i < freqs.size(); ++i) {
      REQUIRE_THAT(static_cast<double>(freqs[i]),
                   WithinRel(static_cast<double>(ref_freqs[i].as_float()), 1e-5));
    }
  }

  SECTION("energy at 440Hz") {
    CqtConfig config;
    config.fmin = ref_fmin;
    config.n_bins = ref_n_bins;
    config.bins_per_octave = ref_bins_per_octave;
    config.hop_length = hop_length;

    CqtResult result = cqt(audio, config);

    // Verify the 440Hz bin has the highest energy (sum across frames)
    const auto& freqs = result.frequencies();
    const auto& mag = result.magnitude();
    int n_frames = result.n_frames();
    int n_bins = result.n_bins();

    std::vector<float> bin_energy(n_bins, 0.0f);
    for (int b = 0; b < n_bins; ++b) {
      for (int f = 0; f < n_frames; ++f) {
        bin_energy[b] += mag[b * n_frames + f];
      }
    }

    // Find the bin with highest energy
    int max_energy_bin = 0;
    for (int b = 1; b < n_bins; ++b) {
      if (bin_energy[b] > bin_energy[max_energy_bin]) {
        max_energy_bin = b;
      }
    }

    // The bin with highest energy should be at or very near 440Hz
    float max_energy_freq = freqs[max_energy_bin];
    REQUIRE(std::abs(max_energy_freq - 440.0f) < 5.0f);
  }
}

// Suppress deprecated warning for icqt reference compatibility test.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

TEST_CASE("iCQT reference compatibility", "[cqt][icqt][reference]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/icqt.json");
  const auto& data = json["data"];

  const int sr = data["sr"].as_int();
  const int n_bins = data["n_bins"].as_int();
  const int hop_length = data["hop_length"].as_int();
  const int length = data["length"].as_int();
  const auto& shape = data["shape"].as_array();
  const int n_frames = shape[1].as_int();

  std::vector<float> frequencies;
  for (const auto& v : data["frequencies"].as_array()) {
    frequencies.push_back(v.as_float());
  }

  std::vector<std::complex<float>> cqt_data(static_cast<size_t>(n_bins) * n_frames);
  const auto& real = data["cqt_real"].as_array();
  const auto& imag = data["cqt_imag"].as_array();
  REQUIRE(real.size() == cqt_data.size());
  REQUIRE(imag.size() == cqt_data.size());
  for (size_t i = 0; i < cqt_data.size(); ++i) {
    cqt_data[i] = std::complex<float>(real[i].as_float(), imag[i].as_float());
  }

  CqtResult cqt_result(std::move(cqt_data), n_bins, n_frames, std::move(frequencies), hop_length,
                       sr);
  Audio reconstructed = icqt(cqt_result, length);

  const auto& ref = data["reconstruction"].as_array();
  REQUIRE(reconstructed.size() == static_cast<size_t>(length));
  REQUIRE(ref.size() == reconstructed.size());

  double ref_energy = 0.0;
  double err_energy = 0.0;
  double max_abs_diff = 0.0;
  for (size_t i = 0; i < reconstructed.size(); ++i) {
    const double expected = ref[i].as_float();
    const double actual = reconstructed.data()[i];
    const double diff = actual - expected;
    ref_energy += expected * expected;
    err_energy += diff * diff;
    max_abs_diff = std::max(max_abs_diff, std::abs(diff));
  }

  const double rel_rmse = std::sqrt(err_energy / ref_energy);
  CAPTURE(rel_rmse, max_abs_diff);
  // rel_rmse is the primary accuracy bar (1% energy error). max_abs_diff is a
  // looser spike guard: the flat-weight overlap-add normalization leaves larger
  // localized error in the first/last hop where frame coverage is incomplete.
  REQUIRE(rel_rmse < 0.01);
  REQUIRE(max_abs_diff < 0.15);
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace {

/// Mean over interior frames of @p mag at @p bin, for a row-major [n_bins x n_frames] grid.
double interior_mean(const std::vector<float>& mag, int bin, int n_frames) {
  double sum = 0.0;
  int count = 0;
  for (int t = 8; t < n_frames - 8; ++t) {
    sum += mag[static_cast<size_t>(bin) * n_frames + t];
    ++count;
  }
  return count > 0 ? sum / count : 0.0;
}

/// Relative L2 error of @p got against @p want over interior frames.
double interior_rel_l2(const std::vector<float>& got, const std::vector<double>& want, int n_bins,
                       int n_frames) {
  double num = 0.0, den = 0.0;
  for (int k = 0; k < n_bins; ++k) {
    for (int t = 8; t < n_frames - 8; ++t) {
      const size_t i = static_cast<size_t>(k) * n_frames + t;
      num += (got[i] - want[i]) * (got[i] - want[i]);
      den += want[i] * want[i];
    }
  }
  return std::sqrt(num / std::max(den, 1e-30));
}

}  // namespace

TEST_CASE("pseudo_cqt, hybrid_cqt and vqt reference compatibility", "[cqt][reference]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/cqt_family.json");
  const auto& data = json["data"];
  const int sr = data["sr"].as_int();
  const int n_bins = data["n_bins"].as_int();
  const double low_hz = data["low_hz"].as_number();
  const double high_hz = data["high_hz"].as_number();

  std::vector<float> samples(static_cast<size_t>(sr));
  for (size_t i = 0; i < samples.size(); ++i) {
    const double t = static_cast<double>(i) / sr;
    samples[i] = static_cast<float>(0.5 * std::sin(kTwoPiD * low_hz * t) +
                                    0.5 * std::sin(kTwoPiD * high_hz * t));
  }
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), sr);
  CqtConfig config;
  config.fmin = data["fmin"].as_float();
  config.n_bins = n_bins;
  config.bins_per_octave = data["bins_per_octave"].as_int();
  config.hop_length = data["hop_length"].as_int();
  VqtConfig vconfig;
  vconfig.fmin = config.fmin;
  vconfig.n_bins = config.n_bins;
  vconfig.bins_per_octave = config.bins_per_octave;
  vconfig.hop_length = config.hop_length;

  const auto check = [&](const char* name, const CqtResult& result) {
    const auto& ref = data[name].as_array();
    std::vector<double> want(ref.size());
    for (size_t i = 0; i < ref.size(); ++i) want[i] = ref[i].as_number();
    const int n_frames = result.n_frames();
    REQUIRE(result.n_bins() == n_bins);
    REQUIRE(static_cast<size_t>(n_bins) * n_frames == want.size());
    const std::vector<float>& got = result.magnitude();
    const double rel_l2 = interior_rel_l2(got, want, n_bins, n_frames);
    std::vector<float> want_f(want.begin(), want.end());
    for (const int bin : {33, 69}) {
      const double ratio_db = 20.0 * std::log10(interior_mean(got, bin, n_frames) /
                                                interior_mean(want_f, bin, n_frames));
      INFO(name << " bin " << bin << " level vs librosa " << ratio_db << " dB, rel L2 " << rel_l2);
      CHECK(std::fabs(ratio_db) < 0.5);
    }
    INFO(name << " rel L2 " << rel_l2);
    CHECK(rel_l2 < 0.1);
  };
  check("pseudo_cqt", pseudo_cqt(audio, config));
  check("hybrid_cqt", hybrid_cqt(audio, config));
  check("vqt", vqt(audio, vconfig));
}
