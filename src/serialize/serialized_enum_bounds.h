#pragma once

/// @file serialized_enum_bounds.h
/// @brief Token names and decode bounds for every enum the project serializer encodes.
///
/// Each enum's @c name() is the one spelling table its document writer, its
/// document reader and the argument parsers of every surface read: the
/// document carries the token, never the ordinal.
///
/// The decoder admits an ordinal only up to the enum's last enumerator. Writing
/// that bound as a literal duplicates the enum definition, and the copy goes
/// stale the moment an enumerator is added: the encoder emits the new ordinal,
/// the decoder rejects the document it just wrote, and the outer catch in
/// project_from_json discards the whole project. Every bound here is therefore
/// stated as an enumerator, and pinned two ways -- @c name() switches over
/// the enum with no @c default label, so a new enumerator is a -Wswitch error
/// (the build is -Werror), and @ref serialized_enum_bound_is_last static_asserts
/// that [0, kLast] is dense and that kLast + 1 is not itself an enumerator.

#include <cstdint>
#include <string>
#include <string_view>

#include "arrangement/edit_model.h"
#include "arrangement/edit_source.h"
#include "arrangement/harmonic_timeline.h"
#include "automation/automation_lane.h"
#include "util/automation_curve.h"

namespace sonare::serialize {

/// @brief Serialized-enum description: the last enumerator plus the token of
///        every enumerator, null for a value that is none.
/// @details The primary template is deliberately left undefined, so encoding a
///          new enum without giving it a bound fails to compile instead of
///          silently inheriting one.
template <typename Enum>
struct SerializedEnum;

template <>
struct SerializedEnum<arrangement::WarpMode> {
  using Enum = arrangement::WarpMode;
  static constexpr Enum kLast = Enum::kTimeStretch;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::kOff:
        return "off";
      case Enum::kRepitch:
        return "repitch";
      case Enum::kTempoSync:
        return "tempoSync";
      case Enum::kTimeStretch:
        return "timeStretch";
    }
    return nullptr;
  }
};

template <>
struct SerializedEnum<arrangement::FadeCurve> {
  using Enum = arrangement::FadeCurve;
  static constexpr Enum kLast = Enum::kLogarithmic;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::kLinear:
        return "linear";
      case Enum::kEqualPower:
        return "equalPower";
      case Enum::kExponential:
        return "exponential";
      case Enum::kLogarithmic:
        return "logarithmic";
    }
    return nullptr;
  }
};

template <>
struct SerializedEnum<arrangement::LoopMode> {
  using Enum = arrangement::LoopMode;
  static constexpr Enum kLast = Enum::kLoop;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::kOff:
        return "off";
      case Enum::kLoop:
        return "loop";
    }
    return nullptr;
  }
};

template <>
struct SerializedEnum<arrangement::OverlapPolicy> {
  using Enum = arrangement::OverlapPolicy;
  static constexpr Enum kLast = Enum::kAllow;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::kDisallow:
        return "disallow";
      case Enum::kAllow:
        return "allow";
    }
    return nullptr;
  }
};

template <>
struct SerializedEnum<arrangement::MarkerKind> {
  using Enum = arrangement::MarkerKind;
  static constexpr Enum kLast = Enum::kKeySignature;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::kMarker:
        return "marker";
      case Enum::kText:
        return "text";
      case Enum::kLyric:
        return "lyric";
      case Enum::kCuePoint:
        return "cuePoint";
      case Enum::kKeySignature:
        return "keySignature";
    }
    return nullptr;
  }
};

template <>
struct SerializedEnum<arrangement::Track::Kind> {
  using Enum = arrangement::Track::Kind;
  static constexpr Enum kLast = Enum::kAux;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::kAudio:
        return "audio";
      case Enum::kMidi:
        return "midi";
      case Enum::kAux:
        return "aux";
    }
    return nullptr;
  }
};

template <>
struct SerializedEnum<arrangement::SourceKind> {
  using Enum = arrangement::SourceKind;
  static constexpr Enum kLast = Enum::kMidi;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::kAudio:
        return "audio";
      case Enum::kMidi:
        return "midi";
    }
    return nullptr;
  }
};

template <>
struct SerializedEnum<arrangement::ChordQuality> {
  using Enum = arrangement::ChordQuality;
  static constexpr Enum kLast = Enum::kSuspended;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::kUnknown:
        return "unknown";
      case Enum::kMajor:
        return "major";
      case Enum::kMinor:
        return "minor";
      case Enum::kDiminished:
        return "diminished";
      case Enum::kAugmented:
        return "augmented";
      case Enum::kDominant:
        return "dominant";
      case Enum::kHalfDiminished:
        return "halfDiminished";
      case Enum::kSuspended:
        return "suspended";
    }
    return nullptr;
  }
};

template <>
struct SerializedEnum<arrangement::KeyMode> {
  using Enum = arrangement::KeyMode;
  static constexpr Enum kLast = Enum::kLocrian;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::kUnknown:
        return "unknown";
      case Enum::kMajor:
        return "major";
      case Enum::kMinor:
        return "minor";
      case Enum::kDorian:
        return "dorian";
      case Enum::kPhrygian:
        return "phrygian";
      case Enum::kLydian:
        return "lydian";
      case Enum::kMixolydian:
        return "mixolydian";
      case Enum::kLocrian:
        return "locrian";
    }
    return nullptr;
  }
};

template <>
struct SerializedEnum<automation::AutomationTargetKind> {
  using Enum = automation::AutomationTargetKind;
  static constexpr Enum kLast = Enum::kTrackPan;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::kOpaque:
        return "opaque";
      case Enum::kTrackFaderDb:
        return "trackFaderDb";
      case Enum::kTrackPan:
        return "trackPan";
    }
    return nullptr;
  }
};

template <>
struct SerializedEnum<AutomationCurve> {
  using Enum = AutomationCurve;
  static constexpr Enum kLast = Enum::SCurve;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::Linear:
        return "linear";
      case Enum::Exponential:
        return "exponential";
      case Enum::Hold:
        return "hold";
      case Enum::SCurve:
        return "sCurve";
    }
    return nullptr;
  }
};

/// @brief The token of @p value, or null when @p value is no enumerator.
template <typename Enum>
constexpr const char* serialized_enum_name(Enum value) noexcept {
  return SerializedEnum<Enum>::name(value);
}

/// @brief Highest ordinal the decoder accepts for @p Enum.
template <typename Enum>
inline constexpr uint32_t kMaxSerializedOrdinal =
    static_cast<uint32_t>(SerializedEnum<Enum>::kLast);

/// @brief True when [0, kLast] are all enumerators and kLast + 1 is not, i.e.
///        the bound really is the last enumerator of a dense ordinal range.
/// @details Density matters as much as the upper bound: the decoder accepts
///          every ordinal at or below the bound, so a gap would admit a value
///          the enum has no meaning for.
template <typename Enum>
constexpr bool serialized_enum_bound_is_last() noexcept {
  for (uint32_t ordinal = 0; ordinal <= kMaxSerializedOrdinal<Enum>; ++ordinal) {
    if (serialized_enum_name(static_cast<Enum>(ordinal)) == nullptr) return false;
  }
  return serialized_enum_name(static_cast<Enum>(kMaxSerializedOrdinal<Enum> + 1u)) == nullptr;
}

static_assert(serialized_enum_bound_is_last<arrangement::WarpMode>(),
              "WarpMode decode bound is not the last enumerator of a dense range");
static_assert(serialized_enum_bound_is_last<arrangement::FadeCurve>(),
              "FadeCurve decode bound is not the last enumerator of a dense range");
static_assert(serialized_enum_bound_is_last<arrangement::LoopMode>(),
              "LoopMode decode bound is not the last enumerator of a dense range");
static_assert(serialized_enum_bound_is_last<arrangement::OverlapPolicy>(),
              "OverlapPolicy decode bound is not the last enumerator of a dense range");
static_assert(serialized_enum_bound_is_last<arrangement::MarkerKind>(),
              "MarkerKind decode bound is not the last enumerator of a dense range");
static_assert(serialized_enum_bound_is_last<arrangement::Track::Kind>(),
              "Track::Kind decode bound is not the last enumerator of a dense range");
static_assert(serialized_enum_bound_is_last<arrangement::SourceKind>(),
              "SourceKind decode bound is not the last enumerator of a dense range");
static_assert(serialized_enum_bound_is_last<arrangement::ChordQuality>(),
              "ChordQuality decode bound is not the last enumerator of a dense range");
static_assert(serialized_enum_bound_is_last<arrangement::KeyMode>(),
              "KeyMode decode bound is not the last enumerator of a dense range");
static_assert(serialized_enum_bound_is_last<automation::AutomationTargetKind>(),
              "AutomationTargetKind decode bound is not the last enumerator of a dense range");
static_assert(serialized_enum_bound_is_last<AutomationCurve>(),
              "AutomationCurve decode bound is not the last enumerator of a dense range");

/// @brief The enumerator @p token names; false when it names none.
template <typename Enum>
bool serialized_enum_from_name(std::string_view token, Enum* out) noexcept {
  for (uint32_t ordinal = 0; ordinal <= kMaxSerializedOrdinal<Enum>; ++ordinal) {
    const auto value = static_cast<Enum>(ordinal);
    if (token == serialized_enum_name(value)) {
      *out = value;
      return true;
    }
  }
  return false;
}

/// @brief Every token of @p Enum in ordinal order, comma-separated: "a, b, c".
template <typename Enum>
std::string serialized_enum_choices() {
  std::string names;
  for (uint32_t ordinal = 0; ordinal <= kMaxSerializedOrdinal<Enum>; ++ordinal) {
    if (!names.empty()) names += ", ";
    names += serialized_enum_name(static_cast<Enum>(ordinal));
  }
  return names;
}

}  // namespace sonare::serialize
