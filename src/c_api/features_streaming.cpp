#include <array>
#include <cmath>
#include <limits>

#include "c_api/features_internal.h"
#include "util/parameter_checks.h"

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

namespace check = sonare::util::check;

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
    copy_text(buf[i].name, sizeof(buf[i].name), v[i].first.c_str());
    buf[i].score = v[i].second;
  }
  return release_array(buf);
}

template <typename Frames>
void free_stream_frame_arrays(Frames* frames) {
  if (!frames) return;
  delete[] frames->timestamps_sec;
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

template <typename Buffer, typename Frames>
void copy_stream_frame_arrays(const Buffer& buffer, Frames* out) {
  sonare_c_detail::StagedOutput<Frames, &free_stream_frame_arrays<Frames>> staged;
  staged->n_frames = static_cast<int>(buffer.n_frames);
  staged->n_mels = buffer.n_mels;
  staged->timestamps_sec = copy_vector(buffer.timestamps);
  staged->mel = copy_vector(buffer.mel);
  staged->chroma = copy_vector(buffer.chroma);
  staged->onset_strength = copy_vector(buffer.onset_strength);
  staged->rms_energy = copy_vector(buffer.rms_energy);
  staged->spectral_centroid = copy_vector(buffer.spectral_centroid);
  staged->spectral_flatness = copy_vector(buffer.spectral_flatness);
  staged->chord_root = copy_vector(buffer.chord_root);
  staged->chord_quality = copy_vector(buffer.chord_quality);
  staged->chord_confidence = copy_vector(buffer.chord_confidence);
  staged->feature_flags = buffer.feature_flags;
  staged->n_chroma = buffer.n_chroma;
  staged.publish(out);
}

}  // namespace

void sonare_stream_config_init(SonareStreamConfig* config) {
  if (config == nullptr) return;
  const StreamConfig defaults;
  *config = {};
  config->struct_size = sizeof(*config);
  config->sample_rate = defaults.sample_rate;
  config->n_fft = defaults.n_fft;
  config->hop_length = static_cast<size_t>(defaults.hop_length);
  config->n_mels = defaults.n_mels;
  config->fmin_hz = defaults.fmin;
  // The core spells the Nyquist default as 0; the C field leaves it absent.
  config->fmax_hz = defaults.fmax == 0.0f ? std::numeric_limits<float>::quiet_NaN() : defaults.fmax;
  config->reference_hz = defaults.tuning_ref_hz;
  // The C ABI streaming surface has no magnitude read path, so the default
  // config advertises it as off (an explicit non-zero is rejected by _create).
  config->compute_magnitude = 0;
  config->compute_mel = defaults.compute_mel ? 1 : 0;
  config->compute_chroma = defaults.compute_chroma ? 1 : 0;
  config->compute_onset = defaults.compute_onset ? 1 : 0;
  config->compute_spectral = defaults.compute_spectral ? 1 : 0;
  config->emit_every_n_frames = defaults.emit_every_n_frames;
  config->magnitude_downsample = defaults.magnitude_downsample;
  config->max_pending_frames = defaults.max_pending_frames;
  config->key_update_interval_sec = defaults.key_update_interval_sec;
  config->bpm_update_interval_sec = defaults.bpm_update_interval_sec;
  config->window = SONARE_WINDOW_HANN;
  config->output_format = SONARE_STREAM_OUTPUT_FLOAT32;
  config->max_progression_entries = defaults.max_progression_entries;
}

SonareError sonare_stream_analyzer_create(const SonareStreamConfig* config,
                                          SonareStreamAnalyzer** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(config);
  if (const SonareError err = run_validation([&] {
        sonare_c_detail::require_struct_size(config->struct_size, sizeof(SonareStreamConfig),
                                             "config", "sonare_stream_config_init");
        check::integer_within(config->sample_rate, kAudioSampleRateDomain, "config.sample_rate");
        check::integer_within(config->n_fft, kStreamConfigNFftDomain, "config.n_fft");
        check::integer_within(config->hop_length, kStreamConfigHopLengthDomain,
                              "config.hop_length");
        if (config->hop_length > static_cast<size_t>(config->n_fft)) {
          check::refuse_relation({"config.hop_length", "config.n_fft"}, "{0} must be at most {1}");
        }
        check::integer_within(config->n_mels, kStreamConfigNMelsDomain, "config.n_mels");
        check::at_least(config->fmin_hz, 0.0, "config.fmin_hz");
        if (!std::isnan(config->fmax_hz)) {
          check::greater_than(config->fmax_hz, 0.0, "config.fmax_hz");
          if (config->fmax_hz <= config->fmin_hz) {
            check::refuse_relation({"config.fmax_hz", "config.fmin_hz"},
                                   "{0} must be greater than {1}");
          }
        }
        check::in_range(config->reference_hz, kMinTuningRefHz, kMaxTuningRefHz,
                        "config.reference_hz");
        check::integer_within(config->emit_every_n_frames, kStreamConfigEmitEveryNFramesDomain,
                              "config.emit_every_n_frames");
        check::integer_at_least(config->magnitude_downsample, 1, "config.magnitude_downsample");
        check::integer_within(static_cast<long long>(config->max_pending_frames),
                              kStreamConfigMaxPendingFramesDomain, "config.max_pending_frames");
        check::greater_than(config->key_update_interval_sec, 0.0, "config.key_update_interval_sec");
        check::greater_than(config->bpm_update_interval_sec, 0.0, "config.bpm_update_interval_sec");
        check::integer_in_range(config->window, SONARE_WINDOW_HANN, SONARE_WINDOW_RECTANGULAR,
                                "config.window");
        check::one_of(config->output_format, std::array<int, 1>{SONARE_STREAM_OUTPUT_FLOAT32},
                      "config.output_format");
        check::integer_within(static_cast<long long>(config->max_progression_entries),
                              kStreamConfigMaxProgressionEntriesDomain,
                              "config.max_progression_entries");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  StreamConfig cfg;
  // Magnitude readout is not surfaced by any SOA read path, so use the shared
  // NotSupported cause with the C config field as its subject.
  cfg.compute_magnitude = config->compute_magnitude != 0;
  validate_soa_stream_config(cfg, "config.compute_magnitude");
  cfg.sample_rate = config->sample_rate;
  cfg.n_fft = config->n_fft;
  cfg.hop_length = static_cast<int>(config->hop_length);
  cfg.n_mels = config->n_mels;
  cfg.fmin = config->fmin_hz;
  cfg.fmax = std::isnan(config->fmax_hz) ? 0.0f : config->fmax_hz;
  cfg.tuning_ref_hz = config->reference_hz;
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
  cfg.max_pending_frames = config->max_pending_frames;
  cfg.max_progression_entries = config->max_progression_entries;
  cfg.output_format = OutputFormat::Float32;
  cfg.key_update_interval_sec = config->key_update_interval_sec;
  cfg.bpm_update_interval_sec = config->bpm_update_interval_sec;
  validate_stream_config_sizing(cfg);

  auto handle = std::make_unique<SonareStreamAnalyzer>();
  handle->analyzer = std::make_unique<StreamAnalyzer>(cfg);
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_stream_analyzer_destroy(SonareStreamAnalyzer* analyzer) { delete analyzer; }

SonareError sonare_stream_analyzer_process(SonareStreamAnalyzer* analyzer, const float* samples,
                                           size_t n_samples, const size_t* sample_offset) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(samples, n_samples);
  SONARE_C_TRY
  if (sample_offset == nullptr) {
    analyzer->analyzer->process(samples, n_samples);
  } else {
    analyzer->analyzer->process(samples, n_samples, *sample_offset);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_finalize(SonareStreamAnalyzer* analyzer) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_TRY
  analyzer->analyzer->finalize();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_available_frames(SonareStreamAnalyzer* analyzer,
                                                    size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_REQUIRE_NONNULL(out_count);
  SONARE_C_TRY
  *out_count = analyzer->analyzer->available_frames();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_read_frames(SonareStreamAnalyzer* analyzer, size_t max_frames,
                                               SonareStreamFrames* out) {
  SONARE_C_API_ENTRY;

  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  FrameBuffer buffer;
  analyzer->analyzer->read_frames_soa(max_frames, buffer);
  copy_stream_frame_arrays(buffer, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

namespace {

// Maps the flat C quantization config onto the C++ QuantizeConfig. A null
// pointer yields the library defaults (matching the no-arg read path).
QuantizeConfig to_quantize_config(const SonareStreamQuantizeConfig* config) {
  QuantizeConfig q;
  if (config) {
    sonare_c_detail::require_struct_size(config->struct_size, sizeof(SonareStreamQuantizeConfig),
                                         "config", "sonare_stream_quantize_config_init");
    q.mel_db_min = config->mel_db_min;
    q.mel_db_max = config->mel_db_max;
    q.onset_max = config->onset_max;
    q.rms_max = config->rms_max;
    q.centroid_max = config->centroid_max;
  }
  return q;
}

}  // namespace

void sonare_stream_quantize_config_init(SonareStreamQuantizeConfig* config) {
  if (config == nullptr) return;
  const QuantizeConfig q;
  *config = {};
  config->struct_size = sizeof(*config);
  config->mel_db_min = q.mel_db_min;
  config->mel_db_max = q.mel_db_max;
  config->onset_max = q.onset_max;
  config->rms_max = q.rms_max;
  config->centroid_max = q.centroid_max;
}

SonareError sonare_stream_analyzer_read_quantized_frames_u8(
    SonareStreamAnalyzer* analyzer, const SonareStreamQuantizeConfig* config, size_t max_frames,
    SonareStreamFramesU8* out) {
  SONARE_C_API_ENTRY;

  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  QuantizedFrameBufferU8 buffer;
  analyzer->analyzer->read_frames_quantized_u8(max_frames, buffer, to_quantize_config(config));
  copy_stream_frame_arrays(buffer, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_read_quantized_frames_i16(
    SonareStreamAnalyzer* analyzer, const SonareStreamQuantizeConfig* config, size_t max_frames,
    SonareStreamFramesI16* out) {
  SONARE_C_API_ENTRY;

  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  QuantizedFrameBufferI16 buffer;
  analyzer->analyzer->read_frames_quantized_i16(max_frames, buffer, to_quantize_config(config));
  copy_stream_frame_arrays(buffer, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_reset(SonareStreamAnalyzer* analyzer,
                                         size_t base_sample_offset) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_TRY
  analyzer->analyzer->reset(base_sample_offset);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_stats(SonareStreamAnalyzer* analyzer, SonareStreamStats* out) {
  SONARE_C_API_ENTRY;

  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  AnalyzerStats s = analyzer->analyzer->stats();
  sonare_c_detail::StagedOutput<SonareStreamStats, &sonare_free_stream_stats> staged;
  staged->total_frames = s.total_frames;
  staged->total_samples = s.total_samples;
  staged->duration_sec = s.duration_seconds;
  staged->pending_frames = s.pending_frames;
  staged->dropped_output_frames = s.dropped_output_frames;
  staged->bpm = s.estimate.bpm;
  staged->bpm_confidence = s.estimate.bpm_confidence;
  staged->bpm_candidate_count = static_cast<size_t>(s.estimate.bpm_candidate_count);
  staged->key = s.estimate.key;
  staged->key_minor = s.estimate.key_minor ? 1 : 0;
  staged->key_confidence = s.estimate.key_confidence;
  staged->chord_root = s.estimate.chord_root;
  staged->chord_quality = s.estimate.chord_quality;
  staged->chord_confidence = s.estimate.chord_confidence;
  staged->chord_start_sec = s.estimate.chord_start_time;
  staged->current_bar = s.estimate.current_bar;
  staged->bar_duration_sec = s.estimate.bar_duration;
  staged->chord_progression_count = s.estimate.chord_progression.size();
  staged->chord_progression = copy_chord_changes(s.estimate.chord_progression);
  staged->bar_chord_progression_count = s.estimate.bar_chord_progression.size();
  staged->bar_chord_progression = copy_bar_chords(s.estimate.bar_chord_progression);
  staged->pattern_length = static_cast<size_t>(s.estimate.pattern_length);
  staged->voted_pattern_count = s.estimate.voted_pattern.size();
  staged->voted_pattern = copy_bar_chords(s.estimate.voted_pattern);
  copy_text(staged->detected_pattern_name, sizeof(staged->detected_pattern_name),
            s.estimate.detected_pattern_name.c_str());
  staged->detected_pattern_score = s.estimate.detected_pattern_score;
  staged->all_pattern_scores_count = s.estimate.all_pattern_scores.size();
  staged->all_pattern_scores = copy_pattern_scores(s.estimate.all_pattern_scores);
  staged->accumulated_sec = s.estimate.accumulated_seconds;
  staged->used_frames = s.estimate.used_frames;
  staged->updated = s.estimate.updated ? 1 : 0;
  staged->dropped_chord_progression_entries = s.dropped_chord_progression_entries;
  staged->dropped_bar_progression_entries = s.dropped_bar_progression_entries;
  staged->non_finite_discard_blocks = s.non_finite_discard_blocks;
  staged.publish(out);
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

SonareError sonare_stream_analyzer_frame_count(SonareStreamAnalyzer* analyzer, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_REQUIRE_NONNULL(out_count);
  SONARE_C_TRY
  *out_count = static_cast<size_t>(analyzer->analyzer->frame_count());
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_current_time_sec(SonareStreamAnalyzer* analyzer,
                                                    float* out_sec) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_sec);
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_REQUIRE_NONNULL(out_sec);
  SONARE_C_TRY
  *out_sec = analyzer->analyzer->current_time();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_sample_rate(SonareStreamAnalyzer* analyzer,
                                               int* out_sample_rate) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_sample_rate);
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_REQUIRE_NONNULL(out_sample_rate);
  SONARE_C_TRY
  *out_sample_rate = analyzer->analyzer->config().sample_rate;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_set_expected_duration(SonareStreamAnalyzer* analyzer,
                                                         float duration_sec) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_TRY
  analyzer->analyzer->set_expected_duration(duration_sec);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_set_normalization_gain(SonareStreamAnalyzer* analyzer,
                                                          float gain) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_TRY
  analyzer->analyzer->set_normalization_gain(gain);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_stream_analyzer_set_reference_hz(SonareStreamAnalyzer* analyzer,
                                                    float reference_hz) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(analyzer);
  SONARE_C_TRY
  analyzer->analyzer->set_tuning_ref_hz(reference_hz);
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_free_stream_frames(SonareStreamFrames* frames) {
  SONARE_C_API_ENTRY;
  free_stream_frame_arrays(frames);
}

void sonare_free_stream_frames_u8(SonareStreamFramesU8* frames) {
  SONARE_C_API_ENTRY;
  free_stream_frame_arrays(frames);
}

void sonare_free_stream_frames_i16(SonareStreamFramesI16* frames) {
  SONARE_C_API_ENTRY;
  free_stream_frame_arrays(frames);
}
