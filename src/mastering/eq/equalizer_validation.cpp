#include <stdexcept>

#include "mastering/eq/equalizer.h"

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

size_t iir_cut_stage_count(const EqBand& band) {
  if (!is_cut_band(band.type) || band.slope_db_oct == 12) {
    return 1;
  }
  const int order = cut_order(band.slope_db_oct);
  if (order == 0) {
    return 0;
  }
  return static_cast<size_t>((order % 2) + (order / 2));
}

}  // namespace

void EqualizerProcessor::validate_process_args(float* const* channels, int num_channels,
                                               int num_samples) {
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }
}

void EqualizerProcessor::validate_band_index(size_t index) {
  if (index >= kMaxBands) {
    throw std::out_of_range("EqualizerProcessor band index out of range");
  }
}

void EqualizerProcessor::validate_supported_band(const EqBand& band, PhaseMode global_phase) {
  if (!band.enabled) {
    return;
  }
  const PhaseMode resolved_phase = band.phase == PhaseMode::Inherit ? global_phase : band.phase;
  const bool brickwall_cut = is_cut_band(band.type) && band.slope_db_oct == 0;
  if (is_cut_band(band.type)) {
    (void)cut_order(band.slope_db_oct);
  }
  if (resolved_phase == PhaseMode::LinearPhase) {
    if (band.dyn.enabled) {
      throw std::invalid_argument("EqualizerProcessor LinearPhase dynamic bands are not realtime");
    }
  }
  if (brickwall_cut && band.dyn.enabled) {
    throw std::invalid_argument("EqualizerProcessor brickwall dynamic bands are not realtime");
  }
  validate_dynamic_params(band.dyn);
}

void EqualizerProcessor::validate_backend_capacity(const std::array<EqBand, kMaxBands>& bands,
                                                   PhaseMode global_phase) {
  const bool any_soloed = [&] {
    for (const auto& band : bands) {
      if (band.enabled && band.soloed && !band.bypassed) {
        return true;
      }
    }
    return false;
  }();

  size_t stereo_iir = 0;
  size_t left_iir = 0;
  size_t right_iir = 0;
  size_t mid_iir = 0;
  size_t side_iir = 0;
  size_t left_channel_fir = 0;
  size_t right_channel_fir = 0;
  size_t mid_fir = 0;
  size_t side_fir = 0;

  const auto add = [](size_t& count, size_t capacity, const char* message) {
    if (++count > capacity) {
      throw std::invalid_argument(message);
    }
  };

  for (const auto& source_band : bands) {
    EqBand band = source_band;
    if (!band.enabled || band.bypassed || (any_soloed && !band.soloed)) {
      continue;
    }
    const PhaseMode resolved_phase = band.phase == PhaseMode::Inherit ? global_phase : band.phase;
    const bool linear_band = resolved_phase == PhaseMode::LinearPhase ||
                             (is_cut_band(band.type) && band.slope_db_oct == 0);
    size_t expansion = 1;
    if (!band.soloed && band.type == EqBandType::TiltShelf) {
      expansion = 2;
    } else if (!band.soloed && band.type == EqBandType::FlatTilt) {
      expansion = 4;
    } else if (!linear_band) {
      expansion = iir_cut_stage_count(band);
    }

    for (size_t n = 0; n < expansion; ++n) {
      if (linear_band) {
        switch (band.placement) {
          case StereoPlacement::Stereo:
            add(left_channel_fir, LinearPhaseEq::kMaxBands,
                "EqualizerProcessor FIR backend band capacity exceeded");
            add(right_channel_fir, LinearPhaseEq::kMaxBands,
                "EqualizerProcessor FIR backend band capacity exceeded");
            break;
          case StereoPlacement::Left:
            add(left_channel_fir, LinearPhaseEq::kMaxBands,
                "EqualizerProcessor FIR backend band capacity exceeded");
            break;
          case StereoPlacement::Right:
            add(right_channel_fir, LinearPhaseEq::kMaxBands,
                "EqualizerProcessor FIR backend band capacity exceeded");
            break;
          case StereoPlacement::Mid:
            add(mid_fir, LinearPhaseEq::kMaxBands,
                "EqualizerProcessor FIR backend band capacity exceeded");
            break;
          case StereoPlacement::Side:
            add(side_fir, LinearPhaseEq::kMaxBands,
                "EqualizerProcessor FIR backend band capacity exceeded");
            break;
        }
      } else {
        switch (band.placement) {
          case StereoPlacement::Stereo:
            add(stereo_iir, ParametricEq::kMaxBands,
                "EqualizerProcessor IIR backend band capacity exceeded");
            break;
          case StereoPlacement::Left:
            add(left_iir, ParametricEq::kMaxBands,
                "EqualizerProcessor IIR backend band capacity exceeded");
            break;
          case StereoPlacement::Right:
            add(right_iir, ParametricEq::kMaxBands,
                "EqualizerProcessor IIR backend band capacity exceeded");
            break;
          case StereoPlacement::Mid:
            add(mid_iir, ParametricEq::kMaxBands,
                "EqualizerProcessor IIR backend band capacity exceeded");
            break;
          case StereoPlacement::Side:
            add(side_iir, ParametricEq::kMaxBands,
                "EqualizerProcessor IIR backend band capacity exceeded");
            break;
        }
      }
    }
  }
}

void EqualizerProcessor::validate_dynamic_params(const DynamicParams& dyn) {
  if (!dyn.enabled) {
    return;
  }
  if (!(dyn.ratio >= 1.0f)) {
    throw std::invalid_argument("dynamic ratio must be at least 1");
  }
  if (!(dyn.sidechain_q > 0.0f) || dyn.attack_ms < 0.0f || dyn.release_ms < 0.0f ||
      dyn.lookahead_ms < 0.0f ||
      (dyn.sidechain_freq_hz != -1.0f && dyn.sidechain_freq_hz <= 0.0f)) {
    throw std::invalid_argument("invalid dynamic EQ configuration");
  }
}
}  // namespace sonare::mastering::eq
