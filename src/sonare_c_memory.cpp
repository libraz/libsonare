#include "rt/command.h"
#include "sonare.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"

using namespace sonare_c_detail;

void sonare_free_floats(float* ptr) { delete[] ptr; }

void sonare_free_ints(int* ptr) { delete[] ptr; }

void sonare_free_string(char* ptr) { delete[] ptr; }

void sonare_free_key_candidates(SonareKeyCandidate* ptr) { delete[] ptr; }

void sonare_free_result(SonareAnalysisResult* result) {
  if (result != nullptr) {
    delete[] result->beat_times;
    result->beat_times = nullptr;
    result->beat_count = 0;
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
    default:
      return "Unknown error";
  }
}

const char* sonare_last_error_message(void) { return last_error_storage().c_str(); }

const char* sonare_version(void) { return SONARE_VERSION_STRING; }

uint32_t sonare_engine_abi_version(void) { return sonare::rt::kEngineAbiVersion; }

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
    r->spectral_centroid = nullptr;
    r->spectral_centroid_count = 0;
    r->spectral_flatness = nullptr;
    r->spectral_flatness_count = 0;
    r->spectral_rolloff = nullptr;
    r->spectral_rolloff_count = 0;
  }
}

void sonare_free_chord_analysis_result(SonareChordAnalysisResult* r) {
  if (r) {
    delete[] r->chords;
    r->chords = nullptr;
    r->chord_count = 0;
  }
}

void sonare_free_bounce_result(SonareEngineBounceResult* result) {
  if (!result) return;
  delete[] result->interleaved;
  result->interleaved = nullptr;
  result->sample_count = 0;
}
