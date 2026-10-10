#pragma once

/// @file param_meta.h
/// @brief Declared presentation metadata of a flat mastering parameter: its unit,
///        its axis scale and an optional display range.
/// @details A config builder states these where it reads the key, next to the
///          type and default the accessors already record, so the catalog
///          publishes what the reader declared rather than a guess from the
///          key's spelling. The metadata describes construction-time acceptance;
///          the realtime parameter path clamps, and neither is described by it.

#include <cstdint>
#include <limits>

namespace sonare::mastering::api::detail {

/// @brief Closed unit vocabulary a numeric parameter declares. @c None is a
///        declaration (a fraction, a selector index, a seed), not an absence.
enum class Unit : std::uint8_t {
  None,
  Db,
  Dbfs,
  Lufs,
  Hz,
  Ms,
  Seconds,
  Samples,
  Meters,
  Centimeters,
  Millimeters,
  Degrees,
  Percent,
  DegreesCelsius,
  Volts,
  InchesPerSecond,
  DbPerOctave,
  Semitones,
  Cents,
  Ratio,
  Bits,
  Count,
};

/// @brief Axis a control draws the parameter on.
enum class Scale : std::uint8_t { Linear, Log };

/// @brief How a parameter's value is ordered against a sibling's.
enum class Relation : std::uint8_t { Lt, Le, Gt, Ge };

/// @brief Published spelling of @p unit.
constexpr const char* unit_name(Unit unit) {
  switch (unit) {
    case Unit::None:
      return "none";
    case Unit::Db:
      return "dB";
    case Unit::Dbfs:
      return "dBFS";
    case Unit::Lufs:
      return "LUFS";
    case Unit::Hz:
      return "Hz";
    case Unit::Ms:
      return "ms";
    case Unit::Seconds:
      return "s";
    case Unit::Samples:
      return "samples";
    case Unit::Meters:
      return "m";
    case Unit::Centimeters:
      return "cm";
    case Unit::Millimeters:
      return "mm";
    case Unit::Degrees:
      return "deg";
    case Unit::Percent:
      return "percent";
    case Unit::DegreesCelsius:
      return "degC";
    case Unit::Volts:
      return "V";
    case Unit::InchesPerSecond:
      return "inPerSec";
    case Unit::DbPerOctave:
      return "dBPerOct";
    case Unit::Semitones:
      return "semitones";
    case Unit::Cents:
      return "cents";
    case Unit::Ratio:
      return "ratio";
    case Unit::Bits:
      return "bits";
    case Unit::Count:
      return "count";
  }
  return nullptr;
}

/// @brief The relation that holds from the other side: `a le b` is `b ge a`.
constexpr Relation inverse(Relation relation) {
  switch (relation) {
    case Relation::Lt:
      return Relation::Gt;
    case Relation::Le:
      return Relation::Ge;
    case Relation::Gt:
      return Relation::Lt;
    case Relation::Ge:
      return Relation::Le;
  }
  return relation;
}

/// @brief Published spelling of @p relation.
constexpr const char* relation_name(Relation relation) {
  switch (relation) {
    case Relation::Lt:
      return "lt";
    case Relation::Le:
      return "le";
    case Relation::Gt:
      return "gt";
    case Relation::Ge:
      return "ge";
  }
  return nullptr;
}

/// @brief What a reader declares about one numeric key.
struct ParamMeta {
  Unit unit = Unit::None;
  Scale scale = Scale::Linear;
  /// Display range; NaN means the accepted range is also the display range.
  double ui_min = std::numeric_limits<double>::quiet_NaN();
  double ui_max = std::numeric_limits<double>::quiet_NaN();
};

/// @brief @p meta drawn on a logarithmic axis.
constexpr ParamMeta logarithmic(ParamMeta meta) {
  meta.scale = Scale::Log;
  return meta;
}

/// @brief Lowest display value of a logarithmic axis in @p unit: a log axis cannot start at zero.
constexpr double log_axis_floor(Unit unit) {
  switch (unit) {
    case Unit::Hz:
      return 20.0;
    case Unit::Ms:
      return 0.1;
    case Unit::Seconds:
      return 0.01;
    case Unit::Samples:
      return 1.0;
    default:
      return 1e-3;
  }
}

/// @brief Highest display value of a logarithmic axis in @p unit whose accepted range is open
/// above.
constexpr double log_axis_ceiling(Unit unit) {
  switch (unit) {
    case Unit::Hz:
      return 20000.0;
    case Unit::Ms:
      return 10000.0;
    case Unit::Seconds:
      return 60.0;
    case Unit::Samples:
      return 1048576.0;
    default:
      return 1e3;
  }
}

/// @brief A display bound that defers to the accepted bound (needed where that bound is exclusive).
inline constexpr double kAcceptedBound = std::numeric_limits<double>::quiet_NaN();

/// @brief @p meta with a display range inside the accepted one.
constexpr ParamMeta display_range(ParamMeta meta, double ui_min, double ui_max) {
  meta.ui_min = ui_min;
  meta.ui_max = ui_max;
  return meta;
}

inline constexpr ParamMeta kNone{Unit::None};
inline constexpr ParamMeta kDb{Unit::Db};
inline constexpr ParamMeta kDbfs{Unit::Dbfs};
inline constexpr ParamMeta kLufs{Unit::Lufs};
inline constexpr ParamMeta kHz{Unit::Hz};
inline constexpr ParamMeta kHzLog{Unit::Hz, Scale::Log};
inline constexpr ParamMeta kMs{Unit::Ms};
inline constexpr ParamMeta kMsLog{Unit::Ms, Scale::Log};
inline constexpr ParamMeta kSeconds{Unit::Seconds};
inline constexpr ParamMeta kSecondsLog{Unit::Seconds, Scale::Log};
inline constexpr ParamMeta kSamples{Unit::Samples};
inline constexpr ParamMeta kMeters{Unit::Meters};
inline constexpr ParamMeta kCentimeters{Unit::Centimeters};
inline constexpr ParamMeta kMillimeters{Unit::Millimeters};
inline constexpr ParamMeta kDegrees{Unit::Degrees};
inline constexpr ParamMeta kPercent{Unit::Percent};
inline constexpr ParamMeta kDegreesCelsius{Unit::DegreesCelsius};
inline constexpr ParamMeta kVolts{Unit::Volts};
inline constexpr ParamMeta kInchesPerSecond{Unit::InchesPerSecond};
inline constexpr ParamMeta kDbPerOctave{Unit::DbPerOctave};
inline constexpr ParamMeta kSemitones{Unit::Semitones};
inline constexpr ParamMeta kCents{Unit::Cents};
inline constexpr ParamMeta kRatio{Unit::Ratio};
inline constexpr ParamMeta kBits{Unit::Bits};
inline constexpr ParamMeta kCount{Unit::Count};

}  // namespace sonare::mastering::api::detail
