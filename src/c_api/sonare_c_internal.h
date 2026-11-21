#pragma once

#include <array>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "automation/automation_lane.h"
#include "automation/parameter.h"
#include "c_api/c_ordinal.h"
#include "core/audio.h"
#include "core/diagnostic.h"
#include "engine/realtime_engine.h"
#if defined(SONARE_WITH_ARRANGEMENT)
#include "host/midi_io.h"
#include "midi/instrument.h"
#include "midi/synth/sf2_file.h"
#include "streaming/realtime_limits.h"
#endif
#include <sonare/sonare_c.h>

#include "util/exception.h"
#include "util/parameter_checks.h"
#include "util/types.h"

struct SonareAudio {
  sonare::Audio audio;
};

// Realtime engine handle. Defined here (rather than in sonare_c_daw.cpp) so that
// other C API translation units can access engine internals without ODR or
// linker fragility. The public header keeps the opaque forward-declaration.
struct SonareEngine {
  sonare::engine::RealtimeEngine engine;
  sonare::automation::ParameterRegistry parameters;
  std::deque<std::string> parameter_strings;
  std::deque<std::string> marker_strings;
  std::vector<sonare::automation::AutomationLane> automation_lanes;
#if defined(SONARE_WITH_ARRANGEMENT)
  /// Engine-owned instrument per destination (built-in synth or SF2 player).
  std::vector<std::pair<uint32_t, std::unique_ptr<sonare::midi::MidiInstrument>>>
      builtin_instruments;
  /// Loaded SoundFont (sonare_engine_load_soundfont); shared read-only with the
  /// SF2 players bound through sonare_engine_set_sf2_instrument.
  std::shared_ptr<const sonare::midi::synth::Sf2File> soundfont;
  sonare::host::FixedMidiInputSource<512> midi_input_source;
  bool midi_input_source_enabled = false;
  /// Last timeline applied by sonare_engine_apply_project_timeline; the engine's
  /// markers point into its name storage, so it is held until the next successful apply.
  std::shared_ptr<const void> applied_timeline;
#endif
};

namespace sonare_c_detail {

using sonare::Audio;
using sonare::ChordQuality;
using sonare::SonareException;

// Aliases of the canonical core limits (sonare::k*AudioSampleRate /
// kMaxAudioBufferSize in core/audio.h). Kept under these C-ABI names so the
// many existing call sites stay unchanged while the bounds have a single
// source of truth shared with the WASM bindings.
constexpr int kMinSampleRate = sonare::kMinAudioSampleRate;
constexpr int kMaxSampleRate = sonare::kMaxAudioSampleRate;
constexpr size_t kMaxBufferSize = sonare::kMaxAudioBufferSize;

// Single source of truth for the message recorded when a caught exception is not
// a std::exception subtype. Shared by SONARE_C_CATCH, SONARE_C_CATCH_RETURN, and
// the run_*_offline helpers so the wording never drifts between them.
constexpr const char* kUnknownExceptionMessage = "Unknown C++ exception (non-std::exception type)";

std::string& last_error_storage() noexcept;
/// @brief Thread-local code recorded beside the detailed-error message.
SonareError& last_error_code_storage() noexcept;
/// @brief Returns the thread-local detailed-error message without throwing.
/// @details A static fallback is returned when storing a new diagnostic failed,
///          so a caught exception cannot escape while its error code is being
///          recorded.
const char* last_error_message() noexcept;
/// @brief Records @p code and @p msg as the calling thread's last error.
/// @details @p code is the SonareError the failing call returns, or would have
///          returned had it not reported failure through a NULL handle.
void set_last_error(SonareError code, const char* msg) noexcept;
/// @brief Refuses a @c struct_size below the struct this library was built with,
///        naming the struct in the recorded message.
inline bool struct_size_accepted(uint32_t struct_size, size_t expected, const char* name) {
  if (struct_size >= expected) return true;
  set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                 (std::string(name) + ".struct_size is below sizeof").c_str());
  return false;
}
/// @brief Refuses a caller-filled struct whose @c struct_size is below the layout this
///        library was built with, which is what a struct not started from its _init carries.
/// @throws sonare::SonareParameterRefusal naming "<subject>.struct_size" and @p init_name.
inline void require_struct_size(uint32_t struct_size, size_t expected, const std::string& subject,
                                const char* init_name) {
  if (struct_size >= expected) return;
  throw sonare::SonareParameterRefusal::predicate(sonare::ErrorCode::InvalidParameter,
                                                  subject + ".struct_size",
                                                  std::string("must be set by ") + init_name);
}
/// @brief Refuses a C hop that is neither positive nor @ref SONARE_STFT_HOP_AUTO, by @p subject.
/// @throws sonare::SonareParameterRefusal naming @p subject.
inline void check_stft_hop(int hop_length, const char* subject) {
  if (hop_length != SONARE_STFT_HOP_AUTO) {
    sonare::util::check::integer_at_least(hop_length, 1, subject);
  }
}
/// @brief The core spelling of a checked C hop: @ref SONARE_STFT_HOP_AUTO becomes 0, the hop
///        the core derives as a quarter of the FFT size.
inline int core_stft_hop(int hop_length) noexcept {
  return hop_length == SONARE_STFT_HOP_AUTO ? 0 : hop_length;
}
/// @brief A core refusal subject and the C parameter that spells it ("sr" -> "sample_rate").
using CName = std::pair<const char*, const char*>;
/// @brief Runs @p call, rethrowing a refusal whose subject root @p names lists under that C
///        parameter's name, so a core keeping its librosa-style names refuses in the entry's.
template <std::size_t N, typename Call>
auto with_c_names(const std::array<CName, N>& names, Call&& call) -> decltype(call()) {
  try {
    return call();
  } catch (const sonare::SonareParameterRefusal& e) {
    throw e.respelled(
        e.code(),
        [&names](const std::string& subject) {
          const size_t end = subject.find_first_of(".[");
          const std::string root = subject.substr(0, end);
          for (const CName& row : names) {
            if (root == row.first) {
              return std::string(row.second) +
                     (end == std::string::npos ? std::string() : subject.substr(end));
            }
          }
          return subject;
        },
        e.caller_spelled());
  }
}
/// @brief Records @p code with @p msg, a string with static storage duration.
/// @details Never allocates: the message is read through the fallback pointer
///          instead of being copied, so a refusal made before any buffer exists
///          can still carry its reason.
void set_last_error_static(SonareError code, const char* msg) noexcept;
/// @brief Clears the thread-local detailed-error message and its code.
/// @details Called at the entry of every public C-ABI call (via SONARE_C_TRY and
///          the run_offline / run_mono_offline helpers) so a message recorded by
///          an earlier call can never leak into an unrelated later call. See the
///          @ref sonare_last_error_message contract in sonare_c.h.
void clear_last_error() noexcept;
/// @brief Clears the last error and, unless an entry is already running on this thread, marks
///        @p entry (the exported function's __func__) as that entry, so a parameter refusal it
///        records names the call the caller made rather than a helper or a nested entry.
/// @return Whether @p entry became the running entry; its scope then calls leave_c_api.
bool enter_c_api(const char* entry) noexcept;
/// @brief Ends the running entry that enter_c_api marked.
void leave_c_api() noexcept;

/// @brief The scope of one SONARE_C_API_ENTRY: the outermost one names the entry until it returns.
class CApiEntryScope {
 public:
  explicit CApiEntryScope(const char* entry) noexcept : owns_(enter_c_api(entry)) {}
  ~CApiEntryScope() {
    if (owns_) leave_c_api();
  }
  CApiEntryScope(const CApiEntryScope&) = delete;
  CApiEntryScope& operator=(const CApiEntryScope&) = delete;

 private:
  bool owns_;
};
/// @brief Clears the thread-local structured diagnostic list. Called at the entry
/// of the C-ABI calls that may publish one, so a stale entry never leaks forward.
void clear_last_diagnostics() noexcept;
/// @brief Thread-local warning channel: the diagnostics a call published, each
/// keeping its code, message and severity. Surfaced by
/// @ref sonare_last_diagnostic_count and @ref sonare_last_diagnostic.
std::vector<sonare::Diagnostic>& last_diagnostics_storage() noexcept;
/// @brief Publishes @p diagnostics, replacing whatever the previous call left.
void set_last_diagnostics(const std::vector<sonare::Diagnostic>& diagnostics);
/// @brief Appends one Warning diagnostic to the list the current call publishes.
void add_last_warning(const char* code, std::string message);
SonareError map_sonare_exception(const SonareException& e);
/// @brief Whether the vocal-edit surface was compiled into this build.
bool vocal_edit_compiled_in() noexcept;
/// @brief Records the exception currently being handled by an active catch.
/// @pre Must be called from a `catch (...)` handler while the exception is
///      active; it rethrows that exception to classify it.
/// @details The diagnostic writer is no-throw, so this function never lets a
///          caught C++ exception escape a C-ABI entry point.
SonareError record_current_exception() noexcept;
/// @brief Records "<param> must not be NULL" as the last error and returns its code.
SonareError refuse_null(const char* param) noexcept;
/// @brief Records "<param> must not be NULL when <count> is not 0" and returns its code.
SonareError refuse_null_when_count(const char* param, const char* count) noexcept;
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
///
///          @p name is the entry's parameter; the refusal is recorded as
///          "<name> <predicate>" with @p name as its subject, and the entry beside it.
///          @p fn is no longer read: sonare_last_refusal_entry names the entry.
SonareError validate_audio_params(const char* fn, const char* name, const float* samples,
                                  size_t length, int sample_rate) noexcept;

/// @brief validate_audio_params for an entry point that reads one window of the buffer.
/// @details Same policy, except that the non-finite scan covers only
///          [@p scan_offset, @p scan_offset + @p scan_count) -- the span the call
///          actually reads -- so its cost is the window's, not the buffer's. For a
///          per-frame entry point polled over a long buffer that is the difference
///          between O(window) and O(length) per call.
SonareError validate_audio_params_window(const char* fn, const char* name, const float* samples,
                                         size_t length, int sample_rate, size_t scan_offset,
                                         size_t scan_count) noexcept;
SonareGrooveType to_c_groove_type(const std::string& groove);
SonareChordQuality to_c_chord_quality(ChordQuality quality);

/// @brief Whether an ordinal names a window type this ABI maps.
/// @details Canonical owner of the SonareWindowType range check. Every entry
///          point taking a window ordinal calls this and returns
///          SONARE_ERROR_INVALID_PARAMETER when it fails, rather than letting an
///          unmapped ordinal fall onto a default: an out-of-range window that
///          silently becomes Hann is indistinguishable from a caller who asked
///          for Hann.
inline bool valid_window(int value) {
  return valid_c_enum(value, SONARE_WINDOW_HANN, SONARE_WINDOW_RECTANGULAR);
}

/// @brief Maps an ordinal that passed @ref valid_window onto the core enum.
/// @details The switch is deliberately exhaustive with no default arm, so adding
///          a SonareWindowType enumerator fails -Wswitch here instead of mapping
///          the new window to Hann. The trailing return is unreachable for any
///          ordinal @ref valid_window accepts and exists only to satisfy the
///          compiler on a value cast in from outside the enum's range.
inline sonare::WindowType to_window_type(int value) {
  switch (admitted_ordinal_to_enum<SonareWindowType>(value)) {
    case SONARE_WINDOW_HANN:
      return sonare::WindowType::Hann;
    case SONARE_WINDOW_HAMMING:
      return sonare::WindowType::Hamming;
    case SONARE_WINDOW_BLACKMAN:
      return sonare::WindowType::Blackman;
    case SONARE_WINDOW_RECTANGULAR:
      return sonare::WindowType::Rectangular;
  }
  return sonare::WindowType::Hann;
}

/// @brief Null-checks and zeroes a (pointer, length) output pair before any
///        validation return; false means refuse with SONARE_ERROR_INVALID_PARAMETER.
template <typename T>
inline bool begin_vector_output(T** out, size_t* out_length) {
  if (!out || !out_length) return false;
  *out = nullptr;
  *out_length = 0;
  return true;
}

/// @brief Clears a caller-owned string out-parameter before any other exit, or
///        records INVALID_PARAMETER when it is NULL.
inline bool begin_string_output(char** out) {
  if (out == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "the output pointer must not be NULL");
    return false;
  }
  *out = nullptr;
  return true;
}

template <typename T>
T* release_array(std::unique_ptr<T[]>& ptr) {
  return ptr.release();
}

/// @brief Holds a C result struct while it is filled, releasing it through
///        @p Release on any exit that does not publish it.
/// @details A throw between two array copies would otherwise leave the caller's
///          struct holding the first arrays beside an error code it treats as
///          "nothing to free". The caller's struct is written once, by publish().
template <typename T, void (*Release)(T*)>
class StagedOutput {
 public:
  StagedOutput() noexcept = default;
  StagedOutput(const StagedOutput&) = delete;
  StagedOutput& operator=(const StagedOutput&) = delete;
  ~StagedOutput() {
    if (!published_) Release(&value_);
  }

  T* operator->() noexcept { return &value_; }
  T& value() noexcept { return value_; }

  /// @brief Hands every array in the staged struct to @p out.
  void publish(T* out) noexcept {
    *out = value_;
    published_ = true;
  }

 private:
  T value_{};
  bool published_ = false;
};

/// @brief Copies a vector into a freshly heap-allocated array owned by the
///        caller (freed via the matching sonare_free_* entry point).
/// @details Canonical owner of the "copy a trivially-copyable vector into a C
///          output array" pattern shared across the C API translation units.
///          EMPTY-RESULT POLICY: an empty vector yields @c nullptr, matching the
///          analysis emitters' (null, 0) convention. @c T must be trivially
///          copyable (float / int / int16_t / uint8_t and friends).
template <typename T>
T* copy_vector(const std::vector<T>& values) {
  if (values.empty()) return nullptr;
  std::unique_ptr<T[]> buf(new T[values.size()]);
  std::memcpy(buf.get(), values.data(), values.size() * sizeof(T));
  return buf.release();
}

/// @brief SonareError-returning vector copy: marshals @p values out through the
///        @p out / @p out_length pair.
/// @details Same allocation/empty policy as the single-argument @ref copy_vector
///          overload (empty vector -> @c *out == nullptr, @c *out_length == 0).
///          Rejects null @p out / @p out_length with
///          @c SONARE_ERROR_INVALID_PARAMETER.
template <typename T>
SonareError copy_vector(const std::vector<T>& values, T** out, size_t* out_length) {
  if (!out) return refuse_null("out");
  if (!out_length) return refuse_null("out_length");
  *out = nullptr;
  *out_length = 0;
  std::unique_ptr<T[]> staged(copy_vector(values));
  *out = staged.release();
  *out_length = values.size();
  return SONARE_OK;
}

/// @brief Copies @p values into a fresh array of exactly @p count elements,
///        padding any shortfall with quiet NaN.
/// @details Distinct from @ref copy_vector: a result shorter than @p count is
///          made visibly invalid (NaN-filled) rather than silently truncated, so
///          per-band emitters (acoustic RT60/EDT/clarity bands) can hand back a
///          fixed-width array without dropping the band-count contract. Returns
///          @c nullptr when @p count == 0.
inline float* copy_float_vector_padded(const std::vector<float>& values, size_t count) {
  if (count == 0) return nullptr;
  std::unique_ptr<float[]> out(new float[count]);
  for (size_t i = 0; i < count; ++i) {
    out[i] = i < values.size() ? values[i] : std::numeric_limits<float>::quiet_NaN();
  }
  return out.release();
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

/// @brief Copies @p src into the fixed-capacity @p dest buffer, always
///        NUL-terminating (truncating if @p src does not fit). A null @p src is
///        treated as the empty string; a null @p dest or zero @p capacity is a
///        no-op. Canonical owner of the fixed-field C-string copy shared by the
///        C API translation units.
inline void copy_text(char* dest, size_t capacity, const char* src) {
  if (!dest || capacity == 0) return;
  const char* text = src ? src : "";
  std::strncpy(dest, text, capacity - 1);
  dest[capacity - 1] = '\0';
}

/// @brief Copies an Audio result into a freshly heap-allocated float array
///        owned by the caller (freed via sonare_free_floats). Shared by the
///        offline Audio -> Audio wrappers (pitch editor / voice changer TUs).
/// @details EMPTY-RESULT POLICY: a zero-length result yields @c *out == nullptr
///          with @c *out_length == 0, matching the analysis emitters'
///          (null, 0) convention. sonare_free_floats(nullptr) is a safe no-op.
inline SonareError copy_audio_result(const Audio& result, float** out, size_t* out_length) {
  if (!out) return refuse_null("out");
  if (!out_length) return refuse_null("out_length");
  *out = nullptr;
  *out_length = 0;
  std::unique_ptr<float[]> staged;
  if (!result.empty()) {
    staged.reset(new float[result.size()]);
    std::memcpy(staged.get(), result.data(), result.size() * sizeof(float));
  }
  *out = staged.release();
  *out_length = result.size();
  return SONARE_OK;
}

/// @brief Joins @p values with '\n' into @p storage and returns a borrowed
///        pointer to the stored string's buffer (valid while @p storage lives).
inline const char* join_names(const std::vector<std::string>& values, std::string& storage) {
  storage.clear();
  for (size_t index = 0; index < values.size(); ++index) {
    if (index > 0) storage += '\n';
    storage += values[index];
  }
  return storage.c_str();
}

/// @brief Joins names into an owned string for immutable per-thread caches.
inline std::string join_names(const std::vector<std::string>& values) {
  std::string storage;
  join_names(values, storage);
  return storage;
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

// Common entry guard for every public SonareError-returning C ABI function,
// except the documented audio-thread process entries below.
// Keep this before all pointer/range validation so a non-throwing early return
// cannot expose detail from an unrelated previous call. The read-only error and
// warning accessors are deliberately excluded: reading a diagnostic must not
// destroy it. SONARE_C_TRY also clears defensively for legacy/helper entry paths.
// tests/conformance/check_c_api_entry_guard.py holds every exported entry to this
// family: cleared on entry, and everything that can throw inside SONARE_C_TRY.
#define SONARE_C_API_ENTRY const sonare_c_detail::CApiEntryScope sonare_c_api_entry_scope(__func__)

namespace sonare_c_detail {

// True for a struct the caller versions through struct_size or struct_version.
template <typename T, typename = void>
struct is_caller_versioned : std::false_type {};
template <typename T>
struct is_caller_versioned<T, std::void_t<decltype(std::declval<T&>().struct_size)>>
    : std::true_type {};
template <typename T, typename = void>
struct is_caller_versioned_by_version : std::false_type {};
template <typename T>
struct is_caller_versioned_by_version<T, std::void_t<decltype(std::declval<T&>().struct_version)>>
    : std::true_type {};

/// @brief Value-initializes every non-NULL out parameter, so a refused call never leaves one
///        unwritten. A caller-versioned struct is not an out to clear and is refused at compile
///        time.
template <typename... Outs>
inline void zero_outs(Outs*... outs) noexcept {
  static_assert(
      ((!is_caller_versioned<Outs>::value && !is_caller_versioned_by_version<Outs>::value) && ...),
      "a struct carrying struct_size or struct_version is filled by the caller");
  ((outs != nullptr ? void(*outs = Outs{}) : void()), ...);
}

}  // namespace sonare_c_detail

// Clear every out parameter first, before any refusal, in one statement per entry.
#define SONARE_C_ZERO_OUTS(...) ::sonare_c_detail::zero_outs(__VA_ARGS__)

// Refuse a NULL argument by its parameter name, spelled as the public header spells it.
// The _WHEN_COUNT form refuses it only while the count that sizes it is not 0.
#define SONARE_C_REQUIRE_NONNULL(param)                                    \
  do {                                                                     \
    if ((param) == nullptr) return ::sonare_c_detail::refuse_null(#param); \
  } while (0)
#define SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(param, count)               \
  do {                                                                  \
    if ((param) == nullptr && (count) != 0) {                           \
      return ::sonare_c_detail::refuse_null_when_count(#param, #count); \
    }                                                                   \
  } while (0)

// The two realtime engine process functions deliberately do not touch the
// thread-local diagnostic strings. First use of a thread-local object can
// allocate TLS storage on some platforms, which would violate their audio-thread
// no-allocation contract. They do not throw or produce detailed diagnostics;
// callers must validate/setup on the control thread instead.
#define SONARE_C_RT_API_ENTRY ((void)0)

// Clears any stale detailed-error message before running the guarded body so a
// message recorded by an earlier call can never leak. The catch arms below set a
// fresh message on every caught-exception path. Validation early-returns that
// occur BEFORE SONARE_C_TRY record no message; sonare_last_error_message is
// therefore the empty string after such a return (see its header contract).
#define SONARE_C_TRY                   \
  sonare_c_detail::clear_last_error(); \
  try {
#define SONARE_C_CATCH                                  \
  }                                                     \
  catch (...) {                                         \
    return sonare_c_detail::record_current_exception(); \
  }

// Catch companion for pointer-/value-returning C-ABI entry points (e.g. the
// *_create / *_add_* handles) that cannot return a SonareError. Mirrors the arms
// of SONARE_C_CATCH, recording the same code and message, but returns @p retval
// on every caught exception. Like SONARE_C_CATCH the leading brace closes the
// preceding try block.
#define SONARE_C_CATCH_RETURN(retval)                  \
  }                                                    \
  catch (...) {                                        \
    (void)sonare_c_detail::record_current_exception(); \
    return (retval);                                   \
  }

// Catch companion for void C-ABI entry points: records the exception and returns.
#define SONARE_C_CATCH_VOID                            \
  }                                                    \
  catch (...) {                                        \
    (void)sonare_c_detail::record_current_exception(); \
    return;                                            \
  }

namespace sonare_c_detail {

/// @brief Runs an Audio → Audio DSP function against a mono C buffer and
///        marshals the result back out through @p out / @p out_length.
/// @details Centralises the validate → from_buffer → process → copy chain that
///          every offline effects wrapper shares, inside the entry guard. The
///          callable receives the wrapped Audio and returns an Audio whose samples
///          are copied into a fresh array the caller frees with sonare_free_floats.
/// @param fn            Not read; sonare_last_refusal_entry names the entry.
/// @param name          The entry's name for @p samples.
/// @param samples       Caller-owned input buffer (validated for null / NaN /
///                      Inf / sample_rate by validate_audio_params).
/// @param length        Number of samples in @p samples.
/// @param sample_rate   Sample rate; validated against [kMinSampleRate,
///                      kMaxSampleRate].
/// @param out           Receives the freshly allocated output array.
/// @param out_length    Receives the output array length.
/// @param process       Callable taking a const Audio& and returning an Audio.
template <typename Fn>
SonareError run_mono_offline(const char* fn, const char* name, const float* samples, size_t length,
                             int sample_rate, float** out, size_t* out_length, Fn process) {
  SONARE_C_TRY
  SONARE_C_ZERO_OUTS(out, out_length);
  const SonareError err = validate_audio_params(fn, name, samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = process(audio);
  // Empty-result policy lives in copy_audio_result: size()==0 -> (*out=nullptr,
  // *out_length=0), matching the analysis emitters' (null, 0) convention.
  return copy_audio_result(result, out, out_length);
  SONARE_C_CATCH
}

/// @brief Runs an offline analysis body against an already-validated buffer.
/// @details Shares run_offline's Audio::from_buffer -> body chain but skips
///          validate_audio_params, for a caller that has already validated the
///          span it passes here -- re-validating would repeat that scan.
template <typename Fn>
SonareError run_prevalidated_offline(const float* samples, size_t length, int sample_rate,
                                     Fn body) {
  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  return body(audio);
  SONARE_C_CATCH
}

/// @brief Runs an offline analysis body against a validated mono C buffer.
/// @details Folds the validate_audio_params -> Audio::from_buffer chain shared by
///          every offline analysis/feature wrapper into the entry guard. The body
///          receives the constructed Audio and returns the SonareError to
///          propagate (normally SONARE_OK). Per-function out-pointer null checks,
///          extra parameter validation, and out-struct zero-initialization must
///          still happen at the call site BEFORE invoking this (they early-return
///          without constructing an Audio). @p body must return SonareError.
template <typename Fn>
SonareError run_offline(const char* fn, const char* name, const float* samples, size_t length,
                        int sample_rate, Fn body) {
  SONARE_C_TRY
  const SonareError err = validate_audio_params(fn, name, samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  return body(audio);
  SONARE_C_CATCH
}

/// @brief @p e with each subject joined under @p root, coded InvalidParameter.
sonare::SonareParameterRefusal relabel_struct_refusal(const sonare::SonareParameterRefusal& e,
                                                      const char* root);

/// @brief Runs a core argument validator ahead of run_offline and returns its verdict as a status.
/// @details Lets an entry refuse its parameters through the core's own rule (recording the
///          core's message as the last error) while keeping that refusal ahead of the audio
///          checks. @p check throws on a refusal and returns nothing otherwise.
template <typename Fn>
SonareError run_validation(Fn check) {
  SONARE_C_TRY
  check();
  return SONARE_OK;
  SONARE_C_CATCH
}

/// @brief run_validation for a validator that names the members of the struct passed as @p root:
///        a parameter refusal it throws is recorded with each subject under @p root
///        ("threshold" becomes "config.threshold"), as InvalidParameter.
template <typename Fn>
SonareError run_validation_under(const char* root, Fn check) {
  SONARE_C_TRY
  try {
    check();
  } catch (const sonare::SonareParameterRefusal& e) {
    throw relabel_struct_refusal(e, root);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

}  // namespace sonare_c_detail
