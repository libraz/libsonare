#pragma once

// Internal test instrumentation for the polyhedral image-source frontier. This
// header deliberately lives under src/ rather than include/ so it is not part
// of the library's supported C++ API or ABI.

#include <cstddef>

namespace sonare::acoustic::test {

using PolyhedralFrontierObserver = void (*)(std::size_t);

// Installs an optional observer used only by the acoustic regression tests.
// Passing nullptr disables it. The observer must be configured before the
// image-source call; it is never a real-time path.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("hidden")))
#endif
void set_polyhedral_frontier_observer(PolyhedralFrontierObserver observer) noexcept;

}  // namespace sonare::acoustic::test
