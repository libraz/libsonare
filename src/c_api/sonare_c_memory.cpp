#include <sonare/sonare_c.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <thread>

#include "c_api/sonare_c_last_refusal.h"
#include "capability_features.h"
#include "rt/command.h"
#include "sonare.h"
#include "sonare_c_internal.h"
#include "util/system_info.h"

using namespace sonare_c_detail;

namespace {

const char* capability_platform() {
#if defined(__EMSCRIPTEN__)
  return "wasm32";
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  return "darwin-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
  return "darwin-x86_64";
#elif defined(__linux__) && defined(__aarch64__)
  return "linux-arm64";
#elif defined(__linux__) && defined(__x86_64__)
  return "linux-x86_64";
#else
  return "unknown";
#endif
}

const char* capability_simd() {
#if defined(__wasm_simd128__)
  return "wasm-simd128";
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  return "neon";
#elif defined(__SSE2__)
  return "sse2";
#else
  return "none";
#endif
}

constexpr const char* json_bool(bool value) { return value ? "true" : "false"; }

constexpr bool capability_mastering_enabled() {
#if defined(SONARE_BUILD_MASTERING) && SONARE_BUILD_MASTERING
  return true;
#else
  return false;
#endif
}

constexpr bool capability_mixing_enabled() {
#if defined(SONARE_BUILD_MIXING) && SONARE_BUILD_MIXING
  return true;
#else
  return false;
#endif
}

constexpr bool capability_fx_enabled() {
#if defined(SONARE_BUILD_FX) && SONARE_BUILD_FX
  return true;
#else
  return false;
#endif
}

// True when the offline mixing assistant is compiled in. Reported separately
// from `mixing` because BUILD_MIXING_ASSISTANT can be dropped on its own, and
// its entry points stay exported either way: they answer NOT_SUPPORTED rather
// than disappearing, so a host cannot detect a stripped build by probing for a
// symbol.
//
// The SONARE_BUILD_ spelling is load-bearing here, as it is for the neighbours
// above. SONARE_WITH_MIXING_ASSISTANT is scoped to the targets that link the
// subsystem, and this translation unit is compiled into sonare_core_objects,
// which is not one of them -- reading it here compiles cleanly and always
// answers false in a statically linked host.
constexpr bool capability_mixing_assistant_enabled() {
#if defined(SONARE_BUILD_MIXING_ASSISTANT) && SONARE_BUILD_MIXING_ASSISTANT
  return true;
#else
  return false;
#endif
}

// True when hosted instruments expose continuously automatable parameters
// (@ref sonare_engine_resolve_instrument_automation_id). Tied to the
// arrangement/MIDI subsystem, which owns the instrument rack.
constexpr bool capability_instrument_param_automation_enabled() {
#if defined(SONARE_BUILD_ARRANGEMENT) && SONARE_BUILD_ARRANGEMENT
  return true;
#else
  return false;
#endif
}

// The four below exist so this descriptor covers every build option that
// changes which commands a binary answers. It reported six features while the
// tree has nine gates, and the three it could not name are exactly the ones a
// caller cannot otherwise detect: the cross-surface CLI contract has to ask a
// binary what it was built with, and a gate missing from here reads as a
// command that vanished for no reason.
constexpr bool capability_arrangement_enabled() {
#if defined(SONARE_BUILD_ARRANGEMENT) && SONARE_BUILD_ARRANGEMENT
  return true;
#else
  return false;
#endif
}

constexpr bool capability_acoustic_sim_enabled() {
#if defined(SONARE_BUILD_ACOUSTIC_SIM) && SONARE_BUILD_ACOUSTIC_SIM
  return true;
#else
  return false;
#endif
}

constexpr bool capability_playback_enabled() {
#if defined(SONARE_BUILD_PLAYBACK) && SONARE_BUILD_PLAYBACK
  return true;
#else
  return false;
#endif
}

constexpr bool capability_pitch_editor_enabled() {
#if defined(SONARE_BUILD_PITCH_EDITOR) && SONARE_BUILD_PITCH_EDITOR
  return true;
#else
  return false;
#endif
}

constexpr bool capability_voice_changer_enabled() {
#if defined(SONARE_BUILD_VOICE_CHANGER) && SONARE_BUILD_VOICE_CHANGER
  return true;
#else
  return false;
#endif
}

constexpr bool capability_ffmpeg_enabled() {
#ifdef SONARE_WITH_FFMPEG
  return true;
#else
  return false;
#endif
}

constexpr bool capability_graph_enabled() {
#if defined(SONARE_BUILD_GRAPH) && SONARE_BUILD_GRAPH
  return true;
#else
  return false;
#endif
}

constexpr bool capability_assist_enabled() {
#if defined(SONARE_BUILD_ASSIST) && SONARE_BUILD_ASSIST
  return true;
#else
  return false;
#endif
}

constexpr bool capability_project_enabled() { return capability_arrangement_enabled(); }

bool capability_vocal_edit_enabled() { return vocal_edit_compiled_in(); }

struct CapabilityFeature {
  const char* name;
  bool (*enabled)();
};

// The one table behind both the `features` object of sonare_capabilities_json()
// and sonare_has_feature(), so the two can never disagree on a name or a value.
#define SONARE_CAPABILITY_ROW(key, suffix, gate) {key, capability_##suffix##_enabled},
constexpr CapabilityFeature kCapabilityFeatures[] = {
    SONARE_CAPABILITY_FEATURES(SONARE_CAPABILITY_ROW)};
#undef SONARE_CAPABILITY_ROW

}  // namespace

void sonare_free_floats(float* ptr) { delete[] ptr; }

void sonare_free_ints(int* ptr) { delete[] ptr; }

void sonare_free_string(char* ptr) { delete[] ptr; }

void sonare_free_key_candidates(SonareKeyCandidate* ptr) { delete[] ptr; }

void sonare_free_analysis_result(SonareAnalysisResult* result) {
  if (result != nullptr) {
    delete[] result->beat_times;
    delete[] result->bpm_candidates;
    delete[] result->time_signature_candidates;
    *result = {};
  }
}

const char* sonare_error_message(SonareError error) {
  switch (error) {
    case SONARE_OK:
      return "OK";
    case SONARE_ERROR_FILE_NOT_FOUND:
      return "File not found";
    case SONARE_ERROR_INVALID_FORMAT:
      return "Invalid format";
    case SONARE_ERROR_DECODE_FAILED:
      return "Decode failed";
    case SONARE_ERROR_INVALID_PARAMETER:
      return "Invalid parameter";
    case SONARE_ERROR_OUT_OF_MEMORY:
      return "Out of memory";
    case SONARE_ERROR_NOT_SUPPORTED:
      return "Not supported in this build";
    case SONARE_ERROR_INVALID_STATE:
      return "Invalid state";
    case SONARE_ERROR_CANCELLED:
      return "Cancelled";
    case SONARE_ERROR_ENCODE_FAILED:
      return "Encode failed";
    case SONARE_ERROR_ABI_MISMATCH:
      return "Native library ABI mismatch";
    case SONARE_ERROR_STALE_RESULT:
      return "Stale result";
    case SONARE_ERROR_UNKNOWN:
      return "Unknown error";
  }
  return "Unknown error";
}

// Diagnostic accessors are the explicit exception to the public-entry clearing
// rule: callers must be able to inspect a message repeatedly until another API
// operation replaces or clears it.
const char* sonare_last_error_message(void) { return last_error_message(); }

SonareError sonare_last_error_code(void) { return last_error_code_storage(); }

size_t sonare_last_diagnostic_count(void) { return last_diagnostics_storage().size(); }

void sonare_diagnostic_init(SonareDiagnostic* diagnostic) {
  if (diagnostic == nullptr) return;
  *diagnostic = SonareDiagnostic{};
  diagnostic->struct_size = sizeof(SonareDiagnostic);
  diagnostic->code = "";
  diagnostic->message = "";
  diagnostic->severity = SONARE_DIAGNOSTIC_INFO;
}

SonareError sonare_last_diagnostic(size_t index, SonareDiagnostic* out) {
  const std::vector<sonare::Diagnostic>& diagnostics = last_diagnostics_storage();
  if (out == nullptr || out->struct_size < sizeof(SonareDiagnostic) ||
      index >= diagnostics.size()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const sonare::Diagnostic& diagnostic = diagnostics[index];
  out->code = diagnostic.code.c_str();
  out->message = diagnostic.message.c_str();
  switch (diagnostic.severity) {
    case sonare::Diagnostic::Severity::Info:
      out->severity = SONARE_DIAGNOSTIC_INFO;
      break;
    case sonare::Diagnostic::Severity::Warning:
      out->severity = SONARE_DIAGNOSTIC_WARNING;
      break;
    case sonare::Diagnostic::Severity::Error:
      out->severity = SONARE_DIAGNOSTIC_ERROR;
      break;
  }
  return SONARE_OK;
}

size_t sonare_last_refusal_subject_count(void) { return last_refusal_subject_count(); }

const char* sonare_last_refusal_subject(size_t index) { return last_refusal_subject(index); }

SonareError sonare_last_refusal_render(const char* const* subjects, size_t subject_count,
                                       char** out_text) {
  return render_last_refusal(subjects, subject_count, out_text);
}

const char* sonare_version(void) { return SONARE_VERSION_STRING; }

static_assert(SONARE_ENGINE_ABI_VERSION == sonare::rt::kEngineAbiVersion,
              "SONARE_ENGINE_ABI_VERSION must equal rt::kEngineAbiVersion");

uint32_t sonare_engine_abi_version(void) { return SONARE_ENGINE_ABI_VERSION; }

SonareError sonare_capabilities_json(char** out_json) {
  SONARE_C_API_ENTRY;
  if (!begin_string_output(out_json)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  std::string capabilities = "{\"version\":\"" SONARE_VERSION_STRING "\",\"abi\":{\"project\":" +
                             std::to_string(SONARE_PROJECT_ABI_VERSION) +
                             ",\"engine\":" + std::to_string(sonare::rt::kEngineAbiVersion) +
                             "},\"platform\":\"" + capability_platform() + "\",\"features\":{";
  bool first = true;
  for (const CapabilityFeature& feature : kCapabilityFeatures) {
    if (!first) capabilities += ',';
    first = false;
    capabilities += '"';
    capabilities += feature.name;
    capabilities += "\":";
    capabilities += json_bool(feature.enabled());
  }
  capabilities += "},\"decode\":{\"builtin\":[\"wav\",\"mp3\"],\"ffmpeg\":[";
#ifdef SONARE_WITH_FFMPEG
  capabilities += "\"m4a\",\"aac\",\"flac\",\"ogg\",\"opus\",\"wma\"";
#endif
  capabilities += "]},\"simd\":\"";
  capabilities += capability_simd();
  capabilities += "\",\"hardwareConcurrency\":";
  const unsigned int concurrency = std::thread::hardware_concurrency();
  capabilities += std::to_string(concurrency == 0 ? 1 : concurrency);
  capabilities += '}';
  *out_json = copy_string(capabilities);
  return SONARE_OK;
  SONARE_C_CATCH
}

namespace {

/// @brief Appends `"key":<gigabytes>` when @p bytes is known, as the native CLI wrote it.
void append_gigabytes(std::string& json, bool& first, const char* key,
                      const std::optional<std::size_t>& bytes) {
  if (!bytes) return;
  if (!first) json += ',';
  first = false;
  // printf keeps iostream and its locale facets out of the WASM module.
  char number[32];
  std::snprintf(number, sizeof number, "%.*g", std::numeric_limits<float>::max_digits10,
                static_cast<double>(static_cast<float>(*bytes) / (1024.0f * 1024.0f * 1024.0f)));
  json += '"';
  json += key;
  json += "\":";
  json += number;
}

/// @brief Appends `"key":<count>` when @p count is known.
void append_count(std::string& json, bool& first, const char* key,
                  const std::optional<int>& count) {
  if (!count) return;
  if (!first) json += ',';
  first = false;
  json += '"';
  json += key;
  json += "\":";
  json += std::to_string(*count);
}

}  // namespace

SonareError sonare_system_info_json(char** out_json) {
  SONARE_C_API_ENTRY;
  if (!begin_string_output(out_json)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  namespace info = sonare::system_info;
  std::string json = "{\"cpu\":{";
  bool first = true;
  append_count(json, first, "logical_cores", info::logical_cores());
  append_count(json, first, "physical_cores", info::physical_cores());
  json += "},\"memory\":{";
  first = true;
  append_gigabytes(json, first, "total_gb", info::total_memory_bytes());
  append_gigabytes(json, first, "available_gb", info::available_memory_bytes());
  json += "},\"parallel\":{\"enabled\":";
  json += json_bool(info::parallel_enabled());
  json += ",\"workers\":" + std::to_string(info::parallel_workers());
  json += ",\"strategy\":\"" + info::parallel_strategy() + "\"}}";
  *out_json = copy_string(json);
  return SONARE_OK;
  SONARE_C_CATCH
}

int sonare_has_feature(const char* name) {
  if (name == nullptr) return 0;
  for (const CapabilityFeature& feature : kCapabilityFeatures) {
    if (std::strcmp(feature.name, name) == 0) return feature.enabled() ? 1 : 0;
  }
  return 0;
}

uint32_t sonare_abi_version(void) { return SONARE_ABI_VERSION; }

void sonare_free_stft_result(SonareStftResult* r) {
  if (r) {
    delete[] r->magnitude;
    delete[] r->power;
    *r = {};
  }
}

void sonare_free_mel_result(SonareMelResult* r) {
  if (r) {
    delete[] r->power;
    delete[] r->db;
    *r = {};
  }
}

void sonare_free_mfcc_result(SonareMfccResult* r) {
  if (r) {
    delete[] r->coefficients;
    *r = {};
  }
}

void sonare_free_chroma_result(SonareChromaResult* r) {
  if (r) {
    delete[] r->features;
    delete[] r->mean_energy;
    *r = {};
  }
}

void sonare_free_pitch_result(SonarePitchResult* r) {
  if (r) {
    delete[] r->f0;
    delete[] r->voiced_prob;
    delete[] r->voiced_flag;
    *r = {};
  }
}

void sonare_free_hpss_result(SonareHpssResult* r) {
  if (r) {
    delete[] r->harmonic;
    delete[] r->percussive;
    *r = {};
  }
}

void sonare_free_bpm_analysis_result(SonareBpmAnalysisResult* r) {
  if (r) {
    delete[] r->candidates;
    delete[] r->autocorrelation;
    delete[] r->tempogram;
    *r = {};
  }
}

void sonare_free_acoustic_result(SonareAcousticResult* r) {
  if (r) {
    delete[] r->rt60_bands;
    delete[] r->edt_bands;
    delete[] r->c50_bands;
    delete[] r->c80_bands;
    *r = {};
  }
}

void sonare_free_rhythm_result(SonareRhythmResult* r) {
  if (r) {
    delete[] r->beat_intervals;
    *r = {};
  }
}

void sonare_free_dynamics_result(SonareDynamicsResult* r) {
  if (r) {
    delete[] r->loudness_times_sec;
    delete[] r->loudness_rms_db;
    *r = {};
  }
}

void sonare_free_timbre_result(SonareTimbreResult* r) {
  if (r) {
    delete[] r->spectral_centroid;
    delete[] r->spectral_flatness;
    delete[] r->spectral_rolloff;
    delete[] r->timbre_over_time;
    *r = {};
  }
}

void sonare_free_chord_analysis_result(SonareChordAnalysisResult* r) {
  if (r) {
    delete[] r->chords;
    *r = {};
  }
}

void sonare_free_string_array(SonareStringArray* result) {
  if (result) {
    if (result->items) {
      for (size_t i = 0; i < result->count; ++i) {
        delete[] result->items[i];
      }
      delete[] result->items;
    }
    result->items = nullptr;
    result->count = 0;
  }
}

void sonare_free_engine_bounce_result(SonareEngineBounceResult* result) {
  if (!result) return;
  delete[] result->interleaved;
  *result = {};
}

void sonare_free_section_result(SonareSectionResult* result) {
  if (result) {
    delete[] result->sections;
    result->sections = nullptr;
    result->section_count = 0;
  }
}

void sonare_free_boundary_result(SonareBoundaryResult* result) {
  if (result) {
    delete[] result->boundaries;
    delete[] result->novelty_curve;
    *result = {};
  }
}

void sonare_free_melody_result(SonareMelodyResult* result) {
  if (result) {
    delete[] result->points;
    *result = {};
  }
}

void sonare_free_cqt_result(SonareCqtResult* result) {
  if (result) {
    delete[] result->magnitude;
    delete[] result->frequencies;
    *result = {};
  }
}
