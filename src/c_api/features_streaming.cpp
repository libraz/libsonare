#include "c_api/features_internal.h"

// ============================================================================
// Streaming - StreamAnalyzer (stateful real-time frame analyzer)
// ============================================================================

// Opaque handle backing SonareStreamAnalyzer. Owns the C++ analyzer instance.
struct SonareStreamAnalyzer {
  std::unique_ptr<sonare::StreamAnalyzer> analyzer;
};

namespace {

// Vector -> caller-owned C array copies funnel through copy_vector<T> in
// sonare_c_internal.h (single canonical owner).

bool finite_positive(float value) { return std::isfinite(value) && value > 0.0f; }

bool finite_non_negative(float value) { return std::isfinite(value) && value >= 0.0f; }

bool valid_window(int value) {
  return value >= SONARE_WINDOW_HANN && value <= SONARE_WINDOW_RECTANGULAR;
}

bool valid_output_format(int value) {
  return value >= SONARE_STREAM_OUTPUT_FLOAT32 && value <= SONARE_STREAM_OUTPUT_UINT8;
}

WindowType to_window_type(int value) {
  switch (static_cast<SonareWindowType>(value)) {
    case SONARE_WINDOW_HAMMING:
      return WindowType::Hamming;
    case SONARE_WINDOW_BLACKMAN:
      return WindowType::Blackman;
    case SONARE_WINDOW_RECTANGULAR:
      return WindowType::Rectangular;
    case SONARE_WINDOW_HANN:
    default:
      return WindowType::Hann;
  }
}

OutputFormat to_output_format(int value) {
  switch (static_cast<SonareStreamOutputFormat>(value)) {
    case SONARE_STREAM_OUTPUT_INT16:
      return OutputFormat::Int16;
    case SONARE_STREAM_OUTPUT_UINT8:
      return OutputFormat::Uint8;
    case SONARE_STREAM_OUTPUT_FLOAT32:
    default:
      return OutputFormat::Float32;
  }
}

SonareStreamChordChange* copy_chord_changes(const std::vector<ChordChange>& v) {
  if (v.empty()) return nullptr;
  std::unique_ptr<SonareStreamChordChange[]> buf(new SonareStreamChordChange[v.size()]);
  for (size_t i = 0; i < v.size(); ++i) {
    buf[i] = {v[i].root, v[i].quality, v[i].start_time, v[i].confidence};
  }
  return release_array(buf);
}

SonareStreamBarChord* copy_bar_chords(const std::vector<BarChord>& v) {
  if (v.empty()) return nullptr;
  std::unique_ptr<SonareStreamBarChord[]> buf(new SonareStreamBarChord[v.size()]);
  for (size_t i = 0; i < v.size(); ++i) {
    buf[i] = {v[i].bar_index, v[i].root, v[i].quality, v[i].start_time, v[i].confidence};
  }
  return release_array(buf);
}

SonareStreamPatternScore* copy_pattern_scores(const std::vector<std::pair<std::string, float>>& v) {
  if (v.empty()) return nullptr;
  std::unique_ptr<SonareStreamPatternScore[]> buf(new SonareStreamPatternScore[v.size()]);
  for (size_t i = 0; i < v.size(); ++i) {
    std::strncpy(buf[i].name, v[i].first.c_str(), sizeof(buf[i].name) - 1);
    buf[i].name[sizeof(buf[i].name) - 1] = '\0';
    buf[i].score = v[i].second;
  }
  return release_array(buf);
}

}  // namespace

SonareError sonare_stream_analyzer_config_default(SonareStreamConfig* config) {
  if (!config) return SONARE_ERROR_INVALID_PARAMETER;
  StreamConfig defaults;
  config->sample_rate = defaults.sample_rate;
  config->n_fft = defaults.n_fft;
  config->hop_length = defaults.hop_length;
  config->n_mels = defaults.n_mels;
  config->fmin = defaults.fmin;
  config->fmax = defaults.fmax;
  config->tuning_ref_hz = defaults.tuning_ref_hz;
  // The C ABI streaming surface has no magnitude read path, so the default
  // config advertises it as off (an explicit non-zero is rejected by _create).
  config->compute_magnitude = 0;
  config->compute_mel = defaults.compute_mel ? 1 : 0;
  config->compute_chroma = defaults.compute_chroma ? 1 : 0;
  config->compute_onset = defaults.compute_onset ? 1 : 0;
  config->compute_spectral = defaults.compute_spectral ? 1 : 0;
  config->emit_every_n_frames = defaults.emit_every_n_frames;
  config->magnitude_downsample = defaults.magnitude_downsample;
  config->key_update_interval_sec = defaults.key_update_interval_sec;
  config->bpm_update_interval_sec = defaults.bpm_update_interval_sec;
  config->window = SONARE_WINDOW_HANN;
  config->output_format = SONARE_STREAM_OUTPUT_FLOAT32;
  return SONARE_OK;
}

SonareError sonare_stream_analyzer_create(const SonareStreamConfig* config,
                                          SonareStreamAnalyzer** out) {
  if (!config || !out) return SONARE_ERROR_INVALID_PARAMETER;
  if (config->sample_rate <= 0 || config->n_fft <= 0 || config->hop_length <= 0 ||
      config->hop_length > config->n_fft || config->n_mels <= 0 ||
      config->emit_every_n_frames <= 0 || config->magnitude_downsample <= 0 ||
      !finite_non_negative(config->fmin) || !finite_non_negative(config->fmax) ||
      (config->fmax > 0.0f && config->fmax <= config->fmin) ||
      !finite_positive(config->tuning_ref_hz) ||
      !finite_positive(config->key_update_interval_sec) ||
      !finite_positive(config->bpm_update_interval_sec) || !valid_window(config->window) ||
      !valid_output_format(config->output_format)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Magnitude readout is not surfaced by any SOA read path, so honestly reject
  // compute_magnitude rather than silently accepting and ignoring it.
  if (config->compute_magnitude != 0) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;

  SONARE_C_TRY
  StreamConfig cfg;
  cfg.sample_rate = config->sample_rate;
  cfg.n_fft = config->n_fft;
  cfg.hop_length = config->hop_length;
  cfg.n_mels = config->n_mels;
  cfg.fmin = config->fmin;
  cfg.fmax = config->fmax;
  cfg.tuning_ref_hz = config->tuning_ref_hz;
  cfg.window = to_window_type(config->window);
  // The SOA read paths (read_frames / read_frames_u8 / read_frames_i16) do not
  // surface the per-frame magnitude spectrum. An explicit compute_magnitude is
  // rejected above; the default config leaves it off, so this is always false.
  cfg.compute_magnitude = false;
  cfg.compute_mel = config->compute_mel != 0;
  cfg.compute_chroma = config->compute_chroma != 0;
  cfg.compute_onset = config->compute_onset != 0;
  cfg.compute_spectral = config->compute_spectral != 0;
  cfg.emit_every_n_frames = config->emit_every_n_frames;
  cfg.magnitude_downsample = config->magnitude_downsample;
  cfg.output_format = to_output_format(config->output_format);
  cfg.key_update_interval_sec = config->key_update_interval_sec;
  cfg.bpm_update_interval_sec = config->bpm_update_interval_sec;

  auto handle = std::make_unique<SonareStreamAnalyzer>();
  handle->analyzer = std::make_unique<StreamAnalyzer>(cfg);
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_stream_analyzer_destroy(SonareStreamAnalyzer* analyzer) { delete analyzer; }

SonareError sonare_stream_analyzer_process(SonareStreamAnalyzer* analyzer, const float* samples,
                                           size_t n_samples) {
  if (!analyzer || !analyzer->analyzer || (!samples && n_samples > 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  analyzer->analyzer->process(samples, n_samples);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_process_with_offset(SonareStreamAnalyzer* analyzer,
                                                       const float* samples, size_t n_samples,
                                                       size_t sample_offset) {
  if (!analyzer || !analyzer->analyzer || (!samples && n_samples > 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  analyzer->analyzer->process(samples, n_samples, sample_offset);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_finalize(SonareStreamAnalyzer* analyzer) {
  if (!analyzer || !analyzer->analyzer) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  analyzer->analyzer->finalize();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_available_frames(SonareStreamAnalyzer* analyzer,
                                                    size_t* out_count) {
  if (!analyzer || !analyzer->analyzer || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_count = analyzer->analyzer->available_frames();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_read_frames(SonareStreamAnalyzer* analyzer, size_t max_frames,
                                               SonareStreamFrames* out) {
  if (!analyzer || !analyzer->analyzer || !out) return SONARE_ERROR_INVALID_PARAMETER;

  *out = {};

  SONARE_C_TRY
  FrameBuffer buffer;
  analyzer->analyzer->read_frames_soa(max_frames, buffer);

  out->n_frames = static_cast<int>(buffer.n_frames);
  out->n_mels = analyzer->analyzer->config().n_mels;
  out->timestamps = copy_vector(buffer.timestamps);
  out->mel = copy_vector(buffer.mel);
  out->chroma = copy_vector(buffer.chroma);
  out->onset_strength = copy_vector(buffer.onset_strength);
  out->rms_energy = copy_vector(buffer.rms_energy);
  out->spectral_centroid = copy_vector(buffer.spectral_centroid);
  out->spectral_flatness = copy_vector(buffer.spectral_flatness);
  out->chord_root = copy_vector(buffer.chord_root);
  out->chord_quality = copy_vector(buffer.chord_quality);
  out->chord_confidence = copy_vector(buffer.chord_confidence);
  return SONARE_OK;
  SONARE_C_CATCH
}

namespace {

// Maps the flat C quantization config onto the C++ QuantizeConfig. A null
// pointer yields the library defaults (matching the no-arg read path).
QuantizeConfig to_quantize_config(const SonareStreamQuantizeConfig* config) {
  QuantizeConfig q;
  if (config) {
    q.mel_db_min = config->mel_db_min;
    q.mel_db_max = config->mel_db_max;
    q.onset_max = config->onset_max;
    q.rms_max = config->rms_max;
    q.centroid_max = config->centroid_max;
  }
  return q;
}

}  // namespace

SonareError sonare_stream_quantize_config_default(SonareStreamQuantizeConfig* config) {
  if (!config) return SONARE_ERROR_INVALID_PARAMETER;
  const QuantizeConfig q;
  config->mel_db_min = q.mel_db_min;
  config->mel_db_max = q.mel_db_max;
  config->onset_max = q.onset_max;
  config->rms_max = q.rms_max;
  config->centroid_max = q.centroid_max;
  return SONARE_OK;
}

SonareError sonare_stream_analyzer_read_frames_u8(SonareStreamAnalyzer* analyzer, size_t max_frames,
                                                  SonareStreamFramesU8* out) {
  return sonare_stream_analyzer_read_frames_u8_ex(analyzer, nullptr, max_frames, out);
}

SonareError sonare_stream_analyzer_read_frames_u8_ex(SonareStreamAnalyzer* analyzer,
                                                     const SonareStreamQuantizeConfig* config,
                                                     size_t max_frames, SonareStreamFramesU8* out) {
  if (!analyzer || !analyzer->analyzer || !out) return SONARE_ERROR_INVALID_PARAMETER;

  *out = {};

  SONARE_C_TRY
  QuantizedFrameBufferU8 buffer;
  analyzer->analyzer->read_frames_quantized_u8(max_frames, buffer, to_quantize_config(config));

  out->n_frames = static_cast<int>(buffer.n_frames);
  out->n_mels = buffer.n_mels;
  out->timestamps = copy_vector(buffer.timestamps);
  out->mel = copy_vector(buffer.mel);
  out->chroma = copy_vector(buffer.chroma);
  out->onset_strength = copy_vector(buffer.onset_strength);
  out->rms_energy = copy_vector(buffer.rms_energy);
  out->spectral_centroid = copy_vector(buffer.spectral_centroid);
  out->spectral_flatness = copy_vector(buffer.spectral_flatness);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_read_frames_i16(SonareStreamAnalyzer* analyzer,
                                                   size_t max_frames, SonareStreamFramesI16* out) {
  return sonare_stream_analyzer_read_frames_i16_ex(analyzer, nullptr, max_frames, out);
}

SonareError sonare_stream_analyzer_read_frames_i16_ex(SonareStreamAnalyzer* analyzer,
                                                      const SonareStreamQuantizeConfig* config,
                                                      size_t max_frames,
                                                      SonareStreamFramesI16* out) {
  if (!analyzer || !analyzer->analyzer || !out) return SONARE_ERROR_INVALID_PARAMETER;

  *out = {};

  SONARE_C_TRY
  QuantizedFrameBufferI16 buffer;
  analyzer->analyzer->read_frames_quantized_i16(max_frames, buffer, to_quantize_config(config));

  out->n_frames = static_cast<int>(buffer.n_frames);
  out->n_mels = buffer.n_mels;
  out->timestamps = copy_vector(buffer.timestamps);
  out->mel = copy_vector(buffer.mel);
  out->chroma = copy_vector(buffer.chroma);
  out->onset_strength = copy_vector(buffer.onset_strength);
  out->rms_energy = copy_vector(buffer.rms_energy);
  out->spectral_centroid = copy_vector(buffer.spectral_centroid);
  out->spectral_flatness = copy_vector(buffer.spectral_flatness);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_reset(SonareStreamAnalyzer* analyzer,
                                         size_t base_sample_offset) {
  if (!analyzer || !analyzer->analyzer) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  analyzer->analyzer->reset(base_sample_offset);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_stats(SonareStreamAnalyzer* analyzer, SonareStreamStats* out) {
  if (!analyzer || !analyzer->analyzer || !out) return SONARE_ERROR_INVALID_PARAMETER;

  *out = {};

  SONARE_C_TRY
  AnalyzerStats s = analyzer->analyzer->stats();
  out->total_frames = s.total_frames;
  out->total_samples = s.total_samples;
  out->duration_seconds = s.duration_seconds;
  out->bpm = s.estimate.bpm;
  out->bpm_confidence = s.estimate.bpm_confidence;
  out->bpm_candidate_count = s.estimate.bpm_candidate_count;
  out->key = s.estimate.key;
  out->key_minor = s.estimate.key_minor ? 1 : 0;
  out->key_confidence = s.estimate.key_confidence;
  out->chord_root = s.estimate.chord_root;
  out->chord_quality = s.estimate.chord_quality;
  out->chord_confidence = s.estimate.chord_confidence;
  out->chord_start_time = s.estimate.chord_start_time;
  out->current_bar = s.estimate.current_bar;
  out->bar_duration = s.estimate.bar_duration;
  out->chord_progression_count = s.estimate.chord_progression.size();
  out->chord_progression = copy_chord_changes(s.estimate.chord_progression);
  out->bar_chord_progression_count = s.estimate.bar_chord_progression.size();
  out->bar_chord_progression = copy_bar_chords(s.estimate.bar_chord_progression);
  out->pattern_length = s.estimate.pattern_length;
  out->voted_pattern_count = s.estimate.voted_pattern.size();
  out->voted_pattern = copy_bar_chords(s.estimate.voted_pattern);
  std::strncpy(out->detected_pattern_name, s.estimate.detected_pattern_name.c_str(),
               sizeof(out->detected_pattern_name) - 1);
  out->detected_pattern_name[sizeof(out->detected_pattern_name) - 1] = '\0';
  out->detected_pattern_score = s.estimate.detected_pattern_score;
  out->all_pattern_scores_count = s.estimate.all_pattern_scores.size();
  out->all_pattern_scores = copy_pattern_scores(s.estimate.all_pattern_scores);
  out->accumulated_seconds = s.estimate.accumulated_seconds;
  out->used_frames = s.estimate.used_frames;
  out->updated = s.estimate.updated ? 1 : 0;
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_free_stream_stats(SonareStreamStats* stats) {
  if (!stats) return;
  delete[] stats->chord_progression;
  delete[] stats->bar_chord_progression;
  delete[] stats->voted_pattern;
  delete[] stats->all_pattern_scores;
  *stats = {};
}

SonareError sonare_stream_analyzer_frame_count(SonareStreamAnalyzer* analyzer, int* out_count) {
  if (!analyzer || !analyzer->analyzer || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_count = analyzer->analyzer->frame_count();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_current_time(SonareStreamAnalyzer* analyzer,
                                                float* out_seconds) {
  if (!analyzer || !analyzer->analyzer || !out_seconds) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_seconds = analyzer->analyzer->current_time();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_sample_rate(SonareStreamAnalyzer* analyzer,
                                               int* out_sample_rate) {
  if (!analyzer || !analyzer->analyzer || !out_sample_rate) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_sample_rate = analyzer->analyzer->config().sample_rate;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_set_expected_duration(SonareStreamAnalyzer* analyzer,
                                                         float duration_seconds) {
  if (!analyzer || !analyzer->analyzer) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  analyzer->analyzer->set_expected_duration(duration_seconds);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_set_normalization_gain(SonareStreamAnalyzer* analyzer,
                                                          float gain) {
  if (!analyzer || !analyzer->analyzer) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  analyzer->analyzer->set_normalization_gain(gain);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_set_tuning_ref_hz(SonareStreamAnalyzer* analyzer, float ref_hz) {
  if (!analyzer || !analyzer->analyzer) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  analyzer->analyzer->set_tuning_ref_hz(ref_hz);
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_free_stream_frames(SonareStreamFrames* frames) {
  if (!frames) return;
  delete[] frames->timestamps;
  delete[] frames->mel;
  delete[] frames->chroma;
  delete[] frames->onset_strength;
  delete[] frames->rms_energy;
  delete[] frames->spectral_centroid;
  delete[] frames->spectral_flatness;
  delete[] frames->chord_root;
  delete[] frames->chord_quality;
  delete[] frames->chord_confidence;
  *frames = {};
}

void sonare_free_stream_frames_u8(SonareStreamFramesU8* frames) {
  if (!frames) return;
  delete[] frames->timestamps;
  delete[] frames->mel;
  delete[] frames->chroma;
  delete[] frames->onset_strength;
  delete[] frames->rms_energy;
  delete[] frames->spectral_centroid;
  delete[] frames->spectral_flatness;
  *frames = {};
}

void sonare_free_stream_frames_i16(SonareStreamFramesI16* frames) {
  if (!frames) return;
  delete[] frames->timestamps;
  delete[] frames->mel;
  delete[] frames->chroma;
  delete[] frames->onset_strength;
  delete[] frames->rms_energy;
  delete[] frames->spectral_centroid;
  delete[] frames->spectral_flatness;
  *frames = {};
}
