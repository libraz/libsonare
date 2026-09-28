#include "playback/shrf_format.h"

#include "util/exception.h"

namespace sonare::playback {

ShrfData parse_shrf(const uint8_t* data, size_t size) {
  (void)data;
  (void)size;
  throw SonareException(ErrorCode::NotImplemented, "SHRF parsing is not implemented");
}

}  // namespace sonare::playback
