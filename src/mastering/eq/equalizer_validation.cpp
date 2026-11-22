#include "mastering/eq/cut_band.h"
#include "mastering/eq/equalizer.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/parameter_checks.h"

namespace sonare::mastering::eq {

namespace check = sonare::util::check;

namespace {

using detail::cut_order;
using detail::is_cut_band;

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

void EqualizerProcessor::validate_band_index(size_t index) {
  if (index >= kMaxBands) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "EqualizerProcessor band index out of range");
  }
}

void EqualizerProcessor::validate_supported_band(const EqBand& band, PhaseMode global_phase) {
  if (!band.enabled) {
    return;
  }
  // The shelf designs raise 10^(dB/20) in float, so a gain past kMaxFiniteGainDb overflows.
  check::at_most(band.gain_db, kMaxFiniteGainDb, "gain_db");
  const PhaseMode resolved_phase = band.phase == PhaseMode::Inherit ? global_phase : band.phase;
  const bool brickwall_cut = is_cut_band(band.type) && band.slope_db_oct == 0;
  if (is_cut_band(band.type)) {
    (void)cut_order(band.slope_db_oct);
  }
  if (resolved_phase == PhaseMode::LinearPhase) {
    if (band.dyn.enabled) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "EqualizerProcessor LinearPhase dynamic bands are not realtime");
    }
    // An all-pass is unit magnitude and exists for its phase, so a linear-phase
    // one is a wire. Rejected rather than run as a no-op, since a caller that
    // reached for the band type wanted something the mode cannot give.
    if (band.type == EqBandType::AllPass) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "EqualizerProcessor AllPass bands have no LinearPhase form");
    }
  }
  if (brickwall_cut && band.dyn.enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "EqualizerProcessor brickwall dynamic bands are not realtime");
  }
  validate_dynamic_params(band.dyn);
}

bool EqualizerProcessor::uses_fir_backend(const EqBand& band, PhaseMode global_phase) noexcept {
  const PhaseMode resolved_phase = band.phase == PhaseMode::Inherit ? global_phase : band.phase;
  return resolved_phase == PhaseMode::LinearPhase ||
         (is_cut_band(band.type) && band.slope_db_oct == 0);
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
      throw SonareException(ErrorCode::InvalidParameter, message);
    }
  };

  for (const auto& source_band : bands) {
    EqBand band = source_band;
    if (!band.enabled || band.bypassed || (any_soloed && !band.soloed)) {
      continue;
    }
    const bool linear_band = uses_fir_backend(band, global_phase);
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
  check::at_least(dyn.ratio, 1.0, "dyn.ratio");
  // Both reach the biquad as gain: the detector delta is (detector_db -
  // threshold_db) scaled by ratio and clamped to range_db.
  check::at_most(dyn.threshold_db, kMaxFiniteGainDb, "dyn.threshold_db");
  check::at_most(dyn.range_db, kMaxFiniteGainDb, "dyn.range_db");
  check::greater_than(dyn.sidechain_q, 0.0, "dyn.sidechain_q");
  check::at_least(dyn.attack_ms, 0.0, "dyn.attack_ms");
  check::at_least(dyn.release_ms, 0.0, "dyn.release_ms");
  check::at_least(dyn.detector_delay_ms, 0.0, "dyn.detector_delay_ms");
  // -1 follows the band frequency.
  if (dyn.sidechain_freq_hz != -1.0f) {
    check::greater_than(dyn.sidechain_freq_hz, 0.0, "dyn.sidechain_freq_hz");
  }
}
}  // namespace sonare::mastering::eq
