#pragma once

#include <cstring>
#include <deque>
#include <memory>
#include <sstream>
#include <stdexcept>
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

/// @brief Allocates a NUL-terminated heap copy of @p value (caller frees via
///        sonare_free_string). Canonical owner of the C-string copy helper
///        shared by all C API translation units.
inline char* copy_string(const std::string& value) {
  std::unique_ptr<char[]> out(new char[value.size() + 1]);
  std::memcpy(out.get(), value.c_str(), value.size() + 1);
  return out.release();
}

/// @brief Joins @p values with '\n' into @p storage and returns a borrowed
///        pointer to the stored string's buffer (valid while @p storage lives).
inline const char* join_names(const std::vector<std::string>& values, std::string& storage) {
  std::ostringstream stream;
  for (size_t index = 0; index < values.size(); ++index) {
    if (index > 0) stream << '\n';
    stream << values[index];
  }
  storage = stream.str();
  return storage.c_str();
}

/// @brief Runs an Audio → Audio DSP function against a mono C buffer and
///        marshals the result back out through @p out / @p out_length.
/// @details Centralises the validate → from_buffer → process → memcpy → catch
///          boilerplate that every offline effects wrapper performed
///          inline. The supplied callable receives the wrapped Audio and must
///          return an Audio whose samples are copied into a fresh
///          heap-allocated array owned by the caller (freed via the existing
///          sonare_free_array entry point).
/// @param samples       Caller-owned input buffer (validated for null / NaN /
///                      Inf / sample_rate by validate_audio_params).
/// @param length        Number of samples in @p samples.
/// @param sample_rate   Sample rate; validated against [kMinSampleRate,
///                      kMaxSampleRate].
/// @param out           Receives the freshly allocated output array.
/// @param out_length    Receives the output array length.
/// @param process       Callable taking a const Audio& and returning an Audio.
template <typename Fn>
SonareError run_mono_offline(const float* samples, size_t length, int sample_rate, float** out,
                             size_t* out_length, Fn process) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  try {
    Audio audio = Audio::from_buffer(samples, length, sample_rate);
    Audio result = process(audio);
    *out_length = result.size();
    *out = new float[result.size()];
    std::memcpy(*out, result.data(), result.size() * sizeof(float));
    return SONARE_OK;
  } catch (const sonare::SonareException& e) {
    set_last_error(e.what());
    return map_sonare_exception(e);
  } catch (const std::bad_alloc& e) {
    set_last_error(e.what());
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (const std::invalid_argument& e) {
    set_last_error(e.what());
    return SONARE_ERROR_INVALID_PARAMETER;
  } catch (const std::logic_error& e) {
    set_last_error(e.what());
    return SONARE_ERROR_INVALID_STATE;
  } catch (const std::exception& e) {
    set_last_error(e.what());
    return SONARE_ERROR_UNKNOWN;
  } catch (...) {
    set_last_error("Unknown C++ exception (non-std::exception type)");
    return SONARE_ERROR_UNKNOWN;
  }
}

/// @brief Runs an offline analysis body against a validated mono C buffer.
/// @details Folds the validate_audio_params -> Audio::from_buffer -> try/catch
///          boilerplate shared by every offline analysis/feature wrapper. The
///          body receives the constructed Audio and returns the SonareError to
///          propagate (normally SONARE_OK). Per-function out-pointer null checks,
///          extra parameter validation, and out-struct zero-initialization must
///          still happen at the call site BEFORE invoking this (they early-return
///          without constructing an Audio). @p body must return SonareError.
template <typename Fn>
SonareError run_offline(const float* samples, size_t length, int sample_rate, Fn body) {
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  try {
    Audio audio = Audio::from_buffer(samples, length, sample_rate);
    return body(audio);
  } catch (const sonare::SonareException& e) {
    set_last_error(e.what());
    return map_sonare_exception(e);
  } catch (const std::bad_alloc& e) {
    set_last_error(e.what());
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (const std::invalid_argument& e) {
    set_last_error(e.what());
    return SONARE_ERROR_INVALID_PARAMETER;
  } catch (const std::logic_error& e) {
    set_last_error(e.what());
    return SONARE_ERROR_INVALID_STATE;
  } catch (const std::exception& e) {
    set_last_error(e.what());
    return SONARE_ERROR_UNKNOWN;
  } catch (...) {
    set_last_error("Unknown C++ exception (non-std::exception type)");
    return SONARE_ERROR_UNKNOWN;
  }
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
  catch (const std::invalid_argument& e) {                                              \
    sonare_c_detail::set_last_error(e.what());                                          \
    return SONARE_ERROR_INVALID_PARAMETER;                                              \
  }                                                                                     \
  catch (const std::logic_error& e) {                                                   \
    sonare_c_detail::set_last_error(e.what());                                          \
    return SONARE_ERROR_INVALID_STATE;                                                  \
  }                                                                                     \
  catch (const std::exception& e) {                                                     \
    sonare_c_detail::set_last_error(e.what());                                          \
    return SONARE_ERROR_UNKNOWN;                                                        \
  }                                                                                     \
  catch (...) {                                                                         \
    sonare_c_detail::set_last_error("Unknown C++ exception (non-std::exception type)"); \
    return SONARE_ERROR_UNKNOWN;                                                        \
  }
