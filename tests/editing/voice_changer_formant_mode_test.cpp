/// @file voice_changer_formant_mode_test.cpp
/// @brief Relative and absolute formant modes of the one-shot voice changer.

#include <catch2/matchers/catch_matchers_string.hpp>

#include "effects/pitch_shift.h"
#include "voice_changer_test_helpers.h"

namespace {

using Catch::Matchers::ContainsSubstring;

struct Resonance {
  double hz;
  double bandwidth_hz;
};

// Harmonics of f0 under three cascaded second-order resonances, each normalised to unity at
// its own centre so the upper formants are not buried by the slope of the lower ones.
std::vector<float> three_formant_vowel(double f0, int sample_rate, int samples) {
  const std::array<Resonance, 3> formants = {{{600.0, 150.0}, {1500.0, 200.0}, {2600.0, 250.0}}};
  std::vector<float> output(static_cast<size_t>(samples), 0.0f);
  for (int h = 1; h * f0 < 0.45 * sample_rate; ++h) {
    const double f = h * f0;
    double gain = 1.0;
    for (const Resonance& r : formants) {
      const double x = f / r.hz;
      const double q = r.hz / r.bandwidth_hz;
      gain *= (1.0 / q) / std::sqrt((1.0 - x * x) * (1.0 - x * x) + (x / q) * (x / q));
    }
    for (int i = 0; i < samples; ++i) {
      output[static_cast<size_t>(i)] += static_cast<float>(
          gain * std::sin(sonare::constants::kTwoPiD * f * static_cast<double>(i) / sample_rate));
    }
  }
  const float peak = *std::max_element(output.begin(), output.end());
  // Lowpassed aspiration noise: a noiseless sum of harmonics is predicted almost exactly by
  // the warp's LPC stage, which then treats the frame as degenerate and passes it through.
  uint32_t state = 12345u;
  float lp1 = 0.0f;
  float lp2 = 0.0f;
  const float a =
      1.0f - std::exp(-sonare::constants::kTwoPi * 3000.0f / static_cast<float>(sample_rate));
  for (float& v : output) {
    state = state * 1664525u + 1013904223u;
    const float noise = static_cast<float>(state >> 8) / 16777216.0f - 0.5f;
    lp1 += a * (noise - lp1);
    lp2 += a * (lp1 - lp2);
    v = 0.5f * v / peak + 0.05f * lp2;
  }
  return output;
}

// Cepstrally smoothed log-magnitude envelope of a segment, written without the library's
// LPC code so the measurement does not share an estimator with the warp it checks.
// Returns the frequencies (Hz) of the two tallest envelope peaks, ascending: the first two
// formants, which stay clear of the rolled-off upper envelope.
std::array<double, 2> envelope_peaks(const std::vector<float>& segment, int sample_rate,
                                     double f0_hz, int lifter) {
  constexpr int kNfft = 16384;
  std::vector<float> frame(static_cast<size_t>(kNfft), 0.0f);
  const int n = std::min(kNfft, static_cast<int>(segment.size()));
  for (int i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(sonare::constants::kTwoPiD * i / (n - 1));
    frame[static_cast<size_t>(i)] = static_cast<float>(segment[static_cast<size_t>(i)] * w);
  }
  sonare::FFT fft(kNfft);
  std::vector<std::complex<float>> spec(static_cast<size_t>(fft.n_bins()));
  fft.forward(frame.data(), spec.data());

  // Sliding maximum over one pitch period: the envelope runs through the harmonic peaks,
  // not the valleys between them. Even extension of its log, so its transform is the real
  // cepstrum.
  const int half_width = static_cast<int>(0.5 * f0_hz * kNfft / sample_rate);
  std::vector<float> sym(static_cast<size_t>(kNfft), 0.0f);
  for (int b = 0; b < fft.n_bins(); ++b) {
    float peak = 0.0f;
    for (int k = std::max(0, b - half_width); k <= std::min(fft.n_bins() - 1, b + half_width);
         ++k) {
      peak = std::max(peak, std::abs(spec[static_cast<size_t>(k)]));
    }
    const float v = std::log(peak + 1.0e-9f);
    sym[static_cast<size_t>(b)] = v;
    if (b > 0 && b < kNfft / 2) sym[static_cast<size_t>(kNfft - b)] = v;
  }
  std::vector<std::complex<float>> ceps(static_cast<size_t>(fft.n_bins()));
  fft.forward(sym.data(), ceps.data());
  for (int q = 0; q < fft.n_bins(); ++q) {
    // Half-Hann taper: a rectangular cut rings at the lifter's own quefrency.
    const float taper =
        q >= lifter ? 0.0f
                    : 0.5f + 0.5f * std::cos(sonare::constants::kPi * static_cast<float>(q) /
                                             static_cast<float>(lifter));
    ceps[static_cast<size_t>(q)] *= taper;
  }
  std::vector<float> smooth(static_cast<size_t>(kNfft), 0.0f);
  fft.inverse(ceps.data(), smooth.data());

  const double bin_hz = static_cast<double>(sample_rate) / kNfft;
  std::vector<std::pair<float, double>> peaks;
  const int lo = static_cast<int>(250.0 / bin_hz);
  const int hi = static_cast<int>(5000.0 / bin_hz);
  for (int b = lo + 1; b < hi; ++b) {
    const float v = smooth[static_cast<size_t>(b)];
    if (v > smooth[static_cast<size_t>(b - 1)] && v >= smooth[static_cast<size_t>(b + 1)]) {
      peaks.emplace_back(v, b * bin_hz);
    }
  }
  REQUIRE(peaks.size() >= 2);
  std::sort(peaks.begin(), peaks.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  std::array<double, 2> hz = {peaks[0].second, peaks[1].second};
  std::sort(hz.begin(), hz.end());
  return hz;
}

struct AbsoluteMeasurement {
  std::array<double, 2> input_peaks;
  std::array<double, 2> output_peaks;
  double f0_ratio;
};

AbsoluteMeasurement measure(int sample_rate, double f0, float semitones, float factor,
                            FormantMode mode) {
  const int samples = sample_rate * 3 / 2;
  const sonare::Audio input =
      sonare::Audio::from_vector(three_formant_vowel(f0, sample_rate, samples), sample_rate);
  VoiceChangerConfig config;
  config.pitch_semitones = semitones;
  config.formant_factor = factor;
  config.formant_mode = mode;
  const sonare::Audio output = VoiceChanger(config).process(input);
  REQUIRE(output.size() > static_cast<size_t>(samples) / 2);

  // A steady stretch clear of both ends and of the frame edges.
  const size_t segment_len = static_cast<size_t>(sample_rate) / 2;
  const size_t start = static_cast<size_t>(sample_rate) / 3;
  const std::vector<float> in_seg(input.data() + start, input.data() + start + segment_len);
  const std::vector<float> out_seg(output.data() + start, output.data() + start + segment_len);

  // The lifter keeps structure coarser than half the lowest pitch period involved.
  const double f0_low = f0 * std::min(1.0, std::exp2(static_cast<double>(semitones) / 12.0));
  const int lifter = static_cast<int>(0.6 * sample_rate / f0_low);
  AbsoluteMeasurement m{};
  m.input_peaks = envelope_peaks(in_seg, sample_rate, f0_low, lifter);
  m.output_peaks = envelope_peaks(out_seg, sample_rate, f0_low, lifter);
  const double in_f0 = dominant_frequency(in_seg, sample_rate, 70.0f, 400.0f);
  const double out_f0 = dominant_frequency(out_seg, sample_rate, 70.0f, 400.0f);
  m.f0_ratio = out_f0 / in_f0;
  return m;
}

}  // namespace

TEST_CASE("Absolute formant mode places the envelope peaks at input times the factor",
          "[voice_changer][formant_mode]") {
  struct Case {
    int sample_rate;
    float semitones;
    float factor;
  };
  const Case cases[] = {{48000, 4.0f, 1.0f},  {48000, -5.0f, 1.0f}, {48000, 4.0f, 1.2f},
                        {48000, -5.0f, 0.9f}, {44100, 4.0f, 1.0f},  {44100, 4.0f, 1.2f},
                        {22050, -5.0f, 1.0f}, {22050, 4.0f, 1.2f}};
  // The envelope is sampled at the harmonics, so a peak position is good to about half a
  // harmonic spacing (62 Hz of a 600 Hz formant, 10%) before the cepstral smoothing; the same
  // estimator reads input and output, and the worst case seen is under 3%.
  constexpr double kPeakTolerance = 0.05;
  constexpr double kF0Tolerance = 0.03;
  for (const Case& c : cases) {
    CAPTURE(c.sample_rate, c.semitones, c.factor);
    const AbsoluteMeasurement m =
        measure(c.sample_rate, 125.0, c.semitones, c.factor, FormantMode::Absolute);
    for (size_t k = 0; k < 2; ++k) {
      CAPTURE(k, m.input_peaks[k], m.output_peaks[k]);
      REQUIRE_THAT(m.output_peaks[k] / m.input_peaks[k],
                   WithinRel(static_cast<double>(c.factor), kPeakTolerance));
    }
    REQUIRE_THAT(m.f0_ratio,
                 WithinRel(std::exp2(static_cast<double>(c.semitones) / 12.0), kF0Tolerance));
  }
}

TEST_CASE("Relative formant mode still moves the formants with the pitch",
          "[voice_changer][formant_mode]") {
  // Relative factor 1 applies no warp, so the envelope rides the resampling ratio.
  const AbsoluteMeasurement m = measure(48000, 125.0, 4.0f, 1.0f, FormantMode::Relative);
  const double ratio = std::exp2(4.0 / 12.0);
  for (size_t k = 0; k < 2; ++k) {
    REQUIRE_THAT(m.output_peaks[k] / m.input_peaks[k], WithinRel(ratio, 0.05));
  }
}

TEST_CASE("Absolute mode warps the unshifted voice by the factor over the pitch ratio",
          "[voice_changer][formant_mode]") {
  constexpr int sample_rate = 22050;
  const sonare::Audio audio = sonare::Audio::from_vector(
      three_formant_vowel(150.0, sample_rate, sample_rate / 2), sample_rate);

  VoiceChangerConfig absolute;
  absolute.pitch_semitones = 3.0f;
  absolute.formant_factor = 1.1f;
  absolute.formant_mode = FormantMode::Absolute;
  const sonare::Audio a = VoiceChanger(absolute).process(audio);

  // The same chain written out: warp by 1.1 / 2^(3/12) with the rate-sized LPC order and
  // time-defined frame, then shift.
  FormantWarpConfig warp_config;
  warp_config.factor = static_cast<float>(1.1 / std::exp2(3.0 / 12.0));
  warp_config.lpc_order = 0;
  warp_config.frame_in_time = true;
  const sonare::Audio warped = FormantWarp(warp_config).process(audio);
  const sonare::Audio expected = sonare::pitch_shift(warped, 3.0f, sonare::PitchShiftConfig{});
  REQUIRE(a.size() == expected.size());
  REQUIRE(std::equal(a.data(), a.data() + a.size(), expected.data()));

  // Relative stays the default: the warp follows the shift, with the fixed LPC order.
  VoiceChangerConfig defaults;
  defaults.pitch_semitones = 3.0f;
  defaults.formant_factor = 1.1f;
  VoiceChangerConfig explicit_relative = defaults;
  explicit_relative.formant_mode = FormantMode::Relative;
  const sonare::Audio d = VoiceChanger(defaults).process(audio);
  const sonare::Audio e = VoiceChanger(explicit_relative).process(audio);
  const sonare::Audio shifted = sonare::pitch_shift(audio, 3.0f, sonare::PitchShiftConfig{});
  const sonare::Audio relative_expected =
      FormantWarp(FormantWarpConfig{1.1f, 12, 1.0f}).process(shifted);
  REQUIRE(std::equal(d.data(), d.data() + d.size(), e.data()));
  REQUIRE(std::equal(d.data(), d.data() + d.size(), relative_expected.data()));
}

TEST_CASE("Absolute mode at no shift and unity factor is the identity",
          "[voice_changer][formant_mode]") {
  constexpr int sample_rate = 22050;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(220.0f, sample_rate, 4096), sample_rate);
  VoiceChangerConfig config;
  config.formant_mode = FormantMode::Absolute;
  const sonare::Audio out = VoiceChanger(config).process(audio);
  REQUIRE(out.size() == audio.size());
  REQUIRE(std::equal(out.data(), out.data() + out.size(), audio.data()));
}

TEST_CASE("Absolute mode refuses a factor the warp cannot reach and names the range",
          "[voice_changer][formant_mode]") {
  constexpr int sample_rate = 22050;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(220.0f, sample_rate, 4096), sample_rate);

  VoiceChangerConfig config;
  config.formant_mode = FormantMode::Absolute;
  config.pitch_semitones = 4.0f;
  config.formant_factor = 2.5f;
  // 2^(4/12) = 1.2599, so the warp range [0.55, 1.65] reaches [0.693, 2.079].
  try {
    VoiceChanger(config).process(audio);
    FAIL("expected a refusal");
  } catch (const sonare::SonareException& e) {
    REQUIRE(e.code() == sonare::ErrorCode::InvalidParameter);
    REQUIRE_THAT(std::string(e.what()), ContainsSubstring("[0.693, 2.079]"));
    REQUIRE_THAT(std::string(e.what()), ContainsSubstring("4 semitones"));
  }

  // Preserving formants (factor 1) at -9 semitones needs a warp of 1.68, past the range.
  config.pitch_semitones = -9.0f;
  config.formant_factor = 1.0f;
  REQUIRE_THROWS_AS(VoiceChanger(config).process(audio), sonare::SonareException);

  // The same request is accepted in relative mode, where the factor is clamped instead.
  config.formant_mode = FormantMode::Relative;
  REQUIRE_NOTHROW(VoiceChanger(config).process(audio));

  config.formant_mode = FormantMode::Absolute;
  config.pitch_semitones = 0.0f;
  config.formant_factor = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS_AS(VoiceChanger(config).process(audio), sonare::SonareException);
}

TEST_CASE("Formant mode names parse and print", "[voice_changer][formant_mode]") {
  REQUIRE(parse_formant_mode("relative") == FormantMode::Relative);
  REQUIRE(parse_formant_mode("absolute") == FormantMode::Absolute);
  REQUIRE_THROWS_AS(parse_formant_mode("Absolute"), sonare::SonareException);
  REQUIRE_THROWS_AS(parse_formant_mode(""), sonare::SonareException);
  REQUIRE(std::string(formant_mode_name(FormantMode::Relative)) == "relative");
  REQUIRE(std::string(formant_mode_name(FormantMode::Absolute)) == "absolute");
}

TEST_CASE("sonare_voice_change_ex selects the formant mode",
          "[voice_changer][formant_mode][c_api]") {
  constexpr int sample_rate = 22050;
  const std::vector<float> samples = three_formant_vowel(150.0, sample_rate, sample_rate / 2);

  auto run = [&](const SonareVoiceChangeConfig* config, std::vector<float>* result) {
    float* out = nullptr;
    size_t out_length = 0;
    const SonareError err = sonare_voice_change_ex(samples.data(), samples.size(), sample_rate,
                                                   config, &out, &out_length);
    if (err == SONARE_OK)
      result->assign(out, out + out_length);
    else
      REQUIRE(out == nullptr);
    sonare_free_floats(out);
    return err;
  };

  SonareVoiceChangeConfig config{};
  config.struct_version = 1;
  config.pitch_semitones = 3.0f;
  config.formant_factor = 1.1f;
  config.formant_mode = SONARE_FORMANT_MODE_RELATIVE;

  // Relative mode is what the positional call has always computed.
  float* legacy = nullptr;
  size_t legacy_length = 0;
  REQUIRE(sonare_voice_change(samples.data(), samples.size(), sample_rate, 3.0f, 1.1f, &legacy,
                              &legacy_length) == SONARE_OK);
  std::vector<float> relative;
  REQUIRE(run(&config, &relative) == SONARE_OK);
  REQUIRE(relative.size() == legacy_length);
  REQUIRE(std::equal(relative.begin(), relative.end(), legacy));
  sonare_free_floats(legacy);

  config.formant_mode = SONARE_FORMANT_MODE_ABSOLUTE;
  std::vector<float> absolute;
  REQUIRE(run(&config, &absolute) == SONARE_OK);
  REQUIRE(absolute.size() == relative.size());
  REQUIRE_FALSE(std::equal(absolute.begin(), absolute.end(), relative.begin()));

  // Unreachable: refused, with the reachable range in the message.
  config.formant_factor = 5.0f;
  std::vector<float> none;
  REQUIRE(run(&config, &none) == SONARE_ERROR_INVALID_PARAMETER);
  // 2^(3/12) = 1.1892, so the warp range reaches [0.6541, 1.962] at +3 semitones.
  REQUIRE_THAT(std::string(sonare_last_error_message()), ContainsSubstring("[0.6541, 1.962]"));

  config.formant_factor = 1.1f;
  config.formant_mode = 2;
  REQUIRE(run(&config, &none) == SONARE_ERROR_INVALID_PARAMETER);
  config.formant_mode = SONARE_FORMANT_MODE_ABSOLUTE;
  config.struct_version = 2;
  REQUIRE(run(&config, &none) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(run(nullptr, &none) == SONARE_ERROR_INVALID_PARAMETER);
}
