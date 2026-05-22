#pragma once

#include <memory>
#include <string>

#include "analysis/chord_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "core/audio.h"
#include "sonare_c.h"
#include "util/exception.h"

struct SonareAudio {
  sonare::Audio audio;
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
