#include "mastering/eq/linear_phase.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "core/fft.h"
#include "core/window.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mastering::eq {

namespace {

bool is_power_of_two(int value) { return value > 0 && (value & (value - 1)) == 0; }

LinearPhaseEqConfig resolve_resolution(LinearPhaseEqConfig config) {
  switch (config.resolution) {
    case LinearPhaseEqConfig::Resolution::Low:
      config.fft_size = 4096;
      config.kernel_size = 1025;
      break;
    case LinearPhaseEqConfig::Resolution::Medium:
      config.fft_size = 8192;
      config.kernel_size = 2049;
      break;
    case LinearPhaseEqConfig::Resolution::High:
      config.fft_size = 16384;
      config.kernel_size = 4097;
      break;
    case LinearPhaseEqConfig::Resolution::VeryHigh:
      config.fft_size = 32768;
      config.kernel_size = 8193;
      break;
    case LinearPhaseEqConfig::Resolution::Maximum:
      config.fft_size = 32768;
      config.kernel_size = 16385;
      break;
    case LinearPhaseEqConfig::Resolution::Custom:
      break;
  }
  return config;
}

double clamp_frequency(double frequency_hz, double sample_rate) {
  return std::clamp(frequency_hz, 1.0, sample_rate * 0.5 - 1.0);
}

sonare::rt::BiquadCoeffs design_band_biquad(const EqBand& band, double sample_rate) {
  const double center = clamp_frequency(band.frequency_hz, sample_rate);
  // Biquad-design corner uses the double-precision two-pi (filter design path).
  const float omega =
      static_cast<float>(sonare::constants::kTwoPiD * center / static_cast<double>(sample_rate));
  const float q = std::max(band.q, 1.0e-6f);

  if (band.coeff_mode == BiquadCoeffMode::Vicanek) {
    switch (band.type) {
      case EqBandType::Peak:
        return sonare::rt::vicanek_peak(omega, q, band.gain_db);
      case EqBandType::LowShelf:
        return sonare::rt::vicanek_low_shelf(omega, band.gain_db);
      case EqBandType::HighShelf:
        return sonare::rt::vicanek_high_shelf(omega, band.gain_db);
      case EqBandType::LowPass:
        return sonare::rt::vicanek_lowpass(omega, q);
      case EqBandType::HighPass:
        return sonare::rt::vicanek_highpass(omega, q);
      case EqBandType::BandPass:
        return sonare::rt::vicanek_bandpass(omega, q);
      case EqBandType::Notch:
        return sonare::rt::vicanek_notch(omega, q);
      case EqBandType::TiltShelf:
      case EqBandType::FlatTilt:
        // Vicanek matched-Z designs have no closed-form for tilt/flat-tilt;
        // surfacing the error lets the caller pick BiquadCoeffMode::RBJ (which
        // does support tilt) instead of silently falling through to a
        // different magnitude/phase response. Note: NaturalPhase mode forces
        // Vicanek (equalizer_routing.cpp), so users selecting NaturalPhase on
        // a tilt band must explicitly opt back to RBJ.
        throw SonareException(ErrorCode::InvalidParameter,
                              "Vicanek EQ coefficient mode does not support TiltShelf/FlatTilt; "
                              "use BiquadCoeffMode::RBJ for these band types");
    }
  }

  switch (band.type) {
    case EqBandType::Peak:
      return sonare::rt::rbj_peak(omega, q, band.gain_db);
    case EqBandType::LowShelf:
      return sonare::rt::rbj_low_shelf(omega, q, band.gain_db);
    case EqBandType::HighShelf:
      return sonare::rt::rbj_high_shelf(omega, q, band.gain_db);
    case EqBandType::LowPass:
      return sonare::rt::rbj_lowpass(omega, q);
    case EqBandType::HighPass:
      return sonare::rt::rbj_highpass(omega, q);
    case EqBandType::BandPass:
      return sonare::rt::rbj_bandpass(omega, q);
    case EqBandType::Notch:
      return sonare::rt::rbj_notch(omega, q);
    case EqBandType::TiltShelf:
    case EqBandType::FlatTilt:
      throw SonareException(ErrorCode::InvalidParameter, "unsupported EQ band type");
  }

  return {};
}

bool is_cut_band(EqBandType type) noexcept {
  return type == EqBandType::LowPass || type == EqBandType::HighPass;
}

int cut_order(int slope_db_oct) {
  if (slope_db_oct == 0) {
    return 0;
  }
  if (slope_db_oct < 6 || slope_db_oct > 96 || (slope_db_oct % 6) != 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "cut slope must be 0 or 6..96 dB/oct in 6 dB steps");
  }
  return slope_db_oct / 6;
}

float cut_cascade_magnitude(const EqBand& band, double frequency_hz, double sample_rate) {
  if (!is_cut_band(band.type) || band.slope_db_oct == 12) {
    const float omega =
        static_cast<float>(sonare::constants::kTwoPi * clamp_frequency(frequency_hz, sample_rate) /
                           static_cast<double>(sample_rate));
    return sonare::rt::biquad_magnitude(design_band_biquad(band, sample_rate), omega);
  }
  const int order = cut_order(band.slope_db_oct);
  if (order == 0) {
    return 1.0f;
  }

  const double frequency = clamp_frequency(frequency_hz, sample_rate);
  const float omega =
      static_cast<float>(sonare::constants::kTwoPi * frequency / static_cast<double>(sample_rate));
  const double center = clamp_frequency(band.frequency_hz, sample_rate);
  const float w0 =
      static_cast<float>(sonare::constants::kTwoPi * center / static_cast<double>(sample_rate));
  float magnitude = 1.0f;
  if ((order % 2) != 0) {
    magnitude *= sonare::rt::biquad_magnitude(band.type == EqBandType::HighPass
                                                  ? sonare::rt::first_order_highpass(w0)
                                                  : sonare::rt::first_order_lowpass(w0),
                                              omega);
  }
  const int pair_count = order / 2;
  for (int pair = pair_count - 1; pair >= 0; --pair) {
    float stage_q = rt::butterworth_stage_q(order, pair);
    if (pair == pair_count - 1 && std::abs(band.q - sonare::constants::kButterworthQ) > 1.0e-6f) {
      stage_q = std::max(band.q, 1.0e-6f);
    }
    magnitude *= sonare::rt::biquad_magnitude(band.type == EqBandType::HighPass
                                                  ? sonare::rt::rbj_highpass(w0, stage_q)
                                                  : sonare::rt::rbj_lowpass(w0, stage_q),
                                              omega);
  }
  return magnitude;
}

}  // namespace

LinearPhaseEq::LinearPhaseEq(LinearPhaseEqConfig config) : config_(resolve_resolution(config)) {
  validate_config();
}

void LinearPhaseEq::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  rebuild_kernel();
  reset();
}

void LinearPhaseEq::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "LinearPhaseEq");
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  }

  ensure_channel_state(num_channels);

  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
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

void LinearPhaseEq::prepare_channels(int num_channels) { ensure_channel_state(num_channels); }

void LinearPhaseEq::set_band(size_t index, const EqBand& band) {
  validate_band_index(index);
  if (band.enabled) {
    if (!(band.frequency_hz > 0.0f) ||
        !(band.frequency_hz < static_cast<float>(sample_rate_ * 0.5))) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "EQ band frequency must be between 0 Hz and Nyquist");
    }
    if (!(band.q > 0.0f)) {
      throw SonareException(ErrorCode::InvalidParameter, "EQ band Q must be positive");
    }
  }

  bands_[index] = band;
  if (prepared_) {
    reconfigure();
  }
}

bool LinearPhaseEq::set_parameter(unsigned int param_id, float value) {
  const size_t band_index = param_id / 3u;
  if (band_index >= kMaxBands) {
    return false;
  }
  EqBand updated = bands_[band_index];
  switch (param_id % 3u) {
    case 0:
      // Clamp to the open interval (0 Hz, Nyquist) so the kernel design path
      // never throws.
      updated.frequency_hz =
          std::clamp(value, 1.0e-3f, static_cast<float>(sample_rate_ * 0.5) - 1.0e-3f);
      break;
    case 1:
      updated.gain_db = value;
      break;
    case 2:
      updated.q = std::max(value, 1.0e-6f);
      break;
    default:
      return false;
  }

  bands_[band_index] = updated;
  if (prepared_) {
    reconfigure();
  }
  return true;
}

bool LinearPhaseEq::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  (void)param_id;
  return false;
}

void LinearPhaseEq::clear_band(size_t index) {
  validate_band_index(index);
  bands_[index] = {};
  if (prepared_) {
    reconfigure();
  }
}

void LinearPhaseEq::clear() {
  for (auto& band : bands_) {
    band = {};
  }
  if (prepared_) {
    reconfigure();
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
    // Symmetric Hann (periodic=false): linear-phase FIR taps require a symmetric
    // window for unity DC gain and a symmetric impulse response.
    kernel_[static_cast<size_t>(i)] =
        zero_phase[static_cast<size_t>(source)] * hann_value(i, config_.kernel_size, false);
  }

  latency_samples_ = half;
  for (auto& state : states_) {
    state.convolver_kernel_current = false;
  }
  ensure_channel_state(static_cast<int>(states_.size()));
}

void LinearPhaseEq::reconfigure() {
  // Non-destructive reconfiguration for band/parameter changes. Only the FIR
  // coefficients (kernel_) change; kernel_size — and therefore the FIR history
  // length and latency — is fixed for a given instance by the resolution/custom
  // config, so the existing per-channel FIR history (and the partitioned
  // convolver's internal ring) remain valid. rebuild_kernel() recomputes the
  // taps and pushes the fresh impulse response into each convolver via
  // ensure_channel_state WITHOUT resetting the convolver, so there is no
  // ~kernel-length silence gap on a change.
  //
  // Note: parameter_is_realtime_safe() returns false for all params — these
  // mutators are intended as prepare-time / offline reconfiguration. Preserving
  // history here simply avoids dumping a block of valid output to silence when
  // a host changes a band between (non-realtime) reconfigurations.
  const size_t previous_history = states_.empty() ? 0 : states_.front().history.size();
  rebuild_kernel();
  // Defensive: kernel_size is config-fixed, so the history length must be
  // unchanged. If a future change ever altered it, fall back to a clean reset
  // (a documented size change is the only case where a silence gap is
  // unavoidable) rather than reading a stale-length history.
  if (!states_.empty() && states_.front().history.size() != previous_history &&
      previous_history != 0) {
    reset();
  }
}

void LinearPhaseEq::ensure_channel_state(int num_channels) {
  if (num_channels < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "num_channels must be non-negative");
  }
  if (kernel_.empty()) {
    return;
  }
  if (states_.size() == static_cast<size_t>(num_channels)) {
    const int partition_size = active_partition_size();
    for (auto& state : states_) {
      if (config_.use_partitioned_convolution && partition_size > 0) {
        if (!state.convolver) {
          state.convolver = std::make_unique<sonare::rt::PartitionedConvolver>(
              sonare::rt::PartitionedConvolverConfig{partition_size});
          state.convolver_kernel_current = false;
        }
        if (!state.convolver_kernel_current) {
          state.convolver->set_impulse_response(kernel_);
          state.convolver_kernel_current = true;
        }
      } else {
        state.convolver.reset();
        state.convolver_kernel_current = false;
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
      state.convolver = std::make_unique<sonare::rt::PartitionedConvolver>(
          sonare::rt::PartitionedConvolverConfig{partition_size});
      state.convolver->set_impulse_response(kernel_);
      state.convolver_kernel_current = true;
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
    throw SonareException(ErrorCode::InvalidParameter,
                          "LinearPhaseEq fft_size must be a power of two");
  }
  if (config_.kernel_size <= 0 || config_.kernel_size > config_.fft_size) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "LinearPhaseEq kernel_size must be between 1 and fft_size");
  }
  if ((config_.kernel_size % 2) == 0) {
    throw SonareException(ErrorCode::InvalidParameter, "LinearPhaseEq kernel_size must be odd");
  }
  if (config_.partition_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "LinearPhaseEq partition_size must be non-negative");
  }
}

void LinearPhaseEq::validate_band_index(size_t index) {
  if (index >= kMaxBands) {
    throw SonareException(ErrorCode::InvalidParameter, "EQ band index out of range");
  }
}

float LinearPhaseEq::band_magnitude(const EqBand& band, double frequency_hz, double sample_rate) {
  const double frequency = clamp_frequency(frequency_hz, sample_rate);
  if (band.slope_db_oct == 0) {
    switch (band.type) {
      case EqBandType::LowPass:
        return frequency <= static_cast<double>(band.frequency_hz) ? 1.0f : 0.0f;
      case EqBandType::HighPass:
        return frequency >= static_cast<double>(band.frequency_hz) ? 1.0f : 0.0f;
      default:
        break;
    }
  }
  const float omega =
      static_cast<float>(sonare::constants::kTwoPi * frequency / static_cast<double>(sample_rate));
  if (is_cut_band(band.type)) {
    return cut_cascade_magnitude(band, frequency_hz, sample_rate);
  }
  return sonare::rt::biquad_magnitude(design_band_biquad(band, sample_rate), omega);
}

}  // namespace sonare::mastering::eq
