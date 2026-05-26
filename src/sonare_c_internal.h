#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "automation/automation_lane.h"
#include "automation/parameter.h"
#include "core/audio.h"
#include "engine/realtime_engine.h"
#include "sonare_c.h"
#include "util/exception.h"
#include "util/types.h"

struct SonareAudio {
  sonare::Audio audio;
};

// Realtime engine handle. Defined here (rather than in sonare_c_daw.cpp) so that
// other C API translation units can access engine internals without ODR or
// linker fragility. The public header keeps the opaque forward-declaration.
struct SonareRealtimeEngine {
  sonare::engine::RealtimeEngine engine;
  sonare::automation::ParameterRegistry parameters;
  std::deque<std::string> parameter_strings;
  std::deque<std::string> marker_strings;
  std::vector<sonare::automation::AutomationLane> automation_lanes;
};

namespace sonare_c_detail {

using sonare::Audio;
using sonare::ChordQuality;
using sonare::SonareException;

constexpr int kMinSampleRate = 8000;
constexpr int kMaxSampleRate = 384000;
constexpr size_t kMaxBufferSize = 500000000;

std::string& last_error_storage();
void set_last_error(const char* msg);
SonareError map_sonare_exception(const SonareException& e);
SonareError validate_audio_params(const float* samples, size_t length, int sample_rate);
SonareGrooveType to_c_groove_type(const std::string& groove);
SonareChordQuality to_c_chord_quality(ChordQuality quality);

template <typename T>
T* release_array(std::unique_ptr<T[]>& ptr) {
  return ptr.release();
}

}  // namespace sonare_c_detail

#define SONARE_C_TRY try {
#define SONARE_C_CATCH                                                                  \
  }                                                                                     \
  catch (const sonare::SonareException& e) {                                            \
    sonare_c_detail::set_last_error(e.what());                                          \
    return sonare_c_detail::map_sonare_exception(e);                                    \
  }                                                                                     \
  catch (const std::bad_alloc& e) {                                                     \
    sonare_c_detail::set_last_error(e.what());                                          \
    return SONARE_ERROR_OUT_OF_MEMORY;                                                  \
  }                                                                                     \
  catch (const std::exception& e) {                                                     \
    sonare_c_detail::set_last_error(e.what());                                          \
    return SONARE_ERROR_UNKNOWN;                                                        \
  }                                                                                     \
  catch (...) {                                                                         \
    sonare_c_detail::set_last_error("Unknown C++ exception (non-std::exception type)"); \
    return SONARE_ERROR_UNKNOWN;                                                        \
  }
