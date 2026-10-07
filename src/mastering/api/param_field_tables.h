#pragma once

/// @file param_field_tables.h
/// @brief Single source of truth for mastering processor parameter fields.
///
/// Each processor's (jsonKey, config-member) pairs are listed exactly once, in
/// an X-macro table. The three consumers that previously repeated these lists
/// each expand the same table:
///   - processor_params.h  — builds a typed config from a flat ParamMap.
///   - chain_json.cpp       — serializes a config to JSON params.
///   - chain_params.cpp     — parses flat chain keys back into a config.
///
/// The tables carry no per-field type tag: the read (`assign`) and write
/// (`add_field`) helpers are overloaded on the config member's own type, so the
/// compiler — not a hand-maintained tag column — decides float/int/bool/enum
/// handling. Adding or renaming a parameter is a one-line table edit that all
/// three consumers pick up.

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "effects/common/mix_law.h"
#include "effects/delay/stereo_delay.h"
#include "effects/modulation/auto_wah.h"
#include "effects/modulation/chorus.h"
#include "effects/modulation/lfo.h"
#include "effects/modulation/phaser.h"
#include "effects/modulation/rotary.h"
#include "effects/modulation/wah.h"
#include "effects/reverb/dattorro_reverb.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/eq/cut_filter.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/pultec.h"
#include "mastering/final/dither.h"
#include "mastering/multiband/crossover.h"
#include "mastering/multiband/multiband_saturation.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "mastering/saturation/amp_sim.h"
#include "mastering/saturation/bitcrusher.h"
#include "mastering/saturation/cab_voicing.h"
#include "mastering/saturation/waveshaper.h"
#include "mastering/stereo/binaural_panner.h"
#include "mastering/stereo/stereo_balance.h"
#include "rt/aliasing_control.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::api::detail {

/// @brief Stands in for any field type during aggregate-arity probing.
/// @details Convertible to everything except the aggregate under test, so a
/// one-field probe cannot succeed by copy-initializing that aggregate instead.
/// Declared, never defined: it is only ever used unevaluated.
template <typename Aggregate>
struct AnyField {
  template <typename Field,
            typename = std::enable_if_t<!std::is_same_v<Aggregate, std::decay_t<Field>>>>
  constexpr operator Field() const noexcept;
};

template <typename Aggregate, typename Indices, typename = void>
struct BraceInitializableWith : std::false_type {};

template <typename Aggregate, std::size_t... I>
struct BraceInitializableWith<Aggregate, std::index_sequence<I...>,
                              std::void_t<decltype(Aggregate{(void(I), AnyField<Aggregate>{})...})>>
    : std::true_type {};

/// @brief Number of fields in an aggregate, counted at compile time.
/// @details Brace initialization accepts any count up to the field count and
/// rejects anything beyond it, so the largest accepted count is the answer.
/// Counts fields, not names: a table row pointing at a renamed field fails to
/// compile on the member itself, but two same-typed fields swapping meaning is
/// invisible here.
template <typename Aggregate, std::size_t N = 0>
constexpr std::size_t field_count() {
  // Without a plain aggregate the probe collapses to 0 (no single-argument
  // constructor matches), so a coverage assertion would report every row as
  // missing. Failing on the real cause here keeps that from being "fixed" by
  // raising the unexposed count, which would pass while guarding nothing.
  static_assert(std::is_aggregate_v<Aggregate>,
                "field_count requires a plain aggregate: no base classes, no user-declared "
                "constructor, no private members");
  if constexpr (BraceInitializableWith<Aggregate, std::make_index_sequence<N + 1>>::value) {
    return field_count<Aggregate, N + 1>();
  } else {
    return N;
  }
}

// ---------------------------------------------------------------------------
// Enum choice names. One exhaustive switch per enum a flat parameter selects:
// a newly declared enumerator fails to compile here (-Wswitch) until it is
// named, and an enum with no table cannot be read as a parameter at all. A name
// is the enumerator's lowerCamel spelling without its `k` prefix; the wire
// value is the underlying value.
// ---------------------------------------------------------------------------

constexpr const char* enum_choice_name(sonare::rt::AliasingControl value) {
  switch (value) {
    case sonare::rt::AliasingControl::None:
      return "none";
    case sonare::rt::AliasingControl::Adaa1:
      return "adaa1";
    case sonare::rt::AliasingControl::Adaa2:
      return "adaa2";
    case sonare::rt::AliasingControl::Oversample4x:
      return "oversample4x";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::dynamics::DetectorMode value) {
  switch (value) {
    case sonare::mastering::dynamics::DetectorMode::Peak:
      return "peak";
    case sonare::mastering::dynamics::DetectorMode::Rms:
      return "rms";
    case sonare::mastering::dynamics::DetectorMode::LogRms:
      return "logRms";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::saturation::WaveshaperCurve value) {
  switch (value) {
    case sonare::mastering::saturation::WaveshaperCurve::Tanh:
      return "tanh";
    case sonare::mastering::saturation::WaveshaperCurve::Arctan:
      return "arctan";
    case sonare::mastering::saturation::WaveshaperCurve::Asymmetric:
      return "asymmetric";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::final::DitherType value) {
  switch (value) {
    case sonare::mastering::final::DitherType::None:
      return "none";
    case sonare::mastering::final::DitherType::Rpdf:
      return "rpdf";
    case sonare::mastering::final::DitherType::Tpdf:
      return "tpdf";
    case sonare::mastering::final::DitherType::NoiseShaped:
      return "noiseShaped";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::repair::DecrackleMode value) {
  switch (value) {
    case sonare::mastering::repair::DecrackleMode::Median:
      return "median";
    case sonare::mastering::repair::DecrackleMode::WaveletShrinkage:
      return "waveletShrinkage";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::repair::DehumMode value) {
  switch (value) {
    case sonare::mastering::repair::DehumMode::Subtract:
      return "subtract";
    case sonare::mastering::repair::DehumMode::Notch:
      return "notch";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::repair::DenoiseMode value) {
  switch (value) {
    case sonare::mastering::repair::DenoiseMode::LogMmse:
      return "logMmse";
    case sonare::mastering::repair::DenoiseMode::MmseStsa:
      return "mmseStsa";
    case sonare::mastering::repair::DenoiseMode::SpectralSubtraction:
      return "spectralSubtraction";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::repair::DenoiseNoiseEstimator value) {
  switch (value) {
    case sonare::mastering::repair::DenoiseNoiseEstimator::Quantile:
      return "quantile";
    case sonare::mastering::repair::DenoiseNoiseEstimator::Mcra:
      return "mcra";
    case sonare::mastering::repair::DenoiseNoiseEstimator::Imcra:
      return "imcra";
    case sonare::mastering::repair::DenoiseNoiseEstimator::Spp:
      return "spp";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::repair::TrimSilenceMode value) {
  switch (value) {
    case sonare::mastering::repair::TrimSilenceMode::Peak:
      return "peak";
    case sonare::mastering::repair::TrimSilenceMode::LufsGated:
      return "lufsGated";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::saturation::BitCrusherDiscType value) {
  switch (value) {
    case sonare::mastering::saturation::BitCrusherDiscType::kLp:
      return "lp";
    case sonare::mastering::saturation::BitCrusherDiscType::kEp:
      return "ep";
    case sonare::mastering::saturation::BitCrusherDiscType::kSp:
      return "sp";
    case sonare::mastering::saturation::BitCrusherDiscType::kRnd:
      return "rnd";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::saturation::BitCrusherFilterType value) {
  switch (value) {
    case sonare::mastering::saturation::BitCrusherFilterType::kOff:
      return "off";
    case sonare::mastering::saturation::BitCrusherFilterType::kLowpass:
      return "lowpass";
    case sonare::mastering::saturation::BitCrusherFilterType::kHighpass:
      return "highpass";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::saturation::QuantizerMode value) {
  switch (value) {
    case sonare::mastering::saturation::QuantizerMode::kFixedDepth:
      return "fixedDepth";
    case sonare::mastering::saturation::QuantizerMode::kOff:
      return "off";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::effects::modulation::PhaserMixMode value) {
  switch (value) {
    case sonare::effects::modulation::PhaserMixMode::kCrossfade:
      return "crossfade";
    case sonare::effects::modulation::PhaserMixMode::kDrySum:
      return "drySum";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::effects::modulation::LfoShape value) {
  switch (value) {
    case sonare::effects::modulation::LfoShape::kSine:
      return "sine";
    case sonare::effects::modulation::LfoShape::kTriangle:
      return "triangle";
    case sonare::effects::modulation::LfoShape::kSquare:
      return "square";
    case sonare::effects::modulation::LfoShape::kSawUp:
      return "sawUp";
    case sonare::effects::modulation::LfoShape::kSawDown:
      return "sawDown";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::stereo::StereoBalanceLaw value) {
  switch (value) {
    case sonare::mastering::stereo::StereoBalanceLaw::kNormalized:
      return "normalized";
    case sonare::mastering::stereo::StereoBalanceLaw::kRawConstantPower:
      return "rawConstantPower";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::stereo::BinauralOutput value) {
  switch (value) {
    case sonare::mastering::stereo::BinauralOutput::kSpeakers:
      return "speakers";
    case sonare::mastering::stereo::BinauralOutput::kPhones:
      return "phones";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::effects::modulation::WahFilterType value) {
  switch (value) {
    case sonare::effects::modulation::WahFilterType::kBandpass:
      return "bandpass";
    case sonare::effects::modulation::WahFilterType::kLowpass:
      return "lowpass";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::effects::modulation::WahSweepLaw value) {
  switch (value) {
    case sonare::effects::modulation::WahSweepLaw::kLinearHz:
      return "linearHz";
    case sonare::effects::modulation::WahSweepLaw::kLinearOctave:
      return "linearOctave";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::effects::modulation::AutoWahDirection value) {
  switch (value) {
    case sonare::effects::modulation::AutoWahDirection::kUp:
      return "up";
    case sonare::effects::modulation::AutoWahDirection::kDown:
      return "down";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::effects::delay::StereoDelayCrossMode value) {
  switch (value) {
    case sonare::effects::delay::StereoDelayCrossMode::kNormal:
      return "normal";
    case sonare::effects::delay::StereoDelayCrossMode::kPingPong:
      return "pingPong";
    case sonare::effects::delay::StereoDelayCrossMode::kCross:
      return "cross";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::effects::reverb::DattorroGateType value) {
  switch (value) {
    case sonare::effects::reverb::DattorroGateType::kNormal:
      return "normal";
    case sonare::effects::reverb::DattorroGateType::kReverse:
      return "reverse";
    case sonare::effects::reverb::DattorroGateType::kSweep1:
      return "sweep1";
    case sonare::effects::reverb::DattorroGateType::kSweep2:
      return "sweep2";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::effects::common::MixLaw value) {
  switch (value) {
    case sonare::effects::common::MixLaw::kCrossfade:
      return "crossfade";
    case sonare::effects::common::MixLaw::kTwoRamps:
      return "twoRamps";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::effects::modulation::DelayInterpolation value) {
  switch (value) {
    case sonare::effects::modulation::DelayInterpolation::kLinear:
      return "linear";
    case sonare::effects::modulation::DelayInterpolation::kLagrange3:
      return "lagrange3";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::effects::modulation::RotaryModel value) {
  switch (value) {
    case sonare::effects::modulation::RotaryModel::kClassic:
      return "classic";
    case sonare::effects::modulation::RotaryModel::kGeometric:
      return "geometric";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::effects::modulation::PreFilterMode value) {
  switch (value) {
    case sonare::effects::modulation::PreFilterMode::kOff:
      return "off";
    case sonare::effects::modulation::PreFilterMode::kLowPass:
      return "lowPass";
    case sonare::effects::modulation::PreFilterMode::kHighPass:
      return "highPass";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::multiband::CrossoverSlope value) {
  switch (value) {
    case sonare::mastering::multiband::CrossoverSlope::LR2:
      return "lr2";
    case sonare::mastering::multiband::CrossoverSlope::LR4:
      return "lr4";
    case sonare::mastering::multiband::CrossoverSlope::LR8:
      return "lr8";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::multiband::CrossoverMode value) {
  switch (value) {
    case sonare::mastering::multiband::CrossoverMode::LinkwitzRiley:
      return "linkwitzRiley";
    case sonare::mastering::multiband::CrossoverMode::Butterworth:
      return "butterworth";
    case sonare::mastering::multiband::CrossoverMode::Bessel:
      return "bessel";
    case sonare::mastering::multiband::CrossoverMode::FirLinearPhase:
      return "firLinearPhase";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(
    sonare::mastering::eq::LinearPhaseEqConfig::Resolution value) {
  switch (value) {
    case sonare::mastering::eq::LinearPhaseEqConfig::Resolution::Custom:
      return "custom";
    case sonare::mastering::eq::LinearPhaseEqConfig::Resolution::Low:
      return "low";
    case sonare::mastering::eq::LinearPhaseEqConfig::Resolution::Medium:
      return "medium";
    case sonare::mastering::eq::LinearPhaseEqConfig::Resolution::High:
      return "high";
    case sonare::mastering::eq::LinearPhaseEqConfig::Resolution::VeryHigh:
      return "veryHigh";
    case sonare::mastering::eq::LinearPhaseEqConfig::Resolution::Maximum:
      return "maximum";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::eq::PultecComponentModel value) {
  switch (value) {
    case sonare::mastering::eq::PultecComponentModel::CurveOnly:
      return "curveOnly";
    case sonare::mastering::eq::PultecComponentModel::Eqp1aWdf:
      return "eqp1aWdf";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::eq::CutFilterSlope value) {
  switch (value) {
    case sonare::mastering::eq::CutFilterSlope::Db12PerOct:
      return "db12PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db24PerOct:
      return "db24PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db6PerOct:
      return "db6PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db18PerOct:
      return "db18PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db30PerOct:
      return "db30PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db36PerOct:
      return "db36PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db42PerOct:
      return "db42PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db48PerOct:
      return "db48PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db54PerOct:
      return "db54PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db60PerOct:
      return "db60PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db66PerOct:
      return "db66PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db72PerOct:
      return "db72PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db78PerOct:
      return "db78PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db84PerOct:
      return "db84PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db90PerOct:
      return "db90PerOct";
    case sonare::mastering::eq::CutFilterSlope::Db96PerOct:
      return "db96PerOct";
    case sonare::mastering::eq::CutFilterSlope::Brickwall:
      return "brickwall";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::eq::EqBandType value) {
  switch (value) {
    case sonare::mastering::eq::EqBandType::Peak:
      return "peak";
    case sonare::mastering::eq::EqBandType::LowShelf:
      return "lowShelf";
    case sonare::mastering::eq::EqBandType::HighShelf:
      return "highShelf";
    case sonare::mastering::eq::EqBandType::LowPass:
      return "lowPass";
    case sonare::mastering::eq::EqBandType::HighPass:
      return "highPass";
    case sonare::mastering::eq::EqBandType::BandPass:
      return "bandPass";
    case sonare::mastering::eq::EqBandType::Notch:
      return "notch";
    case sonare::mastering::eq::EqBandType::TiltShelf:
      return "tiltShelf";
    case sonare::mastering::eq::EqBandType::FlatTilt:
      return "flatTilt";
    case sonare::mastering::eq::EqBandType::AllPass:
      return "allPass";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::eq::StereoPlacement value) {
  switch (value) {
    case sonare::mastering::eq::StereoPlacement::Stereo:
      return "stereo";
    case sonare::mastering::eq::StereoPlacement::Left:
      return "left";
    case sonare::mastering::eq::StereoPlacement::Right:
      return "right";
    case sonare::mastering::eq::StereoPlacement::Mid:
      return "mid";
    case sonare::mastering::eq::StereoPlacement::Side:
      return "side";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::eq::PhaseMode value) {
  switch (value) {
    case sonare::mastering::eq::PhaseMode::Inherit:
      return "inherit";
    case sonare::mastering::eq::PhaseMode::ZeroLatency:
      return "zeroLatency";
    case sonare::mastering::eq::PhaseMode::NaturalPhase:
      return "naturalPhase";
    case sonare::mastering::eq::PhaseMode::LinearPhase:
      return "linearPhase";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::eq::BiquadCoeffMode value) {
  switch (value) {
    case sonare::mastering::eq::BiquadCoeffMode::Rbj:
      return "rbj";
    case sonare::mastering::eq::BiquadCoeffMode::Vicanek:
      return "vicanek";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::multiband::SaturationType value) {
  switch (value) {
    case sonare::mastering::multiband::SaturationType::SoftClip:
      return "softClip";
    case sonare::mastering::multiband::SaturationType::Tape:
      return "tape";
    case sonare::mastering::multiband::SaturationType::Tube:
      return "tube";
    case sonare::mastering::multiband::SaturationType::Exciter:
      return "exciter";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::saturation::CabModel value) {
  switch (value) {
    case sonare::mastering::saturation::CabModel::kGuitar4x12:
      return "guitar4x12";
    case sonare::mastering::saturation::CabModel::kBass8x10:
      return "bass8x10";
    case sonare::mastering::saturation::CabModel::kGuitar1x12Combo:
      return "guitar1x12Combo";
    case sonare::mastering::saturation::CabModel::kGuitar2x12Open:
      return "guitar2x12Open";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::saturation::AmpModel value) {
  switch (value) {
    case sonare::mastering::saturation::AmpModel::kClassicCrunch:
      return "classicCrunch";
    case sonare::mastering::saturation::AmpModel::kFenderClean:
      return "fenderClean";
    case sonare::mastering::saturation::AmpModel::kModernHiGain:
      return "modernHiGain";
    case sonare::mastering::saturation::AmpModel::kTweed:
      return "tweed";
    case sonare::mastering::saturation::AmpModel::kVoxChime:
      return "voxChime";
    case sonare::mastering::saturation::AmpModel::kRectifier:
      return "rectifier";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::saturation::AmpTopology value) {
  switch (value) {
    case sonare::mastering::saturation::AmpTopology::kVoiced:
      return "voiced";
    case sonare::mastering::saturation::AmpTopology::kCircuit:
      return "circuit";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::saturation::PowerTube value) {
  switch (value) {
    case sonare::mastering::saturation::PowerTube::k6L6:
      return "6l6";
    case sonare::mastering::saturation::PowerTube::kEL34:
      return "el34";
    case sonare::mastering::saturation::PowerTube::kEL84:
      return "el84";
    case sonare::mastering::saturation::PowerTube::k6V6:
      return "6v6";
  }
  return nullptr;
}

constexpr const char* enum_choice_name(sonare::mastering::saturation::MicModel value) {
  switch (value) {
    case sonare::mastering::saturation::MicModel::kNone:
      return "none";
    case sonare::mastering::saturation::MicModel::kDynamic:
      return "dynamic";
    case sonare::mastering::saturation::MicModel::kRibbon:
      return "ribbon";
    case sonare::mastering::saturation::MicModel::kCondenser:
      return "condenser";
  }
  return nullptr;
}

/// Highest underlying value scanned for declared enumerators.
inline constexpr int kEnumOrdinalScanLimit = 63;

/// @brief Whether an integer-decoded value names a declared enumerator.
template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
constexpr bool enum_value_declared(Enum value) {
  return enum_choice_name(value) != nullptr;
}

/// @brief One selectable value of a closed parameter set, by name and wire value.
struct EnumChoice {
  std::string name;
  int value = 0;
};

/// @brief An enum's @ref enum_choice_name over its wire value, so the code
///        around it is compiled once rather than once per enum.
using EnumNameFn = const char* (*)(int value);

template <typename Enum>
const char* enum_choice_name_of(int value) {
  return enum_choice_name(static_cast<Enum>(value));
}

/// @brief Every value @p name_of declares, in wire-value order.
inline std::vector<EnumChoice> enum_choices(EnumNameFn name_of) {
  std::vector<EnumChoice> choices;
  for (int value = 0; value <= kEnumOrdinalScanLimit; ++value) {
    if (const char* name = name_of(value)) choices.push_back(EnumChoice{name, value});
  }
  return choices;
}

/// @brief Every declared value of @p Enum in wire-value order.
template <typename Enum>
std::vector<EnumChoice> enum_choices() {
  return enum_choices(&enum_choice_name_of<Enum>);
}

}  // namespace sonare::mastering::api::detail

namespace sonare::mastering::api {

/// @brief Refuses a flat param value that no integral field can hold, naming
///        @p subject and which of the two ways it failed.
/// @details Fractional and out-of-range are different mistakes and a caller can
///   only act on the one they made. One message for both told whoever wrote
///   512.7 that their value was out of range, which it is not. Callers holding
///   the dotted key pass it as @p subject; the table dispatch, which does not,
///   names the parameter class instead. Outside `detail` because the flat param
///   type is shared beyond this header, and so is the contract.
[[noreturn]] inline void reject_integer_param(const std::string& subject, double value) {
  if (numeric::finite(value) && std::trunc(value) != value) {
    throw SonareException(ErrorCode::InvalidParameter, subject + " must be a whole number");
  }
  throw SonareException(ErrorCode::InvalidParameter, subject + " is out of range");
}

/// @brief Converts a flat integer to a size, refusing a negative one by name.
inline std::size_t checked_nonnegative_size(int value, const std::string& subject) {
  if (value < 0) {
    throw SonareException(ErrorCode::InvalidParameter, subject + " must be non-negative");
  }
  return static_cast<std::size_t>(value);
}

/// @brief Assigns a flat param to an integral config field, naming its key.
/// @details The flat API carries every value as a double, so a bare cast folds
/// a fraction onto a legal count and saturates an out-of-range value, which a
/// later bound check then reports as a number the caller never passed.
template <typename Int>
inline void assign_int_param(const std::string& subject, double value, Int& dst) {
  if (!numeric::checked_integral_cast(value, &dst)) {
    reject_integer_param(subject, value);
  }
}

}  // namespace sonare::mastering::api

namespace sonare::mastering::api::detail {

/// @brief Assigns a flat double param value to a typed config member.
/// @details One overload per storage kind so a table entry needs no type tag.
/// Integral and enum members take the value exactly (the flat param API carries
/// every value as a double) and refuse a fractional one rather than rounding it
/// onto a legal neighbour; bool follows the "non-zero is true" convention used
/// throughout the chain param surface.
inline void assign_field(float& dst, double value) {
  if (!numeric::finite(value) ||
      std::fabs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
    throw SonareException(ErrorCode::InvalidParameter, "mastering parameter is out of range");
  }
  dst = static_cast<float>(value);
}
inline void assign_field(double& dst, double value) {
  if (!numeric::finite(value)) {
    throw SonareException(ErrorCode::InvalidParameter, "mastering parameter must be finite");
  }
  dst = value;
}
inline void assign_field(bool& dst, double value) {
  if (!numeric::finite(value)) {
    throw SonareException(ErrorCode::InvalidParameter, "mastering parameter must be finite");
  }
  dst = value != 0.0;
}

template <typename Int,
          std::enable_if_t<std::is_integral_v<Int> && !std::is_same_v<Int, bool>, int> = 0>
inline void assign_field(Int& dst, double value) {
  assign_int_param("mastering integer parameter", value, dst);
}

inline void assign_enum_value(int& dst, double value, EnumNameFn name_of) {
  int converted = 0;
  // An enum selector is an index into a closed set, so a fractional one names no
  // enumerator at all; rounding it would silently select a neighbour.
  if (!numeric::checked_integral_cast(value, &converted)) {
    reject_integer_param("mastering enum parameter", value);
  }
  if (name_of(converted) == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "mastering enum parameter is out of range");
  }
  dst = converted;
}

template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
inline void assign_field(Enum& dst, double value) {
  int converted = static_cast<int>(dst);
  assign_enum_value(converted, value, &enum_choice_name_of<Enum>);
  dst = static_cast<Enum>(converted);
}

/// @brief Reads a typed config member back as the flat surface's @c double.
/// @details The inverse of @ref assign_field, and overloaded on the same storage
///          kinds so a table row still needs no type tag. Used to publish a
///          config field's own initializer as the parameter's catalog default:
///          a builder run against an empty param map falls back to exactly that
///          value, so recording it costs nothing and cannot drift from the
///          struct.
inline double field_as_double(float value) { return static_cast<double>(value); }
inline double field_as_double(double value) { return value; }
inline double field_as_double(bool value) { return value ? 1.0 : 0.0; }

template <typename Int,
          std::enable_if_t<std::is_integral_v<Int> && !std::is_same_v<Int, bool>, int> = 0>
inline double field_as_double(Int value) {
  return static_cast<double>(value);
}

template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
inline double field_as_double(Enum value) {
  return static_cast<double>(static_cast<int>(value));
}

}  // namespace sonare::mastering::api::detail

// ---------------------------------------------------------------------------
// Field tables. X(jsonKey, member, meta) — jsonKey is the flat parameter name (the
// chain surface prefixes it with the stage path); member is the field on the
// processor's *Config struct; meta is the unit and axis the reader declares for
// the key (param_meta.h), which only the config builder consumes. Order matches the historical
// serialization order so chain_config_to_json output is byte-stable.
// ---------------------------------------------------------------------------

// Coverage guard. A config field with no table row is unreachable from every
// binding, because the flat parameter surface is the only configuration path
// they share -- and nothing else detects it: the DSP tests build the config
// struct directly, parity does not model config interiors, and the chain JSON
// snapshot only moves for stages the chain serializes. Pairing each table with
// its config struct turns "someone has to remember" into a build failure at the
// moment the field is added.
//
// SONARE_ASSERT_TABLE_COVERS(table, Config, unexposed) reads as: this table plus
// `unexposed` deliberately-omitted fields accounts for every field of Config.
// A non-zero `unexposed` needs a one-line reason at the call site naming the
// omitted fields -- an unexplained count is how a coverage gap gets laundered
// into an approved exception.
#define SONARE_COUNT_FIELD(key, member, meta) +1
#define SONARE_FIELD_TABLE_SIZE(table) (0 table(SONARE_COUNT_FIELD))
#define SONARE_ASSERT_TABLE_COVERS(table, Config, unexposed)                                   \
  static_assert(SONARE_FIELD_TABLE_SIZE(table) + (unexposed) ==                                \
                    static_cast<int>(::sonare::mastering::api::detail::field_count<Config>()), \
                #table " does not account for every " #Config                                  \
                       " field: add the missing row, or raise the unexposed count "            \
                       "and say why")

// The same guard for a builder that cannot carry a field table, because its keys
// do not stand one to one with its fields: two keys writing one field (an alias),
// one key deriving another field's value, or one field spanning several keys (a
// nested member). Counting keys there answers nothing, so what is pinned is the
// config's own arity, at the site that has to wire a new field.
//
// SONARE_ASSERT_EVERY_FIELD_IS_WIRED(Config, accounted) reads as: someone has
// accounted for all `accounted` fields of Config, and Config has exactly that
// many. The count is the whole arity, so a field deliberately left without a key
// still occupies its place in it and takes a one-line reason at the call site --
// the number never falls to record an omission, which would leave the next field
// to arrive indistinguishable from the one that was decided about.
#define SONARE_ASSERT_EVERY_FIELD_IS_WIRED(Config, accounted)                                   \
  static_assert(                                                                                \
      static_cast<int>(::sonare::mastering::api::detail::field_count<Config>()) == (accounted), \
      #Config                                                                                   \
      " gained or lost a field: wire it to a construction key and move the "                    \
      "count, or say at the call site why it has none")

// --- Dynamics ---

#define SONARE_FIELDS_COMPRESSOR(X)                              \
  X("thresholdDb", threshold_db, display_range(kDb, -60, 0))     \
  X("ratio", ratio, display_range(kRatio, 1, 20))                \
  X("attackMs", attack_ms, display_range(kMsLog, 0, 200))        \
  X("releaseMs", release_ms, display_range(kMsLog, 0, 2000))     \
  X("kneeDb", knee_db, display_range(kDb, 0, 24))                \
  X("makeupGainDb", makeup_gain_db, display_range(kDb, -12, 24)) \
  X("autoMakeup", auto_makeup, kNone)                            \
  X("detector", detector, kNone)                                 \
  X("sidechainHpfEnabled", sidechain_hpf_enabled, kNone)         \
  X("sidechainHpfHz", sidechain_hpf_hz, kHzLog)                  \
  X("pdrTimeMs", pdr_time_ms, kMs)                               \
  X("pdrReleaseScale", pdr_release_scale, kNone)

#define SONARE_FIELDS_LIMITER(X)                             \
  X("thresholdDb", threshold_db, display_range(kDb, -60, 0)) \
  X("lookaheadMs", lookahead_ms, display_range(kMs, 0, 20))  \
  X("releaseMs", release_ms, display_range(kMsLog, 0, 1000)) \
  X("ratio", ratio, kRatio)                                  \
  X("postGainDb", post_gain_db, kDb)

#define SONARE_FIELDS_BRICKWALL_LIMITER(X)                  \
  X("ceilingDb", ceiling_db, display_range(kDbfs, -24, 0))  \
  X("lookaheadMs", lookahead_ms, display_range(kMs, 0, 20)) \
  X("releaseMs", release_ms, display_range(kMsLog, 0, 1000))

#define SONARE_FIELDS_DEESSER(X)                                     \
  X("frequencyHz", frequency_hz, display_range(kHzLog, 1000, 16000)) \
  X("thresholdDb", threshold_db, display_range(kDb, -60, 0))         \
  X("ratio", ratio, display_range(kRatio, 1, 20))                    \
  X("attackMs", attack_ms, kMsLog)                                   \
  X("releaseMs", release_ms, kMsLog)                                 \
  X("rangeDb", range_db, kDb)                                        \
  X("bandpassQ", bandpass_q, kNone)

#define SONARE_FIELDS_EXPANDER(X)     \
  X("thresholdDb", threshold_db, kDb) \
  X("ratio", ratio, kRatio)           \
  X("attackMs", attack_ms, kMsLog)    \
  X("releaseMs", release_ms, kMsLog)  \
  X("rangeDb", range_db, kDb)

#define SONARE_FIELDS_GATE(X)                                \
  X("thresholdDb", threshold_db, display_range(kDb, -50, 0)) \
  X("attackMs", attack_ms, display_range(kMsLog, 0, 100))    \
  X("releaseMs", release_ms, display_range(kMsLog, 0, 2000)) \
  X("rangeDb", range_db, display_range(kDb, -80, 0))         \
  X("holdMs", hold_ms, display_range(kMs, 0, 1000))          \
  X("closeThresholdDb", close_threshold_db, kDb)             \
  X("keyHpfHz", key_hpf_hz, kHz)

#define SONARE_FIELDS_PARALLEL_COMP(X)          \
  X("thresholdDb", threshold_db, kDb)           \
  X("ratio", ratio, kRatio)                     \
  X("attackMs", attack_ms, kMsLog)              \
  X("releaseMs", release_ms, kMsLog)            \
  X("makeupGainDb", makeup_gain_db, kDb)        \
  X("mix", mix, kNone)                          \
  X("linkedDetection", linked_detection, kNone) \
  X("outputLimiter", output_limiter, kNone)     \
  X("outputCeilingDb", output_ceiling_db, kDbfs)

#define SONARE_FIELDS_SIDECHAIN_ROUTER(X)                \
  X("thresholdDb", threshold_db, kDb)                    \
  X("ratio", ratio, kRatio)                              \
  X("attackMs", attack_ms, kMsLog)                       \
  X("releaseMs", release_ms, kMsLog)                     \
  X("rangeDb", range_db, kDb)                            \
  X("sidechainHpfEnabled", sidechain_hpf_enabled, kNone) \
  X("sidechainHpfHz", sidechain_hpf_hz, kHzLog)          \
  X("monoSumming", mono_summing, kNone)                  \
  X("keyListen", key_listen, kNone)                      \
  X("lookaheadMs", lookahead_ms, kMs)

#define SONARE_FIELDS_DUCKING(X)      \
  X("thresholdDb", threshold_db, kDb) \
  X("ratio", ratio, kRatio)           \
  X("attackMs", attack_ms, kMsLog)    \
  X("releaseMs", release_ms, kMsLog)  \
  X("rangeDb", range_db, kDb)         \
  X("lookaheadMs", lookahead_ms, kMs)

#define SONARE_FIELDS_TRANSIENT_SHAPER(X)      \
  X("attackGainDb", attack_gain_db, kDb)       \
  X("sustainGainDb", sustain_gain_db, kDb)     \
  X("fastAttackMs", fast_attack_ms, kMsLog)    \
  X("fastReleaseMs", fast_release_ms, kMsLog)  \
  X("slowAttackMs", slow_attack_ms, kMsLog)    \
  X("slowReleaseMs", slow_release_ms, kMsLog)  \
  X("sensitivity", sensitivity, kNone)         \
  X("maxGainDb", max_gain_db, kDb)             \
  X("gainSmoothingMs", gain_smoothing_ms, kMs) \
  X("lookaheadMs", lookahead_ms, kMs)

#define SONARE_FIELDS_UPWARD_COMPRESSOR(X) \
  X("thresholdDb", threshold_db, kDb)      \
  X("ratio", ratio, kRatio)                \
  X("attackMs", attack_ms, kMsLog)         \
  X("releaseMs", release_ms, kMsLog)       \
  X("rangeDb", range_db, kDb)

#define SONARE_FIELDS_UPWARD_EXPANDER(X) SONARE_FIELDS_UPWARD_COMPRESSOR(X)

#define SONARE_FIELDS_VOCAL_RIDER(X)           \
  X("targetDb", target_db, kDb)                \
  X("maxBoostDb", max_boost_db, kDb)           \
  X("maxCutDb", max_cut_db, kDb)               \
  X("attackMs", attack_ms, kMsLog)             \
  X("releaseMs", release_ms, kMsLog)           \
  X("outputGainDb", output_gain_db, kDb)       \
  X("gainSmoothingMs", gain_smoothing_ms, kMs) \
  X("noiseFloorDb", noise_floor_db, kDb)       \
  X("linkedDetection", linked_detection, kNone)

// --- Saturation ---

#define SONARE_FIELDS_TAPE(X)                                    \
  X("driveDb", drive_db, display_range(kDb, -12, 24))            \
  X("saturation", saturation, display_range(kNone, 0, 1))        \
  X("hysteresis", hysteresis, kNone)                             \
  X("outputGainDb", output_gain_db, display_range(kDb, -24, 24)) \
  X("speedIps", speed_ips, kInchesPerSecond)                     \
  X("headBumpDb", head_bump_db, kDb)                             \
  X("bias", bias, kNone)                                         \
  X("gapLoss", gap_loss, kNone)                                  \
  X("oversampleFactor", oversample_factor, kRatio)

#define SONARE_FIELDS_EXCITER(X)         \
  X("frequencyHz", frequency_hz, kHzLog) \
  X("driveDb", drive_db, kDb)            \
  X("amount", amount, kNone)             \
  X("q", q, kNone)                       \
  X("evenOddMix", even_odd_mix, kNone)   \
  X("aliasing", aliasing, kNone)

#define SONARE_FIELDS_BITCRUSHER(X)                \
  X("bitDepth", bit_depth, kBits)                  \
  X("downsampleFactor", downsample_factor, kRatio) \
  X("mix", mix, kNone)                             \
  X("ditherType", dither_type, kNone)              \
  X("ditherSeed", dither_seed, kNone)              \
  X("holdHz", hold_hz, kHz)                        \
  X("quantizerMode", quantizer_mode, kNone)        \
  X("radioNoiseLevel", radio_noise_level, kNone)   \
  X("wpNoiseLevel", wp_noise_level, kNone)         \
  X("discNoiseLevel", disc_noise_level, kNone)     \
  X("humLevel", hum_level, kNone)                  \
  X("noiseDetune", noise_detune, kNone)            \
  X("wpNoisePink", wp_noise_pink, kNone)           \
  X("discType", disc_type, kNone)                  \
  X("humHz", hum_hz, kHz)                          \
  X("noiseLpfHz", noise_lpf_hz, kHz)               \
  X("wpNoiseLpfHz", wp_noise_lpf_hz, kHz)          \
  X("discNoiseLpfHz", disc_noise_lpf_hz, kHz)      \
  X("humLpfHz", hum_lpf_hz, kHz)                   \
  X("preFilterHz", pre_filter_hz, kHz)             \
  X("postFilterHz", post_filter_hz, kHz)           \
  X("filterType", filter_type, kNone)              \
  X("mono", mono, kNone)                           \
  X("typeLadder", type_ladder, kNone)              \
  X("mixLaw", mix_law, kNone)

#define SONARE_FIELDS_HARD_CLIPPER(X) \
  X("ceiling", ceiling, kNone)        \
  X("aliasing", aliasing, kNone)

#define SONARE_FIELDS_SOFT_CLIPPER(X)                            \
  X("driveDb", drive_db, display_range(kDb, -12, 24))            \
  X("ceiling", ceiling, display_range(kNone, kAcceptedBound, 1)) \
  X("mix", mix, display_range(kNone, 0, 1))                      \
  X("aliasing", aliasing, kNone)

#define SONARE_FIELDS_WAVESHAPER(X)                              \
  X("driveDb", drive_db, display_range(kDb, -12, 24))            \
  X("mix", mix, display_range(kNone, 0, 1))                      \
  X("outputGainDb", output_gain_db, display_range(kDb, -24, 24)) \
  X("bias", bias, kNone)                                         \
  X("curve", curve, kNone)                                       \
  X("aliasing", aliasing, kNone)

#define SONARE_FIELDS_TUBE(X)                      \
  X("driveDb", drive_db, kDb)                      \
  X("bias", bias, kNone)                           \
  X("mix", mix, kNone)                             \
  X("oversampleFactor", oversample_factor, kRatio) \
  X("biasV", bias_v, kVolts)                       \
  X("harmonicDrive", harmonic_drive, kNone)

#define SONARE_FIELDS_TRANSFORMER(X) \
  X("driveDb", drive_db, kDb)        \
  X("asymmetry", asymmetry, kNone)   \
  X("mix", mix, kNone)

// Shared by saturation.overdrive and saturation.distortion.
#define SONARE_FIELDS_PEDAL(X) \
  X("gainDb", gain_db, kDb)    \
  X("toneHz", tone_hz, kHzLog) \
  X("levelDb", level_db, kDb)

// --- Spectral ---

#define SONARE_FIELDS_AIR_BAND(X)                    \
  X("amount", amount, kNone)                         \
  X("shelfFrequencyHz", shelf_frequency_hz, kHzLog)  \
  X("dynamicThresholdDb", dynamic_threshold_db, kDb) \
  X("dynamicRangeDb", dynamic_range_db, kDb)

#define SONARE_FIELDS_LOW_END_FOCUS(X)              \
  X("cutoffHz", cutoff_hz, kHzLog)                  \
  X("width", width, kNone)                          \
  X("subharmonicAmount", subharmonic_amount, kNone) \
  X("transientTightness", transient_tightness, kNone)

#define SONARE_FIELDS_PRESENCE_ENHANCER(X)            \
  X("amount", amount, kNone)                          \
  X("drive", drive, kNone)                            \
  X("centerFrequencyHz", center_frequency_hz, kHzLog) \
  X("q", q, kNone)                                    \
  X("aliasing", aliasing, kNone)

#define SONARE_FIELDS_SPECTRAL_SHAPER(X)          \
  X("threshold", threshold, kNone)                \
  X("amount", amount, kNone)                      \
  X("frequencyHz", frequency_hz, kHzLog)          \
  X("highFrequencyHz", high_frequency_hz, kHzLog) \
  X("attackMs", attack_ms, kMsLog)                \
  X("releaseMs", release_ms, kMsLog)              \
  X("rangeDb", range_db, kDb)

// --- Stereo ---

#define SONARE_FIELDS_AUTO_PAN(X) \
  X("rateHz", rate_hz, kHz)       \
  X("depth", depth, kNone)        \
  X("phase", phase, kNone)        \
  X("shape", shape, kNone)

#define SONARE_FIELDS_BINAURAL_PANNER(X) \
  X("azimuthDeg", azimuth_deg, kDegrees) \
  X("autoTurn", auto_turn, kNone)        \
  X("turnRateHz", turn_rate_hz, kHz)     \
  X("clockwise", clockwise, kNone)       \
  X("output", output, kNone)             \
  X("dryWet", dry_wet, kNone)

#define SONARE_FIELDS_HAAS_ENHANCER(X) \
  X("delayMs", delay_ms, kMs)          \
  X("mix", mix, kNone)                 \
  X("delayRight", delay_right, kNone)

// The imager additionally range-checks width/decorrelationAmount in its config
// builder; only the field overlay is table-driven.
#define SONARE_FIELDS_IMAGER(X)                                              \
  X("width", width, display_range(kNone, 0, 2))                              \
  X("outputGainDb", output_gain_db, display_range(kDb, -24, 24))             \
  X("decorrelationAmount", decorrelation_amount, display_range(kNone, 0, 1)) \
  X("preserveEnergy", preserve_energy, kNone)

#define SONARE_FIELDS_MONO_MAKER(X)               \
  X("amount", amount, display_range(kNone, 0, 1)) \
  X("frequencyHz", frequency_hz, kHzLog)

#define SONARE_FIELDS_PHASE_ALIGN(X)         \
  X("delaySamples", delay_samples, kSamples) \
  X("delayRight", delay_right, kNone)        \
  X("fractionalDelaySamples", fractional_delay_samples, kSamples)

#define SONARE_FIELDS_STEREO_BALANCE(X)     \
  X("balance", balance, kNone)              \
  X("constantPower", constant_power, kNone) \
  X("law", law, kNone)

// --- Utility ---

#define SONARE_FIELDS_GAIN(X) X("levelDb", level_db, kDb)

// --- Maximizer ---

#define SONARE_FIELDS_MAXIMIZER(X)                           \
  X("inputGainDb", input_gain_db, display_range(kDb, 0, 24)) \
  X("ceilingDb", ceiling_db, display_range(kDbfs, -24, 0))   \
  X("lookaheadMs", lookahead_ms, kMs)                        \
  X("releaseMs", release_ms, display_range(kMsLog, 0, 1000))

#define SONARE_FIELDS_TRUE_PEAK_LIMITER(X)         \
  X("ceilingDb", ceiling_db, kDbfs)                \
  X("lookaheadMs", lookahead_ms, kMs)              \
  X("releaseMs", release_ms, kMsLog)               \
  X("oversampleFactor", oversample_factor, kRatio) \
  X("applyGainAtInputRate", apply_gain_at_input_rate, kNone)

#define SONARE_FIELDS_SOFT_KNEE_MAX(X) \
  X("inputGainDb", input_gain_db, kDb) \
  X("ceilingDb", ceiling_db, kDbfs)    \
  X("kneeDb", knee_db, kDb)            \
  X("releaseMs", release_ms, kMsLog)

#define SONARE_FIELDS_ADAPTIVE_RELEASE(X)   \
  X("ceilingDb", ceiling_db, kDbfs)         \
  X("lookaheadMs", lookahead_ms, kMs)       \
  X("minReleaseMs", min_release_ms, kMsLog) \
  X("maxReleaseMs", max_release_ms, kMsLog) \
  X("crestWindowMs", crest_window_ms, kMs)  \
  X("crestLow", crest_low, kNone)           \
  X("crestHigh", crest_high, kNone)         \
  X("releaseSmoothingMs", release_smoothing_ms, kMs)

// --- Repair ---
// A repair stage is read from the flat keys the offline named path has always used; the
// stage's own validate_config states every accepted range, so the descriptors measure it.

#define SONARE_FIELDS_DECLICK(X)                                          \
  X("threshold", threshold, display_range(kNone, 0.1, 1))                 \
  X("neighborRatio", neighbor_ratio, display_range(kRatio, 1, 20))        \
  X("maxClickSamples", max_click_samples, display_range(kSamples, 1, 64)) \
  X("lpcOrder", lpc_order, kCount)                                        \
  X("residualRatio", residual_ratio, display_range(kRatio, 1, 50))

#define SONARE_FIELDS_DECLIP(X)                                    \
  X("clipThreshold", clip_threshold, display_range(kNone, 0.5, 1)) \
  X("lpcOrder", lpc_order, kCount)                                 \
  X("iterations", iterations, kCount)                              \
  X("lpcBlend", lpc_blend, kNone)

#define SONARE_FIELDS_DECRACKLE(X)                         \
  X("threshold", threshold, display_range(kNone, 0.05, 1)) \
  X("mode", mode, kNone)                                   \
  X("levels", levels, kCount)

#define SONARE_FIELDS_DEHUM(X)                                        \
  X("fundamentalHz", fundamental_hz, display_range(kHzLog, 20, 1000)) \
  X("harmonics", harmonics, kCount)                                   \
  X("q", q, display_range(kNone, 1, 100))                             \
  X("adaptive", adaptive, kNone)                                      \
  X("searchRangeHz", search_range_hz, display_range(kHz, 0, 20))      \
  X("adaptation", adaptation, kNone)                                  \
  X("frameSize", frame_size, display_range(kSamples, 256, 8192))      \
  X("pllBandwidth", pll_bandwidth, kNone)                             \
  X("mode", mode, kNone)

#define SONARE_FIELDS_DENOISE_CLASSICAL(X)                       \
  X("mode", mode, kNone)                                         \
  X("noiseEstimator", noise_estimator, kNone)                    \
  X("nFft", n_fft, kSamples)                                     \
  X("hopLength", hop_length, kSamples)                           \
  X("ddAlpha", dd_alpha, kNone)                                  \
  X("reductionDb", reduction_db, kDb)                            \
  X("overSubtraction", over_subtraction, kRatio)                 \
  X("spectralFloor", spectral_floor, kNone)                      \
  X("noiseEstimationQuantile", noise_estimation_quantile, kNone) \
  X("speechPresenceGain", speech_presence_gain, kNone)           \
  X("gainSmoothing", gain_smoothing, kNone)

#define SONARE_FIELDS_DEREVERB_CLASSICAL(X)                \
  X("threshold", threshold, kNone)                         \
  X("attenuation", attenuation, kNone)                     \
  X("nFft", n_fft, kSamples)                               \
  X("hopLength", hop_length, kSamples)                     \
  X("t60Sec", t60_sec, display_range(kSecondsLog, 0.1, 3)) \
  X("lateDelayMs", late_delay_ms, kMs)                     \
  X("overSubtraction", over_subtraction, kRatio)           \
  X("spectralFloor", spectral_floor, kNone)                \
  X("wpeEnabled", wpe_enabled, kNone)                      \
  X("wpeIterations", wpe_iterations, kCount)               \
  X("wpeTaps", wpe_taps, kCount)                           \
  X("wpeStrength", wpe_strength, kNone)

#define SONARE_FIELDS_TRIM_SILENCE(X)                                     \
  X("threshold", threshold, display_range(kNone, 0, 0.1))                 \
  X("paddingSamples", padding_samples, display_range(kSamples, 0, 48000)) \
  X("mode", mode, kNone)                                                  \
  X("gateLufs", gate_lufs, kLufs)                                         \
  X("windowMs", window_ms, display_range(kMs, 100, 1000))

// --- Chain-only stages ---
// These do not have a processor_params.h config builder, but their flat-key
// round trip (chain_json.cpp <-> chain_params.cpp) benefits from the same shared
// table. Their fields live directly on the chain stage struct (no .config
// member), so the chain consumers supply the matching accessor.

#define SONARE_FIELDS_EQ_TILT(X) \
  X("tiltDb", tilt_db, kDb)      \
  X("pivotHz", pivot_hz, kHzLog)

#define SONARE_FIELDS_LOUDNESS(X)                            \
  X("targetLufs", target_lufs, kLufs)                        \
  X("ceilingDb", ceiling_db, kDbfs)                          \
  X("truePeakOversample", true_peak_oversample, kRatio)      \
  X("releaseMs", release_ms, kMsLog)                         \
  X("applyGainAtInputRate", apply_gain_at_input_rate, kNone) \
  X("maxLimiterGainReductionDb", max_limiter_gain_reduction_db, kDb)
