#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "mastering/eq/equalizer.h"
#include "mastering/match/ab_switcher.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_loudness.h"
#include "mastering/match/reference_spectrum.h"
#include "mastering/match/tonal_balance.h"
#include "rt/biquad_design.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/db.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::match;
using sonare::constants::kPi;

namespace {
using sonare::test::rms;

Audio sine_audio(float frequency_hz, float amplitude, int sample_rate = 48000,
                 float duration_sec = 1.0f) {
  const int samples = static_cast<int>(duration_sec * static_cast<float>(sample_rate));
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude *
        static_cast<float>(std::sin(sonare::constants::kTwoPiD * frequency_hz * i / sample_rate));
  }
  return Audio::from_vector(std::move(out), sample_rate);
}

/// @brief Composite dB magnitude of a peaking band set at one frequency.
/// @details Cascaded sections multiply in magnitude, so their dB responses add.
///          Coefficients are designed exactly as ParametricEq designs them.
float composite_response_db(const std::vector<sonare::mastering::eq::EqBand>& bands,
                            float frequency_hz, double sample_rate) {
  const auto omega = static_cast<float>(sonare::constants::kTwoPiD *
                                        static_cast<double>(frequency_hz) / sample_rate);
  float total_db = 0.0f;
  for (const auto& band : bands) {
    const auto w0 = static_cast<float>(sonare::constants::kTwoPiD *
                                       static_cast<double>(band.frequency_hz) / sample_rate);
    const auto section = sonare::rt::rbj_peak(w0, band.q, band.gain_db);
    total_db += sonare::linear_to_db(sonare::rt::biquad_magnitude(section, omega));
  }
  return total_db;
}

MatchEqCurve log_spaced_curve(float min_hz, float max_hz, size_t points, int sample_rate) {
  MatchEqCurve curve;
  curve.sample_rate = sample_rate;
  curve.frequencies.resize(points);
  curve.gain_db.assign(points, 0.0f);
  for (size_t i = 0; i < points; ++i) {
    const double ratio = static_cast<double>(i) / static_cast<double>(points - 1);
    curve.frequencies[i] = static_cast<float>(
        static_cast<double>(min_hz) *
        std::pow(static_cast<double>(max_hz) / static_cast<double>(min_hz), ratio));
  }
  return curve;
}

/// @brief FFT-bin frequency grid, the shape production curves actually carry.
/// @details Bins are uniform in Hz, so the low end of a configured range holds a
///          handful of bins and the top holds thousands. Placement has to spread
///          across the range on this grid, not just on a log-spaced one.
MatchEqCurve fft_bin_curve(int fft_size, int sample_rate, float gain_db) {
  MatchEqCurve curve;
  curve.sample_rate = sample_rate;
  const float spacing = static_cast<float>(sample_rate) / static_cast<float>(fft_size);
  for (size_t i = 0; i <= static_cast<size_t>(fft_size / 2); ++i) {
    curve.frequencies.push_back(static_cast<float>(i) * spacing);
    curve.gain_db.push_back(gain_db);
  }
  return curve;
}

/// @brief Octave span from the lowest placed band to the highest.
float placed_span_octaves(const std::vector<sonare::mastering::eq::EqBand>& bands) {
  if (bands.size() < 2) {
    return 0.0f;
  }
  return std::log2(bands.back().frequency_hz / bands.front().frequency_hz);
}

/// @brief Widest octave hole between two neighbouring placed bands.
float widest_band_gap_octaves(const std::vector<sonare::mastering::eq::EqBand>& bands) {
  float widest = 0.0f;
  for (size_t i = 1; i < bands.size(); ++i) {
    widest = std::max(widest, std::log2(bands[i].frequency_hz / bands[i - 1].frequency_hz));
  }
  return widest;
}

/// @brief Gaussian bump in dB, centred on @p centre_hz with a log-frequency width.
float log_gaussian_db(float frequency_hz, float centre_hz, float width_octaves, float peak_db) {
  const double octaves =
      std::log2(static_cast<double>(frequency_hz) / static_cast<double>(centre_hz));
  const double width = static_cast<double>(width_octaves);
  const double shape = std::exp(-(octaves * octaves) / (2.0 * width * width));
  return static_cast<float>(static_cast<double>(peak_db) * shape);
}

}  // namespace

TEST_CASE("ReferenceSpectrum extracts smoothed bins", "[mastering][match]") {
  const auto spectrum = reference_spectrum(sine_audio(1000.0f, 0.5f), {1024, 256, true, 3});

  REQUIRE(spectrum.sample_rate == 48000);
  REQUIRE(spectrum.frequencies.size() == 513);
  REQUIRE(spectrum.db.size() == spectrum.frequencies.size());
}

TEST_CASE("ReferenceSpectrum averages across the full signal", "[mastering][match]") {
  std::vector<float> samples(48000, 0.0f);
  for (size_t i = 0; i < samples.size() / 2; ++i) {
    samples[i] =
        0.7f * static_cast<float>(std::sin(2.0 * kPi * 500.0 * static_cast<double>(i) / 48000.0));
  }
  for (size_t i = samples.size() / 2; i < samples.size(); ++i) {
    samples[i] =
        0.7f * static_cast<float>(std::sin(2.0 * kPi * 4000.0 * static_cast<double>(i) / 48000.0));
  }

  const auto spectrum =
      reference_spectrum(Audio::from_vector(std::move(samples), 48000), {2048, 512, false, 3});

  const auto near_500 = static_cast<size_t>(std::round(500.0 / (48000.0 / 2048.0)));
  const auto near_4000 = static_cast<size_t>(std::round(4000.0 / (48000.0 / 2048.0)));
  REQUIRE(spectrum.db[near_500] > -20.0f);
  REQUIRE(spectrum.db[near_4000] > -20.0f);
}

TEST_CASE("MatchEq bands follow reference minus source difference", "[mastering][match]") {
  ReferenceSpectrum source{{100.0f, 1000.0f, 10000.0f}, {-20.0f, -20.0f, -20.0f}, 48000};
  ReferenceSpectrum reference{{100.0f, 1000.0f, 10000.0f}, {-14.0f, -8.0f, -26.0f}, 48000};

  const auto bands = match_eq_bands(source, reference, {3, 6.0f, 100.0f, 20000.0f, 1.2f, 0});

  REQUIRE(bands.size() == 3);
  REQUIRE(bands[0].enabled);
  REQUIRE(bands[0].gain_db > 0.0f);
  REQUIRE(bands[1].gain_db <= 6.0f);
  REQUIRE(bands[2].gain_db < 0.0f);
}

TEST_CASE("MatchEq live bands are placed at curve extrema and can configure EqualizerProcessor",
          "[mastering][match]") {
  ReferenceSpectrum source{{100.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f},
                           {-20.0f, -20.0f, -20.0f, -20.0f, -20.0f},
                           48000};
  ReferenceSpectrum reference{{100.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f},
                              {-20.0f, -10.0f, -20.0f, -30.0f, -20.0f},
                              48000};

  const auto bands = match_eq_bands(source, reference, {2, 12.0f, 100.0f, 12000.0f, 1.4f, 0});

  REQUIRE(bands.size() == 2);
  REQUIRE_THAT(bands[0].frequency_hz, WithinAbs(250.0f, 0.001f));
  REQUIRE(bands[0].gain_db > 9.0f);
  REQUIRE_THAT(bands[1].frequency_hz, WithinAbs(4000.0f, 0.001f));
  REQUIRE(bands[1].gain_db < -9.0f);

  sonare::mastering::eq::EqualizerProcessor eq({1});
  eq.prepare(48000.0, 512);
  configure_equalizer_from_match(eq, source, reference, {2, 12.0f, 100.0f, 12000.0f, 1.4f, 0});

  REQUIRE(eq.band(0).enabled);
  REQUIRE_THAT(eq.band(0).frequency_hz, WithinAbs(250.0f, 0.001f));
  REQUIRE(eq.band(1).enabled);
  REQUIRE_THAT(eq.band(1).frequency_hz, WithinAbs(4000.0f, 0.001f));
  REQUIRE_FALSE(eq.band(2).enabled);
}

TEST_CASE("MatchEq curve keeps dense smoothed correction data", "[mastering][match]") {
  ReferenceSpectrum source{
      {100.0f, 200.0f, 400.0f, 800.0f}, {-20.0f, -20.0f, -20.0f, -20.0f}, 48000};
  ReferenceSpectrum reference{
      {100.0f, 200.0f, 400.0f, 800.0f}, {-14.0f, -14.0f, -26.0f, -26.0f}, 48000};

  const auto curve = match_eq_curve(source, reference, {4, 12.0f, 100.0f, 800.0f, 1.0f, 1});

  REQUIRE(curve.frequencies.size() == 4);
  REQUIRE(curve.gain_db.size() == curve.frequencies.size());
  REQUIRE(curve.gain_db.front() > 0.0f);
  REQUIRE(curve.gain_db.back() < 0.0f);
}

TEST_CASE("MatchEq band gains follow the composite response of the whole band set",
          "[mastering][match]") {
  constexpr int kSampleRate = 48000;
  constexpr float kLimitToleranceDb = 0.001f;
  const MatchEqConfig config{8, 12.0f, 40.0f, 18000.0f, 1.0f, 0};

  MatchEqCurve curve = log_spaced_curve(40.0f, 18000.0f, 256, kSampleRate);
  for (size_t i = 0; i < curve.frequencies.size(); ++i) {
    curve.gain_db[i] = log_gaussian_db(curve.frequencies[i], 1000.0f, 0.6f, 6.0f) +
                       log_gaussian_db(curve.frequencies[i], 5000.0f, 0.5f, -5.0f);
  }

  const auto bands = match_eq_bands_from_curve(curve, config);
  REQUIRE(bands.size() == config.max_bands);

  float max_deviation_db = 0.0f;
  float peak_db = 0.0f;
  for (size_t i = 0; i < curve.frequencies.size(); ++i) {
    const float realised = composite_response_db(bands, curve.frequencies[i], kSampleRate);
    max_deviation_db = std::max(max_deviation_db, std::abs(realised - curve.gain_db[i]));
    peak_db = std::max(peak_db, std::abs(realised));
  }

  // The eight selected bands sit close enough that their responses reinforce:
  // reading each gain off the curve at its own centre would realise 14.3 dB
  // against this 12 dB limit and miss the target by 8.4 dB.
  REQUIRE(max_deviation_db < 1.5f);
  REQUIRE(peak_db <= config.max_gain_db + kLimitToleranceDb);
}

TEST_CASE("MatchEq places its full band set at zero gain when the limit is zero",
          "[mastering][match]") {
  constexpr int kSampleRate = 48000;
  const MatchEqConfig limited{8, 12.0f, 40.0f, 18000.0f, 1.0f, 0};
  MatchEqConfig zero_limit = limited;
  zero_limit.max_gain_db = 0.0f;

  MatchEqCurve curve = log_spaced_curve(40.0f, 18000.0f, 256, kSampleRate);
  for (size_t i = 0; i < curve.frequencies.size(); ++i) {
    curve.gain_db[i] = log_gaussian_db(curve.frequencies[i], 1000.0f, 0.6f, 6.0f) +
                       log_gaussian_db(curve.frequencies[i], 5000.0f, 0.5f, -5.0f);
  }

  // The same curve under a usable limit demands real gain, so the zeros below are
  // the limit's doing rather than a curve that asks for nothing.
  const auto limited_bands = match_eq_bands_from_curve(curve, limited);
  REQUIRE(limited_bands.size() == limited.max_bands);
  float max_abs_gain_db = 0.0f;
  for (const auto& band : limited_bands) {
    max_abs_gain_db = std::max(max_abs_gain_db, std::abs(band.gain_db));
  }
  REQUIRE(max_abs_gain_db > 1.0f);

  // A zero limit is a valid configuration, not an error. Band selection does not
  // consult the limit, so the same bands are placed at the same frequencies and
  // stay enabled; only the solved gains collapse, and to exactly zero rather
  // than to a clamped residue.
  const auto bands = match_eq_bands_from_curve(curve, zero_limit);
  REQUIRE(bands.size() == limited_bands.size());
  for (size_t i = 0; i < bands.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(bands[i].frequency_hz, WithinAbs(limited_bands[i].frequency_hz, 0.001f));
    REQUIRE(bands[i].enabled);
    REQUIRE(bands[i].gain_db == 0.0f);
  }
}

TEST_CASE("MatchEq places a featureless correction across the range and fits it without overshoot",
          "[mastering][match]") {
  constexpr int kSampleRate = 48000;
  constexpr float kLimitToleranceDb = 0.001f;
  const MatchEqConfig config{8, 12.0f, 40.0f, 18000.0f, 1.0f, 0};

  // A flat correction offers the selector no extrema to rank by, so every band
  // past the two range endpoints comes from the diversity fill.
  MatchEqCurve curve = log_spaced_curve(40.0f, 18000.0f, 256, kSampleRate);
  std::fill(curve.gain_db.begin(), curve.gain_db.end(), 6.0f);

  const auto bands = match_eq_bands_from_curve(curve, config);
  REQUIRE(bands.size() == config.max_bands);

  // Coverage, not clustering: a featureless curve asks for the same correction
  // everywhere, so the band set has to reach across the configured range and
  // leave no wide hole inside it. The fill is greedy farthest-first, which
  // bisects the widest remaining hole, so eight bands land 2.18 octaves apart at
  // worst against the 8.81-octave range. The bounds below leave that room and
  // still reject spending the budget on one corner.
  const float configured_span_octaves =
      std::log2(config.max_frequency_hz / config.min_frequency_hz);
  REQUIRE(placed_span_octaves(bands) > configured_span_octaves * 0.9f);
  REQUIRE(widest_band_gap_octaves(bands) < 3.0f);

  float peak_db = 0.0f;
  float max_overshoot_db = 0.0f;
  for (size_t i = 0; i < curve.frequencies.size(); ++i) {
    const float realised = composite_response_db(bands, curve.frequencies[i], kSampleRate);
    peak_db = std::max(peak_db, std::abs(realised));
    max_overshoot_db = std::max(max_overshoot_db, realised - curve.gain_db[i]);
  }

  // Eight bands spread across the range still overlap: reading each gain off the
  // curve realises 9.9 dB against this 6 dB target, so the joint solve has to
  // back the gains off. It stays inside the 12 dB limit at this target, which is
  // why the sibling case below runs the same curve at the limit to make the
  // clamp itself engage. Only the overshoot is bounded here: a peaking band set
  // cannot hold a flat shelf between its centres, so the undershoot in between
  // is a band-shape property rather than a gain-stacking one.
  REQUIRE(peak_db <= config.max_gain_db + kLimitToleranceDb);
  REQUIRE(max_overshoot_db < 4.0f);
}

TEST_CASE("MatchEq scales the band set when the fitted response exceeds max_gain_db",
          "[mastering][match]") {
  constexpr int kSampleRate = 48000;
  constexpr float kLimitToleranceDb = 0.001f;
  const MatchEqConfig config{8, 12.0f, 40.0f, 18000.0f, 1.0f, 0};

  // A reference more than max_gain_db louder than the source across the whole
  // spectrum clamps the curve flat at the limit, so any fit error at all pushes
  // the realised response past it and the limiter has to engage.
  MatchEqCurve curve = log_spaced_curve(40.0f, 18000.0f, 256, kSampleRate);
  std::fill(curve.gain_db.begin(), curve.gain_db.end(), config.max_gain_db);

  const auto bands = match_eq_bands_from_curve(curve, config);
  REQUIRE(bands.size() == config.max_bands);

  // This curve is as featureless as the one above, so it exercises the same fill
  // and has to spread the same way. Asserting it here keeps the limiter case from
  // passing on a band set collapsed into one corner of the range.
  const float configured_span_octaves =
      std::log2(config.max_frequency_hz / config.min_frequency_hz);
  REQUIRE(placed_span_octaves(bands) > configured_span_octaves * 0.9f);
  REQUIRE(widest_band_gap_octaves(bands) < 3.0f);

  float peak_db = 0.0f;
  for (size_t i = 0; i < curve.frequencies.size(); ++i) {
    peak_db = std::max(peak_db,
                       std::abs(composite_response_db(bands, curve.frequencies[i], kSampleRate)));
  }

  // Per-band gains equal to the curve realise 20.5 dB here, so the clamp engages
  // and the realised response has to land just under the limit rather than at it.
  REQUIRE(peak_db <= config.max_gain_db + kLimitToleranceDb);
  REQUIRE(peak_db > config.max_gain_db - 1.0f);
}

TEST_CASE("MatchEq spreads a featureless correction across the configured range",
          "[mastering][match]") {
  constexpr int kSampleRate = 48000;
  constexpr int kFftSize = 2048;
  // Greedy farthest-first insertion bisects the widest remaining hole, so the
  // worst gap it can leave is bounded by a small multiple of the even spacing
  // rather than by the minimum-spacing floor. Measured worst across band counts,
  // configured ranges, FFT sizes and Q values is 1.82x; the collapse this
  // replaces sat at 5.0x.
  constexpr float kMaxGapVersusEven = 2.5f;
  const size_t band_count = GENERATE(size_t{4}, size_t{6}, size_t{8}, size_t{12});
  CAPTURE(band_count);

  const MatchEqConfig config{band_count, 12.0f, 40.0f, 18000.0f, 1.0f, 0};
  const MatchEqCurve curve = fft_bin_curve(kFftSize, kSampleRate, 6.0f);

  const auto bands = match_eq_bands_from_curve(curve, config);
  REQUIRE(bands.size() == band_count);

  // The lowest FFT bin inside the range is 46.9 Hz against a 40 Hz floor, which
  // is 0.23 of the 8.81 configured octaves and the only reason coverage is not 1.
  const float configured_span_octaves =
      std::log2(config.max_frequency_hz / config.min_frequency_hz);
  REQUIRE(placed_span_octaves(bands) > configured_span_octaves * 0.9f);

  const float even_spacing_octaves = configured_span_octaves / static_cast<float>(band_count - 1);
  REQUIRE(widest_band_gap_octaves(bands) < even_spacing_octaves * kMaxGapVersusEven);
}

TEST_CASE("MatchEq places every band on a curve extremum when extrema fill the budget",
          "[mastering][match]") {
  constexpr int kSampleRate = 48000;
  constexpr float kCentreTolerance = 0.02f;
  const std::vector<float> peak_centres_hz = {80.0f, 500.0f, 3000.0f, 12000.0f};

  MatchEqCurve curve = log_spaced_curve(40.0f, 18000.0f, 256, kSampleRate);
  for (size_t i = 0; i < curve.frequencies.size(); ++i) {
    curve.gain_db[i] = log_gaussian_db(curve.frequencies[i], peak_centres_hz[0], 0.35f, 5.0f) +
                       log_gaussian_db(curve.frequencies[i], peak_centres_hz[1], 0.35f, -4.0f) +
                       log_gaussian_db(curve.frequencies[i], peak_centres_hz[2], 0.35f, 6.0f) +
                       log_gaussian_db(curve.frequencies[i], peak_centres_hz[3], 0.35f, -5.0f);
  }

  // Four peaks and four slots: the extremum pass fills the budget on its own and
  // the diversity fill never runs, so each band must sit on a peak of the curve
  // and not on an evenly spaced grid point.
  MatchEqConfig config{peak_centres_hz.size(), 12.0f, 40.0f, 18000.0f, 1.0f, 0};
  const auto bands = match_eq_bands_from_curve(curve, config);
  REQUIRE(bands.size() == peak_centres_hz.size());
  for (size_t i = 0; i < bands.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(bands[i].frequency_hz,
                 WithinAbs(peak_centres_hz[i], peak_centres_hz[i] * kCentreTolerance));
  }
  REQUIRE(bands[0].gain_db > 0.0f);
  REQUIRE(bands[1].gain_db < 0.0f);
  REQUIRE(bands[2].gain_db > 0.0f);
  REQUIRE(bands[3].gain_db < 0.0f);

  // Widening the budget lets the fill add bands, but it must not displace what the
  // extremum pass already chose.
  config.max_bands = peak_centres_hz.size() + 4;
  const auto widened = match_eq_bands_from_curve(curve, config);
  REQUIRE(widened.size() == config.max_bands);
  for (const auto& band : bands) {
    CAPTURE(band.frequency_hz);
    const bool retained = std::any_of(widened.begin(), widened.end(), [&](const auto& candidate) {
      return std::abs(candidate.frequency_hz - band.frequency_hz) < 0.001f;
    });
    REQUIRE(retained);
  }
}

TEST_CASE("MatchEq FIR kernel is linear phase and follows curve gain", "[mastering][match]") {
  MatchEqCurve curve{{100.0f, 1000.0f, 10000.0f}, {0.0f, 6.0f, 0.0f}};

  const auto kernel = match_eq_fir_kernel(curve, 48000, {1024, 257});

  REQUIRE(kernel.size() == 257);
  REQUIRE_THAT(kernel.front(), WithinAbs(kernel.back(), 0.0001f));
  REQUIRE_THAT(kernel[10], WithinAbs(kernel[kernel.size() - 11], 0.0001f));
}

TEST_CASE("MatchEq FIR kernel supports minimum-phase mode", "[mastering][match]") {
  MatchEqCurve curve{{100.0f, 1000.0f, 10000.0f}, {0.0f, 6.0f, 0.0f}};

  const auto linear = match_eq_fir_kernel(curve, 48000, {1024, 257});
  const auto minimum =
      match_eq_fir_kernel(curve, 48000, {1024, 257, MatchEqFirPhase::MinimumPhase});

  REQUIRE(minimum.size() == 257);
  REQUIRE(std::abs(minimum.front()) > std::abs(linear.front()) * 10.0f);
  REQUIRE(std::abs(minimum.front() - minimum.back()) > 0.0001f);
}

TEST_CASE("ApplyMatchEq boosts material toward the reference spectrum", "[mastering][match]") {
  const Audio input = sine_audio(1000.0f, 0.1f);
  ReferenceSpectrum source{{100.0f, 1000.0f, 10000.0f}, {-20.0f, -20.0f, -20.0f}, 48000};
  ReferenceSpectrum reference{{100.0f, 1000.0f, 10000.0f}, {-20.0f, -14.0f, -20.0f}, 48000};

  const auto output =
      apply_match_eq(input, source, reference, {8, 6.0f, 100.0f, 10000.0f, 1.0f, 0}, {1024, 257});

  REQUIRE(output.size() == input.size());
  REQUIRE(rms(output, 512) > rms(input, 512) * 1.5f);
}

TEST_CASE("ApplyMatchEq uses partitioned FIR and minimum-phase options", "[mastering][match]") {
  const Audio input = sine_audio(1000.0f, 0.1f);
  ReferenceSpectrum source{{100.0f, 1000.0f, 10000.0f}, {-20.0f, -20.0f, -20.0f}, 48000};
  ReferenceSpectrum reference{{100.0f, 1000.0f, 10000.0f}, {-20.0f, -14.0f, -20.0f}, 48000};

  const auto linear = apply_match_eq(input, source, reference, {8, 6.0f, 100.0f, 10000.0f, 1.0f, 0},
                                     {1024, 257, MatchEqFirPhase::LinearPhase, 128});
  const auto minimum =
      apply_match_eq(input, source, reference, {8, 6.0f, 100.0f, 10000.0f, 1.0f, 0},
                     {1024, 257, MatchEqFirPhase::MinimumPhase, 128});

  REQUIRE(linear.size() == input.size());
  REQUIRE(minimum.size() == input.size());
  REQUIRE(rms(linear, 512) > rms(input, 512) * 1.5f);
  REQUIRE(rms(minimum, 512) > rms(input, 512) * 1.5f);
}

TEST_CASE("MatchEq can time-align reference audio by cross-correlation", "[mastering][match]") {
  std::vector<float> source_samples(512, 0.0f);
  for (size_t i = 0; i < source_samples.size(); ++i) {
    source_samples[i] = static_cast<float>(std::sin(0.13 * static_cast<double>(i)) +
                                           0.2 * std::sin(0.017 * static_cast<double>(i * i)));
  }
  std::vector<float> reference_samples(512, 0.0f);
  for (size_t i = 0; i + 5 < reference_samples.size(); ++i) {
    reference_samples[i + 5] = source_samples[i];
  }

  const auto source = Audio::from_vector(source_samples, 48000);
  const auto reference = Audio::from_vector(reference_samples, 48000);

  REQUIRE_THAT(estimate_reference_delay_samples(source, reference, 16), WithinAbs(5.0f, 0.01f));
  const auto aligned = align_reference_to_source(source, reference, 16);

  for (size_t i = 0; i + 5 < aligned.size(); ++i) {
    REQUIRE_THAT(aligned[i], WithinAbs(source[i], 0.00001f));
  }
}

TEST_CASE("TonalBalance summarizes broad band deviations", "[mastering][match]") {
  ReferenceSpectrum source{
      {100.0f, 500.0f, 3000.0f, 12000.0f}, {-10.0f, -20.0f, -30.0f, -40.0f}, 48000};
  ReferenceSpectrum reference{
      {100.0f, 500.0f, 3000.0f, 12000.0f}, {-12.0f, -18.0f, -35.0f, -35.0f}, 48000};

  const auto balance = tonal_balance(source, reference);

  REQUIRE(balance.size() == 4);
  REQUIRE_THAT(balance[0].deviation_db, WithinAbs(2.0f, 0.001f));
  REQUIRE_THAT(balance[1].deviation_db, WithinAbs(-2.0f, 0.001f));
  REQUIRE_THAT(balance[2].deviation_db, WithinAbs(5.0f, 0.001f));
  REQUIRE_THAT(balance[3].deviation_db, WithinAbs(-5.0f, 0.001f));
}

TEST_CASE("TonalBalance can summarize log-frequency bands", "[mastering][match]") {
  ReferenceSpectrum source{{100.0f, 200.0f}, {-10.0f, -20.0f}, 48000};
  ReferenceSpectrum reference{{100.0f, 200.0f}, {-15.0f, -18.0f}, 48000};

  const auto balance = tonal_balance_log_bands(source, reference, 1, 100.0f, 400.0f);

  REQUIRE(balance.size() == 2);
  REQUIRE_THAT(balance[0].deviation_db, WithinAbs(5.0f, 0.001f));
  REQUIRE_THAT(balance[1].deviation_db, WithinAbs(-2.0f, 0.001f));
}

TEST_CASE("ReferenceLoudness reports gain required to match reference", "[mastering][match]") {
  const auto quiet = sine_audio(1000.0f, 0.05f);
  const auto loud = sine_audio(1000.0f, 0.2f);

  const auto result = reference_loudness(quiet, loud);

  REQUIRE(result.gain_to_match_db > 10.0f);
  REQUIRE(result.reference_lufs > result.source_lufs);
}

TEST_CASE("ABSwitcher selects and crossfades audio", "[mastering][match]") {
  const std::vector<float> a = {0.0f, 0.25f, 0.5f};
  const std::vector<float> b = {1.0f, 0.75f, 0.5f};
  const Audio aa = Audio::from_buffer(a.data(), a.size(), 48000);
  const Audio bb = Audio::from_buffer(b.data(), b.size(), 48000);

  const auto selected = ab_switch(aa, bb, ABSelection::B);
  const auto mixed = ab_crossfade(aa, bb, 0.25f);

  REQUIRE_THAT(selected[0], WithinAbs(1.0f, 0.001f));
  REQUIRE(mixed.size() == 3);
  REQUIRE_THAT(mixed[0], WithinAbs(0.25f, 0.001f));
  REQUIRE_THAT(mixed[1], WithinAbs(0.375f, 0.001f));
}

TEST_CASE("Match helpers validate inputs", "[mastering][match]") {
  const Audio empty;
  REQUIRE_THROWS(reference_spectrum(empty));

  ReferenceSpectrum source{{100.0f}, {-10.0f}, 48000};
  ReferenceSpectrum reference{{100.0f}, {-10.0f}, 44100};
  REQUIRE_THROWS(match_eq_bands(source, reference));
  REQUIRE_THROWS(tonal_balance(source, reference));

  const auto audio = sine_audio(1000.0f, 0.1f);
  REQUIRE_THROWS(ab_crossfade(audio, audio, 1.5f));
  REQUIRE_THROWS(ab_switch(audio, audio, static_cast<ABSelection>(2)));

  MatchEqCurve curve{{100.0f, 1000.0f}, {0.0f, 3.0f}};
  MatchEqFirConfig invalid_phase;
  invalid_phase.phase = static_cast<MatchEqFirPhase>(2);
  REQUIRE_THROWS(match_eq_fir_kernel(curve, 48000, invalid_phase));
}

TEST_CASE("MatchEq FIR realizes unity outside the matched band", "[mastering][match]") {
  // A match curve is only defined over [min_frequency_hz, max_frequency_hz].
  // The parametric realization places no band outside it, so its response
  // returns to 0 dB there; the FIR path sampled the curve at every bin from DC
  // to Nyquist and the interpolator holds the endpoint value outside its range,
  // so a low-heavy reference put the curve's edge gain on DC and on Nyquist —
  // a DC offset and super-low energy eating the headroom of everything
  // downstream.
  constexpr int kSampleRate = 48000;
  constexpr int kFftSize = 4096;
  // A kernel long enough to resolve the 200 Hz band edge: the taper below it
  // spans 100-200 Hz, and a kernel that cannot resolve 100 Hz cannot realize it.
  constexpr int kKernelSize = 1025;
  // A curve that is strongly boosted at both edges, so an untapered
  // extrapolation is unmistakable at DC and Nyquist.
  MatchEqCurve curve{{200.0f, 1000.0f, 12000.0f}, {12.0f, 0.0f, 12.0f}, kSampleRate};

  const auto kernel = match_eq_fir_kernel(curve, kSampleRate, {kFftSize, kKernelSize});
  REQUIRE(kernel.size() == static_cast<size_t>(kKernelSize));

  // DC gain is the tap sum; the Nyquist gain is the alternating-sign sum.
  double dc = 0.0;
  double nyquist = 0.0;
  for (size_t i = 0; i < kernel.size(); ++i) {
    dc += kernel[i];
    nyquist += (i % 2 == 0 ? 1.0 : -1.0) * kernel[i];
  }
  const double dc_db = 20.0 * std::log10(std::abs(dc));
  const double nyquist_db = 20.0 * std::log10(std::abs(nyquist));
  CHECK(std::abs(dc_db) < 0.5);
  CHECK(std::abs(nyquist_db) < 0.5);

  // Non-vacuity: the band the curve does describe still gets its gain, so the
  // taper bounded the extrapolation rather than flattening the whole kernel.
  const auto response_db = [&](double hz) {
    std::complex<double> h{0.0, 0.0};
    const double w = 2.0 * sonare::constants::kPiD * hz / kSampleRate;
    for (size_t i = 0; i < kernel.size(); ++i) {
      const double phase = w * static_cast<double>(i);
      h += static_cast<double>(kernel[i]) * std::complex<double>(std::cos(phase), -std::sin(phase));
    }
    return 20.0 * std::log10(std::abs(h));
  };
  CHECK(response_db(200.0) > 6.0);
  CHECK(response_db(12000.0) > 6.0);
}
