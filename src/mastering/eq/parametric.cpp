#include "mastering/eq/parametric.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::eq {
namespace {

constexpr double kPi = 3.14159265358979323846;

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

void ParametricEq::process(float* const* channels, int num_channels, int num_samples) {
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

  if (num_channels_ != num_channels) {
    num_channels_ = num_channels;
    for (auto& band_states : states_) {
      band_states.assign(static_cast<size_t>(num_channels), {});
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
  const double w0 = 2.0 * kPi * static_cast<double>(band.frequency_hz) / sample_rate;
  const double cos_w0 = std::cos(w0);
  const double sin_w0 = std::sin(w0);
  const double alpha = sin_w0 / (2.0 * q);
  const double a = std::pow(10.0, static_cast<double>(band.gain_db) / 40.0);

  switch (band.type) {
    case EqBandType::Peak:
      return ParametricEq::normalize(1.0 + alpha * a, -2.0 * cos_w0, 1.0 - alpha * a,
                                     1.0 + alpha / a, -2.0 * cos_w0, 1.0 - alpha / a);

    case EqBandType::LowShelf: {
      const double sqrt_a = std::sqrt(a);
      const double two_sqrt_a_alpha = 2.0 * sqrt_a * alpha;
      return ParametricEq::normalize(a * ((a + 1.0) - (a - 1.0) * cos_w0 + two_sqrt_a_alpha),
                                     2.0 * a * ((a - 1.0) - (a + 1.0) * cos_w0),
                                     a * ((a + 1.0) - (a - 1.0) * cos_w0 - two_sqrt_a_alpha),
                                     (a + 1.0) + (a - 1.0) * cos_w0 + two_sqrt_a_alpha,
                                     -2.0 * ((a - 1.0) + (a + 1.0) * cos_w0),
                                     (a + 1.0) + (a - 1.0) * cos_w0 - two_sqrt_a_alpha);
    }

    case EqBandType::HighShelf: {
      const double sqrt_a = std::sqrt(a);
      const double two_sqrt_a_alpha = 2.0 * sqrt_a * alpha;
      return ParametricEq::normalize(a * ((a + 1.0) + (a - 1.0) * cos_w0 + two_sqrt_a_alpha),
                                     -2.0 * a * ((a - 1.0) + (a + 1.0) * cos_w0),
                                     a * ((a + 1.0) + (a - 1.0) * cos_w0 - two_sqrt_a_alpha),
                                     (a + 1.0) - (a - 1.0) * cos_w0 + two_sqrt_a_alpha,
                                     2.0 * ((a - 1.0) - (a + 1.0) * cos_w0),
                                     (a + 1.0) - (a - 1.0) * cos_w0 - two_sqrt_a_alpha);
    }

    case EqBandType::LowPass:
      return ParametricEq::normalize((1.0 - cos_w0) * 0.5, 1.0 - cos_w0, (1.0 - cos_w0) * 0.5,
                                     1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha);

    case EqBandType::HighPass:
      return ParametricEq::normalize((1.0 + cos_w0) * 0.5, -(1.0 + cos_w0), (1.0 + cos_w0) * 0.5,
                                     1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha);

    case EqBandType::BandPass:
      return ParametricEq::normalize(alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha);

    case EqBandType::Notch:
      return ParametricEq::normalize(1.0, -2.0 * cos_w0, 1.0, 1.0 + alpha, -2.0 * cos_w0,
                                     1.0 - alpha);
  }

  return {};
}

ParametricEq::Coefficients ParametricEq::normalize(double b0, double b1, double b2, double a0,
                                                   double a1, double a2) {
  if (!(std::abs(a0) > 0.0)) {
    throw std::runtime_error("invalid EQ coefficient normalization");
  }

  const double inv_a0 = 1.0 / a0;
  return {
      static_cast<float>(b0 * inv_a0), static_cast<float>(b1 * inv_a0),
      static_cast<float>(b2 * inv_a0), static_cast<float>(a1 * inv_a0),
      static_cast<float>(a2 * inv_a0),
  };
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
