#pragma once

/// @file room_field_checks.h
/// @brief Per-member refusals for the shoebox room, placement and RIR synthesis fields.
/// @details Each check has the acceptance of the matching diagnostic in acoustic/room_model.cpp and
///          acoustic/rir_synthesizer.cpp, and runs ahead of it so a refusal names the member.

#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "acoustic/late_reverb.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "util/parameter_checks.h"

namespace sonare::effects::common {

/// @brief What RirSynthConfig::ism_order accepts.
inline constexpr util::check::NumericDomain kRirIsmOrderDomain =
    util::check::integer_domain(0, std::numeric_limits<int>::max());

/// Config member paths of the three geometry members a room config holds.
struct RoomMemberPaths {
  std::string dims;      ///< RoomDimensions member, e.g. "dims"
  std::string source;    ///< source Vec3 member
  std::string listener;  ///< listener Vec3 member
};

/// Refuses a dimension that is not finite, positive and within the model bound, then a source or
/// listener coordinate outside [0, extent] on its axis.
inline void check_room_geometry(const RoomDimensions& dims, const ::sonare::acoustic::Vec3& source,
                                const ::sonare::acoustic::Vec3& listener,
                                const RoomMemberPaths& paths) {
  namespace check = ::sonare::util::check;
  const struct {
    const char* axis;
    float extent;
  } extents[3] = {{"length", dims.length}, {"width", dims.width}, {"height", dims.height}};
  for (const auto& e : extents) {
    check::in_interval(e.extent, 0.0, false, ::sonare::acoustic::kMaxRoomDimensionM, true,
                       (paths.dims + "." + e.axis).c_str());
  }
  const struct {
    const std::string* path;
    const ::sonare::acoustic::Vec3* point;
  } points[2] = {{&paths.source, &source}, {&paths.listener, &listener}};
  for (const auto& p : points) {
    const float coords[3] = {p.point->x, p.point->y, p.point->z};
    const char* axes[3] = {".x", ".y", ".z"};
    for (int i = 0; i < 3; ++i) {
      check::in_range(coords[i], 0.0, extents[i].extent, (*p.path + axes[i]).c_str());
    }
  }
}

/// Refuses an image-source order below 0, a timing outside its cap, and, while air absorption is
/// on, a temperature at or below absolute zero or a humidity outside [0, 100].
inline void check_rir_synth_config(const ::sonare::acoustic::RirSynthConfig& config) {
  namespace check = ::sonare::util::check;
  check::integer_within(config.ism_order, kRirIsmOrderDomain, "ism_order");
  check::in_range(config.max_seconds, 0.0, ::sonare::acoustic::kMaxRirSeconds, "max_seconds");
  check::in_range(config.mixing_time_ms, 0.0, ::sonare::acoustic::kMaxRirMixingTimeMs,
                  "mixing_time_ms");
  check::in_range(config.crossfade_ms, 0.0, ::sonare::acoustic::kMaxRirCrossfadeMs, "crossfade_ms");
  if (config.air_absorption_enabled) {
    check::greater_than(config.air.temperature_c, ::sonare::acoustic::kAbsoluteZeroCelsius,
                        "air.temperature_c");
    check::in_range(config.air.humidity_percent, 0.0, 100.0, "air.humidity_percent");
  }
}

/// Refuses a receiver spacing outside (0, kMaxReceiverSpacingM], then a spacing that puts either
/// receiver outside the room, naming the plan-view coordinates of @p supplied beside the room.
inline void check_receiver_pair(const ::sonare::acoustic::ShoeboxRoom& room,
                                const ::sonare::acoustic::SourceListener& placement,
                                float spacing_m, const RoomMemberPaths& paths,
                                const ::sonare::acoustic::SuppliedPlacement& supplied = {}) {
  namespace check = ::sonare::util::check;
  if (!supplied.receiver_spacing && !(spacing_m > 0.0f)) {
    // A chosen spacing is 0 only when the listener sits on a wall the pair axis crosses.
    const bool x = ::sonare::acoustic::pair_blocking_wall_axis(room.dims, placement) == 0;
    check::in_interval(x ? placement.listener.x : placement.listener.y, 0.0, false,
                       x ? room.dims.length : room.dims.width, false,
                       (paths.listener + (x ? ".x" : ".y")).c_str());
  }
  check::in_interval(spacing_m, 0.0, false, ::sonare::acoustic::kMaxReceiverSpacingM, true,
                     "receiver_spacing_m");
  const ::sonare::acoustic::ReceiverPair pair =
      ::sonare::acoustic::receiver_pair(placement, spacing_m);
  if (!::sonare::acoustic::point_inside_shoebox(room, pair.left) ||
      !::sonare::acoustic::point_inside_shoebox(room, pair.right)) {
    std::vector<std::string> subjects = {"receiver_spacing_m"};
    const struct {
      const std::string* path;
      const std::array<bool, 3>* given;
    } points[2] = {{&paths.listener, &supplied.listener}, {&paths.source, &supplied.source}};
    for (const auto& point : points) {
      if ((*point.given)[0]) subjects.push_back(*point.path + ".x");
      if ((*point.given)[1]) subjects.push_back(*point.path + ".y");
    }
    subjects.push_back(paths.dims + ".length");
    subjects.push_back(paths.dims + ".width");
    std::string text = "{0} must place both receivers inside the room for ";
    for (size_t i = 1; i < subjects.size(); ++i) {
      text += (i == 1 ? "" : i + 1 == subjects.size() ? " and " : ", ");
      text += "{" + std::to_string(i) + "}";
    }
    check::refuse_relation(std::move(subjects), text);
  }
}

}  // namespace sonare::effects::common
