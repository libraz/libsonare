#include "acoustic/room_types.h"

namespace sonare::acoustic {

float room_volume(const RoomDimensions& dims) noexcept {
  return dims.length * dims.width * dims.height;
}

float room_surface_area(const RoomDimensions& dims) noexcept {
  return 2.0f * (dims.length * dims.width + dims.length * dims.height + dims.width * dims.height);
}

}  // namespace sonare::acoustic
