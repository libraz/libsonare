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

#include "effects/modulation/chorus.h"
#include "effects/modulation/phaser.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/eq/cut_filter.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/pultec.h"
#include "mastering/final/dither.h"
#include "mastering/multiband/crossover.h"
#include "mastering/multiband/multiband_saturation.h"
#include "mastering/saturation/amp_sim.h"
#include "mastering/saturation/bitcrusher.h"
#include "mastering/saturation/cab_voicing.h"
#include "mastering/saturation/waveshaper.h"
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

/// @brief Every declared value of @p Enum in wire-value order.
template <typename Enum>
std::vector<EnumChoice> enum_choices() {
  std::vector<EnumChoice> choices;
  for (int value = 0; value <= kEnumOrdinalScanLimit; ++value) {
    if (const char* name = enum_choice_name(static_cast<Enum>(value))) {
      choices.push_back(EnumChoice{name, value});
    }
  }
  return choices;
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

template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
inline void assign_field(Enum& dst, double value) {
  int converted = 0;
  // An enum selector is an index into a closed set, so a fractional one names no
  // enumerator at all; rounding it would silently select a neighbour.
  if (!numeric::checked_integral_cast(value, &converted)) {
    reject_integer_param("mastering enum parameter", value);
  }
  const Enum candidate = static_cast<Enum>(converted);
  if (!enum_value_declared(candidate)) {
    throw SonareException(ErrorCode::InvalidParameter, "mastering enum parameter is out of range");
  }
  dst = candidate;
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
// Field tables. X(jsonKey, member) — jsonKey is the flat parameter name (the
// chain surface prefixes it with the stage path); member is the field on the
// processor's *Config struct. Order matches the historical serialization order
// so chain_config_to_json output is byte-stable.
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
#define SONARE_COUNT_FIELD(key, member) +1
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

#define SONARE_FIELDS_COMPRESSOR(X)               \
  X("thresholdDb", threshold_db)                  \
  X("ratio", ratio)                               \
  X("attackMs", attack_ms)                        \
  X("releaseMs", release_ms)                      \
  X("kneeDb", knee_db)                            \
  X("makeupGainDb", makeup_gain_db)               \
  X("autoMakeup", auto_makeup)                    \
  X("detector", detector)                         \
  X("sidechainHpfEnabled", sidechain_hpf_enabled) \
  X("sidechainHpfHz", sidechain_hpf_hz)           \
  X("pdrTimeMs", pdr_time_ms)                     \
  X("pdrReleaseScale", pdr_release_scale)

#define SONARE_FIELDS_LIMITER(X) \
  X("thresholdDb", threshold_db) \
  X("lookaheadMs", lookahead_ms) \
  X("releaseMs", release_ms)

#define SONARE_FIELDS_BRICKWALL_LIMITER(X) \
  X("ceilingDb", ceiling_db)               \
  X("lookaheadMs", lookahead_ms)           \
  X("releaseMs", release_ms)

#define SONARE_FIELDS_DEESSER(X) \
  X("frequencyHz", frequency_hz) \
  X("thresholdDb", threshold_db) \
  X("ratio", ratio)              \
  X("attackMs", attack_ms)       \
  X("releaseMs", release_ms)     \
  X("rangeDb", range_db)         \
  X("bandpassQ", bandpass_q)

#define SONARE_FIELDS_EXPANDER(X) \
  X("thresholdDb", threshold_db)  \
  X("ratio", ratio)               \
  X("attackMs", attack_ms)        \
  X("releaseMs", release_ms)      \
  X("rangeDb", range_db)

#define SONARE_FIELDS_GATE(X)               \
  X("thresholdDb", threshold_db)            \
  X("attackMs", attack_ms)                  \
  X("releaseMs", release_ms)                \
  X("rangeDb", range_db)                    \
  X("holdMs", hold_ms)                      \
  X("closeThresholdDb", close_threshold_db) \
  X("keyHpfHz", key_hpf_hz)

#define SONARE_FIELDS_PARALLEL_COMP(X)   \
  X("thresholdDb", threshold_db)         \
  X("ratio", ratio)                      \
  X("attackMs", attack_ms)               \
  X("releaseMs", release_ms)             \
  X("makeupGainDb", makeup_gain_db)      \
  X("mix", mix)                          \
  X("linkedDetection", linked_detection) \
  X("outputLimiter", output_limiter)     \
  X("outputCeilingDb", output_ceiling_db)

#define SONARE_FIELDS_SIDECHAIN_ROUTER(X)         \
  X("thresholdDb", threshold_db)                  \
  X("ratio", ratio)                               \
  X("attackMs", attack_ms)                        \
  X("releaseMs", release_ms)                      \
  X("rangeDb", range_db)                          \
  X("sidechainHpfEnabled", sidechain_hpf_enabled) \
  X("sidechainHpfHz", sidechain_hpf_hz)           \
  X("monoSumming", mono_summing)                  \
  X("keyListen", key_listen)                      \
  X("lookaheadMs", lookahead_ms)

#define SONARE_FIELDS_DUCKING(X) \
  X("thresholdDb", threshold_db) \
  X("ratio", ratio)              \
  X("attackMs", attack_ms)       \
  X("releaseMs", release_ms)     \
  X("rangeDb", range_db)         \
  X("lookaheadMs", lookahead_ms)

#define SONARE_FIELDS_TRANSIENT_SHAPER(X) \
  X("attackGainDb", attack_gain_db)       \
  X("sustainGainDb", sustain_gain_db)     \
  X("fastAttackMs", fast_attack_ms)       \
  X("fastReleaseMs", fast_release_ms)     \
  X("slowAttackMs", slow_attack_ms)       \
  X("slowReleaseMs", slow_release_ms)     \
  X("sensitivity", sensitivity)           \
  X("maxGainDb", max_gain_db)             \
  X("gainSmoothingMs", gain_smoothing_ms) \
  X("lookaheadMs", lookahead_ms)

#define SONARE_FIELDS_UPWARD_COMPRESSOR(X) \
  X("thresholdDb", threshold_db)           \
  X("ratio", ratio)                        \
  X("attackMs", attack_ms)                 \
  X("releaseMs", release_ms)               \
  X("rangeDb", range_db)

#define SONARE_FIELDS_UPWARD_EXPANDER(X) SONARE_FIELDS_UPWARD_COMPRESSOR(X)

#define SONARE_FIELDS_VOCAL_RIDER(X)      \
  X("targetDb", target_db)                \
  X("maxBoostDb", max_boost_db)           \
  X("maxCutDb", max_cut_db)               \
  X("attackMs", attack_ms)                \
  X("releaseMs", release_ms)              \
  X("outputGainDb", output_gain_db)       \
  X("gainSmoothingMs", gain_smoothing_ms) \
  X("noiseFloorDb", noise_floor_db)       \
  X("linkedDetection", linked_detection)

// --- Saturation ---

#define SONARE_FIELDS_TAPE(X)       \
  X("driveDb", drive_db)            \
  X("saturation", saturation)       \
  X("hysteresis", hysteresis)       \
  X("outputGainDb", output_gain_db) \
  X("speedIps", speed_ips)          \
  X("headBumpDb", head_bump_db)     \
  X("bias", bias)                   \
  X("gapLoss", gap_loss)            \
  X("oversampleFactor", oversample_factor)

#define SONARE_FIELDS_EXCITER(X) \
  X("frequencyHz", frequency_hz) \
  X("driveDb", drive_db)         \
  X("amount", amount)            \
  X("q", q)                      \
  X("evenOddMix", even_odd_mix)  \
  X("aliasing", aliasing)

#define SONARE_FIELDS_BITCRUSHER(X)        \
  X("bitDepth", bit_depth)                 \
  X("downsampleFactor", downsample_factor) \
  X("mix", mix)                            \
  X("ditherType", dither_type)             \
  X("ditherSeed", dither_seed)             \
  X("holdHz", hold_hz)                     \
  X("quantizerMode", quantizer_mode)

#define SONARE_FIELDS_HARD_CLIPPER(X) \
  X("ceiling", ceiling)               \
  X("aliasing", aliasing)

#define SONARE_FIELDS_SOFT_CLIPPER(X) \
  X("driveDb", drive_db)              \
  X("ceiling", ceiling)               \
  X("mix", mix)                       \
  X("aliasing", aliasing)

#define SONARE_FIELDS_WAVESHAPER(X) \
  X("driveDb", drive_db)            \
  X("mix", mix)                     \
  X("outputGainDb", output_gain_db) \
  X("bias", bias)                   \
  X("curve", curve)                 \
  X("aliasing", aliasing)

#define SONARE_FIELDS_TUBE(X)              \
  X("driveDb", drive_db)                   \
  X("bias", bias)                          \
  X("mix", mix)                            \
  X("oversampleFactor", oversample_factor) \
  X("biasV", bias_v)                       \
  X("harmonicDrive", harmonic_drive)

#define SONARE_FIELDS_TRANSFORMER(X) \
  X("driveDb", drive_db)             \
  X("asymmetry", asymmetry)          \
  X("mix", mix)

// --- Spectral ---

#define SONARE_FIELDS_AIR_BAND(X)               \
  X("amount", amount)                           \
  X("shelfFrequencyHz", shelf_frequency_hz)     \
  X("dynamicThresholdDb", dynamic_threshold_db) \
  X("dynamicRangeDb", dynamic_range_db)

#define SONARE_FIELDS_LOW_END_FOCUS(X)       \
  X("cutoffHz", cutoff_hz)                   \
  X("width", width)                          \
  X("subharmonicAmount", subharmonic_amount) \
  X("transientTightness", transient_tightness)

#define SONARE_FIELDS_PRESENCE_ENHANCER(X)    \
  X("amount", amount)                         \
  X("drive", drive)                           \
  X("centerFrequencyHz", center_frequency_hz) \
  X("q", q)                                   \
  X("aliasing", aliasing)

#define SONARE_FIELDS_SPECTRAL_SHAPER(X)  \
  X("threshold", threshold)               \
  X("amount", amount)                     \
  X("frequencyHz", frequency_hz)          \
  X("highFrequencyHz", high_frequency_hz) \
  X("attackMs", attack_ms)                \
  X("releaseMs", release_ms)              \
  X("rangeDb", range_db)

// --- Stereo ---

#define SONARE_FIELDS_AUTO_PAN(X) \
  X("rateHz", rate_hz)            \
  X("depth", depth)               \
  X("phase", phase)

#define SONARE_FIELDS_HAAS_ENHANCER(X) \
  X("delayMs", delay_ms)               \
  X("mix", mix)                        \
  X("delayRight", delay_right)

// The imager additionally range-checks width/decorrelationAmount in its config
// builder; only the field overlay is table-driven.
#define SONARE_FIELDS_IMAGER(X)                  \
  X("width", width)                              \
  X("outputGainDb", output_gain_db)              \
  X("decorrelationAmount", decorrelation_amount) \
  X("preserveEnergy", preserve_energy)

#define SONARE_FIELDS_MONO_MAKER(X) \
  X("amount", amount)               \
  X("frequencyHz", frequency_hz)

#define SONARE_FIELDS_PHASE_ALIGN(X) \
  X("delaySamples", delay_samples)   \
  X("delayRight", delay_right)       \
  X("fractionalDelaySamples", fractional_delay_samples)

#define SONARE_FIELDS_STEREO_BALANCE(X) \
  X("balance", balance)                 \
  X("constantPower", constant_power)

// --- Utility ---

#define SONARE_FIELDS_GAIN(X) X("levelDb", level_db)

// --- Maximizer ---

#define SONARE_FIELDS_MAXIMIZER(X) \
  X("inputGainDb", input_gain_db)  \
  X("ceilingDb", ceiling_db)       \
  X("lookaheadMs", lookahead_ms)   \
  X("releaseMs", release_ms)

#define SONARE_FIELDS_TRUE_PEAK_LIMITER(X) \
  X("ceilingDb", ceiling_db)               \
  X("lookaheadMs", lookahead_ms)           \
  X("releaseMs", release_ms)               \
  X("oversampleFactor", oversample_factor) \
  X("applyGainAtInputRate", apply_gain_at_input_rate)

#define SONARE_FIELDS_SOFT_KNEE_MAX(X) \
  X("inputGainDb", input_gain_db)      \
  X("ceilingDb", ceiling_db)           \
  X("kneeDb", knee_db)                 \
  X("releaseMs", release_ms)

#define SONARE_FIELDS_ADAPTIVE_RELEASE(X) \
  X("ceilingDb", ceiling_db)              \
  X("lookaheadMs", lookahead_ms)          \
  X("minReleaseMs", min_release_ms)       \
  X("maxReleaseMs", max_release_ms)       \
  X("crestWindowMs", crest_window_ms)     \
  X("crestLow", crest_low)                \
  X("crestHigh", crest_high)              \
  X("releaseSmoothingMs", release_smoothing_ms)

// --- Chain-only stages ---
// These do not have a processor_params.h config builder, but their flat-key
// round trip (chain_json.cpp <-> chain_params.cpp) benefits from the same shared
// table. Their fields live directly on the chain stage struct (no .config
// member), so the chain consumers supply the matching accessor.

#define SONARE_FIELDS_EQ_TILT(X) \
  X("tiltDb", tilt_db)           \
  X("pivotHz", pivot_hz)

#define SONARE_FIELDS_LOUDNESS(X)                     \
  X("targetLufs", target_lufs)                        \
  X("ceilingDb", ceiling_db)                          \
  X("truePeakOversample", true_peak_oversample)       \
  X("releaseMs", release_ms)                          \
  X("applyGainAtInputRate", apply_gain_at_input_rate) \
  X("maxLimiterGainReductionDb", max_limiter_gain_reduction_db)
