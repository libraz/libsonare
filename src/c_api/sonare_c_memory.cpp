#include <sonare/sonare_c.h>

#include <string>
#include <thread>

#include "rt/command.h"
#include "sonare.h"
#include "sonare_c_internal.h"

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

}  // namespace

void sonare_free_floats(float* ptr) { delete[] ptr; }

void sonare_free_ints(int* ptr) { delete[] ptr; }

void sonare_free_string(char* ptr) { delete[] ptr; }

void sonare_free_key_candidates(SonareKeyCandidate* ptr) { delete[] ptr; }

void sonare_free_result(SonareAnalysisResult* result) {
  if (result != nullptr) {
    delete[] result->beat_times;
    result->beat_times = nullptr;
    result->beat_count = 0;
    delete[] result->bpm_candidates;
    result->bpm_candidates = nullptr;
    result->bpm_candidate_count = 0;
    delete[] result->time_signature_candidates;
    result->time_signature_candidates = nullptr;
    result->time_signature_candidate_count = 0;
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
    case SONARE_ERROR_UNKNOWN:
      return "Unknown error";
  }
  return "Unknown error";
}

// Diagnostic accessors are the explicit exception to the public-entry clearing
// rule: callers must be able to inspect a message repeatedly until another API
// operation replaces or clears it.
const char* sonare_last_error_message(void) { return last_error_storage().c_str(); }

const char* sonare_last_warning_message(void) { return last_warning_storage().c_str(); }

const char* sonare_version(void) { return SONARE_VERSION_STRING; }

uint32_t sonare_engine_abi_version(void) { return sonare::rt::kEngineAbiVersion; }

const char* sonare_capabilities_json(void) {
  SONARE_C_TRY
  thread_local std::string capabilities;
  capabilities = "{\"version\":\"" SONARE_VERSION_STRING "\",\"abi\":{\"project\":" +
                 std::to_string(SONARE_PROJECT_ABI_VERSION) +
                 ",\"engine\":" + std::to_string(sonare::rt::kEngineAbiVersion) +
                 "},\"platform\":\"" + capability_platform() +
                 "\",\"features\":{\"mastering\":" + json_bool(capability_mastering_enabled()) +
                 ",\"mixing\":" + json_bool(capability_mixing_enabled()) +
                 ",\"fx\":" + json_bool(capability_fx_enabled()) +
                 ",\"ffmpeg\":" + json_bool(sonare_has_ffmpeg_support() != 0) +
                 ",\"instrumentParamAutomation\":" +
                 json_bool(capability_instrument_param_automation_enabled()) +
                 "},\"decode\":{\"builtin\":[\"wav\",\"mp3\"],\"ffmpeg\":[";
#ifdef SONARE_WITH_FFMPEG
  capabilities += "\"m4a\",\"aac\",\"flac\",\"ogg\",\"opus\",\"wma\"";
#endif
  capabilities += "]},\"simd\":\"";
  capabilities += capability_simd();
  capabilities += "\",\"hardwareConcurrency\":";
  const unsigned int concurrency = std::thread::hardware_concurrency();
  capabilities += std::to_string(concurrency == 0 ? 1 : concurrency);
  capabilities += '}';
  return capabilities.c_str();
  SONARE_C_CATCH_RETURN(nullptr)
}

uint32_t sonare_abi_version(void) { return SONARE_ABI_VERSION; }

int sonare_has_ffmpeg_support(void) {
#ifdef SONARE_WITH_FFMPEG
  return 1;
#else
  return 0;
#endif
}

void sonare_free_stft_result(SonareStftResult* r) {
  if (r) {
    delete[] r->magnitude;
    delete[] r->power;
    r->magnitude = nullptr;
    r->power = nullptr;
  }
}

void sonare_free_mel_result(SonareMelResult* r) {
  if (r) {
    delete[] r->power;
    delete[] r->db;
    r->power = nullptr;
    r->db = nullptr;
  }
}

void sonare_free_mfcc_result(SonareMfccResult* r) {
  if (r) {
    delete[] r->coefficients;
    r->coefficients = nullptr;
  }
}

void sonare_free_chroma_result(SonareChromaResult* r) {
  if (r) {
    delete[] r->features;
    delete[] r->mean_energy;
    r->features = nullptr;
    r->mean_energy = nullptr;
  }
}

void sonare_free_pitch_result(SonarePitchResult* r) {
  if (r) {
    delete[] r->f0;
    delete[] r->voiced_prob;
    delete[] r->voiced_flag;
    r->f0 = nullptr;
    r->voiced_prob = nullptr;
    r->voiced_flag = nullptr;
  }
}

void sonare_free_hpss_result(SonareHpssResult* r) {
  if (r) {
    delete[] r->harmonic;
    delete[] r->percussive;
    r->harmonic = nullptr;
    r->percussive = nullptr;
  }
}

void sonare_free_bpm_analysis_result(SonareBpmAnalysisResult* r) {
  if (r) {
    delete[] r->candidates;
    delete[] r->autocorrelation;
    delete[] r->tempogram;
    r->candidates = nullptr;
    r->candidate_count = 0;
    r->autocorrelation = nullptr;
    r->autocorrelation_count = 0;
    r->tempogram = nullptr;
    r->tempogram_count = 0;
  }
}

void sonare_free_acoustic_result(SonareAcousticResult* r) {
  if (r) {
    delete[] r->rt60_bands;
    delete[] r->edt_bands;
    delete[] r->c50_bands;
    delete[] r->c80_bands;
    r->rt60_bands = nullptr;
    r->edt_bands = nullptr;
    r->c50_bands = nullptr;
    r->c80_bands = nullptr;
    r->band_count = 0;
  }
}

void sonare_free_rhythm_result(SonareRhythmResult* r) {
  if (r) {
    delete[] r->beat_intervals;
    r->beat_intervals = nullptr;
    r->beat_interval_count = 0;
  }
}

void sonare_free_dynamics_result(SonareDynamicsResult* r) {
  if (r) {
    delete[] r->loudness_times;
    delete[] r->loudness_rms_db;
    r->loudness_times = nullptr;
    r->loudness_rms_db = nullptr;
    r->loudness_count = 0;
  }
}

void sonare_free_timbre_result(SonareTimbreResult* r) {
  if (r) {
    delete[] r->spectral_centroid;
    delete[] r->spectral_flatness;
    delete[] r->spectral_rolloff;
    delete[] r->timbre_over_time;
    r->spectral_centroid = nullptr;
    r->spectral_centroid_count = 0;
    r->spectral_flatness = nullptr;
    r->spectral_flatness_count = 0;
    r->spectral_rolloff = nullptr;
    r->spectral_rolloff_count = 0;
    r->timbre_over_time = nullptr;
    r->timbre_over_time_count = 0;
  }
}

void sonare_free_chord_analysis_result(SonareChordAnalysisResult* r) {
  if (r) {
    delete[] r->chords;
    r->chords = nullptr;
    r->chord_count = 0;
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

void sonare_free_bounce_result(SonareEngineBounceResult* result) {
  if (!result) return;
  delete[] result->interleaved;
  result->interleaved = nullptr;
  result->sample_count = 0;
}

void sonare_free_section_result(SonareSectionResult* result) {
  if (result) {
    delete[] result->sections;
    result->sections = nullptr;
    result->section_count = 0;
  }
}

void sonare_free_melody_result(SonareMelodyResult* result) {
  if (result) {
    delete[] result->points;
    result->points = nullptr;
    result->point_count = 0;
  }
}

void sonare_free_cqt_result(SonareCqtResult* result) {
  if (result) {
    delete[] result->magnitude;
    delete[] result->frequencies;
    result->magnitude = nullptr;
    result->frequencies = nullptr;
  }
}
