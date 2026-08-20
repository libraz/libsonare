#pragma once

/// @file serialized_enum_bounds.h
/// @brief Decode bounds for every enum the project serializer encodes.
///
/// The decoder admits an ordinal only up to the enum's last enumerator. Writing
/// that bound as a literal duplicates the enum definition, and the copy goes
/// stale the moment an enumerator is added: the encoder emits the new ordinal,
/// the decoder rejects the document it just wrote, and the outer catch in
/// project_from_json discards the whole project. Every bound here is therefore
/// stated as an enumerator, and pinned two ways -- @c declared() switches over
/// the enum with no @c default label, so a new enumerator is a -Wswitch error
/// (the build is -Werror), and @ref serialized_enum_bound_is_last static_asserts
/// that [0, kLast] is dense and that kLast + 1 is not itself an enumerator.

#include <cstdint>

#include "arrangement/edit_model.h"
#include "arrangement/edit_source.h"
#include "arrangement/harmonic_timeline.h"
#include "automation/automation_lane.h"
#include "util/automation_curve.h"

namespace sonare::serialize {

/// @brief Serialized-enum description: the last enumerator plus the exhaustive
///        enumerator set it must be the last of.
/// @details The primary template is deliberately left undefined, so encoding a
///          new enum without giving it a bound fails to compile instead of
///          silently inheriting one.
template <typename Enum>
struct SerializedEnum;

template <>
struct SerializedEnum<arrangement::WarpMode> {
  using Enum = arrangement::WarpMode;
  static constexpr Enum kLast = Enum::kTimeStretch;
  static constexpr bool declared(Enum value) noexcept {
    bool known = false;
    switch (value) {
      case Enum::kOff:
      case Enum::kRepitch:
      case Enum::kTempoSync:
      case Enum::kTimeStretch:
        known = true;
        break;
    }
    return known;
  }
};

template <>
struct SerializedEnum<arrangement::FadeCurve> {
  using Enum = arrangement::FadeCurve;
  static constexpr Enum kLast = Enum::kLogarithmic;
  static constexpr bool declared(Enum value) noexcept {
    bool known = false;
    switch (value) {
      case Enum::kLinear:
      case Enum::kEqualPower:
      case Enum::kExponential:
      case Enum::kLogarithmic:
        known = true;
        break;
    }
    return known;
  }
};

template <>
struct SerializedEnum<arrangement::LoopMode> {
  using Enum = arrangement::LoopMode;
  static constexpr Enum kLast = Enum::kLoop;
  static constexpr bool declared(Enum value) noexcept {
    bool known = false;
    switch (value) {
      case Enum::kOff:
      case Enum::kLoop:
        known = true;
        break;
    }
    return known;
  }
};

template <>
struct SerializedEnum<arrangement::OverlapPolicy> {
  using Enum = arrangement::OverlapPolicy;
  static constexpr Enum kLast = Enum::kAllow;
  static constexpr bool declared(Enum value) noexcept {
    bool known = false;
    switch (value) {
      case Enum::kDisallow:
      case Enum::kAllow:
        known = true;
        break;
    }
    return known;
  }
};

template <>
struct SerializedEnum<arrangement::MarkerKind> {
  using Enum = arrangement::MarkerKind;
  static constexpr Enum kLast = Enum::kKeySignature;
  static constexpr bool declared(Enum value) noexcept {
    bool known = false;
    switch (value) {
      case Enum::kMarker:
      case Enum::kText:
      case Enum::kLyric:
      case Enum::kCuePoint:
      case Enum::kKeySignature:
        known = true;
        break;
    }
    return known;
  }
};

template <>
struct SerializedEnum<arrangement::Track::Kind> {
  using Enum = arrangement::Track::Kind;
  static constexpr Enum kLast = Enum::kAux;
  static constexpr bool declared(Enum value) noexcept {
    bool known = false;
    switch (value) {
      case Enum::kAudio:
      case Enum::kMidi:
      case Enum::kAux:
        known = true;
        break;
    }
    return known;
  }
};

template <>
struct SerializedEnum<arrangement::SourceKind> {
  using Enum = arrangement::SourceKind;
  static constexpr Enum kLast = Enum::kMidi;
  static constexpr bool declared(Enum value) noexcept {
    bool known = false;
    switch (value) {
      case Enum::kAudio:
      case Enum::kMidi:
        known = true;
        break;
    }
    return known;
  }
};

template <>
struct SerializedEnum<arrangement::ChordQuality> {
  using Enum = arrangement::ChordQuality;
  static constexpr Enum kLast = Enum::kSuspended;
  static constexpr bool declared(Enum value) noexcept {
    bool known = false;
    switch (value) {
      case Enum::kUnknown:
      case Enum::kMajor:
      case Enum::kMinor:
      case Enum::kDiminished:
      case Enum::kAugmented:
      case Enum::kDominant:
      case Enum::kHalfDiminished:
      case Enum::kSuspended:
        known = true;
        break;
    }
    return known;
  }
};

template <>
struct SerializedEnum<arrangement::KeyMode> {
  using Enum = arrangement::KeyMode;
  static constexpr Enum kLast = Enum::kLocrian;
  static constexpr bool declared(Enum value) noexcept {
    bool known = false;
    switch (value) {
      case Enum::kUnknown:
      case Enum::kMajor:
      case Enum::kMinor:
      case Enum::kDorian:
      case Enum::kPhrygian:
      case Enum::kLydian:
      case Enum::kMixolydian:
      case Enum::kLocrian:
        known = true;
        break;
    }
    return known;
  }
};

template <>
struct SerializedEnum<automation::AutomationTargetKind> {
  using Enum = automation::AutomationTargetKind;
  static constexpr Enum kLast = Enum::kTrackPan;
  static constexpr bool declared(Enum value) noexcept {
    bool known = false;
    switch (value) {
      case Enum::kOpaque:
      case Enum::kTrackFaderDb:
      case Enum::kTrackPan:
        known = true;
        break;
    }
    return known;
  }
};

template <>
struct SerializedEnum<AutomationCurve> {
  using Enum = AutomationCurve;
  static constexpr Enum kLast = Enum::SCurve;
  static constexpr bool declared(Enum value) noexcept {
    bool known = false;
    switch (value) {
      case Enum::Linear:
      case Enum::Exponential:
      case Enum::Hold:
      case Enum::SCurve:
        known = true;
        break;
    }
    return known;
  }
};

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
    if (!SerializedEnum<Enum>::declared(static_cast<Enum>(ordinal))) return false;
  }
  return !SerializedEnum<Enum>::declared(static_cast<Enum>(kMaxSerializedOrdinal<Enum> + 1u));
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

}  // namespace sonare::serialize
