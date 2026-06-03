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
/// @brief Clears the thread-local detailed-error message.
/// @details Called at the entry of every public C-ABI call (via SONARE_C_TRY and
///          the run_offline / run_mono_offline helpers) so a message recorded by
///          an earlier call can never leak into an unrelated later call. See the
///          @ref sonare_last_error_message contract in sonare_c.h.
void clear_last_error();
SonareError map_sonare_exception(const SonareException& e);
/// @brief Validates an offline audio buffer shared by every run_*_offline entry.
/// @details EMPTY-AUDIO POLICY: a NULL @p samples or @c length == 0 is rejected
///          with @c SONARE_ERROR_INVALID_PARAMETER (empty audio is never treated
///          as a valid zero-length analysis). Also rejects out-of-range
///          @p sample_rate ([kMinSampleRate, kMaxSampleRate]), @p length above
///          @c kMaxBufferSize, and any non-finite sample. This is the single
///          source of truth for that policy; all C-ABI offline functions funnel
///          through it (directly or via run_offline / run_mono_offline), so the
///          empty-input contract is uniform across the whole C API.
///
///          PERFORMANCE NOTE: the non-finite check is an O(n) scan run on EVERY
///          offline call. For a pipeline that calls several offline functions on
///          the SAME long buffer this re-scans the buffer each time. This is an
///          accepted trade-off (the buffer is caller-owned and may be mutated
///          between calls, so the result cannot be safely cached here). Callers
///          that need to run many analyses over one immutable buffer should
///          instead decode once into a SonareAudio handle
///          (sonare_audio_from_buffer) and use the handle-based analysis entry
///          points (sonare_audio_* in sonare_c_types.h), which validate the
///          buffer at construction time and never re-scan per analysis.
SonareError validate_audio_params(const float* samples, size_t length, int sample_rate);
SonareGrooveType to_c_groove_type(const std::string& groove);
SonareChordQuality to_c_chord_quality(ChordQuality quality);

template <typename T>
T* release_array(std::unique_ptr<T[]>& ptr) {
  return ptr.release();
}

/// @brief Variadic no-op used by SONARE_C_STUB_NOT_SUPPORTED to swallow unused
///        parameters in out-of-feature build configurations without resorting
///        to per-parameter (void) casts.
template <typename... T>
inline void ignore_args(const T&...) noexcept {}

/// @brief Allocates a NUL-terminated heap copy of @p value (caller frees via
///        sonare_free_string). Canonical owner of the C-string copy helper
///        shared by all C API translation units.
inline char* copy_string(const std::string& value) {
  std::unique_ptr<char[]> out(new char[value.size() + 1]);
  std::memcpy(out.get(), value.c_str(), value.size() + 1);
  return out.release();
}

/// @brief Copies an Audio result into a freshly heap-allocated float array
///        owned by the caller (freed via sonare_free_floats). Shared by the
///        offline Audio -> Audio wrappers (pitch editor / voice changer TUs).
/// @details EMPTY-RESULT POLICY: a zero-length result yields @c *out == nullptr
///          with @c *out_length == 0, matching the analysis emitters'
///          (null, 0) convention. sonare_free_floats(nullptr) is a safe no-op.
inline SonareError copy_audio_result(const Audio& result, float** out, size_t* out_length) {
  *out_length = result.size();
  if (result.size() == 0) {
    *out = nullptr;
    return SONARE_OK;
  }
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
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
///          sonare_free_floats entry point).
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
  // Clear any stale detailed-error message from an earlier call so a validation
  // early-return (which records no message) cannot leak an unrelated one.
  clear_last_error();
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  try {
    Audio audio = Audio::from_buffer(samples, length, sample_rate);
    Audio result = process(audio);
    // Empty-result policy lives in copy_audio_result: size()==0 -> (*out=nullptr,
    // *out_length=0), matching the analysis emitters' (null, 0) convention.
    return copy_audio_result(result, out, out_length);
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
  // Clear any stale detailed-error message from an earlier call (see
  // run_mono_offline for the rationale / the sonare_last_error_message contract).
  clear_last_error();
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

// Stubs out a C API function body when an optional module is not compiled in:
// swallows the (otherwise unused) parameters via ignore_args and returns
// SONARE_ERROR_NOT_SUPPORTED. Shared by the optional-module translation units
// (voice changer / pitch editor) so they suppress unused-parameter warnings the
// same way instead of hand-rolling (void) casts.
#define SONARE_C_STUB_NOT_SUPPORTED(...)       \
  do {                                         \
    sonare_c_detail::ignore_args(__VA_ARGS__); \
    return SONARE_ERROR_NOT_SUPPORTED;         \
  } while (false)

// Clears any stale detailed-error message before running the guarded body so a
// message recorded by an earlier call can never leak. The catch arms below set a
// fresh message on every caught-exception path. Validation early-returns that
// occur BEFORE SONARE_C_TRY record no message; sonare_last_error_message is
// therefore the empty string after such a return (see its header contract).
#define SONARE_C_TRY                   \
  sonare_c_detail::clear_last_error(); \
  try {
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
