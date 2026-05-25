#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/eq/equalizer.h"
#include "util/constants.h"

namespace sonare::mastering::eq {

namespace {

bool is_cut_band(EqBandType type) noexcept {
  return type == EqBandType::LowPass || type == EqBandType::HighPass;
}

int cut_order(int slope_db_oct) {
  if (slope_db_oct == 0) {
    return 0;
  }
  if (slope_db_oct < 6 || slope_db_oct > 96 || (slope_db_oct % 6) != 0) {
    throw std::invalid_argument("cut slope must be 0 or 6..96 dB/oct in 6 dB steps");
  }
  return slope_db_oct / 6;
}

float butterworth_stage_q(int order, int pair) {
  const double angle = (static_cast<double>(2 * pair + 1) * sonare::constants::kPiD) /
                       (2.0 * static_cast<double>(order));
  return static_cast<float>(1.0 / (2.0 * std::sin(angle)));
}

template <typename Append>
void append_iir_cut_cascade(const EqBand& band, Append&& append) {
  if (!is_cut_band(band.type) || band.slope_db_oct == 12) {
    append(band);
    return;
  }
  const int order = cut_order(band.slope_db_oct);
  if (order == 0) {
    return;
  }
  if ((order % 2) != 0) {
    EqBand first_order = band;
    first_order.slope_db_oct = 6;
    append(first_order);
  }

  const int pair_count = order / 2;
  for (int pair = pair_count - 1; pair >= 0; --pair) {
    EqBand stage = band;
    stage.slope_db_oct = 12;
    stage.q = butterworth_stage_q(order, pair);
    if (pair == pair_count - 1 && std::abs(band.q - sonare::constants::kButterworthQ) > 1.0e-6f) {
      stage.q = std::max(band.q, 1.0e-6f);
    }
    append(stage);
  }
}

template <typename Append>
void append_tilt_bands(const EqBand& band, Append&& append) {
  if (band.type == EqBandType::TiltShelf) {
    EqBand low = band;
    EqBand high = band;
    low.type = EqBandType::LowShelf;
    high.type = EqBandType::HighShelf;
    low.gain_db = -band.gain_db * 0.5f;
    high.gain_db = band.gain_db * 0.5f;
    low.q = sonare::constants::kButterworthQ;
    high.q = sonare::constants::kButterworthQ;
    append(low);
    append(high);
    return;
  }
  // FlatTilt: four spread shelves approximate a constant dB/oct slope.
  const float quarter_gain = band.gain_db * 0.25f;
  const float kFlatTiltQ = 0.40f;
  const float kOuterSpread = 8.0f;
  const float inner_spread = sonare::constants::kSqrt2;
  EqBand lo_outer = band;
  EqBand lo_inner = band;
  EqBand hi_inner = band;
  EqBand hi_outer = band;
  lo_outer.type = EqBandType::LowShelf;
  lo_inner.type = EqBandType::LowShelf;
  hi_inner.type = EqBandType::HighShelf;
  hi_outer.type = EqBandType::HighShelf;
  lo_outer.frequency_hz = band.frequency_hz / kOuterSpread;
  lo_inner.frequency_hz = band.frequency_hz / inner_spread;
  hi_inner.frequency_hz = band.frequency_hz * inner_spread;
  hi_outer.frequency_hz = band.frequency_hz * kOuterSpread;
  lo_outer.gain_db = -quarter_gain;
  lo_inner.gain_db = -quarter_gain;
  hi_inner.gain_db = quarter_gain;
  hi_outer.gain_db = quarter_gain;
  lo_outer.q = kFlatTiltQ;
  lo_inner.q = kFlatTiltQ;
  hi_inner.q = kFlatTiltQ;
  hi_outer.q = kFlatTiltQ;
  append(lo_outer);
  append(lo_inner);
  append(hi_inner);
  append(hi_outer);
}

}  // namespace

void EqualizerProcessor::update_iir_bands_preserving_state(int num_samples) {
  has_mid_side_bands_ = false;
  has_dynamic_bands_ = false;
  const bool any_soloed = [&] {
    for (const auto& band : bands_) {
      if (band.enabled && band.soloed && !band.bypassed) {
        return true;
      }
    }
    return false;
  }();

  size_t stereo_index = 0;
  size_t left_index = 0;
  size_t right_index = 0;
  size_t mid_index = 0;
  size_t side_index = 0;
  const auto append = [&](EqBand routed) {
    if (!routed.enabled) {
      return;
    }
    auto add_to = [&](ParametricEq& backend, size_t& index) {
      if (index >= ParametricEq::kMaxBands) {
        throw std::invalid_argument("EqualizerProcessor IIR backend band capacity exceeded");
      }
      backend.set_band(index++, routed);
    };
    switch (routed.placement) {
      case StereoPlacement::Stereo:
        add_to(stereo_iir_, stereo_index);
        break;
      case StereoPlacement::Left:
        add_to(left_iir_, left_index);
        break;
      case StereoPlacement::Right:
        add_to(right_iir_, right_index);
        break;
      case StereoPlacement::Mid:
        has_mid_side_bands_ = true;
        add_to(mid_iir_, mid_index);
        break;
      case StereoPlacement::Side:
        has_mid_side_bands_ = true;
        add_to(side_iir_, side_index);
        break;
    }
  };

  for (size_t i = 0; i < kMaxBands; ++i) {
    EqBand band = backend_band(bands_[i], phase_mode_, gain_scale_);
    if (band.bypassed || (any_soloed && !band.soloed)) {
      band.enabled = false;
    }
    if (band.enabled && band.dyn.enabled) {
      has_dynamic_bands_ = true;
      const float threshold =
          band.dyn.auto_threshold ? auto_threshold_db_[i] : band.dyn.threshold_db;
      const float target_gain =
          band.gain_db +
          gain_scale_ * dynamic_gain_delta(bands_[i], last_band_detector_db_[i], threshold);
      if (num_samples > 0) {
        const double smoothing_samples = std::max(sample_rate_ * 0.005, 1.0);
        const float coeff = static_cast<float>(
            1.0 - std::exp(-static_cast<double>(num_samples) / smoothing_samples));
        smoothed_gain_db_[i] += coeff * (target_gain - smoothed_gain_db_[i]);
        band.gain_db = smoothed_gain_db_[i];
      } else {
        smoothed_gain_db_[i] = target_gain;
        band.gain_db = target_gain;
      }
      last_applied_gain_db_[i] = band.gain_db;
    } else {
      last_applied_gain_db_[i] = 0.0f;
    }

    const bool linear_band =
        (band.phase == PhaseMode::Inherit ? phase_mode_ : band.phase) == PhaseMode::LinearPhase;
    if (linear_band) {
      continue;
    }
    if (any_soloed && band.enabled && band.soloed) {
      band.type = EqBandType::BandPass;
      band.gain_db = 0.0f;
      append(band);
      continue;
    }
    if (band.enabled && (band.type == EqBandType::TiltShelf || band.type == EqBandType::FlatTilt)) {
      append_tilt_bands(band, append);
    } else {
      append_iir_cut_cascade(band, append);
    }
  }

  const auto clear_tail = [](ParametricEq& backend, size_t from) {
    for (size_t i = from; i < ParametricEq::kMaxBands; ++i) {
      backend.clear_band(i);
    }
  };
  clear_tail(stereo_iir_, stereo_index);
  clear_tail(left_iir_, left_index);
  clear_tail(right_iir_, right_index);
  clear_tail(mid_iir_, mid_index);
  clear_tail(side_iir_, side_index);
}

void EqualizerProcessor::rebuild_iir(int num_samples) {
  const bool rebuild_linear = num_samples <= 0;
  stereo_iir_.clear();
  left_iir_.clear();
  right_iir_.clear();
  mid_iir_.clear();
  side_iir_.clear();
  if (rebuild_linear) {
    left_channel_fir_.clear();
    right_channel_fir_.clear();
    mid_fir_.clear();
    side_fir_.clear();
    has_linear_bands_ = false;
    has_lr_linear_bands_ = false;
    has_mid_side_linear_bands_ = false;
  }
  has_mid_side_bands_ = false;
  has_dynamic_bands_ = false;
  const bool any_soloed = [&] {
    for (const auto& band : bands_) {
      if (band.enabled && band.soloed && !band.bypassed) {
        return true;
      }
    }
    return false;
  }();

  size_t stereo_index = 0;
  size_t left_index = 0;
  size_t right_index = 0;
  size_t mid_index = 0;
  size_t side_index = 0;
  size_t left_channel_index = 0;
  size_t right_channel_index = 0;
  size_t mid_linear_index = 0;
  size_t side_linear_index = 0;
  const auto append = [&](EqBand routed) {
    if (!routed.enabled) {
      return;
    }
    auto add_to = [&](ParametricEq& backend, size_t& index) {
      if (index >= ParametricEq::kMaxBands) {
        throw std::invalid_argument("EqualizerProcessor IIR backend band capacity exceeded");
      }
      backend.set_band(index++, routed);
    };
    switch (routed.placement) {
      case StereoPlacement::Stereo:
        add_to(stereo_iir_, stereo_index);
        break;
      case StereoPlacement::Left:
        add_to(left_iir_, left_index);
        break;
      case StereoPlacement::Right:
        add_to(right_iir_, right_index);
        break;
      case StereoPlacement::Mid:
        has_mid_side_bands_ = has_mid_side_bands_ || routed.enabled;
        add_to(mid_iir_, mid_index);
        break;
      case StereoPlacement::Side:
        has_mid_side_bands_ = has_mid_side_bands_ || routed.enabled;
        add_to(side_iir_, side_index);
        break;
    }
  };
  const auto append_linear = [&](EqBand routed) {
    if (!rebuild_linear || !routed.enabled) {
      return;
    }
    routed.phase = PhaseMode::LinearPhase;
    routed.dyn.enabled = false;
    auto add_to = [&](LinearPhaseEq& backend, size_t& index) {
      if (index >= LinearPhaseEq::kMaxBands) {
        throw std::invalid_argument("EqualizerProcessor FIR backend band capacity exceeded");
      }
      backend.set_band(index++, routed);
      has_linear_bands_ = true;
    };
    switch (routed.placement) {
      case StereoPlacement::Stereo:
        add_to(left_channel_fir_, left_channel_index);
        add_to(right_channel_fir_, right_channel_index);
        has_lr_linear_bands_ = true;
        break;
      case StereoPlacement::Left:
        add_to(left_channel_fir_, left_channel_index);
        has_lr_linear_bands_ = true;
        break;
      case StereoPlacement::Right:
        add_to(right_channel_fir_, right_channel_index);
        has_lr_linear_bands_ = true;
        break;
      case StereoPlacement::Mid:
        add_to(mid_fir_, mid_linear_index);
        has_mid_side_linear_bands_ = true;
        break;
      case StereoPlacement::Side:
        add_to(side_fir_, side_linear_index);
        has_mid_side_linear_bands_ = true;
        break;
    }
  };

  for (size_t i = 0; i < kMaxBands; ++i) {
    EqBand band = backend_band(bands_[i], phase_mode_, gain_scale_);
    if (band.bypassed || (any_soloed && !band.soloed)) {
      band.enabled = false;
    }
    if (band.enabled && band.dyn.enabled) {
      has_dynamic_bands_ = true;
      const float threshold =
          band.dyn.auto_threshold ? auto_threshold_db_[i] : band.dyn.threshold_db;
      const float target_gain =
          band.gain_db +
          gain_scale_ * dynamic_gain_delta(bands_[i], last_band_detector_db_[i], threshold);
      if (prepared_ && num_samples > 0) {
        const double smoothing_samples = std::max(sample_rate_ * 0.005, 1.0);
        const float coeff = static_cast<float>(
            1.0 - std::exp(-static_cast<double>(num_samples) / smoothing_samples));
        smoothed_gain_db_[i] += coeff * (target_gain - smoothed_gain_db_[i]);
        band.gain_db = smoothed_gain_db_[i];
      } else {
        smoothed_gain_db_[i] = target_gain;
        band.gain_db = target_gain;
      }
      last_applied_gain_db_[i] = band.gain_db;
    } else {
      last_applied_gain_db_[i] = 0.0f;
    }

    const bool linear_band =
        (band.phase == PhaseMode::Inherit ? phase_mode_ : band.phase) == PhaseMode::LinearPhase;
    if (any_soloed && band.enabled && band.soloed) {
      band.type = EqBandType::BandPass;
      band.gain_db = 0.0f;
      if (linear_band) {
        append_linear(band);
      } else {
        append(band);
      }
      continue;
    }

    if (band.enabled && (band.type == EqBandType::TiltShelf || band.type == EqBandType::FlatTilt)) {
      if (linear_band) {
        append_tilt_bands(band, append_linear);
      } else {
        append_tilt_bands(band, append);
      }
    } else {
      if (linear_band) {
        append_linear(band);
      } else {
        append_iir_cut_cascade(band, append);
      }
    }
  }
}

EqBand EqualizerProcessor::backend_band(EqBand band, PhaseMode global_phase, float gain_scale) {
  const PhaseMode resolved_phase = band.phase == PhaseMode::Inherit ? global_phase : band.phase;
  band.gain_db *= gain_scale;
  if (is_cut_band(band.type)) {
    (void)cut_order(band.slope_db_oct);
    if (band.slope_db_oct == 0) {
      band.phase = PhaseMode::LinearPhase;
    }
  }
  band.coeff_mode =
      resolved_phase == PhaseMode::NaturalPhase ? BiquadCoeffMode::Vicanek : band.coeff_mode;
  if (band.proportional_q && band.type == EqBandType::Peak) {
    const float strength = std::clamp(band.proportional_q_strength, 0.0f, 0.25f);
    band.q *= 1.0f + strength * std::abs(band.gain_db);
  }
  return band;
}

void EqualizerProcessor::process_mono_backend(ParametricEq& backend, float* samples,
                                              int num_samples) {
  float* mono[] = {samples};
  backend.process(mono, 1, num_samples);
}

void EqualizerProcessor::process_mono_fir(LinearPhaseEq& backend, float* samples, int num_samples) {
  float* mono[] = {samples};
  backend.process(mono, 1, num_samples);
}
}  // namespace sonare::mastering::eq
