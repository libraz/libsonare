#pragma once

/// @file marker.h
/// @brief Timeline marker registry and cue navigation helpers.

#include <cstdint>
#include <memory>
#include <vector>

#include "rt/rt_publisher.h"

namespace sonare::transport {

struct Marker {
  double ppq = 0.0;
  uint32_t id = 0;
  // Non-owning pointer: must reference storage that outlives the MarkerMap
  // (e.g. a string literal or a caller-owned stable buffer). Passing a
  // temporary's c_str() will dangle.
  const char* name = "";
  // Marker kind + key signature. `kind` mirrors SonareMarkerKind values
  // (0 = marker, 1 = text, 2 = lyric, 3 = cue point, 4 = key signature); the
  // key fields apply only to the key-signature kind.
  uint8_t kind = 0;
  int8_t key_fifths = 0;
  bool key_minor = false;
};

class MarkerMap {
 public:
  void set_markers(std::vector<Marker> markers);

  size_t marker_count() const noexcept;
  bool marker_by_id(uint32_t id, Marker* out) const noexcept;
  bool marker_by_index(size_t index, Marker* out) const noexcept;
  bool next_marker(double ppq, Marker* out) const noexcept;
  bool previous_marker(double ppq, Marker* out) const noexcept;

 private:
  rt::RtSnapshot<std::vector<Marker>> markers_;
};

}  // namespace sonare::transport
