#include "mastering/eq/linear_phase.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

#include "core/fft.h"
#include "core/window.h"
#include "mastering/common/biquad_design.h"
#include "util/constants.h"

namespace sonare::mastering::eq {
namespace {

bool is_power_of_two(int value) { return value > 0 && (value & (value - 1)) == 0; }

double clamp_frequency(double frequency_hz, double sample_rate) {
  return std::clamp(frequency_hz, 1.0, sample_rate * 0.5 - 1.0);
}

common::BiquadCoeffs design_band_biquad(const EqBand& band, double sample_rate) {
  const double center = clamp_frequency(band.frequency_hz, sample_rate);
  const float omega =
      static_cast<float>(sonare::constants::kTwoPi * center / static_cast<double>(sample_rate));
  const float q = std::max(band.q, 1.0e-6f);

  if (band.coeff_mode == BiquadCoeffMode::Vicanek) {
    switch (band.type) {
      case EqBandType::Peak:
        return common::vicanek_peak(omega, q, band.gain_db);
      case EqBandType::LowShelf:
        return common::vicanek_low_shelf(omega, band.gain_db);
      case EqBandType::HighShelf:
        return common::vicanek_high_shelf(omega, band.gain_db);
      case EqBandType::LowPass:
        return common::vicanek_lowpass(omega, q);
      case EqBandType::HighPass:
        return common::vicanek_highpass(omega, q);
      case EqBandType::BandPass:
        return common::vicanek_bandpass(omega, q);
      case EqBandType::Notch:
        return common::vicanek_notch(omega, q);
    }
  }

  switch (band.type) {
    case EqBandType::Peak:
      return common::rbj_peak(omega, q, band.gain_db);
    case EqBandType::LowShelf:
      return common::rbj_low_shelf(omega, q, band.gain_db);
    case EqBandType::HighShelf:
      return common::rbj_high_shelf(omega, q, band.gain_db);
    case EqBandType::LowPass:
      return common::rbj_lowpass(omega, q);
    case EqBandType::HighPass:
      return common::rbj_highpass(omega, q);
    case EqBandType::BandPass:
      return common::rbj_bandpass(omega, q);
    case EqBandType::Notch:
      return common::rbj_notch(omega, q);
  }

  return {};
}

}  // namespace

LinearPhaseEq::LinearPhaseEq(LinearPhaseEqConfig config) : config_(config) { validate_config(); }

void LinearPhaseEq::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  rebuild_kernel();
  reset();
}

void LinearPhaseEq::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("LinearPhaseEq must be prepared before processing");
  }
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }

  ensure_channel_state(num_channels);

  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    auto& state = states_[static_cast<size_t>(ch)];
    float* samples = channels[ch];
    const int partition_size = active_partition_size();
    if (state.convolver && partition_size > 0 && (num_samples % partition_size) == 0) {
      for (int offset = 0; offset < num_samples; offset += partition_size) {
        state.convolver->process_block(samples + offset, samples + offset);
      }
    } else {
      process_direct(samples, num_samples, state);
    }
  }
}

void LinearPhaseEq::reset() {
  for (auto& state : states_) {
    std::fill(state.history.begin(), state.history.end(), 0.0f);
    state.write_index = 0;
    if (state.convolver) state.convolver->reset();
  }
}

void LinearPhaseEq::set_band(size_t index, const EqBand& band) {
  validate_band_index(index);
  if (band.enabled) {
    if (!(band.frequency_hz > 0.0f) ||
        !(band.frequency_hz < static_cast<float>(sample_rate_ * 0.5))) {
      throw std::invalid_argument("EQ band frequency must be between 0 Hz and Nyquist");
    }
    if (!(band.q > 0.0f)) {
      throw std::invalid_argument("EQ band Q must be positive");
    }
  }

  bands_[index] = band;
  if (prepared_) {
    rebuild_kernel();
    reset();
  }
}

void LinearPhaseEq::clear_band(size_t index) {
  validate_band_index(index);
  bands_[index] = {};
  if (prepared_) {
    rebuild_kernel();
    reset();
  }
}

void LinearPhaseEq::clear() {
  for (auto& band : bands_) {
    band = {};
  }
  if (prepared_) {
    rebuild_kernel();
    reset();
  }
}

const EqBand& LinearPhaseEq::band(size_t index) const {
  validate_band_index(index);
  return bands_[index];
}

void LinearPhaseEq::rebuild_kernel() {
  validate_config();

  FFT fft(config_.fft_size);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  for (int bin = 0; bin < fft.n_bins(); ++bin) {
    const double frequency_hz = static_cast<double>(bin) * sample_rate_ / config_.fft_size;
    float magnitude = 1.0f;
    for (const auto& band : bands_) {
      if (band.enabled) {
        magnitude *= band_magnitude(band, frequency_hz, sample_rate_);
      }
    }
    spectrum[static_cast<size_t>(bin)] = {magnitude, 0.0f};
  }

  std::vector<float> zero_phase(static_cast<size_t>(config_.fft_size), 0.0f);
  fft.inverse(spectrum.data(), zero_phase.data());

  const size_t kernel_size = static_cast<size_t>(config_.kernel_size);
  kernel_.assign(kernel_size, 0.0f);
  const int half = config_.kernel_size / 2;
  for (int i = 0; i < config_.kernel_size; ++i) {
    const int source = (i - half + config_.fft_size) % config_.fft_size;
    kernel_[static_cast<size_t>(i)] =
        zero_phase[static_cast<size_t>(source)] * hann_value(i, config_.kernel_size);
  }

  latency_samples_ = half;
  ensure_channel_state(static_cast<int>(states_.size()));
}

void LinearPhaseEq::ensure_channel_state(int num_channels) {
  if (num_channels < 0) {
    throw std::invalid_argument("num_channels must be non-negative");
  }
  if (kernel_.empty()) {
    return;
  }
  if (states_.size() == static_cast<size_t>(num_channels)) {
    const int partition_size = active_partition_size();
    for (auto& state : states_) {
      if (config_.use_partitioned_convolution && partition_size > 0) {
        if (!state.convolver) {
          state.convolver = std::make_unique<common::PartitionedConvolver>(
              common::PartitionedConvolverConfig{partition_size});
        }
        state.convolver->set_impulse_response(kernel_);
      } else {
        state.convolver.reset();
      }
    }
    return;
  }

  states_.clear();
  states_.resize(static_cast<size_t>(num_channels));
  for (auto& state : states_) {
    state.history.assign(kernel_.size(), 0.0f);
    state.write_index = 0;
    const int partition_size = active_partition_size();
    if (config_.use_partitioned_convolution && partition_size > 0) {
      state.convolver = std::make_unique<common::PartitionedConvolver>(
          common::PartitionedConvolverConfig{partition_size});
      state.convolver->set_impulse_response(kernel_);
    }
  }
}

void LinearPhaseEq::process_direct(float* samples, int num_samples, ChannelState& state) const {
  const size_t kernel_size = kernel_.size();
  for (int i = 0; i < num_samples; ++i) {
    state.history[state.write_index] = samples[i];

    double y = 0.0;
    size_t read = state.write_index;
    for (size_t k = 0; k < kernel_size; ++k) {
      y += static_cast<double>(kernel_[k]) * state.history[read];
      read = (read == 0) ? kernel_size - 1 : read - 1;
    }

    samples[i] = static_cast<float>(y);
    state.write_index = (state.write_index + 1) % kernel_size;
  }
}

int LinearPhaseEq::active_partition_size() const noexcept {
  return config_.partition_size > 0 ? config_.partition_size : max_block_size_;
}

void LinearPhaseEq::validate_config() const {
  if (!is_power_of_two(config_.fft_size)) {
    throw std::invalid_argument("LinearPhaseEq fft_size must be a power of two");
  }
  if (config_.kernel_size <= 0 || config_.kernel_size > config_.fft_size) {
    throw std::invalid_argument("LinearPhaseEq kernel_size must be between 1 and fft_size");
  }
  if ((config_.kernel_size % 2) == 0) {
    throw std::invalid_argument("LinearPhaseEq kernel_size must be odd");
  }
  if (config_.partition_size < 0) {
    throw std::invalid_argument("LinearPhaseEq partition_size must be non-negative");
  }
}

void LinearPhaseEq::validate_band_index(size_t index) {
  if (index >= kMaxBands) {
    throw std::out_of_range("EQ band index out of range");
  }
}

float LinearPhaseEq::band_magnitude(const EqBand& band, double frequency_hz, double sample_rate) {
  const double frequency = clamp_frequency(frequency_hz, sample_rate);
  const float omega =
      static_cast<float>(sonare::constants::kTwoPi * frequency / static_cast<double>(sample_rate));
  return common::biquad_magnitude(design_band_biquad(band, sample_rate), omega);
}

}  // namespace sonare::mastering::eq
