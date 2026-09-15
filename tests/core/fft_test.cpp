/// @file fft_test.cpp
/// @brief Tests for FFT wrapper.

#include "core/fft.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {
using sonare::constants::kPi;
using sonare::constants::kTwoPi;
}  // namespace

TEST_CASE("FFT forward / inverse roundtrip", "[fft]") {
  constexpr int n_fft = 64;
  FFT fft(n_fft);

  // Test signal: sine wave at bin 4
  std::vector<float> input(n_fft);
  for (int i = 0; i < n_fft; ++i) {
    input[i] = std::sin(kTwoPi * 4 * i / n_fft);
  }

  // Forward FFT
  std::vector<std::complex<float>> spectrum(fft.n_bins());
  fft.forward(input.data(), spectrum.data());

  // Expect peak at bin 4
  float max_mag = 0;
  int max_bin = 0;
  for (int i = 0; i < fft.n_bins(); ++i) {
    float mag = std::abs(spectrum[i]);
    if (mag > max_mag) {
      max_mag = mag;
      max_bin = i;
    }
  }
  REQUIRE(max_bin == 4);

  // Inverse FFT
  std::vector<float> output(n_fft);
  fft.inverse(spectrum.data(), output.data());

  // Output should match input
  for (int i = 0; i < n_fft; ++i) {
    REQUIRE_THAT(output[i], WithinAbs(input[i], 1e-5f));
  }
}

TEST_CASE("FFT DC component", "[fft]") {
  constexpr int n_fft = 8;
  FFT fft(n_fft);

  // DC signal (all 1.0)
  std::vector<float> input(n_fft, 1.0f);
  std::vector<std::complex<float>> spectrum(fft.n_bins());

  fft.forward(input.data(), spectrum.data());

  // Bin 0 (DC) should have all energy
  REQUIRE_THAT(std::abs(spectrum[0]), WithinAbs(8.0f, 1e-5f));
  for (int i = 1; i < fft.n_bins(); ++i) {
    REQUIRE_THAT(std::abs(spectrum[i]), WithinAbs(0.0f, 1e-5f));
  }
}

TEST_CASE("FFT non-power-of-2 sizes", "[fft]") {
  // KissFFT supports non-power-of-2 sizes, but they may be slower
  SECTION("n_fft = 100") {
    FFT fft(100);
    REQUIRE(fft.n_fft() == 100);
    REQUIRE(fft.n_bins() == 51);  // 100/2 + 1

    std::vector<float> input(100, 0.0f);
    input[0] = 1.0f;  // Impulse
    std::vector<std::complex<float>> spectrum(51);
    fft.forward(input.data(), spectrum.data());

    // Impulse has flat spectrum (all bins equal magnitude)
    float expected_mag = 1.0f;
    for (int i = 0; i < 51; ++i) {
      REQUIRE_THAT(std::abs(spectrum[i]), WithinAbs(expected_mag, 1e-5f));
    }
  }

  SECTION("n_fft = 48") {
    FFT fft(48);
    REQUIRE(fft.n_fft() == 48);
    REQUIRE(fft.n_bins() == 25);

    // Roundtrip test
    std::vector<float> input(48);
    for (int i = 0; i < 48; ++i) {
      input[i] = std::sin(kTwoPi * 3 * i / 48);
    }

    std::vector<std::complex<float>> spectrum(25);
    fft.forward(input.data(), spectrum.data());

    std::vector<float> output(48);
    fft.inverse(spectrum.data(), output.data());

    for (int i = 0; i < 48; ++i) {
      REQUIRE_THAT(output[i], WithinAbs(input[i], 1e-5f));
    }
  }
}

TEST_CASE("FFT phase accuracy", "[fft]") {
  constexpr int n_fft = 64;
  FFT fft(n_fft);

  SECTION("cosine wave has zero phase at peak bin") {
    // Cosine starts at maximum, so phase at peak bin should be 0
    std::vector<float> input(n_fft);
    for (int i = 0; i < n_fft; ++i) {
      input[i] = std::cos(kTwoPi * 8 * i / n_fft);
    }

    std::vector<std::complex<float>> spectrum(fft.n_bins());
    fft.forward(input.data(), spectrum.data());

    float phase_at_bin8 = std::arg(spectrum[8]);
    REQUIRE_THAT(phase_at_bin8, WithinAbs(0.0f, 1e-5f));
  }

  SECTION("sine wave has -pi/2 phase at peak bin") {
    // Sine starts at zero going up, so phase at peak bin should be -pi/2
    std::vector<float> input(n_fft);
    for (int i = 0; i < n_fft; ++i) {
      input[i] = std::sin(kTwoPi * 8 * i / n_fft);
    }

    std::vector<std::complex<float>> spectrum(fft.n_bins());
    fft.forward(input.data(), spectrum.data());

    float phase_at_bin8 = std::arg(spectrum[8]);
    REQUIRE_THAT(phase_at_bin8, WithinAbs(-kPi / 2.0f, 1e-5f));
  }

  SECTION("shifted cosine has correct phase") {
    // Cosine with quarter-wave shift should have phase = -pi/2
    std::vector<float> input(n_fft);
    float shift = kPi / 2.0f;  // Quarter period shift
    for (int i = 0; i < n_fft; ++i) {
      input[i] = std::cos(kTwoPi * 4 * i / n_fft - shift);
    }

    std::vector<std::complex<float>> spectrum(fft.n_bins());
    fft.forward(input.data(), spectrum.data());

    float phase_at_bin4 = std::arg(spectrum[4]);
    REQUIRE_THAT(phase_at_bin4, WithinAbs(-shift, 1e-4f));
  }
}

TEST_CASE("FFT energy preservation (Parseval's theorem)", "[fft]") {
  constexpr int n_fft = 128;
  FFT fft(n_fft);

  // Random-ish signal
  std::vector<float> input(n_fft);
  for (int i = 0; i < n_fft; ++i) {
    input[i] = std::sin(kTwoPi * 3 * i / n_fft) + 0.5f * std::cos(kTwoPi * 7 * i / n_fft) +
               0.3f * std::sin(kTwoPi * 15 * i / n_fft);
  }

  // Time domain energy
  float time_energy = 0.0f;
  for (int i = 0; i < n_fft; ++i) {
    time_energy += input[i] * input[i];
  }

  // Frequency domain energy
  std::vector<std::complex<float>> spectrum(fft.n_bins());
  fft.forward(input.data(), spectrum.data());

  float freq_energy = 0.0f;
  // DC and Nyquist bins count once, others count twice (due to symmetry)
  freq_energy += std::norm(spectrum[0]);                 // DC
  freq_energy += std::norm(spectrum[fft.n_bins() - 1]);  // Nyquist
  for (int i = 1; i < fft.n_bins() - 1; ++i) {
    freq_energy += 2.0f * std::norm(spectrum[i]);
  }
  freq_energy /= static_cast<float>(n_fft);

  REQUIRE_THAT(freq_energy, WithinAbs(time_energy, 1e-4f));
}

TEST_CASE("FFT multiple frequencies detection", "[fft]") {
  constexpr int n_fft = 256;
  FFT fft(n_fft);

  // Composite signal with frequencies at bins 10, 25, and 50
  std::vector<float> input(n_fft);
  for (int i = 0; i < n_fft; ++i) {
    input[i] = 1.0f * std::sin(kTwoPi * 10 * i / n_fft) + 0.5f * std::sin(kTwoPi * 25 * i / n_fft) +
               0.25f * std::sin(kTwoPi * 50 * i / n_fft);
  }

  std::vector<std::complex<float>> spectrum(fft.n_bins());
  fft.forward(input.data(), spectrum.data());

  // Check magnitudes at expected bins (sine has amplitude/2 * n_fft)
  float mag10 = std::abs(spectrum[10]);
  float mag25 = std::abs(spectrum[25]);
  float mag50 = std::abs(spectrum[50]);

  // Relative magnitudes should be 1.0 : 0.5 : 0.25
  REQUIRE_THAT(mag25 / mag10, WithinAbs(0.5f, 0.01f));
  REQUIRE_THAT(mag50 / mag10, WithinAbs(0.25f, 0.01f));

  // Other bins should be near zero
  for (int i = 0; i < fft.n_bins(); ++i) {
    if (i != 10 && i != 25 && i != 50) {
      REQUIRE(std::abs(spectrum[i]) < mag50 * 0.1f);
    }
  }
}

TEST_CASE("FFT input validation", "[fft]") {
  SECTION("FFT construction rejects non-positive and odd sizes as invalid parameters") {
    for (int n_fft : {0, 1, 3, 513}) {
      try {
        FFT fft(n_fft);
        FAIL("invalid FFT size was accepted");
      } catch (const SonareException& error) {
        REQUIRE(error.code() == ErrorCode::InvalidParameter);
      }
    }
  }

  SECTION("FFT forward with null input") {
    FFT fft(1024);
    std::vector<std::complex<float>> output(513);
    REQUIRE_THROWS_AS(fft.forward(nullptr, output.data()), SonareException);
  }

  SECTION("FFT forward with null output") {
    FFT fft(1024);
    std::vector<float> input(1024, 0.0f);
    REQUIRE_THROWS_AS(fft.forward(input.data(), nullptr), SonareException);
  }

  SECTION("FFT inverse with null input") {
    FFT fft(1024);
    std::vector<float> output(1024);
    REQUIRE_THROWS_AS(fft.inverse(nullptr, output.data()), SonareException);
  }

  SECTION("FFT inverse with null output") {
    FFT fft(1024);
    std::vector<std::complex<float>> input(513);
    REQUIRE_THROWS_AS(fft.inverse(input.data(), nullptr), SonareException);
  }

  SECTION("FFT forward_complex with null input") {
    FFT fft(1024);
    std::vector<std::complex<float>> output(1024);
    REQUIRE_THROWS_AS(fft.forward_complex(nullptr, output.data()), SonareException);
    try {
      fft.forward_complex(nullptr, output.data());
    } catch (const SonareException& e) {
      REQUIRE(e.code() == ErrorCode::InvalidParameter);
    }
  }

  SECTION("FFT forward_complex with null output") {
    FFT fft(1024);
    std::vector<std::complex<float>> input(1024);
    REQUIRE_THROWS_AS(fft.forward_complex(input.data(), nullptr), SonareException);
    try {
      fft.forward_complex(input.data(), nullptr);
    } catch (const SonareException& e) {
      REQUIRE(e.code() == ErrorCode::InvalidParameter);
    }
  }
}

TEST_CASE("FFT forward_complex builds its backend on first use", "[fft]") {
  // The complex transform's setup and buffers are created on the first forward_complex()
  // call rather than in the constructor. The null-pointer guards above return before that
  // point, so they cannot show the lazily built state is correct. These two sizes straddle
  // the backend split: one is served by the SIMD backend, the other falls back.
  for (int n : {64, 12}) {
    CAPTURE(n);

    std::vector<float> real_input(static_cast<size_t>(n));
    std::vector<std::complex<float>> input(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const float value =
          std::sin(kTwoPi * 3.0f * static_cast<float>(i) / static_cast<float>(n)) +
          0.25f * std::cos(kTwoPi * 5.0f * static_cast<float>(i) / static_cast<float>(n));
      real_input[static_cast<size_t>(i)] = value;
      input[static_cast<size_t>(i)] = {value, 0.0f};
    }

    // An instance whose very first transform is the complex one.
    FFT complex_first(n);
    std::vector<std::complex<float>> from_complex(static_cast<size_t>(n));
    complex_first.forward_complex(input.data(), from_complex.data());

    // The real transform of the same signal agrees on the one-sided bins, whatever the
    // sign convention is -- so this pins the lazily built complex path without hard-coding
    // a reference DFT.
    FFT real_only(n);
    std::vector<std::complex<float>> from_real(static_cast<size_t>(n / 2 + 1));
    real_only.forward(real_input.data(), from_real.data());
    for (int k = 0; k <= n / 2; ++k) {
      CAPTURE(k);
      REQUIRE_THAT(from_complex[static_cast<size_t>(k)].real(),
                   WithinAbs(from_real[static_cast<size_t>(k)].real(), 1e-3f));
      REQUIRE_THAT(from_complex[static_cast<size_t>(k)].imag(),
                   WithinAbs(from_real[static_cast<size_t>(k)].imag(), 1e-3f));
    }

    // Running a real transform first must not change what the complex one returns, and a
    // repeat call must reuse the state instead of rebuilding it.
    FFT real_first(n);
    std::vector<std::complex<float>> warmup(static_cast<size_t>(n / 2 + 1));
    real_first.forward(real_input.data(), warmup.data());
    std::vector<std::complex<float>> after_real(static_cast<size_t>(n));
    real_first.forward_complex(input.data(), after_real.data());
    std::vector<std::complex<float>> repeated(static_cast<size_t>(n));
    real_first.forward_complex(input.data(), repeated.data());
    for (int k = 0; k < n; ++k) {
      CAPTURE(k);
      REQUIRE_THAT(after_real[static_cast<size_t>(k)].real(),
                   WithinAbs(from_complex[static_cast<size_t>(k)].real(), 1e-4f));
      REQUIRE_THAT(after_real[static_cast<size_t>(k)].imag(),
                   WithinAbs(from_complex[static_cast<size_t>(k)].imag(), 1e-4f));
      REQUIRE(repeated[static_cast<size_t>(k)] == after_real[static_cast<size_t>(k)]);
    }
  }
}

TEST_CASE("FFT real transforms build their backend on first use", "[fft]") {
  // The real setup, its scratch buffers and the two KissFFT real configs are built on the
  // first forward() / inverse() call rather than in the constructor, so what an instance
  // holds -- and at which addresses -- depends on which transforms it has already run.
  // The output must not depend on any of that, which is why these comparisons are exact:
  // a rounding-tolerant assertion cannot see the few-ulp shift a changed traversal or a
  // changed SIMD path would produce. The two sizes straddle the backend split as above.
  for (int n : {64, 12}) {
    CAPTURE(n);
    const size_t bins = static_cast<size_t>(n / 2 + 1);
    const size_t spectrum_bytes = bins * sizeof(std::complex<float>);
    const size_t signal_bytes = static_cast<size_t>(n) * sizeof(float);

    std::vector<float> signal(static_cast<size_t>(n));
    std::vector<std::complex<float>> complex_signal(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(n);
      const float value = std::sin(kTwoPi * 3.0f * t) + 0.25f * std::cos(kTwoPi * 5.0f * t);
      signal[static_cast<size_t>(i)] = value;
      complex_signal[static_cast<size_t>(i)] = {value, 0.5f * std::sin(kTwoPi * 7.0f * t)};
    }
    std::vector<std::complex<float>> spectrum(bins);
    for (size_t k = 0; k < bins; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(bins);
      spectrum[k] = {std::cos(kTwoPi * 2.0f * t), std::sin(kTwoPi * 2.0f * t)};
    }

    // References, each taken from an instance that runs one transform and nothing else.
    // The inverse-only instance is also the case that crashes outright if the real state
    // is built from forward() alone.
    FFT forward_only(n);
    std::vector<std::complex<float>> forward_ref(bins);
    forward_only.forward(signal.data(), forward_ref.data());

    FFT inverse_only(n);
    std::vector<float> inverse_ref(static_cast<size_t>(n));
    inverse_only.inverse(spectrum.data(), inverse_ref.data());

    // Every other first-use order has to reproduce those bytes.
    FFT forward_after_inverse(n);
    std::vector<float> time_scratch(static_cast<size_t>(n));
    forward_after_inverse.inverse(spectrum.data(), time_scratch.data());
    std::vector<std::complex<float>> forward_late(bins);
    forward_after_inverse.forward(signal.data(), forward_late.data());
    REQUIRE(std::memcmp(forward_late.data(), forward_ref.data(), spectrum_bytes) == 0);

    FFT forward_after_complex(n);
    std::vector<std::complex<float>> complex_scratch(static_cast<size_t>(n));
    forward_after_complex.forward_complex(complex_signal.data(), complex_scratch.data());
    std::vector<std::complex<float>> forward_after_cplx(bins);
    forward_after_complex.forward(signal.data(), forward_after_cplx.data());
    REQUIRE(std::memcmp(forward_after_cplx.data(), forward_ref.data(), spectrum_bytes) == 0);

    FFT inverse_after_forward(n);
    std::vector<std::complex<float>> spectrum_scratch(bins);
    inverse_after_forward.forward(signal.data(), spectrum_scratch.data());
    std::vector<float> inverse_late(static_cast<size_t>(n));
    inverse_after_forward.inverse(spectrum.data(), inverse_late.data());
    REQUIRE(std::memcmp(inverse_late.data(), inverse_ref.data(), signal_bytes) == 0);

    FFT inverse_after_complex(n);
    inverse_after_complex.forward_complex(complex_signal.data(), complex_scratch.data());
    std::vector<float> inverse_after_cplx(static_cast<size_t>(n));
    inverse_after_complex.inverse(spectrum.data(), inverse_after_cplx.data());
    REQUIRE(std::memcmp(inverse_after_cplx.data(), inverse_ref.data(), signal_bytes) == 0);

    // A repeat call reuses the state instead of rebuilding it.
    std::vector<std::complex<float>> forward_repeat(bins);
    forward_only.forward(signal.data(), forward_repeat.data());
    REQUIRE(std::memcmp(forward_repeat.data(), forward_ref.data(), spectrum_bytes) == 0);
    std::vector<float> inverse_repeat(static_cast<size_t>(n));
    inverse_only.inverse(spectrum.data(), inverse_repeat.data());
    REQUIRE(std::memcmp(inverse_repeat.data(), inverse_ref.data(), signal_bytes) == 0);

    // Non-vacuity: the same comparison over a perturbed input fails, so the equalities
    // above are a statement about the output rather than about the comparison.
    std::vector<float> perturbed(signal);
    perturbed[0] += 1.0f;
    FFT perturbed_fft(n);
    std::vector<std::complex<float>> forward_perturbed(bins);
    perturbed_fft.forward(perturbed.data(), forward_perturbed.data());
    REQUIRE(std::memcmp(forward_perturbed.data(), forward_ref.data(), spectrum_bytes) != 0);
  }
}

TEST_CASE("FFT::prepare leaves the first transform allocation-free", "[fft][rt]") {
  // A realtime owner calls prepare() so that its first transform neither allocates on the
  // audio thread nor, in a noexcept caller, throws there. The shared AllocationGuard cannot
  // measure this one: both backends take their memory through malloc (pffft_aligned_malloc,
  // kiss_fftr_alloc) rather than operator new, so the malloc zone's in-use bytes are the
  // instrument, and the case is macOS-only for that reason. CI runs the C++ suite on
  // ubuntu-latest AND macos-latest, so the macOS row is what executes this case.
#if !defined(__APPLE__)
  SKIP("allocation is measured through the macOS malloc zone statistics");
#else
  const auto in_use = []() {
    malloc_statistics_t stats;
    malloc_zone_statistics(malloc_default_zone(), &stats);
    return static_cast<long long>(stats.size_in_use);
  };

  // The sizes straddle the backend split as above, and are large enough that every
  // allocation under test lands in the measured zone rather than the small-object one.
  for (int n : {2048, 2040}) {
    CAPTURE(n);
    const size_t bins = static_cast<size_t>(n / 2 + 1);
    std::vector<float> signal(static_cast<size_t>(n), 0.25f);
    std::vector<std::complex<float>> spectrum(bins, std::complex<float>{0.5f, -0.25f});
    std::vector<std::complex<float>> spectrum_out(bins);
    std::vector<float> time_out(static_cast<size_t>(n));
    std::vector<std::complex<float>> complex_in(static_cast<size_t>(n),
                                                std::complex<float>{0.25f, 0.125f});
    std::vector<std::complex<float>> complex_out(static_cast<size_t>(n));

    {
      // Bring the allocator to a steady state before anything is measured.
      FFT warm(n);
      warm.prepare(/*real_forward=*/true, /*real_inverse=*/true, /*complex_forward=*/true);
      warm.forward(signal.data(), spectrum_out.data());
      warm.inverse(spectrum.data(), time_out.data());
      warm.forward_complex(complex_in.data(), complex_out.data());
    }

    // Controls: each first transform allocates when prepare() has not run. A control that
    // fails means the instrument is blind, and the assertion below would then hold whatever
    // prepare() did. Each delta is taken before any assertion machinery runs.
    {
      FFT unprepared(n);
      const long long before = in_use();
      unprepared.forward(signal.data(), spectrum_out.data());
      const long long delta = in_use() - before;
      REQUIRE(delta > 0);
    }
    {
      FFT unprepared(n);
      const long long before = in_use();
      unprepared.inverse(spectrum.data(), time_out.data());
      const long long delta = in_use() - before;
      REQUIRE(delta > 0);
    }
    {
      FFT unprepared(n);
      const long long before = in_use();
      unprepared.forward_complex(complex_in.data(), complex_out.data());
      const long long delta = in_use() - before;
      REQUIRE(delta > 0);
    }

    FFT prepared(n);
    prepared.prepare(/*real_forward=*/true, /*real_inverse=*/true, /*complex_forward=*/true);
    const long long before = in_use();
    prepared.forward(signal.data(), spectrum_out.data());
    prepared.inverse(spectrum.data(), time_out.data());
    prepared.forward_complex(complex_in.data(), complex_out.data());
    const long long delta = in_use() - before;
    REQUIRE(delta == 0);
  }
#endif  // !defined(__APPLE__)
}
