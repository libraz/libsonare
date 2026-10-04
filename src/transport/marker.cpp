#include "transport/marker.h"

#include <algorithm>
#include <memory>

namespace sonare::transport {

void MarkerMap::set_markers(std::vector<Marker> markers) {
  std::sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) {
    if (a.ppq != b.ppq) return a.ppq < b.ppq;
    return a.id < b.id;
  });
  markers_.publish(std::make_shared<const std::vector<Marker>>(std::move(markers)));
}

size_t MarkerMap::marker_count() const noexcept {
  const auto* markers = markers_.load();
  return markers ? markers->size() : 0;
}

bool MarkerMap::marker_by_id(uint32_t id, Marker* out) const noexcept {
  const auto* markers = markers_.load();
  if (!markers) return false;
  for (const Marker& marker : *markers) {
    if (marker.id == id) {
      if (out) *out = marker;
      return true;
    }
  }
  return false;
}

bool MarkerMap::marker_by_index(size_t index, Marker* out) const noexcept {
  const auto* markers = markers_.load();
  if (!markers || index >= markers->size()) return false;
  if (out) *out = (*markers)[index];
  return true;
}

bool MarkerMap::next_marker(double ppq, Marker* out) const noexcept {
  const auto* markers = markers_.load();
  if (!markers) return false;
  for (const Marker& marker : *markers) {
    if (marker.ppq > ppq) {
      if (out) *out = marker;
      return true;
    }
  }
  return false;
}

bool MarkerMap::previous_marker(double ppq, Marker* out) const noexcept {
  const auto* markers = markers_.load();
  if (!markers) return false;
  for (auto it = markers->rbegin(); it != markers->rend(); ++it) {
    if (it->ppq < ppq) {
      if (out) *out = *it;
      return true;
    }
  }
  return false;
}

}  // namespace sonare::transport
