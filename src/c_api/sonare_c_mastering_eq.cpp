#include <sonare/sonare_c.h>

#include <memory>

#include "c_api/eq_band_json.h"
#include "mastering/eq/equalizer.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

// Keep the C API band count in sync with the C++ processor and the spectrum
// engine's fixed-size band_gain_db array (std::array<float, 24>).
static_assert(SONARE_EQ_MAX_BANDS == sonare::mastering::eq::EqualizerProcessor::kMaxBands,
              "SONARE_EQ_MAX_BANDS must match EqualizerProcessor::kMaxBands");

struct SonareEq {
  sonare::mastering::eq::EqualizerProcessor processor;
  double sample_rate = 48000.0;
  int max_block_size = 0;
};

namespace {

sonare::mastering::eq::PhaseMode parse_phase(int mode) {
  using sonare::mastering::eq::PhaseMode;
  switch (mode) {
    case 1:
      return PhaseMode::ZeroLatency;
    case 2:
      return PhaseMode::NaturalPhase;
    case 3:
      return PhaseMode::LinearPhase;
    default:
      throw SonareException(ErrorCode::InvalidParameter, "unknown EQ phase mode");
  }
}

}  // namespace

SonareEq* sonare_eq_create(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size < 0) {
    return nullptr;
  }
  try {
    auto* handle = new SonareEq;
    handle->sample_rate = sample_rate;
    handle->max_block_size = max_block_size;
    handle->processor.prepare(sample_rate, max_block_size);
    return handle;
  } catch (...) {
    return nullptr;
  }
}

void sonare_eq_destroy(SonareEq* eq) { delete eq; }

SonareError sonare_eq_set_band(SonareEq* eq, int index, const char* band_json) {
  if (!eq || !band_json || index < 0 || index >= static_cast<int>(SONARE_EQ_MAX_BANDS)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  eq->processor.set_band(static_cast<size_t>(index), sonare::c_api::parse_eq_band_json(band_json));
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_eq_clear(SonareEq* eq) {
  if (eq) {
    eq->processor.clear();
  }
}

SonareError sonare_eq_set_phase_mode(SonareEq* eq, int mode) {
  if (!eq) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  eq->processor.set_phase_mode(parse_phase(mode));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_eq_match(SonareEq* eq, const float* source, const float* reference,
                            size_t length, int sample_rate, int max_bands) {
  if (!eq || max_bands <= 0 || max_bands > static_cast<int>(SONARE_EQ_MAX_BANDS)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SonareError err = validate_audio_params(source, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(reference, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  sonare::mastering::match::MatchEqConfig config;
  config.max_bands = static_cast<size_t>(max_bands);
  const Audio source_audio = Audio::from_buffer(source, length, sample_rate);
  const Audio reference_audio = Audio::from_buffer(reference, length, sample_rate);
  sonare::mastering::match::configure_equalizer_from_match(
      eq->processor, sonare::mastering::match::reference_spectrum(source_audio),
      sonare::mastering::match::reference_spectrum(reference_audio), config);
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_eq_set_auto_gain(SonareEq* eq, int enabled) {
  if (eq) {
    eq->processor.set_auto_gain_enabled(enabled != 0);
  }
}

float sonare_eq_last_auto_gain_db(const SonareEq* eq) {
  return eq ? eq->processor.last_auto_gain_db() : 0.0f;
}

SonareError sonare_eq_set_gain_scale(SonareEq* eq, float scale) {
  if (!eq) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  eq->processor.set_gain_scale(scale);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_eq_set_output_gain_db(SonareEq* eq, float gain_db) {
  if (!eq) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  eq->processor.set_output_gain_db(gain_db);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_eq_set_output_pan(SonareEq* eq, float pan) {
  if (!eq) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  eq->processor.set_output_pan(pan);
  return SONARE_OK;
  SONARE_C_CATCH
}

int sonare_eq_latency_samples(const SonareEq* eq) {
  return eq ? eq->processor.latency_samples() : 0;
}

SonareError sonare_eq_set_sidechain(SonareEq* eq, const float* const* channels, int num_channels,
                                    int num_samples) {
  if (!eq) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  eq->processor.set_sidechain(channels, num_channels, num_samples);
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_eq_clear_sidechain(SonareEq* eq) {
  if (eq) {
    eq->processor.clear_sidechain();
  }
}

SonareError sonare_eq_process(SonareEq* eq, float* const* channels, int num_channels,
                              int num_samples) {
  if (!eq) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  eq->processor.process(channels, num_channels, num_samples);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_eq_spectrum(const SonareEq* eq, SonareEqSnapshot* out) {
  if (!eq || !out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const auto snapshot = eq->processor.spectrum_snapshot();
  *out = {};
  out->pre_count = snapshot.pre_count;
  out->post_count = snapshot.post_count;
  out->last_auto_gain_db = eq->processor.last_auto_gain_db();
  out->seq = snapshot.seq;
  for (size_t i = 0; i < SONARE_EQ_SPECTRUM_STREAM_CAPACITY; ++i) {
    out->pre_left[i] = snapshot.pre[i].left;
    out->pre_right[i] = snapshot.pre[i].right;
    out->post_left[i] = snapshot.post[i].left;
    out->post_right[i] = snapshot.post[i].right;
  }
  for (size_t i = 0; i < SONARE_EQ_MAX_BANDS; ++i) {
    out->band_gain_db[i] = snapshot.band_gain_db[i];
  }
  for (size_t i = 0; i < SONARE_EQ_SPECTRUM_PROFILE_BANDS; ++i) {
    out->profile_db[i] = snapshot.profile_db[i];
  }
  return SONARE_OK;
  SONARE_C_CATCH
}
