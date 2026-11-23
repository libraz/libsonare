#pragma once

/// @file automation_lane.h
/// @brief Persistent PPQ breakpoint automation lane.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "automation/parameter.h"

namespace sonare::automation {

/// Persistent target classification for a project automation lane.
///
/// Opaque lanes retain host-defined target semantics.
enum class AutomationTargetKind : uint32_t {
  kOpaque = 0,
  kTrackFaderDb = 1,
  kTrackPan = 2,
  kTrackSendDb = 3,
  kTrackWidth = 4,
  kTrackInsertParam = 5,
};

static_assert(static_cast<uint32_t>(AutomationTargetKind::kOpaque) == 0u,
              "AutomationTargetKind opaque ordinal drift");
static_assert(static_cast<uint32_t>(AutomationTargetKind::kTrackFaderDb) == 1u,
              "AutomationTargetKind fader ordinal drift");
static_assert(static_cast<uint32_t>(AutomationTargetKind::kTrackPan) == 2u,
              "AutomationTargetKind pan ordinal drift");

constexpr bool valid_automation_target_kind(AutomationTargetKind kind) noexcept {
  return static_cast<uint32_t>(kind) <=
         static_cast<uint32_t>(AutomationTargetKind::kTrackInsertParam);
}

struct AutomationTargetIdentity {
  std::string send_id;
  std::optional<std::size_t> insert_index;
  std::string processor;
  std::string param_name;

  bool operator==(const AutomationTargetIdentity& other) const noexcept {
    return send_id == other.send_id && insert_index == other.insert_index &&
           processor == other.processor && param_name == other.param_name;
  }
};

struct Breakpoint {
  double ppq = 0.0;
  float value = 0.0f;
  CurveType curve_to_next = CurveType::Linear;

  bool operator==(const Breakpoint& o) const noexcept {
    return ppq == o.ppq && value == o.value && curve_to_next == o.curve_to_next;
  }
  bool operator!=(const Breakpoint& o) const noexcept { return !(*this == o); }
};

inline bool valid_public_breakpoint(double ppq, float value, int curve_ordinal) noexcept {
  return std::isfinite(ppq) && std::isfinite(value) && curve_ordinal >= 0 &&
         curve_ordinal <= static_cast<int>(CurveType::SCurve);
}

inline bool valid_public_breakpoint(const Breakpoint& point) noexcept {
  return valid_public_breakpoint(point.ppq, point.value, static_cast<int>(point.curve_to_next));
}

class AutomationLane {
 public:
  AutomationLane() = default;
  explicit AutomationLane(uint32_t target_param_id,
                          AutomationTargetKind target_kind = AutomationTargetKind::kOpaque);
  AutomationLane(uint32_t target_param_id, AutomationTargetKind target_kind,
                 AutomationTargetIdentity identity);

  uint32_t lane_id() const noexcept { return lane_id_; }
  void set_lane_id(uint32_t id) noexcept { lane_id_ = id; }
  uint32_t target_param_id() const noexcept { return target_param_id_; }
  void set_target_param_id(uint32_t id) noexcept { target_param_id_ = id; }
  AutomationTargetKind target_kind() const noexcept { return target_kind_; }
  void set_target_kind(AutomationTargetKind kind) noexcept { target_kind_ = kind; }
  const AutomationTargetIdentity& target_identity() const noexcept { return identity_; }
  bool valid_target_identity() const noexcept;
  bool same_target_identity(const AutomationLane& other) const noexcept {
    return target_kind_ == other.target_kind_ && identity_ == other.identity_;
  }
  /// Sorts by ppq (stable) and drops duplicate-ppq points, keeping the FIRST
  /// occurrence in the supplied list. A lane therefore cannot represent an
  /// instantaneous jump as two points at the same ppq — place the second point
  /// an epsilon later (or use a Hold curve on the preceding segment) instead.
  void set_points(std::vector<Breakpoint> points);
  const std::vector<Breakpoint>& points() const noexcept { return points_; }

  float value_at(double ppq) const noexcept;
  double next_breakpoint_after(double ppq) const noexcept;

  bool operator==(const AutomationLane& o) const noexcept {
    return lane_id_ == o.lane_id_ && target_param_id_ == o.target_param_id_ &&
           target_kind_ == o.target_kind_ && identity_ == o.identity_ && points_ == o.points_;
  }
  bool operator!=(const AutomationLane& o) const noexcept { return !(*this == o); }

 private:
  uint32_t lane_id_ = 0;
  uint32_t target_param_id_ = 0;
  AutomationTargetKind target_kind_ = AutomationTargetKind::kOpaque;
  AutomationTargetIdentity identity_;
  std::vector<Breakpoint> points_;
};

}  // namespace sonare::automation
