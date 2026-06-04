/// @file cqt_test.cpp
/// @brief Tests for Constant-Q Transform.

#include "feature/cqt.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <iterator>
#include <vector>

#include "support/audio_fixtures.h"
#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinRel;

namespace {
using sonare::test::generate_sine;

/// @brief Generates a chord (multiple frequencies).
Audio generate_chord(const std::vector<float>& freqs, float duration, int sr = 22050) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);
  for (float freq : freqs) {
    for (int i = 0; i < n_samples; ++i) {
      float t = static_cast<float>(i) / sr;
      samples[i] += std::sin(2.0f * sonare::constants::kPiD * freq * t) / freqs.size();
    }
  }
  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Finds bin index for given frequency.
int find_bin_for_freq(const std::vector<float>& freqs, float target) {
  int best_bin = 0;
  float min_diff = std::abs(freqs[0] - target);
  for (size_t i = 1; i < freqs.size(); ++i) {
    float diff = std::abs(freqs[i] - target);
    if (diff < min_diff) {
      min_diff = diff;
      best_bin = static_cast<int>(i);
    }
  }
  return best_bin;
}

}  // namespace

TEST_CASE("cqt_frequencies basic", "[cqt]") {
  float fmin = 32.7f;  // C1
  int n_bins = 12;
  int bins_per_octave = 12;

  auto freqs = cqt_frequencies(fmin, n_bins, bins_per_octave);

  REQUIRE(freqs.size() == 12);
  REQUIRE_THAT(freqs[0], WithinRel(fmin, 0.001f));

  // One octave up should be 2x
  REQUIRE_THAT(freqs[12 - 1], WithinRel(fmin * std::pow(2.0f, 11.0f / 12.0f), 0.01f));
}

TEST_CASE("cqt_frequencies multiple octaves", "[cqt]") {
  float fmin = 65.4f;  // C2
  int n_bins = 84;     // 7 octaves
  int bins_per_octave = 12;

  auto freqs = cqt_frequencies(fmin, n_bins, bins_per_octave);

  REQUIRE(freqs.size() == 84);

  // Check octave relationship
  // fmin * 2^7 = 7 octaves up
  float fmax_expected = fmin * std::pow(2.0f, 83.0f / 12.0f);
  REQUIRE_THAT(freqs[83], WithinRel(fmax_expected, 0.01f));
}

TEST_CASE("CqtKernel creation", "[cqt]") {
  CqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 24;  // 2 octaves
  config.bins_per_octave = 12;

  auto kernel = CqtKernel::create(22050, config);

  REQUIRE(kernel != nullptr);
  REQUIRE(kernel->n_bins() == 24);
  REQUIRE(kernel->fft_length() > 0);
  REQUIRE(kernel->frequencies().size() == 24);
}

TEST_CASE("cqt single sine wave", "[cqt]") {
  float freq = 440.0f;  // A4
  Audio audio = generate_sine(freq, 1.0f, 22050);

  CqtConfig config;
  config.fmin = 65.4f;  // C2
  config.n_bins = 48;   // 4 octaves
  config.hop_length = 512;

  CqtResult result = cqt(audio, config);

  REQUIRE(!result.empty());
  REQUIRE(result.n_bins() == 48);
  REQUIRE(result.n_frames() > 0);

  // Find the bin corresponding to 440 Hz
  int expected_bin = find_bin_for_freq(result.frequencies(), freq);

  // Get magnitude
  const auto& mag = result.magnitude();

  // Sum energy in each bin over all frames
  std::vector<float> bin_energy(result.n_bins(), 0.0f);
  for (int k = 0; k < result.n_bins(); ++k) {
    for (int t = 0; t < result.n_frames(); ++t) {
      bin_energy[k] += mag[k * result.n_frames() + t];
    }
  }

  // Expected bin should have significant energy
  REQUIRE(bin_energy[expected_bin] > 0.0f);

  // Find max energy bin
  int max_bin = 0;
  for (int k = 1; k < result.n_bins(); ++k) {
    if (bin_energy[k] > bin_energy[max_bin]) {
      max_bin = k;
    }
  }

  // Max energy bin should be close to expected bin (within 2 bins)
  REQUIRE(std::abs(max_bin - expected_bin) <= 2);
}

TEST_CASE("cqt chord detection", "[cqt]") {
  // C major chord: C4 (261.63), E4 (329.63), G4 (392.00)
  std::vector<float> chord_freqs = {261.63f, 329.63f, 392.0f};
  Audio audio = generate_chord(chord_freqs, 1.0f, 22050);

  CqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 48;
  config.hop_length = 512;

  CqtResult result = cqt(audio, config);

  const auto& mag = result.magnitude();

  // Sum energy per bin
  std::vector<float> bin_energy(result.n_bins(), 0.0f);
  for (int k = 0; k < result.n_bins(); ++k) {
    for (int t = 0; t < result.n_frames(); ++t) {
      bin_energy[k] += mag[k * result.n_frames() + t];
    }
  }

  // Find bins for each chord note
  for (float freq : chord_freqs) {
    int expected_bin = find_bin_for_freq(result.frequencies(), freq);
    // Check that this bin has non-zero energy
    REQUIRE(bin_energy[expected_bin] > 0.0f);
  }
}

TEST_CASE("CqtResult accessors", "[cqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  CqtConfig config;
  config.fmin = 100.0f;
  config.n_bins = 24;
  config.hop_length = 256;

  CqtResult result = cqt(audio, config);

  REQUIRE(result.n_bins() == 24);
  REQUIRE(result.n_frames() > 0);
  REQUIRE(result.hop_length() == 256);
  REQUIRE(result.sample_rate() == 22050);
  REQUIRE(result.duration() > 0.0f);
  REQUIRE(!result.empty());
}

TEST_CASE("CqtResult magnitude and power", "[cqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  CqtConfig config;
  config.n_bins = 24;

  CqtResult result = cqt(audio, config);

  const auto& mag = result.magnitude();
  const auto& pwr = result.power();

  REQUIRE(mag.size() == static_cast<size_t>(result.n_bins() * result.n_frames()));
  REQUIRE(pwr.size() == mag.size());

  // Power should be magnitude squared
  for (size_t i = 0; i < mag.size(); ++i) {
    REQUIRE_THAT(pwr[i], WithinRel(mag[i] * mag[i], 0.001f));
  }
}

TEST_CASE("CqtResult to_db", "[cqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  CqtResult result = cqt(audio, CqtConfig());

  auto db = result.to_db();

  REQUIRE(db.size() == static_cast<size_t>(result.n_bins() * result.n_frames()));

  // dB values should be finite
  for (float val : db) {
    REQUIRE(std::isfinite(val));
  }
}

TEST_CASE("cqt_to_chroma", "[cqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  CqtConfig config;
  config.n_bins = 36;  // 3 octaves

  CqtResult result = cqt(audio, config);

  auto chroma = cqt_to_chroma(result, 12);

  REQUIRE(chroma.size() == static_cast<size_t>(12 * result.n_frames()));

  // Chroma values should be in [0, 1]
  for (float val : chroma) {
    REQUIRE(val >= 0.0f);
    REQUIRE(val <= 1.0f + 1e-6f);
  }
}

TEST_CASE("cqt_to_chroma preserves L-inf normalization after fold refactor", "[cqt]") {
  // cqt_to_chroma was refactored to share its bin->pitch-class accumulation with
  // a helper, but must keep its observable behavior: sum CQT magnitudes per pitch
  // class, then L-inf normalize each frame. For a single dominant tone every
  // non-silent frame must therefore peak at exactly 1.0 in the dominant class.
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  CqtConfig config;
  config.n_bins = 36;  // 3 octaves

  CqtResult result = cqt(audio, config);
  auto chroma = cqt_to_chroma(result, 12);
  const int n_frames = result.n_frames();
  REQUIRE(n_frames > 0);

  for (int t = 0; t < n_frames; ++t) {
    float max_val = 0.0f;
    for (int c = 0; c < 12; ++c) {
      max_val = std::max(max_val, chroma[static_cast<size_t>(c) * n_frames + t]);
    }
    // A frame is either silent (no energy folded) or L-inf normalized to peak 1.0.
    REQUIRE((max_val == 0.0f || std::abs(max_val - 1.0f) <= 1e-5f));
  }
}

TEST_CASE("cqt_to_chroma honours a non-C-aligned fmin", "[cqt]") {
  // fmin = A0 (27.5 Hz) means CQT bin 0 is pitch class A, not C. A 440 Hz tone
  // (A4) must still fold to chroma class A (9); the old `k % n_chroma` fold
  // ignored fmin and would have reported class 0 (C).
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  CqtConfig config;
  config.fmin = 27.5f;  // A0
  config.n_bins = 60;   // 5 octaves @ 12 bpo

  CqtResult result = cqt(audio, config);
  auto chroma = cqt_to_chroma(result, 12);
  const int n_frames = result.n_frames();
  REQUIRE(n_frames > 0);

  // Sum energy per chroma class across frames and find the dominant class.
  std::array<double, 12> energy{};
  for (int c = 0; c < 12; ++c) {
    for (int t = 0; t < n_frames; ++t) {
      energy[static_cast<size_t>(c)] += chroma[static_cast<size_t>(c) * n_frames + t];
    }
  }
  const auto peak = std::max_element(energy.begin(), energy.end());
  REQUIRE(std::distance(energy.begin(), peak) == 9);  // A
}

TEST_CASE("cqt with progress callback", "[cqt]") {
  // 0.5 s is plenty to exercise the callback path; keep this smoke test fast.
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  std::vector<float> progress_values;
  CqtResult result = cqt(audio, CqtConfig(), [&](float p) { progress_values.push_back(p); });

  REQUIRE(!progress_values.empty());
  REQUIRE(progress_values.back() >= 0.99f);  // Should reach ~100%
}

// Suppress deprecated warning for icqt test (testing deprecated function)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

TEST_CASE("icqt reconstruction", "[cqt]") {
  Audio original = generate_sine(440.0f, 0.5f, 22050);

  CqtConfig config;
  config.n_bins = 36;
  config.hop_length = 256;

  CqtResult result = cqt(original, config);
  Audio reconstructed = icqt(result, original.size());

  REQUIRE(reconstructed.sample_rate() == original.sample_rate());
  REQUIRE(!reconstructed.empty());

  double ref_energy = 0.0;
  double rec_energy = 0.0;
  for (size_t i = 0; i < std::min(original.size(), reconstructed.size()); ++i) {
    const double x = original.data()[i];
    const double y = reconstructed.data()[i];
    ref_energy += x * x;
    rec_energy += y * y;
  }
  REQUIRE(rec_energy > ref_energy * 1e-4);
  REQUIRE(rec_energy < ref_energy * 10.0);
  for (size_t i = 0; i < reconstructed.size(); ++i) {
    REQUIRE(std::isfinite(reconstructed.data()[i]));
  }
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

TEST_CASE("CQT phase correctness", "[cqt]") {
  // Generate a 440Hz sine wave
  float freq = 440.0f;
  int sr = 22050;
  float duration = 1.0f;
  Audio audio = generate_sine(freq, duration, sr);

  CqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 48;
  config.hop_length = 512;

  CqtResult result = cqt(audio, config);

  // Find the bin closest to 440Hz
  int target_bin = find_bin_for_freq(result.frequencies(), freq);

  // Verify that the phase progresses across frames. librosa's CQT (and our
  // matching implementation) uses a positive-phasor kernel exp(+jω n), which
  // means the bin phase advances by +ω·hop/sr per frame for a pure tone.
  float expected_phase_diff = 2.0f * sonare::constants::kPiD * freq * config.hop_length / sr;

  // Normalize expected_phase_diff to [-pi, pi]
  while (expected_phase_diff > sonare::constants::kPiD)
    expected_phase_diff -= 2.0f * sonare::constants::kPiD;
  while (expected_phase_diff < -sonare::constants::kPiD)
    expected_phase_diff += 2.0f * sonare::constants::kPiD;

  // Check phase progression in steady-state frames (skip edges where windowing effects dominate)
  int start_frame = result.n_frames() / 4;
  int end_frame = 3 * result.n_frames() / 4;
  int match_count = 0;
  int total_count = 0;

  for (int t = start_frame; t < end_frame - 1; ++t) {
    std::complex<float> c0 = result.at(target_bin, t);
    std::complex<float> c1 = result.at(target_bin, t + 1);

    // Skip frames with very low magnitude (unreliable phase)
    if (std::abs(c0) < 1e-6f || std::abs(c1) < 1e-6f) continue;

    float phase0 = std::arg(c0);
    float phase1 = std::arg(c1);
    float phase_diff = phase1 - phase0;

    // Normalize to [-pi, pi]
    while (phase_diff > sonare::constants::kPiD) phase_diff -= 2.0f * sonare::constants::kPiD;
    while (phase_diff < -sonare::constants::kPiD) phase_diff += 2.0f * sonare::constants::kPiD;

    // Phase difference should match expected (with tolerance for windowing effects)
    if (std::abs(phase_diff - expected_phase_diff) < 0.5f) {
      ++match_count;
    }
    ++total_count;
  }

  // At least 80% of frames should have correct phase progression
  REQUIRE(total_count > 0);
  REQUIRE(static_cast<float>(match_count) / total_count >= 0.8f);
}

TEST_CASE("cqt empty audio throws", "[cqt]") {
  Audio empty_audio;
  REQUIRE_THROWS(cqt(empty_audio, CqtConfig()));
}

TEST_CASE("CqtResult at accessor", "[cqt]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);
  CqtResult result = cqt(audio, CqtConfig());

  // Valid access
  REQUIRE_NOTHROW(result.at(0, 0));

  // Invalid access
  REQUIRE_THROWS(result.at(-1, 0));
  REQUIRE_THROWS(result.at(0, result.n_frames()));
  REQUIRE_THROWS(result.at(result.n_bins(), 0));
}

// D-21 regression: changing only `filter_scale` must invalidate the kernel
// cache. Previously the cache key omitted filter_scale, so two configurations
// that differ only in filter scaling produced identical (incorrectly cached)
// kernels and therefore identical outputs.
TEST_CASE("CQT kernel cache distinguishes filter_scale", "[cqt][regression][cache]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  CqtConfig cfg_a;
  cfg_a.fmin = 65.4f;
  cfg_a.n_bins = 36;
  cfg_a.hop_length = 256;
  cfg_a.bins_per_octave = 12;
  cfg_a.filter_scale = 1.0f;

  CqtConfig cfg_b = cfg_a;
  cfg_b.filter_scale = 2.0f;  // longer filters -> different kernel

  CqtResult res_a = cqt(audio, cfg_a);
  CqtResult res_b = cqt(audio, cfg_b);

  REQUIRE(res_a.n_bins() == res_b.n_bins());
  // filter_scale=2 widens the kernel support, which can grow n_fft and shift
  // the frame count. The relevant correctness signal is that the outputs are
  // *not* bit-identical (cache collision would force equality).
  const auto& mag_a = res_a.magnitude();
  const auto& mag_b = res_b.magnitude();

  // Compare overlapping frames bin by bin and require at least one meaningful
  // difference. Identical caches would make every value equal.
  const int n_frames = std::min(res_a.n_frames(), res_b.n_frames());
  const int n_bins = res_a.n_bins();
  double max_diff = 0.0;
  for (int k = 0; k < n_bins; ++k) {
    for (int t = 0; t < n_frames; ++t) {
      const float va = mag_a[k * res_a.n_frames() + t];
      const float vb = mag_b[k * res_b.n_frames() + t];
      max_diff = std::max(max_diff, static_cast<double>(std::abs(va - vb)));
    }
  }
  CAPTURE(max_diff);
  REQUIRE(max_diff > 1e-3);
}

// Regression: the CQT kernel cache key must include `window` so that two
// configurations differing only in the window function never alias to the same
// cached kernel. The current `cqt()` implementation hardcodes a Hann wavelet
// in `wavelet()`, so the externally observable output does not yet depend on
// `config.window`; this test exercises the cache key path with different
// windows to guard against a stale-kernel regression if the CQT kernel ever
// starts honoring `config.window` (as the VQT kernel already does).
TEST_CASE("CQT kernel cache distinguishes window type", "[cqt][regression][cache]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  CqtConfig cfg_a;
  cfg_a.fmin = 65.4f;
  cfg_a.n_bins = 36;
  cfg_a.hop_length = 256;
  cfg_a.bins_per_octave = 12;
  cfg_a.window = WindowType::Hann;

  CqtConfig cfg_b = cfg_a;
  cfg_b.window = WindowType::Hamming;  // different window -> different cache slot

  CqtResult res_a = cqt(audio, cfg_a);
  CqtResult res_b = cqt(audio, cfg_b);

  REQUIRE(res_a.n_bins() == res_b.n_bins());
  REQUIRE(res_a.n_frames() == res_b.n_frames());

  // Re-issuing the original configuration must still return the original
  // (Hann) kernel rather than a Hamming-keyed entry from the second call.
  CqtResult res_a_again = cqt(audio, cfg_a);
  const auto& mag_a = res_a.magnitude();
  const auto& mag_a_again = res_a_again.magnitude();
  REQUIRE(mag_a.size() == mag_a_again.size());
  for (size_t i = 0; i < mag_a.size(); ++i) {
    REQUIRE(mag_a[i] == mag_a_again[i]);
  }
}

// Suppress deprecated warning for icqt round-trip tests.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

// D-24 regression: with the freq_power normalization restricted to the
// positive-only half (n_fft/2 + 1) of a conjugate-form kernel, the icqt output
// was over-amplified by roughly 2x for analytic-like wavelets. After summing
// across all n_fft kernel bins, a sine round-trip should now sit close to the
// original amplitude rather than ~2x above it.
TEST_CASE("icqt round-trip amplitude matches input within ~1.5x", "[cqt][icqt][regression]") {
  const int sr = 22050;
  Audio original = generate_sine(440.0f, 0.5f, sr);

  CqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 48;
  config.hop_length = 256;

  CqtResult result = cqt(original, config);
  Audio reconstructed = icqt(result, static_cast<int>(original.size()));

  REQUIRE(reconstructed.size() == original.size());

  // Trim the leading/trailing 10% to skip transient frames where windowing /
  // OLA coverage is incomplete and the amplitude ratio is unreliable.
  const size_t margin = original.size() / 10;
  double ref_energy = 0.0;
  double rec_energy = 0.0;
  for (size_t i = margin; i < original.size() - margin; ++i) {
    const double x = original.data()[i];
    const double y = reconstructed.data()[i];
    ref_energy += x * x;
    rec_energy += y * y;
  }
  REQUIRE(ref_energy > 0.0);

  // Energy ratio (rec/ref). Under the old half-spectrum normalisation this
  // came out near 4x (amplitude ratio ~2x squared). With the full-spectrum
  // fix it must stay within 1.5x amplitude (i.e. 2.25x energy) of the input.
  const double energy_ratio = rec_energy / ref_energy;
  CAPTURE(energy_ratio);
  REQUIRE(energy_ratio > 1.0 / 2.25);
  REQUIRE(energy_ratio < 2.25);
}

// D-9 / D-24 round-trip quality: reconstruction error must stay better than
// -5 dB relative to the input on a clean tone. CQT inversion is inherently
// lossy (no exact inverse exists) so this is a coarse sanity bar — the
// pre-fix state with broken OLA / half-spectrum normalisation produced an
// energy ratio near 4x, which corresponds to SNR < 0 dB. Anything above 0 dB
// passes "the reconstruction looks like the input", and we lock in 5 dB so a
// future regression in either fix is caught immediately.
TEST_CASE("icqt round-trip SNR is positive for a clean tone", "[cqt][icqt][regression]") {
  const int sr = 22050;
  Audio original = generate_sine(440.0f, 0.5f, sr);

  CqtConfig config;
  config.fmin = 65.4f;
  config.n_bins = 48;
  config.hop_length = 256;

  CqtResult result = cqt(original, config);
  Audio reconstructed = icqt(result, static_cast<int>(original.size()));

  // Skip the first/last hop_length samples where OLA coverage is incomplete.
  const size_t margin = static_cast<size_t>(config.hop_length);
  REQUIRE(original.size() > 2 * margin);
  double ref_energy = 0.0;
  double err_energy = 0.0;
  for (size_t i = margin; i < original.size() - margin; ++i) {
    const double x = original.data()[i];
    const double y = reconstructed.data()[i];
    const double e = y - x;
    ref_energy += x * x;
    err_energy += e * e;
  }
  REQUIRE(ref_energy > 0.0);
  const double snr_db = 10.0 * std::log10(ref_energy / std::max(err_energy, 1e-20));
  CAPTURE(snr_db);
  REQUIRE(snr_db > 5.0);
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

// Parity regression: hybrid_cqt joins a full-CQT low half (librosa scale=True,
// per-bin 1/sqrt(length)) with a pseudo-CQT high half. Before the fix the
// pseudo half was a row-stochastic Gaussian average of |STFT| with NO length
// scaling, so the two halves were on different amplitude conventions and
// concatenation left a magnitude *step* at the split bin. After matching the
// pseudo bins to the scale=True convention (multiplying by 1/sqrt(length)), the
// magnitude profile must be continuous across the split for a broadband input.
//
// The assertion is intentionally coarse-but-deterministic: on a flat-ish
// multi-tone signal that spans the split, the average bin energy in a window
// just above the split must not differ from the window just below by more than
// a generous factor. A raw convention mismatch produces a step of one to two
// orders of magnitude (1/sqrt(length) at low-hundreds-of-samples lengths),
// which this bound catches while still tolerating the genuine spectral tilt of
// the test signal and pseudo-vs-full modelling error.
TEST_CASE("hybrid_cqt has no amplitude step across the split bin", "[cqt][hybrid][regression]") {
  const int sr = 22050;

  // Broadband, roughly flat input: equal-amplitude tones on a dense semitone
  // grid spanning the bins around the split so both halves see real energy.
  std::vector<float> tone_freqs;
  for (float f = 130.0f; f < 4000.0f; f *= std::pow(2.0f, 1.0f / 12.0f)) {
    tone_freqs.push_back(f);
  }
  Audio audio = generate_chord(tone_freqs, 1.0f, sr);

  CqtConfig config;
  config.fmin = 65.4f;  // C2
  config.n_bins = 84;   // 7 octaves
  config.bins_per_octave = 12;
  config.hop_length = 512;

  CqtResult result = hybrid_cqt(audio, config);
  REQUIRE(!result.empty());
  REQUIRE(result.n_bins() == 84);
  REQUIRE(result.n_frames() > 0);

  const int n_bins = result.n_bins();
  const int n_frames = result.n_frames();
  const auto& mag = result.magnitude();

  // Time-averaged magnitude per bin.
  std::vector<double> bin_mean(n_bins, 0.0);
  for (int k = 0; k < n_bins; ++k) {
    double acc = 0.0;
    for (int t = 0; t < n_frames; ++t) {
      acc += mag[k * n_frames + t];
    }
    bin_mean[k] = acc / n_frames;
  }

  // Recompute the split bin with the same rule used by hybrid_cqt(): the first
  // bin whose CQT filter length <= max(256, 2*hop). Below this is full CQT;
  // at/above it is the pseudo half.
  const float Q = 1.0f / (std::pow(2.0f, 1.0f / config.bins_per_octave) - 1.0f);
  auto freqs = cqt_frequencies(config.fmin, config.n_bins, config.bins_per_octave);
  const int short_threshold = std::max(256, 2 * config.hop_length);
  int n_split = config.n_bins;
  for (int k = 0; k < config.n_bins; ++k) {
    const int len = static_cast<int>(std::ceil(Q * sr / std::max(freqs[k], 1.0f)));
    if (len <= short_threshold) {
      n_split = k;
      break;
    }
  }

  // The split must fall in the interior so both halves are exercised.
  REQUIRE(n_split > 4);
  REQUIRE(n_split < n_bins - 4);

  // Compare a small window of bins just below vs. just above the split. Use the
  // mean magnitude over the window to suppress per-tone ripple.
  const int win = 3;
  double below = 0.0;
  double above = 0.0;
  for (int i = 1; i <= win; ++i) {
    below += bin_mean[n_split - i];
    above += bin_mean[n_split + i - 1];
  }
  below /= win;
  above /= win;

  REQUIRE(below > 0.0);
  REQUIRE(above > 0.0);

  const double ratio = above / below;
  CAPTURE(n_split, below, above, ratio);

  // No order-of-magnitude jump at the seam. A raw convention mismatch left the
  // pseudo half larger than the full half by ~sqrt(length) (length ~ a few
  // hundred samples near the split), i.e. a ratio well above 5x; the matched
  // scaling keeps the seam within a small factor either way.
  REQUIRE(ratio < 5.0);
  REQUIRE(ratio > 1.0 / 5.0);
}
