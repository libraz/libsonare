#include "editing/voice_changer/channel_vocoder.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "util/constants.h"

namespace sonare::editing::voice_changer {

namespace {

using constants::kButterworthQ;
using constants::kCentsPerOctave;
using constants::kHalfPi;
using constants::kSqrt2D;
using constants::kTwoPiD;

constexpr double kBandLowHz = 80.0;
constexpr double kBandHighHz = 10000.0;
constexpr double kSibilanceCornerHz = 6000.0;
constexpr double kMsPerSecond = 1000.0;
// Glasberg & Moore ERB-rate scale: E(f) = kErbScale * log10(1 + kErbSlope * f).
constexpr double kErbScale = 21.4;
constexpr double kErbSlope = 0.00437;
constexpr float kDenormalFloor = 1.0e-20f;

double hz_to_erb(double hz) { return kErbScale * std::log10(1.0 + kErbSlope * hz); }
double erb_to_hz(double erb) { return (std::pow(10.0, erb / kErbScale) - 1.0) / kErbSlope; }

std::complex<double> unit_delay(double omega, int n) {
  return std::polar(1.0, -omega * static_cast<double>(n));
}

std::complex<double> response(const rt::BiquadCoeffs& c, double omega) {
  const auto z1 = unit_delay(omega, 1);
  const auto z2 = unit_delay(omega, 2);
  const double b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
  return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
}

/// Group delay in samples of one section: Re(S_B / B) - Re(S_A / A), with S = sum n * p_n * z^-n.
double section_group_delay(const rt::BiquadCoeffs& c, double omega) {
  const auto z1 = unit_delay(omega, 1);
  const auto z2 = unit_delay(omega, 2);
  const double b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
  const std::complex<double> num = b0 + b1 * z1 + b2 * z2;
  const std::complex<double> num_s = b1 * z1 + 2.0 * b2 * z2;
  const std::complex<double> den = 1.0 + a1 * z1 + a2 * z2;
  const std::complex<double> den_s = a1 * z1 + 2.0 * a2 * z2;
  return (num_s / num).real() - (den_s / den).real();
}

float poly_blep(float t, float dt) noexcept {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0f;
  }
  if (t > 1.0f - dt) {
    t = (t - 1.0f) / dt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

float saw_sample(float t, float dt) noexcept { return 2.0f * t - 1.0f - poly_blep(t, dt); }

float pulse_sample(float t, float dt) noexcept {
  float shifted = t + 0.5f;
  if (shifted >= 1.0f) shifted -= 1.0f;
  return (t < 0.5f ? 1.0f : -1.0f) + poly_blep(t, dt) - poly_blep(shifted, dt);
}

float one_pole_coeff(float ms, double sr) {
  return static_cast<float>(1.0 - std::exp(-kMsPerSecond / (static_cast<double>(ms) * sr)));
}

}  // namespace

void ChannelVocoder::prepare(double sr, int max_block, int bands) {
  sr_ = sr;
  bands_ = std::clamp(bands, 8, kMaxBands);
  carrier_.assign(static_cast<std::size_t>(std::max(max_block, 1)), 0.0f);
  design_bank();

  const double corner =
      std::min(kSibilanceCornerHz, static_cast<double>(kMaxDesignFrequencyRatio) * sr_);
  sibilance_corner_hz_ = static_cast<float>(corner);
  sibilance_hp_.set(rt::rbj_highpass(static_cast<float>(kTwoPiD * corner / sr_), kButterworthQ));

  config_ = VocoderConfig{};
  formant_alpha_ = 1.0f;
  attack_coeff_ = one_pole_coeff(config_.attack_ms, sr_);
  release_coeff_ = one_pole_coeff(config_.release_ms, sr_);
  update_formant_map();
  reset();
}

void ChannelVocoder::design_bank() {
  const double high_hz = std::min(kBandHighHz, static_cast<double>(kMaxDesignFrequencyRatio) * sr_);
  const double erb_lo = hz_to_erb(kBandLowHz);
  const double erb_step = (hz_to_erb(high_hz) - erb_lo) / bands_;
  // Two identical sections reach -3 dB at the band edges when each has Q = 0.6436 * fc / width.
  const double cascade_q = std::sqrt(kSqrt2D - 1.0);

  for (int k = 0; k < bands_; ++k) {
    Band& band = band_[static_cast<std::size_t>(k)];
    const double low = erb_to_hz(erb_lo + k * erb_step);
    const double high = erb_to_hz(erb_lo + (k + 1) * erb_step);
    const double centre = erb_to_hz(erb_lo + (k + 0.5) * erb_step);
    const double omega = kTwoPiD * centre / sr_;
    const float q = static_cast<float>(cascade_q * centre / (high - low));

    band.centre_hz = static_cast<float>(centre);
    band.low_edge_hz = static_cast<float>(low);
    band.high_edge_hz = static_cast<float>(high);
    for (int s = 0; s < kStages; ++s) {
      rt::BiquadCoeffs c = rt::rbj_bandpass(static_cast<float>(omega), q);
      // Peak gain 1 at the centre as realized in float coefficients.
      const float peak = static_cast<float>(std::abs(response(c, omega)));
      c.b0 /= peak;
      c.b1 /= peak;
      c.b2 /= peak;
      band.coeffs[s] = c;
    }
  }

  const Band& lowest = band_[0];
  const double omega0 = kTwoPiD * static_cast<double>(lowest.centre_hz) / sr_;
  double delay = 0.0;
  for (int s = 0; s < kStages; ++s) delay += section_group_delay(lowest.coeffs[s], omega0);
  group_delay_ = static_cast<int>(std::lround(delay));
}

void ChannelVocoder::reset() {
  for (int k = 0; k < bands_; ++k) {
    Band& band = band_[static_cast<std::size_t>(k)];
    for (int s = 0; s < kStages; ++s) {
      band.modulator[s].set(band.coeffs[s]);
      band.modulator[s].reset();
      band.carrier[s].set(band.coeffs[s]);
      band.carrier[s].reset();
    }
    band.envelope = 0.0f;
  }
  sibilance_hp_.reset();
  for (auto& voice : phase_) voice.fill(0.0);
}

void ChannelVocoder::set_config(const VocoderConfig& config, float formant_alpha) {
  config_ = config;
  formant_alpha_ = (std::isfinite(formant_alpha) && formant_alpha > 0.0f) ? formant_alpha : 1.0f;
  attack_coeff_ = one_pole_coeff(config_.attack_ms, sr_);
  release_coeff_ = one_pole_coeff(config_.release_ms, sr_);
  update_formant_map();
}

void ChannelVocoder::update_formant_map() {
  const int last = bands_ - 1;
  for (int k = 0; k < bands_; ++k) {
    const std::size_t ki = static_cast<std::size_t>(k);
    if (formant_alpha_ == 1.0f) {
      map_index_[ki] = k;
      map_frac_[ki] = 0.0f;
      continue;
    }
    // Source position on the modulator's log-frequency axis: the band centre divided by alpha.
    const double source = static_cast<double>(band_[ki].centre_hz) / formant_alpha_;
    if (source <= static_cast<double>(band_[0].centre_hz)) {
      map_index_[ki] = 0;
      map_frac_[ki] = 0.0f;
    } else if (source >= static_cast<double>(band_[static_cast<std::size_t>(last)].centre_hz)) {
      map_index_[ki] = last - 1;
      map_frac_[ki] = 1.0f;
    } else {
      int j = 0;
      while (j + 1 < last &&
             static_cast<double>(band_[static_cast<std::size_t>(j + 1)].centre_hz) <= source) {
        ++j;
      }
      const double lo = band_[static_cast<std::size_t>(j)].centre_hz;
      const double hi = band_[static_cast<std::size_t>(j + 1)].centre_hz;
      map_index_[ki] = j;
      map_frac_[ki] = static_cast<float>(std::log(source / lo) / std::log(hi / lo));
    }
  }
}

void ChannelVocoder::render_carrier(const VoiceTargets& notes, const float* external_carrier,
                                    int n) noexcept {
  float* carrier = carrier_.data();
  if (config_.carrier_source == VocoderCarrierSource::External) {
    if (external_carrier != nullptr) {
      std::copy(external_carrier, external_carrier + n, carrier);
    } else {
      std::fill(carrier, carrier + n, 0.0f);
    }
    return;
  }

  std::fill(carrier, carrier + n, 0.0f);
  const float max_hz = kMaxDesignFrequencyRatio * static_cast<float>(sr_);
  const int voices = std::min(notes.count, kMaxVoices);
  int active = 0;
  for (int i = 0; i < voices; ++i) {
    const VoiceTarget& v = notes.voices[i];
    if (v.active && std::isfinite(v.target_hz) && v.target_hz > 0.0f) ++active;
  }
  if (active == 0) return;

  const bool ensemble = config_.carrier_wave == VocoderCarrierWave::Ensemble;
  const int oscillators = ensemble ? kOscPerVoice : 1;
  const float amp = 1.0f / std::sqrt(static_cast<float>(active * oscillators));
  const float detune_ratio = std::exp2(config_.ensemble_detune_cents / kCentsPerOctave);
  const float ratios[kOscPerVoice] = {1.0f, detune_ratio, 1.0f / detune_ratio};

  for (int i = 0; i < voices; ++i) {
    const VoiceTarget& v = notes.voices[i];
    if (!(v.active && std::isfinite(v.target_hz) && v.target_hz > 0.0f)) continue;
    const float base_hz = std::min(v.target_hz, max_hz);
    for (int o = 0; o < oscillators; ++o) {
      const double inc = static_cast<double>(base_hz) * (ensemble ? ratios[o] : 1.0f) / sr_;
      const float dt = static_cast<float>(std::min(inc, 0.5));
      double phase = phase_[static_cast<std::size_t>(i)][static_cast<std::size_t>(o)];
      const bool pulse = config_.carrier_wave == VocoderCarrierWave::Pulse;
      for (int s = 0; s < n; ++s) {
        const float t = static_cast<float>(phase);
        carrier[s] += amp * (pulse ? pulse_sample(t, dt) : saw_sample(t, dt));
        phase += inc;
        phase -= std::floor(phase);
      }
      phase_[static_cast<std::size_t>(i)][static_cast<std::size_t>(o)] = phase;
    }
  }
}

void ChannelVocoder::process(const float* modulator, const float* external_carrier,
                             const VoiceTargets& notes, float* out, int n) noexcept {
  const int max_block = static_cast<int>(carrier_.size());
  for (int start = 0; start < n; start += max_block) {
    const int len = std::min(max_block, n - start);
    const float* mod = modulator + start;
    float* dst = out + start;
    render_carrier(notes, external_carrier != nullptr ? external_carrier + start : nullptr, len);

    for (int s = 0; s < len; ++s) {
      const float x = mod[s];
      const float carrier_in = carrier_[static_cast<std::size_t>(s)];

      for (int k = 0; k < bands_; ++k) {
        Band& band = band_[static_cast<std::size_t>(k)];
        const float m = band.modulator[1].process(band.modulator[0].process(x));
        const float rectified = std::abs(m) * kHalfPi;
        const float coeff = rectified > band.envelope ? attack_coeff_ : release_coeff_;
        band.envelope += coeff * (rectified - band.envelope);
        if (band.envelope < kDenormalFloor) band.envelope = 0.0f;
      }

      float sum = 0.0f;
      for (int k = 0; k < bands_; ++k) {
        Band& band = band_[static_cast<std::size_t>(k)];
        const std::size_t ki = static_cast<std::size_t>(k);
        const std::size_t j = static_cast<std::size_t>(map_index_[ki]);
        const float f = map_frac_[ki];
        const float env = f == 0.0f ? band_[j].envelope
                                    : (1.0f - f) * band_[j].envelope + f * band_[j + 1].envelope;
        const float c = band.carrier[1].process(band.carrier[0].process(carrier_in));
        sum += c * env;
      }

      const float sibilant = sibilance_hp_.process(x);
      dst[s] = config_.level * (sum + config_.sibilance * sibilant);
    }
  }
}

int ChannelVocoder::group_delay_samples() const noexcept { return group_delay_; }

}  // namespace sonare::editing::voice_changer
