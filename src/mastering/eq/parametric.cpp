#include "mastering/eq/parametric.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/biquad_design.h"
#include "mastering/common/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::mastering::eq {
namespace {

using sonare::constants::kPiD;

float safe_q(float q) {
  if (!(q > 0.0f)) {
    throw std::invalid_argument("EQ band Q must be positive");
  }
  return std::max(q, 1.0e-6f);
}

}  // namespace

void ParametricEq::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  reset();

  for (size_t i = 0; i < kMaxBands; ++i) {
    update_coefficients(i);
  }
}

void ParametricEq::prepare_channels(int num_channels) {
  if (num_channels < 0) {
    throw std::invalid_argument("num_channels must be non-negative");
  }
  num_channels_ = num_channels;
  for (auto& band_states : states_) {
    band_states.resize(static_cast<size_t>(num_channels));
  }
}

void ParametricEq::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared();
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }

  if (num_channels_ < num_channels) {
    prepare_channels(num_channels);
  } else if (num_channels_ != num_channels && num_channels_ == 0) {
    num_channels_ = num_channels;
    for (auto& band_states : states_) {
      band_states.resize(static_cast<size_t>(num_channels));
    }
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }

  for (size_t band_index = 0; band_index < kMaxBands; ++band_index) {
    if (!bands_[band_index].enabled) {
      continue;
    }

    const auto c = coefficients_[band_index];
    auto& band_states = states_[band_index];
    for (int ch = 0; ch < num_channels; ++ch) {
      auto& state = band_states[static_cast<size_t>(ch)];
      float* samples = channels[ch];
      for (int i = 0; i < num_samples; ++i) {
        const float x = samples[i];
        const float y = c.b0 * x + state.z1;
        state.z1 = c.b1 * x - c.a1 * y + state.z2;
        state.z2 = c.b2 * x - c.a2 * y;
        samples[i] = y;
      }
    }
  }
}

void ParametricEq::reset() {
  for (auto& band_states : states_) {
    for (auto& state : band_states) {
      state = {};
    }
  }
}

void ParametricEq::set_band(size_t index, const EqBand& band) {
  validate_band_index(index);
  bands_[index] = band;
  update_coefficients(index);
}

bool ParametricEq::set_parameter(unsigned int param_id, float value) {
  const size_t band_index = param_id / 3u;
  if (band_index >= kMaxBands) {
    return false;
  }
  EqBand band = bands_[band_index];
  switch (param_id % 3u) {
    case 0:
      // Clamp to the open interval (0 Hz, Nyquist) so coefficient design never
      // throws on the audio thread.
      band.frequency_hz =
          std::clamp(value, 1.0e-3f, static_cast<float>(sample_rate_ * 0.5) - 1.0e-3f);
      break;
    case 1:
      band.gain_db = value;
      break;
    case 2:
      band.q = std::max(value, 1.0e-6f);
      break;
    default:
      return false;
  }
  // set_band recomputes only this band's coefficients without touching state.
  set_band(band_index, band);
  return true;
}

void ParametricEq::clear_band(size_t index) {
  validate_band_index(index);
  bands_[index] = {};
  coefficients_[index] = {};
  if (index < states_.size()) {
    for (auto& state : states_[index]) {
      state = {};
    }
  }
}

void ParametricEq::clear() {
  for (size_t i = 0; i < kMaxBands; ++i) {
    clear_band(i);
  }
}

const EqBand& ParametricEq::band(size_t index) const {
  validate_band_index(index);
  return bands_[index];
}

ParametricEq::Coefficients ParametricEq::make_coefficients(const EqBand& band, double sample_rate) {
  if (!band.enabled) {
    return {};
  }
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (!(band.frequency_hz > 0.0f) || !(band.frequency_hz < static_cast<float>(sample_rate * 0.5))) {
    throw std::invalid_argument("EQ band frequency must be between 0 Hz and Nyquist");
  }

  const double q = safe_q(band.q);
  const double w0 = 2.0 * kPiD * static_cast<double>(band.frequency_hz) / sample_rate;
  const float w0f = static_cast<float>(w0);
  const float qf = static_cast<float>(q);
  const auto from_common = [](const common::BiquadCoeffs& c) {
    return Coefficients{c.b0, c.b1, c.b2, c.a1, c.a2};
  };
  if (band.slope_db_oct == 6) {
    if (band.type == EqBandType::LowPass) {
      return from_common(common::first_order_lowpass(w0f));
    }
    if (band.type == EqBandType::HighPass) {
      return from_common(common::first_order_highpass(w0f));
    }
  }

  if (band.coeff_mode == BiquadCoeffMode::Vicanek) {
    switch (band.type) {
      case EqBandType::Peak:
        return from_common(common::vicanek_peak(w0f, qf, band.gain_db));
      case EqBandType::LowPass:
        return from_common(common::vicanek_lowpass(w0f, qf));
      case EqBandType::HighPass:
        return from_common(common::vicanek_highpass(w0f, qf));
      case EqBandType::BandPass:
        return from_common(common::vicanek_bandpass(w0f, qf));
      case EqBandType::Notch:
        return from_common(common::vicanek_notch(w0f, qf));
      case EqBandType::LowShelf:
        return from_common(common::vicanek_low_shelf(w0f, band.gain_db));
      case EqBandType::HighShelf:
        return from_common(common::vicanek_high_shelf(w0f, band.gain_db));
      case EqBandType::TiltShelf:
      case EqBandType::FlatTilt:
        throw std::invalid_argument("unsupported Vicanek EQ band type");
    }
  }

  switch (band.type) {
    case EqBandType::Peak:
      return from_common(common::rbj_peak(w0f, qf, band.gain_db));

    case EqBandType::LowPass:
      return from_common(common::rbj_lowpass(w0f, qf));

    case EqBandType::HighPass:
      return from_common(common::rbj_highpass(w0f, qf));

    case EqBandType::BandPass:
      return from_common(common::rbj_bandpass(w0f, qf));

    case EqBandType::Notch:
      return from_common(common::rbj_notch(w0f, qf));

    case EqBandType::LowShelf:
      return from_common(common::rbj_low_shelf(w0f, qf, band.gain_db));

    case EqBandType::HighShelf:
      return from_common(common::rbj_high_shelf(w0f, qf, band.gain_db));

    case EqBandType::TiltShelf:
    case EqBandType::FlatTilt:
      throw std::invalid_argument("unsupported EQ band type");
  }

  return {};
}

void ParametricEq::update_coefficients(size_t index) {
  validate_band_index(index);
  coefficients_[index] = make_coefficients(bands_[index], sample_rate_);
}

void ParametricEq::validate_band_index(size_t index) {
  if (index >= kMaxBands) {
    throw std::out_of_range("EQ band index out of range");
  }
}

void ParametricEq::ensure_prepared() const {
  if (!prepared_) {
    throw std::logic_error("ParametricEq must be prepared before processing");
  }
}

}  // namespace sonare::mastering::eq
