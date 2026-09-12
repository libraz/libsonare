/// @file timbre_tile_test.cpp
/// @brief The tiled magnitude read in TimbreAnalyzer against an untiled oracle.
///
/// @details TimbreAnalyzer reads magnitude in bounded frame tiles rather than
///          materializing an [n_bins x n_frames] array beside the power cache it
///          would be derived from. Tiling changes each descriptor's trip count, so
///          the comparison is exact equality: a tolerance would not see a value
///          that moved because of where it sat in the iteration space.
///
///          Three cache states are exercised, and they now share ONE oracle: after
///          removing the magnitude/power cross-derivation, magnitude() is abs(z)
///          regardless of which accessor ran first, so a state that disagreed with
///          the oracle would be the defect this test exists to catch.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "analysis/timbre_analyzer.h"
#include "core/spectrum.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/spectral.h"
#include "util/constants.h"

using namespace sonare;

namespace {

constexpr int kSr = 22050;
constexpr int kNfft = 256;
constexpr int kHop = 64;
/// 600 frames against a 256-frame tile: two full tiles and an 88-frame partial.
/// A single-tile shape cannot see a tile-offset error, so the partial is the point.
constexpr int kTargetFrames = 600;
constexpr int kFluxLag = 1;  // matches kRoughnessFluxLag

/// @brief Audio whose STFT has silence and a burst in the FIRST and LAST tiles.
/// @details A descriptor that reads only the first tile correctly still fails.
Audio make_audio() {
  const int n = (kTargetFrames - 1) * kHop;
  std::vector<float> x(static_cast<size_t>(n), 0.0f);
  uint32_t s = 22695477u;
  const auto next = [&s]() {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 8) / static_cast<float>(1u << 24);
  };
  for (int i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSr);
    x[static_cast<size_t>(i)] =
        0.4f * std::sin(constants::kTwoPi * 440.0f * t) + 0.05f * (2.0f * next() - 1.0f);
  }
  const auto fill = [&](int f0, int f1, bool loud) {
    for (int i = f0 * kHop; i < std::min(n, f1 * kHop); ++i) {
      x[static_cast<size_t>(i)] = loud ? 0.9f * (2.0f * next() - 1.0f) : 0.0f;
    }
  };
  fill(4, 9, false);      // silence, first tile
  fill(20, 24, true);     // burst, first tile
  fill(530, 535, false);  // silence, last (partial) tile
  fill(560, 564, true);   // burst, last (partial) tile
  return Audio::from_vector(std::move(x), kSr);
}

StftConfig config() {
  StftConfig c;
  c.n_fft = kNfft;
  c.hop_length = kHop;
  return c;
}

/// @brief The magnitude the tile holds, built whole: sqrt(power) or abs(z).
/// @details Both paths are one fixed formula of the complex spectrum now, so the
///          two are bit-identical; kept as two paths so a section can still choose
///          which cache it forces to populate first.
std::vector<float> oracle_magnitude(const Spectrogram& spec, bool from_power) {
  const size_t n = static_cast<size_t>(spec.n_bins()) * static_cast<size_t>(spec.n_frames());
  std::vector<float> mag(n);
  if (from_power) {
    const std::vector<float>& p = spec.power();
    for (size_t i = 0; i < n; ++i) mag[i] = std::sqrt(p[i]);
  } else {
    const std::complex<float>* z = spec.complex_data();
    for (size_t i = 0; i < n; ++i) mag[i] = std::abs(z[i]);
  }
  return mag;
}

struct Descriptors {
  std::vector<float> centroid;
  std::vector<float> flatness;
  std::vector<float> rolloff;
};

Descriptors untiled(const std::vector<float>& mag, int n_bins, int n_frames) {
  return Descriptors{spectral_centroid(mag.data(), n_bins, n_frames, kSr, kNfft),
                     spectral_flatness(mag.data(), n_bins, n_frames),
                     spectral_rolloff(mag.data(), n_bins, n_frames, kSr, kNfft, 0.85f)};
}

/// @brief Mismatches between two vectors, counted rather than reported one at a time.
int differ(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return -1;
  int d = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) ++d;
  }
  return d;
}

/// @brief The descriptors a one-frame tile-window offset would produce.
/// @details The non-vacuity control: the comparison above is only evidence if this
///          differs. A tile-offset error is the defect a single-tile fixture cannot
///          see, so it is the one worth proving detectable.
Descriptors offset_by_one(const std::vector<float>& mag, int n_bins, int n_frames, int tile) {
  Descriptors out;
  std::vector<float> buf;
  for (int t0 = 0; t0 < n_frames; t0 += tile) {
    const int len = std::min(tile, n_frames - t0);
    const int bad = std::min(t0 + 1, n_frames - len);
    buf.assign(static_cast<size_t>(n_bins) * static_cast<size_t>(len), 0.0f);
    for (int b = 0; b < n_bins; ++b) {
      const float* src = mag.data() + static_cast<size_t>(b) * static_cast<size_t>(n_frames) + bad;
      std::copy(src, src + len, buf.data() + static_cast<size_t>(b) * static_cast<size_t>(len));
    }
    const Descriptors d = untiled(buf, n_bins, len);
    out.centroid.insert(out.centroid.end(), d.centroid.begin(), d.centroid.end());
    out.flatness.insert(out.flatness.end(), d.flatness.begin(), d.flatness.end());
    out.rolloff.insert(out.rolloff.end(), d.rolloff.begin(), d.rolloff.end());
  }
  return out;
}

void compare(const TimbreAnalyzer& analyzer, const Descriptors& ref) {
  CHECK(differ(analyzer.spectral_centroid(), ref.centroid) == 0);
  CHECK(differ(analyzer.spectral_flatness(), ref.flatness) == 0);
  CHECK(differ(analyzer.spectral_rolloff(), ref.rolloff) == 0);
}

}  // namespace

TEST_CASE("the two magnitude formulas actually disagree on this fixture",
          "[timbre][tile][spectrum]") {
  // Non-vacuity guard for the cache-derivation axis, sited at the assertion that
  // depends on it. sqrt(cached power) and abs(z) are two formulas for one quantity
  // and which runs depends on call order; the per-state sections below are only
  // testing anything if they disagree here. A fixture that made them agree would
  // pass every state section while checking nothing.
  const Audio audio = make_audio();
  Spectrogram spec = Spectrogram::compute(audio, config());
  const std::vector<float> from_power = oracle_magnitude(spec, /*from_power=*/true);
  const std::vector<float> from_complex = oracle_magnitude(spec, /*from_power=*/false);
  REQUIRE(from_power.size() == from_complex.size());
  int d = 0;
  for (size_t i = 0; i < from_power.size(); ++i) {
    if (from_power[i] != from_complex[i]) ++d;
  }
  CAPTURE(from_power.size(), d);
  CHECK(d > 0);
  WARN("magnitude formulas differ in " << d << " of " << from_power.size() << " cells");
}

TEST_CASE("tiled magnitude matches an untiled oracle, bit-identical across cache states",
          "[timbre][tile][spectrum]") {
  const Audio audio = make_audio();
  TimbreConfig cfg;
  cfg.n_fft = kNfft;
  cfg.hop_length = kHop;

  MelFilterConfig mel_cfg;
  mel_cfg.n_mels = cfg.n_mels;

  // One oracle for all three sections below. Neither magnitude() nor power() derives
  // from the other any more, so the same abs(z) reference applies whichever cache a
  // section forces to populate first -- the three states matching this one oracle is
  // exactly them matching each other bit for bit.
  Spectrogram probe = Spectrogram::compute(audio, config());
  REQUIRE(probe.n_frames() == kTargetFrames);
  // A partial final tile is what makes a tile-offset error visible at all.
  REQUIRE(probe.n_frames() % 256 != 0);
  const std::vector<float> oracle = oracle_magnitude(probe, /*from_power=*/false);
  const Descriptors ref = untiled(oracle, probe.n_bins(), probe.n_frames());

  SECTION("power cached first -- the ordering analyze() produces") {
    Spectrogram spec = Spectrogram::compute(audio, config());
    const MelSpectrogram mel = MelSpectrogram::from_spectrogram(spec, kSr, mel_cfg);
    const TimbreAnalyzer analyzer(spec, mel, cfg);
    compare(analyzer, ref);
  }

  SECTION("magnitude cached first -- the tile loop must use the cache, not recompute") {
    Spectrogram spec = Spectrogram::compute(audio, config());
    REQUIRE(spec.magnitude().size() == oracle.size());
    const MelSpectrogram mel = MelSpectrogram::from_spectrogram(spec, kSr, mel_cfg);
    const TimbreAnalyzer analyzer(spec, mel, cfg);
    compare(analyzer, ref);
  }

  SECTION("neither cached -- the tile computes abs(z) fresh") {
    Spectrogram spec = Spectrogram::compute(audio, config());
    // Mel comes from a second spectrogram so this one's caches stay empty.
    Spectrogram other = Spectrogram::compute(audio, config());
    const MelSpectrogram mel = MelSpectrogram::from_spectrogram(other, kSr, mel_cfg);
    const TimbreAnalyzer analyzer(spec, mel, cfg);
    compare(analyzer, ref);
  }

  SECTION("flux tiles with lag overlap -- its zero prefix is positional") {
    // Flux is the one descriptor that reads two frames, and TimbreAnalyzer keeps
    // its result private, so the overlap arithmetic is checked here directly.
    // Without the overlap a zero would appear at every tile boundary instead of
    // only at frame 0, which is what makes the prefix positional rather than a
    // property of the data.
    const int n_bins = probe.n_bins();
    const int n_frames = probe.n_frames();
    const std::vector<float> ref_flux = spectral_flux(oracle.data(), n_bins, n_frames, kFluxLag);

    std::vector<float> tiled;
    std::vector<float> buf;
    const int tile = 256;
    for (int first = 0; first < n_frames; first += tile) {
      const int lo = std::max(0, first - kFluxLag);
      const int hi = std::min(n_frames, first + tile);
      const int len = hi - lo;
      const int discard = first - lo;
      buf.assign(static_cast<size_t>(n_bins) * static_cast<size_t>(len), 0.0f);
      for (int b = 0; b < n_bins; ++b) {
        const float* src =
            oracle.data() + static_cast<size_t>(b) * static_cast<size_t>(n_frames) + lo;
        std::copy(src, src + len, buf.data() + static_cast<size_t>(b) * static_cast<size_t>(len));
      }
      const std::vector<float> f = spectral_flux(buf.data(), n_bins, len, kFluxLag);
      tiled.insert(tiled.end(), f.begin() + discard, f.end());
    }
    CHECK(differ(tiled, ref_flux) == 0);

    // Non-vacuity: drop the overlap and the boundary frames must go wrong.
    std::vector<float> no_overlap;
    for (int first = 0; first < n_frames; first += tile) {
      const int len = std::min(tile, n_frames - first);
      buf.assign(static_cast<size_t>(n_bins) * static_cast<size_t>(len), 0.0f);
      for (int b = 0; b < n_bins; ++b) {
        const float* src =
            oracle.data() + static_cast<size_t>(b) * static_cast<size_t>(n_frames) + first;
        std::copy(src, src + len, buf.data() + static_cast<size_t>(b) * static_cast<size_t>(len));
      }
      const std::vector<float> f = spectral_flux(buf.data(), n_bins, len, kFluxLag);
      no_overlap.insert(no_overlap.end(), f.begin(), f.end());
    }
    CHECK(differ(no_overlap, ref_flux) > 0);
  }

  SECTION("the comparison can go red -- a one-frame tile offset") {
    Spectrogram spec = Spectrogram::compute(audio, config());
    const MelSpectrogram mel = MelSpectrogram::from_spectrogram(spec, kSr, mel_cfg);
    const TimbreAnalyzer analyzer(spec, mel, cfg);
    const Descriptors bad = offset_by_one(oracle, spec.n_bins(), spec.n_frames(), 256);
    CHECK(differ(analyzer.spectral_centroid(), bad.centroid) > 0);
    CHECK(differ(analyzer.spectral_flatness(), bad.flatness) > 0);
    CHECK(differ(analyzer.spectral_rolloff(), bad.rolloff) > 0);
  }
}

TEST_CASE("sonare_stft's power equals re^2+im^2 independent of a preceding magnitude read",
          "[timbre][tile][spectrum][c-api]") {
  // The cross-entry-point guard: sonare_stft (src/c_api/features_spectrogram_mel.cpp)
  // reads magnitude() then power() on one Spectrogram, which used to return
  // power = abs(z)^2 rather than the re^2+im^2 every other power consumer gets.
  const Audio audio = make_audio();

  SonareStftResult result{};
  REQUIRE(sonare_stft(audio.data(), audio.size(), audio.sample_rate(), kNfft, kHop, &result) ==
          SONARE_OK);

  // Independent reference, built from a second spectrogram's complex data without
  // ever calling Spectrogram::power() or ::magnitude().
  Spectrogram spec = Spectrogram::compute(audio, config());
  const std::complex<float>* z = spec.complex_data();
  const size_t n = static_cast<size_t>(spec.n_bins()) * static_cast<size_t>(spec.n_frames());
  REQUIRE(n == static_cast<size_t>(result.n_bins) * static_cast<size_t>(result.n_frames));

  int d = 0;
  for (size_t i = 0; i < n; ++i) {
    const float expected = z[i].real() * z[i].real() + z[i].imag() * z[i].imag();
    if (result.power[i] != expected) ++d;
  }
  CHECK(d == 0);

  sonare_free_stft_result(&result);
}
