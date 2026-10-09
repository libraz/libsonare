#pragma once

#include <mach/mach_time.h>

#include <cstdint>
#include <limits>

namespace sonare::host::backends::detail {

inline bool host_ticks_to_ns(uint64_t ticks, const mach_timebase_info_data_t& timebase,
                             uint64_t* out) noexcept {
  if (out == nullptr || timebase.denom == 0) return false;
  const unsigned __int128 scaled = static_cast<unsigned __int128>(ticks) *
                                   static_cast<unsigned __int128>(timebase.numer) /
                                   static_cast<unsigned __int128>(timebase.denom);
  if (scaled > std::numeric_limits<uint64_t>::max()) return false;
  *out = static_cast<uint64_t>(scaled);
  return true;
}

}  // namespace sonare::host::backends::detail
