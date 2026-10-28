#include "voicelab/synthetic_voice.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>

#include "util/constants.h"

namespace sonare::voicelab {

using constants::kCentsPerOctave;
using constants::kPiD;
using constants::kTwoPiD;

namespace {

constexpr int kVowelCount = 5;
// Peterson & Barney (1952) adult-male means: /a/ = "hod", /e/ = "head", /i/ = "heed",
// /o/ = "hawed", /u/ = "who'd".
constexpr std::array<std::array<float, 3>, kVowelCount> kVowelFormantsHz = {{
    {730.0f, 1090.0f, 2440.0f},
    {530.0f, 1840.0f, 2480.0f},
    {270.0f, 2290.0f, 3010.0f},
    {570.0f, 840.0f, 2410.0f},
    {300.0f, 870.0f, 2240.0f},
}};
// F1..F3 bandwidths, then the vowel-independent F4 / F5 (centre, bandwidth) of a male tract.
constexpr std::array<double, 3> kFormantBandwidthsHz = {80.0, 100.0, 120.0};
constexpr std::array<std::array<double, 2>, 2> kUpperFormantsHz = {
    {{3500.0, 150.0}, {4500.0, 200.0}}};
// Glottal source tilt with lip radiation: a one-pole lowpass, -6 dB/octave above the corner.
constexpr double kSourceCornerHz = 150.0;
// The envelope is all-pole on a fixed 11.025 kHz prototype axis, so it is the same function of
// Hz at every output rate and an order-12 all-pole fit can represent it exactly below 5.5 kHz.
constexpr double kEnvelopeRate = 11025.0;
// Above the prototype Nyquist the envelope continues from its edge value at -12 dB/octave.
constexpr double kEnvelopeRolloffPower = 2.0;
// Harmonics fade out between these fractions of Nyquist, so a glide never switches one abruptly.
constexpr double kTaperStartNyquist = 0.85;
constexpr double kTaperEndNyquist = 0.95;
// Samples between two evaluations of the harmonic amplitude table (linearly interpolated).
constexpr int kAmplitudeBlock = 32;
constexpr std::uint32_t kNoiseSeed = 0x5eed1234u;

/// Magnitude of a unity-DC-gain two-pole resonator on the prototype axis at angular frequency w.
double resonator_magnitude(double centre_hz, double bandwidth_hz, double w) {
  const double r = std::exp(-kPiD * bandwidth_hz / kEnvelopeRate);
  const double b = 2.0 * r * std::cos(kTwoPiD * centre_hz / kEnvelopeRate);
  const double c = -r * r;
  const std::complex<double> z1 = std::polar(1.0, -w);
  const std::complex<double> den = 1.0 - b * z1 - c * z1 * z1;
  return (1.0 - b - c) / std::abs(den);
}

/// Spectral envelope (source tilt x five resonances, then the output-rate taper) at f_hz.
double envelope(const std::array<float, 3>& formants, double f_hz, double sr) {
  const double nyquist = 0.5 * sr;
  if (f_hz >= kTaperEndNyquist * nyquist) return 0.0;
  const double edge_hz = 0.5 * kEnvelopeRate;
  const double w = kTwoPiD * std::min(f_hz, edge_hz) / kEnvelopeRate;
  const double pole = std::exp(-kTwoPiD * kSourceCornerHz / kEnvelopeRate);
  double g = (1.0 - pole) / std::abs(1.0 - pole * std::polar(1.0, -w));
  for (int i = 0; i < 3; ++i) {
    g *= resonator_magnitude(formants[static_cast<size_t>(i)],
                             kFormantBandwidthsHz[static_cast<size_t>(i)], w);
  }
  for (const auto& upper : kUpperFormantsHz) g *= resonator_magnitude(upper[0], upper[1], w);
  if (f_hz > edge_hz) g *= std::pow(edge_hz / f_hz, kEnvelopeRolloffPower);
  if (f_hz > kTaperStartNyquist * nyquist) {
    const double u =
        (f_hz - kTaperStartNyquist * nyquist) / ((kTaperEndNyquist - kTaperStartNyquist) * nyquist);
    g *= 0.5 * (1.0 + std::cos(kPiD * u));
  }
  return g;
}

/// Harmonic amplitudes at f0, scaled so the harmonic sum has the given RMS.
void amplitude_table(const std::array<float, 3>& formants, double f0, double sr, double rms,
                     std::vector<double>* table) {
  const int count = static_cast<int>(std::floor(kTaperEndNyquist * 0.5 * sr / f0)) + 1;
  table->assign(static_cast<size_t>(count), 0.0);
  double power = 0.0;
  for (int k = 1; k < count; ++k) {
    const double a = envelope(formants, k * f0, sr);
    (*table)[static_cast<size_t>(k)] = a;
    power += 0.5 * a * a;
  }
  const double scale = power > 0.0 ? rms / std::sqrt(power) : 0.0;
  for (double& a : *table) a *= scale;
}

/// Instantaneous f0 of a segment at time t: exponential glide with sinusoidal vibrato.
double segment_f0(const VoiceSegment& s, double t) {
  const double u = s.duration_s > 0.0 ? t / s.duration_s : 0.0;
  const double glide = static_cast<double>(s.f0_start_hz) *
                       std::pow(static_cast<double>(s.f0_end_hz) / s.f0_start_hz, u);
  const double vib = s.vibrato_cents / kCentsPerOctave * std::sin(kTwoPiD * s.vibrato_hz * t);
  return glide * std::exp2(vib);
}

}  // namespace

std::array<float, 3> vowel_formants_hz(int vowel) {
  const int index = std::clamp(vowel, 0, kVowelCount - 1);
  return kVowelFormantsHz[static_cast<size_t>(index)];
}

std::vector<float> synthesize_voice(const std::vector<VoiceSegment>& segments, double sr,
                                    std::vector<float>* true_f0_per_sample) {
  double total_s = 0.0;
  for (const VoiceSegment& s : segments) total_s += std::max(0.0, s.duration_s);
  const auto total = static_cast<std::size_t>(std::llround(total_s * sr));
  std::vector<float> out(total, 0.0f);
  if (true_f0_per_sample != nullptr) true_f0_per_sample->assign(total, 0.0f);

  std::mt19937 rng(kNoiseSeed);
  double phase = 0.0;
  double elapsed_s = 0.0;
  std::size_t begin = 0;
  std::vector<double> amp_a;
  std::vector<double> amp_b;
  for (const VoiceSegment& s : segments) {
    const double duration = std::max(0.0, s.duration_s);
    elapsed_s += duration;
    const std::size_t end = std::min(total, static_cast<std::size_t>(std::llround(elapsed_s * sr)));
    const std::size_t len = end - begin;
    const double rms = std::pow(10.0, s.level_db / 20.0);
    const double ramp = std::min(static_cast<double>(s.ramp_ms) * 1e-3 * sr, 0.5 * len);
    const std::array<float, 3> formants = vowel_formants_hz(s.vowel);

    if (s.voiced) amplitude_table(formants, segment_f0(s, 0.0), sr, rms, &amp_b);
    for (std::size_t block = 0; block < len; block += kAmplitudeBlock) {
      const std::size_t block_end = std::min(len, block + kAmplitudeBlock);
      if (s.voiced) {
        amp_a.swap(amp_b);
        amplitude_table(formants, segment_f0(s, block_end / sr), sr, rms, &amp_b);
        const std::size_t size = std::max(amp_a.size(), amp_b.size());
        amp_a.resize(size, 0.0);
        amp_b.resize(size, 0.0);
      }
      for (std::size_t j = block; j < block_end; ++j) {
        const double t = j / sr;
        double gain = 1.0;
        if (ramp > 0.0) {
          const double edge = std::min(static_cast<double>(j), static_cast<double>(len - 1 - j));
          if (edge < ramp) gain = 0.5 * (1.0 - std::cos(kPiD * edge / ramp));
        }
        double y = 0.0;
        if (s.voiced) {
          const double f0 = segment_f0(s, t);
          const double frac = static_cast<double>(j - block) / kAmplitudeBlock;
          // sin(k*phase) by the Chebyshev recurrence s_{k+1} = 2 cos(phase) s_k - s_{k-1}.
          const double c2 = 2.0 * std::cos(phase);
          double prev = 0.0;
          double cur = std::sin(phase);
          for (std::size_t k = 1; k < amp_a.size(); ++k) {
            y += (amp_a[k] + frac * (amp_b[k] - amp_a[k])) * cur;
            const double next = c2 * cur - prev;
            prev = cur;
            cur = next;
          }
          phase = std::fmod(phase + kTwoPiD * f0 / sr, kTwoPiD);
          if (true_f0_per_sample != nullptr) {
            (*true_f0_per_sample)[begin + j] = static_cast<float>(f0);
          }
        } else {
          // Uniform white noise in [-1, 1) has RMS 1/sqrt(3).
          const double u = static_cast<double>(rng()) / 4294967296.0 * 2.0 - 1.0;
          y = u * std::sqrt(3.0) * rms;
        }
        out[begin + j] = static_cast<float>(gain * y);
      }
    }
    begin = end;
  }
  return out;
}

}  // namespace sonare::voicelab
