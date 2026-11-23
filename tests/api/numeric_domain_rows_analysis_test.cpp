/// @file numeric_domain_rows_analysis_test.cpp
/// @brief The analysis partition of the numeric-domain table against its C entries.

#include "c_api/numeric_domain_rows_analysis.h"

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "numeric_domain_rows_support.h"

namespace {

using namespace sonare::test::numeric_domains;
using sonare_c_detail::kAnalysisDomainRows;
using sonare_c_detail::numeric_domain_table;

// A probe value below zero is the largest size_t, past every domain's upper bound.
size_t as_size(double value) {
  return value < 0.0 ? std::numeric_limits<size_t>::max() : static_cast<size_t>(value);
}

SonareError probe_dynamics(const std::string& param, double value) {
  float window_sec = 0.4f;
  size_t hop_length = 512;
  float threshold = 6.0f;
  if (is_param(param, "window_sec")) window_sec = static_cast<float>(value);
  if (is_param(param, "hop_length")) hop_length = as_size(value);
  if (is_param(param, "compression_threshold")) threshold = static_cast<float>(value);
  SonareDynamicsResult out{};
  const SonareError err =
      sonare_analyze_dynamics(analysis_samples().data(), analysis_samples().size(), kAnalysisRate,
                              window_sec, hop_length, threshold, &out);
  if (err == SONARE_OK) sonare_free_dynamics_result(&out);
  return err;
}

SonareError probe_impulse_response(const std::string& param, double value) {
  const int bands = is_param(param, "n_octave_bands") ? static_cast<int>(value) : 6;
  SonareAnalyzeImpulseResponseOptions options{};
  sonare_analyze_impulse_response_options_init(&options);
  if (is_param(param, "min_decay_db")) options.min_decay_db = static_cast<float>(value);
  SonareAcousticResult out{};
  const SonareError err = sonare_analyze_impulse_response(
      analysis_samples().data(), analysis_samples().size(), kAnalysisRate, bands, &options, &out);
  if (err == SONARE_OK) sonare_free_acoustic_result(&out);
  return err;
}

SonareError probe_detect_acoustic(const std::string& param, double value) {
  int octave = 6;
  int third = 24;
  float min_decay_db = 30.0f;
  float margin_db = 10.0f;
  if (is_param(param, "n_octave_bands")) octave = static_cast<int>(value);
  if (is_param(param, "n_third_octave_subbands")) third = static_cast<int>(value);
  if (is_param(param, "min_decay_db")) min_decay_db = static_cast<float>(value);
  if (is_param(param, "noise_floor_margin_db")) margin_db = static_cast<float>(value);
  SonareAcousticResult out{};
  const SonareError err =
      sonare_detect_acoustic(analysis_samples().data(), analysis_samples().size(), kAnalysisRate,
                             octave, third, min_decay_db, margin_db, &out);
  if (err == SONARE_OK) sonare_free_acoustic_result(&out);
  return err;
}

SonareError probe_timbre(const std::string& param, double value) {
  int n_fft = 2048;
  size_t hop_length = 512;
  int n_mels = 128;
  int n_mfcc = 13;
  float window_sec = 0.5f;
  if (is_param(param, "n_fft")) n_fft = static_cast<int>(value);
  if (is_param(param, "hop_length")) hop_length = as_size(value);
  if (is_param(param, "n_mels")) n_mels = static_cast<int>(value);
  if (is_param(param, "n_mfcc")) n_mfcc = static_cast<int>(value);
  if (is_param(param, "window_sec")) window_sec = static_cast<float>(value);
  SonareTimbreResult out{};
  const SonareError err =
      sonare_analyze_timbre(analysis_samples().data(), analysis_samples().size(), kAnalysisRate,
                            n_fft, hop_length, n_mels, n_mfcc, window_sec, &out);
  if (err == SONARE_OK) sonare_free_timbre_result(&out);
  return err;
}

struct MelodyArguments {
  float f0_min_hz = 65.0f;
  float f0_max_hz = 2093.0f;
  size_t frame_length = 2048;
  size_t hop_length = 256;
  float threshold = 0.1f;
};

MelodyArguments melody_arguments(const std::string& param, double value) {
  MelodyArguments a;
  if (is_param(param, "f0_min_hz")) a.f0_min_hz = static_cast<float>(value);
  if (is_param(param, "frame_length")) a.frame_length = as_size(value);
  if (is_param(param, "hop_length")) a.hop_length = as_size(value);
  if (is_param(param, "threshold")) a.threshold = static_cast<float>(value);
  return a;
}

SonareError probe_melody(const std::string& param, double value) {
  const MelodyArguments a = melody_arguments(param, value);
  SonareAnalyzeMelodyOptions options{};
  sonare_analyze_melody_options_init(&options);
  SonareMelodyResult out{};
  const SonareError err = sonare_analyze_melody(
      analysis_samples().data(), analysis_samples().size(), kAnalysisRate, a.f0_min_hz, a.f0_max_hz,
      a.frame_length, a.hop_length, a.threshold, &options, &out);
  if (err == SONARE_OK) sonare_free_melody_result(&out);
  return err;
}

const float* pcm() { return analysis_samples().data(); }
size_t pcm_length() { return analysis_samples().size(); }

/// The framing every STFT-based entry takes, at working defaults.
struct Frame {
  int n_fft = 2048;
  size_t hop_length = 512;
};

Frame frame_of(const std::string& param, double value, const std::string& prefix = "") {
  Frame f;
  if (is_param(param, (prefix + "n_fft").c_str())) f.n_fft = static_cast<int>(value);
  if (is_param(param, (prefix + "hop_length").c_str())) f.hop_length = as_size(value);
  return f;
}

/// A short onset-envelope-like series for the tempogram family.
const std::vector<float>& envelope() {
  static const std::vector<float> values = [] {
    std::vector<float> v(600);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<float>(i % 7) * 0.1f;
    return v;
  }();
  return values;
}

const std::vector<float>& planes() {
  static const std::vector<float> values(4096, 0.25f);
  return values;
}

SonareError probe_analyze_bpm(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  SonareBpmAnalysisResult out{};
  const SonareError err = sonare_analyze_bpm(pcm(), pcm_length(), kAnalysisRate, 60.0f, 200.0f,
                                             f.n_fft, f.hop_length, 3, &out);
  if (err == SONARE_OK) sonare_free_bpm_analysis_result(&out);
  return err;
}

SonareError probe_analyze_rhythm(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  SonareRhythmResult out{};
  const SonareError err = sonare_analyze_rhythm(pcm(), pcm_length(), kAnalysisRate, 60.0f, 200.0f,
                                                120.0f, f.n_fft, f.hop_length, &out);
  if (err == SONARE_OK) sonare_free_rhythm_result(&out);
  return err;
}

// The key entries share one options struct; the frame fields are set from the probed value.
SonareError probe_detect_key(const std::string& param, double value) {
  const Frame f = frame_of(param, value, "options.");
  SonareDetectKeyOptions o;
  sonare_detect_key_options_init(&o);
  o.n_fft = f.n_fft;
  o.hop_length = f.hop_length;
  SonareKey key{};
  return sonare_detect_key(pcm(), pcm_length(), kAnalysisRate, &o, &key, nullptr);
}

SonareError probe_detect_key_candidates(const std::string& param, double value) {
  const Frame f = frame_of(param, value, "options.");
  SonareDetectKeyOptions o;
  sonare_detect_key_options_init(&o);
  o.n_fft = f.n_fft;
  o.hop_length = f.hop_length;
  SonareKeyCandidate* candidates = nullptr;
  size_t count = 0;
  const SonareError err = sonare_detect_key_candidates(pcm(), pcm_length(), kAnalysisRate, &o,
                                                       &candidates, &count, nullptr);
  if (err == SONARE_OK) sonare_free_key_candidates(candidates);
  return err;
}

/// Chord options started from the defaults, the named field set to @p value.
SonareChordDetectionOptions chord_options(const std::string& param, double value) {
  SonareChordDetectionOptions options{};
  sonare_chord_detection_options_init(&options);
  if (is_param(param, "options.n_fft")) options.n_fft = static_cast<int>(value);
  if (is_param(param, "options.hop_length")) options.hop_length = as_size(value);
  if (is_param(param, "options.hmm_beam_width")) options.hmm_beam_width = static_cast<int>(value);
  return options;
}

SonareError probe_detect_chords(const std::string& param, double value) {
  const SonareChordDetectionOptions options = chord_options(param, value);
  SonareChordAnalysisResult out{};
  const SonareError err = sonare_detect_chords(pcm(), pcm_length(), kAnalysisRate, &options, &out);
  if (err == SONARE_OK) sonare_free_chord_analysis_result(&out);
  return err;
}

SonareError probe_chord_functional_analysis(const std::string& param, double value) {
  const SonareChordDetectionOptions options = chord_options(param, value);
  SonareChordAnalysisResult chords{};
  SonareStringArray roman{};
  SonareStringArray functions{};
  const SonareError err =
      sonare_chord_functional_analysis(pcm(), pcm_length(), kAnalysisRate, &options, SONARE_PITCH_C,
                                       SONARE_MODE_MAJOR, &chords, &roman, &functions);
  if (err == SONARE_OK) {
    sonare_free_chord_analysis_result(&chords);
    sonare_free_string_array(&roman);
    sonare_free_string_array(&functions);
  }
  return err;
}

SonareError probe_analyze_sections(const std::string& param, double value) {
  SonareSectionOptions options{};
  sonare_section_options_init(&options);
  if (is_param(param, "options.n_fft")) options.n_fft = static_cast<int>(value);
  if (is_param(param, "options.hop_length")) options.hop_length = as_size(value);
  SonareSectionResult out{};
  const SonareError err =
      sonare_analyze_sections(pcm(), pcm_length(), kAnalysisRate, &options, &out);
  if (err == SONARE_OK) sonare_free_section_result(&out);
  return err;
}

SonareError probe_detect_boundaries(const std::string& param, double value) {
  SonareBoundaryOptions options{};
  sonare_boundary_options_init(&options);
  if (is_param(param, "options.n_fft")) options.n_fft = static_cast<int>(value);
  if (is_param(param, "options.hop_length")) options.hop_length = as_size(value);
  if (is_param(param, "options.kernel_size")) options.kernel_size = static_cast<int>(value);
  if (is_param(param, "options.n_mfcc")) options.n_mfcc = static_cast<int>(value);
  if (is_param(param, "options.n_chroma")) options.n_chroma = static_cast<int>(value);
  SonareBoundaryResult out{};
  const SonareError err =
      sonare_detect_boundaries(pcm(), pcm_length(), kAnalysisRate, &options, &out);
  if (err == SONARE_OK) sonare_free_boundary_result(&out);
  return err;
}

SonareMusicAnalyzeOptions music_options(const std::string& param, double value) {
  SonareMusicAnalyzeOptions options{};
  sonare_music_analyze_options_init(&options);
  const int v = static_cast<int>(value);
  if (is_param(param, "options.n_fft")) options.n_fft = v;
  if (is_param(param, "options.hop_length")) options.hop_length = as_size(value);
  if (is_param(param, "options.chroma_hop_multiplier")) options.chroma_hop_multiplier = v;
  if (is_param(param, "options.chord_hmm_beam_width")) options.chord_hmm_beam_width = v;
  if (is_param(param, "options.tempo_update_interval_beats")) {
    options.tempo_update_interval_beats = v;
  }
  if (is_param(param, "options.meter_denominator")) options.meter_denominator = v;
  return options;
}

SonareError probe_analyze_json(const std::string& param, double value) {
  const SonareMusicAnalyzeOptions options = music_options(param, value);
  char* json = nullptr;
  const SonareError err = sonare_analyze_json(pcm(), pcm_length(), kAnalysisRate, &options, &json);
  if (err == SONARE_OK) sonare_free_string(json);
  return err;
}

SonareError probe_estimate_meter_json(const std::string& param, double value) {
  SonareMeterOptions options{};
  sonare_meter_options_init(&options);
  if (is_param(param, "options.denominator")) options.denominator = static_cast<int>(value);
  const float accent[8] = {1.0f, 0.4f, 0.6f, 0.4f, 1.0f, 0.4f, 0.6f, 0.4f};
  SonareMeterBeats beats{};
  sonare_meter_beats_init(&beats);
  beats.accent = accent;
  beats.beat_count = 8;
  char* json = nullptr;
  const SonareError err = sonare_estimate_meter_json(&beats, &options, &json);
  if (err == SONARE_OK) sonare_free_string(json);
  return err;
}

SonareError probe_chroma(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  SonareChromaResult out{};
  const SonareError err =
      sonare_chroma(pcm(), pcm_length(), kAnalysisRate, f.n_fft, f.hop_length, &out);
  if (err == SONARE_OK) sonare_free_chroma_result(&out);
  return err;
}

/// Chroma-over-CQT arguments: the hop, the pitch-class count and the bin grid.
struct ChromaCqt {
  size_t hop_length = 512;
  int n_chroma = 12;
  int bins_per_octave = 36;
};

ChromaCqt chroma_cqt_of(const std::string& param, double value,
                        const std::string& bins_prefix = "") {
  ChromaCqt c;
  if (is_param(param, "hop_length")) c.hop_length = as_size(value);
  if (is_param(param, "n_chroma")) c.n_chroma = static_cast<int>(value);
  if (is_param(param, (bins_prefix + "bins_per_octave").c_str())) {
    c.bins_per_octave = static_cast<int>(value);
  }
  return c;
}

SonareError finish_chroma(SonareError err, SonareChromaResult* out) {
  if (err == SONARE_OK) sonare_free_chroma_result(out);
  return err;
}

SonareError probe_chroma_cqt(const std::string& param, double value) {
  const ChromaCqt c = chroma_cqt_of(param, value, "options.");
  SonareChromaCqtOptions options{};
  sonare_chroma_cqt_options_init(&options);
  options.bins_per_octave = c.bins_per_octave;
  SonareChromaResult out{};
  return finish_chroma(sonare_chroma_cqt(pcm(), pcm_length(), kAnalysisRate, c.hop_length,
                                         c.n_chroma, &options, &out),
                       &out);
}

SonareError probe_chroma_cens(const std::string& param, double value) {
  const ChromaCqt c = chroma_cqt_of(param, value, "options.");
  SonareChromaCensOptions options{};
  sonare_chroma_cens_options_init(&options);
  options.bins_per_octave = c.bins_per_octave;
  SonareChromaResult out{};
  return finish_chroma(sonare_chroma_cens(pcm(), pcm_length(), kAnalysisRate, c.hop_length,
                                          c.n_chroma, &options, &out),
                       &out);
}

SonareError probe_bass_chroma(const std::string& param, double value) {
  const ChromaCqt c = chroma_cqt_of(param, value);
  SonareChromaResult out{};
  return finish_chroma(
      sonare_bass_chroma(pcm(), pcm_length(), kAnalysisRate, c.hop_length, c.n_chroma, &out), &out);
}

/// Constant-Q arguments: the hop, the bin count and the bin grid.
struct Cqt {
  size_t hop_length = 512;
  int n_bins = 48;
  int bins_per_octave = 12;
  float gamma = 0.0f;
};

Cqt cqt_of(const std::string& param, double value) {
  Cqt c;
  if (is_param(param, "hop_length")) c.hop_length = as_size(value);
  if (is_param(param, "n_bins")) c.n_bins = static_cast<int>(value);
  if (is_param(param, "bins_per_octave")) c.bins_per_octave = static_cast<int>(value);
  if (is_param(param, "gamma")) c.gamma = static_cast<float>(value);
  return c;
}

SonareError finish_cqt(SonareError err, SonareCqtResult* out) {
  if (err == SONARE_OK) sonare_free_cqt_result(out);
  return err;
}

SonareError probe_cqt(const std::string& param, double value) {
  const Cqt c = cqt_of(param, value);
  SonareCqtResult out{};
  return finish_cqt(sonare_cqt(pcm(), pcm_length(), kAnalysisRate, c.hop_length, 130.81f, c.n_bins,
                               c.bins_per_octave, 1.0f, &out),
                    &out);
}

SonareError probe_pseudo_cqt(const std::string& param, double value) {
  const Cqt c = cqt_of(param, value);
  SonareCqtResult out{};
  return finish_cqt(sonare_pseudo_cqt(pcm(), pcm_length(), kAnalysisRate, c.hop_length, 130.81f,
                                      c.n_bins, c.bins_per_octave, 1.0f, &out),
                    &out);
}

SonareError probe_hybrid_cqt(const std::string& param, double value) {
  const Cqt c = cqt_of(param, value);
  SonareCqtResult out{};
  return finish_cqt(sonare_hybrid_cqt(pcm(), pcm_length(), kAnalysisRate, c.hop_length, 130.81f,
                                      c.n_bins, c.bins_per_octave, 1.0f, &out),
                    &out);
}

SonareError probe_vqt(const std::string& param, double value) {
  const Cqt c = cqt_of(param, value);
  SonareCqtResult out{};
  return finish_cqt(sonare_vqt(pcm(), pcm_length(), kAnalysisRate, c.hop_length, 130.81f, c.n_bins,
                               c.bins_per_octave, 1.0f, c.gamma, &out),
                    &out);
}

// The CQT/VQT inverses read a magnitude matrix of n_bins * 4 frames.
SonareError finish_floats(SonareError err, float* out) {
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_cqt_to_audio(const std::string& param, double value) {
  const Cqt c = cqt_of(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  const size_t input_length = c.n_bins > 0 ? static_cast<size_t>(c.n_bins) * 4 : 0;
  return finish_floats(
      sonare_cqt_to_audio(planes().data(), input_length, c.n_bins, 4, kAnalysisRate, c.hop_length,
                          130.81f, c.bins_per_octave, 1, &out, &out_length),
      out);
}

SonareError probe_vqt_to_audio(const std::string& param, double value) {
  const Cqt c = cqt_of(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  const size_t input_length = c.n_bins > 0 ? static_cast<size_t>(c.n_bins) * 4 : 0;
  return finish_floats(
      sonare_vqt_to_audio(planes().data(), input_length, c.n_bins, 4, kAnalysisRate, c.hop_length,
                          130.81f, c.bins_per_octave, c.gamma, 1, &out, &out_length),
      out);
}

SonareError probe_stft(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  SonareStftResult out{};
  const SonareError err =
      sonare_stft(pcm(), pcm_length(), kAnalysisRate, f.n_fft, f.hop_length, nullptr, &out);
  if (err == SONARE_OK) sonare_free_stft_result(&out);
  return err;
}

SonareError probe_istft(const std::string& param, double value) {
  int n_fft = 16;
  int n_bins = n_fft / 2 + 1;
  int n_frames = 2;
  size_t hop_length = 4;
  SonareIstftOptions options{};
  sonare_istft_options_init(&options);
  if (is_param(param, "n_fft")) {
    n_fft = static_cast<int>(value);
    n_bins = n_fft / 2 + 1;
    hop_length = static_cast<size_t>(std::max(1, n_fft / 2));
  }
  if (is_param(param, "n_bins")) {
    n_bins = static_cast<int>(value);
    if (n_bins >= 3) {
      n_fft = 2 * (n_bins - 1);
      hop_length = static_cast<size_t>(n_fft / 2);
    }
  }
  if (is_param(param, "n_frames")) n_frames = static_cast<int>(value);
  if (is_param(param, "hop_length")) {
    n_fft = 128;
    n_bins = n_fft / 2 + 1;
    hop_length = as_size(value);
  }
  if (is_param(param, "length_samples")) options.length_samples = as_size(value);

  const std::vector<float> magnitude(4096, 1.0f);
  const std::vector<float> phase(4096, 0.0f);
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err = sonare_istft(magnitude.data(), phase.data(), n_bins, n_frames, n_fft,
                                       hop_length, &options, &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

/// Mel arguments: the framing, the band count and the coefficient count.
struct Mel {
  Frame frame;
  int n_mels = 64;
  int n_mfcc = 13;
};

Mel mel_of(const std::string& param, double value) {
  Mel m;
  m.frame = frame_of(param, value);
  if (is_param(param, "n_mels")) m.n_mels = static_cast<int>(value);
  if (is_param(param, "n_mfcc")) m.n_mfcc = static_cast<int>(value);
  return m;
}

SonareError probe_mel_filterbank(const std::string& param, double value) {
  int sample_rate = kAnalysisRate;
  int n_fft = 512;
  int n_mels = 40;
  if (is_param(param, "sample_rate")) sample_rate = static_cast<int>(value);
  if (is_param(param, "n_fft")) n_fft = static_cast<int>(value);
  if (is_param(param, "n_mels")) n_mels = static_cast<int>(value);
  float* out = nullptr;
  size_t out_length = 0;
  int out_n_mels = 0;
  int out_n_bins = 0;
  const SonareError err = sonare_mel_filterbank(sample_rate, n_fft, n_mels, nullptr, &out,
                                                &out_length, &out_n_mels, &out_n_bins);
  sonare_free_floats(out);
  return err;
}

SonareError probe_mel_frequencies(const std::string& param, double value) {
  const int n_mels = is_param(param, "n_mels") ? static_cast<int>(value) : 40;
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err = sonare_mel_frequencies(n_mels, 0.0f, 8000.0f, 0, &out, &out_length);
  sonare_free_floats(out);
  return err;
}

SonareError probe_mel_spectrogram(const std::string& param, double value) {
  const Mel m = mel_of(param, value);
  SonareMelSpectrogramOptions options{};
  sonare_mel_spectrogram_options_init(&options);
  SonareMelResult out{};
  const SonareError err = sonare_mel_spectrogram(pcm(), pcm_length(), kAnalysisRate, m.frame.n_fft,
                                                 m.frame.hop_length, m.n_mels, &options, &out);
  if (err == SONARE_OK) sonare_free_mel_result(&out);
  return err;
}

SonareError probe_mfcc(const std::string& param, double value) {
  const Mel m = mel_of(param, value);
  SonareMfccOptions options{};
  sonare_mfcc_options_init(&options);
  SonareMfccResult out{};
  const SonareError err = sonare_mfcc(pcm(), pcm_length(), kAnalysisRate, m.frame.n_fft,
                                      m.frame.hop_length, m.n_mels, m.n_mfcc, &options, &out);
  if (err == SONARE_OK) sonare_free_mfcc_result(&out);
  return err;
}

SonareError probe_mel_delta(const std::string& param, double value) {
  int n_features = 4;
  int n_frames = 16;
  int width = 3;
  if (is_param(param, "n_features")) n_features = static_cast<int>(value);
  if (is_param(param, "n_frames")) n_frames = static_cast<int>(value);
  if (is_param(param, "width")) width = static_cast<int>(value);
  float* out = nullptr;
  return finish_floats(sonare_mel_delta(planes().data(), n_features, n_frames, width, &out), out);
}

SonareError probe_onset_strength_multi(const std::string& param, double value) {
  const Mel m = mel_of(param, value);
  int n_bands = 4;
  if (is_param(param, "n_bands")) n_bands = static_cast<int>(value);
  float* out = nullptr;
  size_t out_length = 0;
  int n_frames = 0;
  return finish_floats(sonare_onset_strength_multi(pcm(), pcm_length(), kAnalysisRate,
                                                   m.frame.n_fft, m.frame.hop_length, m.n_mels,
                                                   n_bands, &out, &out_length, &n_frames),
                       out);
}

SonareError probe_spectral_flux(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  int lag = 1;
  if (is_param(param, "lag")) lag = static_cast<int>(value);
  float* out = nullptr;
  size_t count = 0;
  return finish_floats(sonare_spectral_flux(pcm(), pcm_length(), kAnalysisRate, f.n_fft,
                                            f.hop_length, lag, &out, &count),
                       out);
}

SonareError probe_spectral_contrast(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  int n_bands = 6;
  if (is_param(param, "n_bands")) n_bands = static_cast<int>(value);
  float* out = nullptr;
  size_t length = 0;
  int rows = 0;
  int cols = 0;
  return finish_floats(
      sonare_spectral_contrast(pcm(), pcm_length(), kAnalysisRate, f.n_fft, f.hop_length, n_bands,
                               200.0f, 0.02f, &out, &length, &rows, &cols),
      out);
}

SonareError probe_poly_features(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  int order = 1;
  if (is_param(param, "order")) order = static_cast<int>(value);
  float* out = nullptr;
  size_t length = 0;
  int rows = 0;
  int cols = 0;
  return finish_floats(sonare_poly_features(pcm(), pcm_length(), kAnalysisRate, f.n_fft,
                                            f.hop_length, order, &out, &length, &rows, &cols),
                       out);
}

SonareError probe_pitch_tuning(const std::string& param, double value) {
  int bins_per_octave = 12;
  if (is_param(param, "bins_per_octave")) bins_per_octave = static_cast<int>(value);
  const float frequencies[4] = {220.0f, 330.0f, 440.0f, 550.0f};
  float tuning = 0.0f;
  return sonare_pitch_tuning(frequencies, 4, 0.01f, bins_per_octave, &tuning);
}

SonareError probe_estimate_tuning(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  int bins_per_octave = 12;
  if (is_param(param, "bins_per_octave")) bins_per_octave = static_cast<int>(value);
  float tuning = 0.0f;
  return sonare_estimate_tuning(pcm(), pcm_length(), kAnalysisRate, f.n_fft, f.hop_length, 0.01f,
                                bins_per_octave, &tuning);
}

SonareError probe_piptrack(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  size_t length = 0;
  int n_bins = 0;
  int n_frames = 0;
  float* pitches = nullptr;
  float* magnitudes = nullptr;
  const SonareError err =
      sonare_piptrack(pcm(), pcm_length(), kAnalysisRate, f.n_fft, f.hop_length, 150.0f, 4000.0f,
                      0.1f, &pitches, &magnitudes, &length, &n_bins, &n_frames);
  if (err == SONARE_OK) {
    sonare_free_floats(pitches);
    sonare_free_floats(magnitudes);
  }
  return err;
}

/// YIN-style framing: a window and a hop in samples.
struct Tracking {
  size_t frame_length = 2048;
  size_t hop_length = 512;
};

Tracking tracking_of(const std::string& param, double value) {
  Tracking t;
  if (is_param(param, "frame_length")) t.frame_length = as_size(value);
  if (is_param(param, "hop_length")) t.hop_length = as_size(value);
  return t;
}

SonareError probe_pitch_yin(const std::string& param, double value) {
  const Tracking t = tracking_of(param, value);
  SonarePitchResult out{};
  const SonareError err = sonare_pitch_yin(pcm(), pcm_length(), kAnalysisRate, t.frame_length,
                                           t.hop_length, 65.0f, 2093.0f, 0.1f, &out);
  if (err == SONARE_OK) sonare_free_pitch_result(&out);
  return err;
}

SonareError probe_pitch_pyin(const std::string& param, double value) {
  const Tracking t = tracking_of(param, value);
  SonarePitchResult out{};
  const SonareError err = sonare_pitch_pyin(pcm(), pcm_length(), kAnalysisRate, t.frame_length,
                                            t.hop_length, 65.0f, 2093.0f, 0.1f, 0, &out);
  if (err == SONARE_OK) sonare_free_pitch_result(&out);
  return err;
}

SonareError probe_reassigned_spectrogram(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  SonareReassignedSpectrogramResult out{};
  const SonareError err = sonare_reassigned_spectrogram(pcm(), pcm_length(), kAnalysisRate, f.n_fft,
                                                        f.hop_length, 1e-6f, 0, &out);
  if (err == SONARE_OK) sonare_free_reassigned_spectrogram_result(&out);
  return err;
}

/// Tempogram-family arguments: the hop, the window and the rate.
struct Tempo {
  int sample_rate = kAnalysisRate;
  size_t hop_length = 512;
  size_t win_length = 384;
  int n_bins = 12;
};

Tempo tempo_of(const std::string& param, double value) {
  Tempo t;
  if (is_param(param, "sr") || is_param(param, "sample_rate")) {
    t.sample_rate = static_cast<int>(value);
  }
  if (is_param(param, "hop_length")) t.hop_length = as_size(value);
  if (is_param(param, "win_length")) t.win_length = as_size(value);
  if (is_param(param, "n_bins")) t.n_bins = static_cast<int>(value);
  return t;
}

SonareError probe_tempogram(const std::string& param, double value) {
  const Tempo t = tempo_of(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  int n_frames = 0;
  return finish_floats(
      sonare_tempogram(envelope().data(), envelope().size(), t.sample_rate, t.hop_length,
                       t.win_length, 1, 1, SONARE_TEMPOGRAM_AUTOCORRELATION, &out, &out_length,
                       &n_frames),
      out);
}

SonareError probe_fourier_tempogram(const std::string& param, double value) {
  const Tempo t = tempo_of(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  int n_frames = 0;
  return finish_floats(
      sonare_fourier_tempogram(envelope().data(), envelope().size(), t.sample_rate, t.hop_length,
                               t.win_length, 1, 1, &out, &out_length, &n_frames),
      out);
}

SonareError probe_cyclic_tempogram(const std::string& param, double value) {
  const Tempo t = tempo_of(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  int n_frames = 0;
  return finish_floats(
      sonare_cyclic_tempogram(envelope().data(), envelope().size(), t.sample_rate, t.hop_length,
                              t.win_length, 30.0f, t.n_bins, &out, &out_length, &n_frames),
      out);
}

SonareError probe_plp(const std::string& param, double value) {
  const Tempo t = tempo_of(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_floats(sonare_plp(envelope().data(), envelope().size(), t.sample_rate, t.hop_length,
                                  30.0f, 300.0f, t.win_length, &out, &out_length),
                       out);
}

SonareError probe_tempogram_ratio(const std::string& param, double value) {
  const Tempo t = tempo_of(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_floats(
      sonare_tempogram_ratio(envelope().data(), envelope().size(), t.win_length, t.sample_rate,
                             t.hop_length, nullptr, 0, &out, &out_length),
      out);
}

SonareError finish_nnls(SonareError err, float* out) { return finish_floats(err, out); }

SonareError probe_nnls_chroma(const std::string& param, double value) {
  SonareNnlsChromaOptions options{};
  sonare_nnls_chroma_options_init(&options);
  options.stft_blend_weight = 0.5f;
  options.stft_blend_n_fft = 2048;
  if (is_param(param, "options.stft_blend_n_fft"))
    options.stft_blend_n_fft = static_cast<int>(value);
  if (is_param(param, "options.hop_length")) options.hop_length = as_size(value);
  float* out = nullptr;
  size_t out_length = 0;
  int n_frames = 0;
  return finish_nnls(sonare_nnls_chroma(pcm(), pcm_length(), kAnalysisRate, &options, &out,
                                        &out_length, &n_frames),
                     out);
}

SonareError probe_griffin_lim(const std::string& param, double value) {
  int n_fft = 128;
  size_t hop_length = 32;
  if (is_param(param, "n_fft")) n_fft = static_cast<int>(value);
  if (is_param(param, "hop_length")) hop_length = as_size(value);
  const int n_bins = n_fft / 2 + 1;
  const int n_frames = 4;
  float* out = nullptr;
  size_t out_length = 0;
  const size_t input_length = n_bins > 0 ? static_cast<size_t>(n_bins) * n_frames : 0;
  return finish_floats(sonare_griffin_lim(planes().data(), input_length, n_bins, n_frames, n_fft,
                                          hop_length, kAnalysisRate, 1, 0.99f, &out, &out_length),
                       out);
}

SonareError probe_tone(const std::string& param, double value) {
  int sample_rate = kAnalysisRate;
  if (is_param(param, "sample_rate")) sample_rate = static_cast<int>(value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_floats(sonare_tone(440.0f, sample_rate, 0.1f, 0.0f, 1.0f, &out, &out_length), out);
}

SonareError probe_chirp(const std::string& param, double value) {
  int sample_rate = kAnalysisRate;
  if (is_param(param, "sample_rate")) sample_rate = static_cast<int>(value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_floats(sonare_chirp(220.0f, 880.0f, sample_rate, 0.1f, 1, &out, &out_length), out);
}

SonareError probe_clicks(const std::string& param, double value) {
  int sample_rate = kAnalysisRate;
  size_t length = 4096;
  if (is_param(param, "sample_rate")) sample_rate = static_cast<int>(value);
  if (is_param(param, "length")) length = as_size(value);
  const float times[2] = {0.0f, 0.05f};
  float* out = nullptr;
  size_t out_length = 0;
  return finish_floats(
      sonare_clicks(times, 2, sample_rate, length, 1000.0f, 0.01f, &out, &out_length), out);
}

SonareError probe_downmix(const std::string& param, double value) {
  size_t frames = 16;
  size_t num_channels = 2;
  if (is_param(param, "frames")) frames = static_cast<size_t>(value);
  if (is_param(param, "num_channels")) num_channels = static_cast<size_t>(value);
  float* out = nullptr;
  return finish_floats(sonare_downmix(planes().data(), frames, num_channels,
                                      SONARE_CHANNEL_LAYOUT_MONO, nullptr, &out),
                       out);
}

SonareError probe_audio_from_file(const std::string& param, double value) {
  SonareAudioFileOptions options;
  sonare_audio_file_options_init(&options);
  if (is_param(param, "options.channel_index")) options.channel_index = static_cast<int>(value);
  SonareAudio* out = nullptr;
  const SonareError err = sonare_audio_from_file("/nonexistent/probe.wav", &options, &out);
  if (err == SONARE_OK) sonare_audio_destroy(out);
  return err;
}

SonareError probe_chord_functions(const std::string& param, double value) {
  size_t count = 1;
  // A count below zero is the largest size_t, past the domain's upper bound.
  if (is_param(param, "count")) {
    count = value < 0.0 ? std::numeric_limits<size_t>::max() : static_cast<size_t>(value);
  }
  const SonareChord chords[1] = {};
  SonareStringArray roman{};
  SonareStringArray functions{};
  const SonareError err =
      sonare_chord_functions(chords, count, SONARE_PITCH_C, SONARE_MODE_MAJOR, &roman, &functions);
  if (err == SONARE_OK) {
    sonare_free_string_array(&roman);
    sonare_free_string_array(&functions);
  }
  return err;
}

#if defined(SONARE_WITH_PITCH_EDITOR)
SonareError probe_note_segments(const std::string& param, double value) {
  size_t n_frames = 64;
  if (is_param(param, "n_frames")) n_frames = static_cast<size_t>(value);
  const std::vector<float> f0(64, 220.0f);
  const std::vector<float> voiced(64, 1.0f);
  SonareNoteSegmentsResult out{};
  const SonareError err =
      sonare_note_segments(f0.data(), voiced.data(), nullptr, n_frames, 100.0f, nullptr, &out);
  if (err == SONARE_OK) sonare_free_note_segments_result(&out);
  return err;
}
#endif

SonareError finish_spectral(SonareError err, float* out) { return finish_floats(err, out); }

SonareError probe_spectral_centroid(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  float* out = nullptr;
  size_t count = 0;
  return finish_spectral(sonare_spectral_centroid(pcm(), pcm_length(), kAnalysisRate, f.n_fft,
                                                  f.hop_length, &out, &count),
                         out);
}

SonareError probe_spectral_bandwidth(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  SonareSpectralBandwidthOptions options{};
  sonare_spectral_bandwidth_options_init(&options);
  float* out = nullptr;
  size_t count = 0;
  return finish_spectral(sonare_spectral_bandwidth(pcm(), pcm_length(), kAnalysisRate, f.n_fft,
                                                   f.hop_length, &options, &out, &count),
                         out);
}

SonareError probe_spectral_rolloff(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  float* out = nullptr;
  size_t count = 0;
  return finish_spectral(sonare_spectral_rolloff(pcm(), pcm_length(), kAnalysisRate, f.n_fft,
                                                 f.hop_length, 0.85f, &out, &count),
                         out);
}

SonareError probe_spectral_flatness(const std::string& param, double value) {
  const Frame f = frame_of(param, value);
  float* out = nullptr;
  size_t count = 0;
  return finish_spectral(sonare_spectral_flatness(pcm(), pcm_length(), kAnalysisRate, f.n_fft,
                                                  f.hop_length, &out, &count),
                         out);
}

SonareError probe_zero_crossing_rate(const std::string& param, double value) {
  const Tracking t = tracking_of(param, value);
  float* out = nullptr;
  size_t count = 0;
  return finish_spectral(sonare_zero_crossing_rate(pcm(), pcm_length(), kAnalysisRate,
                                                   t.frame_length, t.hop_length, &out, &count),
                         out);
}

SonareError probe_rms_energy(const std::string& param, double value) {
  const Tracking t = tracking_of(param, value);
  float* out = nullptr;
  size_t count = 0;
  return finish_spectral(sonare_rms_energy(pcm(), pcm_length(), kAnalysisRate, t.frame_length,
                                           t.hop_length, &out, &count),
                         out);
}

SonareError probe_onset_strength(const std::string& param, double value) {
  const Mel m = mel_of(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_spectral(sonare_onset_strength(pcm(), pcm_length(), kAnalysisRate, m.frame.n_fft,
                                               m.frame.hop_length, m.n_mels, &out, &out_length),
                         out);
}

// The conversions return a value, so the refusal is read back from the last error.
SonareError probe_frames_to_time(const std::string& param, double value) {
  int sr = kAnalysisRate;
  size_t hop_length = 512;
  if (is_param(param, "sr")) sr = static_cast<int>(value);
  if (is_param(param, "hop_length")) hop_length = as_size(value);
  (void)sonare_frames_to_time(10, sr, hop_length);
  return sonare_last_error_code();
}

SonareError probe_time_to_frames(const std::string& param, double value) {
  int sr = kAnalysisRate;
  size_t hop_length = 512;
  if (is_param(param, "sr")) sr = static_cast<int>(value);
  if (is_param(param, "hop_length")) hop_length = as_size(value);
  (void)sonare_time_to_frames(1.0f, sr, hop_length);
  return sonare_last_error_code();
}

#if defined(SONARE_WITH_PITCH_EDITOR)
SonareError probe_polyphonic_analyze(const std::string& param, double value) {
  SonarePolyphonicConfig config{};
  sonare_polyphonic_config_init(&config);
  if (is_param(param, "config.n_fft")) config.n_fft = static_cast<int32_t>(value);
  if (is_param(param, "config.hop_length")) config.hop_length = as_size(value);
  if (is_param(param, "config.win_length")) config.win_length = as_size(value);
  if (is_param(param, "config.max_refine_hz")) config.max_refine_hz = static_cast<float>(value);
  if (is_param(param, "config.reattack_ratio")) config.reattack_ratio = static_cast<float>(value);
  SonarePolyphonicAnalysis* out = nullptr;
  const SonareError err =
      sonare_polyphonic_analyze(pcm(), pcm_length(), kAnalysisRate, &config, &out);
  if (err == SONARE_OK) sonare_polyphonic_analysis_destroy(out);
  return err;
}
#endif

#if defined(SONARE_WITH_ARRANGEMENT)
SonareError probe_align_take_to_reference(const std::string& param, double value) {
  SonareTakeAlignConfig config{};
  sonare_take_align_config_init(&config);
  if (is_param(param, "config.hop_length")) config.hop_length = static_cast<int32_t>(value);
  if (is_param(param, "config.bins_per_octave")) {
    config.bins_per_octave = static_cast<int32_t>(value);
  }
  SonareProjectWarpAnchor* anchors = nullptr;
  size_t count = 0;
  SonareTakeAlignment alignment{};
  const SonareError err =
      sonare_align_take_to_reference(pcm(), pcm_length(), kAnalysisRate, pcm(), pcm_length(),
                                     kAnalysisRate, &config, &anchors, &count, &alignment);
  if (err == SONARE_OK) sonare_free_project_warp_anchors(anchors);
  return err;
}

SonareProjectTempoOptions tempo_options(const std::string& param, double value) {
  SonareProjectTempoOptions options{};
  sonare_project_tempo_options_init(&options);
  if (is_param(param, "options.tempo_update_interval_beats")) {
    options.tempo_update_interval_beats = static_cast<int>(value);
  }
  return options;
}

SonareError probe_project_analyze_tempo(const std::string& param, double value) {
  SonareProject* project = nullptr;
  if (sonare_project_create(&project) != SONARE_OK) return SONARE_ERROR_INVALID_STATE;
  const SonareProjectTempoOptions options = tempo_options(param, value);
  SonareProjectTempoCandidate candidates[SONARE_PROJECT_MAX_TEMPO_CANDIDATES] = {};
  size_t count = 0;
  const SonareError err =
      sonare_project_analyze_tempo(project, pcm(), pcm_length(), kAnalysisRate, &options,
                                   candidates, SONARE_PROJECT_MAX_TEMPO_CANDIDATES, &count);
  sonare_project_destroy(project);
  return err;
}

SonareError probe_project_auto_tempo(const std::string& param, double value) {
  SonareProject* project = nullptr;
  if (sonare_project_create(&project) != SONARE_OK) return SONARE_ERROR_INVALID_STATE;
  const SonareProjectTempoOptions options = tempo_options(param, value);
  float bpm = 0.0f;
  const SonareError err =
      sonare_project_auto_tempo(project, pcm(), pcm_length(), kAnalysisRate, &options, 0, 0, &bpm);
  sonare_project_destroy(project);
  return err;
}
#endif

const std::map<std::string, Probe>& probes() {
  static const std::map<std::string, Probe> table = {
      {"analyze_dynamics", probe_dynamics},
      {"analyze_impulse_response", probe_impulse_response},
      {"analyze_melody", probe_melody},
      {"analyze_timbre", probe_timbre},
      {"detect_acoustic", probe_detect_acoustic},
      {"analyze_bpm", probe_analyze_bpm},
      {"analyze_rhythm", probe_analyze_rhythm},
      {"detect_key", probe_detect_key},
      {"detect_key_candidates", probe_detect_key_candidates},
      {"detect_chords", probe_detect_chords},
      {"chord_functional_analysis", probe_chord_functional_analysis},
      {"analyze_sections", probe_analyze_sections},
      {"detect_boundaries", probe_detect_boundaries},
      {"analyze_json", probe_analyze_json},
      {"estimate_meter_json", probe_estimate_meter_json},
      {"chroma", probe_chroma},
      {"chroma_cqt", probe_chroma_cqt},
      {"chroma_cens", probe_chroma_cens},
      {"bass_chroma", probe_bass_chroma},
      {"cqt", probe_cqt},
      {"pseudo_cqt", probe_pseudo_cqt},
      {"hybrid_cqt", probe_hybrid_cqt},
      {"vqt", probe_vqt},
      {"cqt_to_audio", probe_cqt_to_audio},
      {"vqt_to_audio", probe_vqt_to_audio},
      {"stft", probe_stft},
      {"istft", probe_istft},
      {"mel_filterbank", probe_mel_filterbank},
      {"mel_frequencies", probe_mel_frequencies},
      {"mel_spectrogram", probe_mel_spectrogram},
      {"mfcc", probe_mfcc},
      {"mel_delta", probe_mel_delta},
      {"onset_strength_multi", probe_onset_strength_multi},
      {"spectral_flux", probe_spectral_flux},
      {"spectral_contrast", probe_spectral_contrast},
      {"poly_features", probe_poly_features},
      {"pitch_tuning", probe_pitch_tuning},
      {"estimate_tuning", probe_estimate_tuning},
      {"piptrack", probe_piptrack},
      {"pitch_yin", probe_pitch_yin},
      {"pitch_pyin", probe_pitch_pyin},
      {"reassigned_spectrogram", probe_reassigned_spectrogram},
      {"tempogram", probe_tempogram},
      {"fourier_tempogram", probe_fourier_tempogram},
      {"cyclic_tempogram", probe_cyclic_tempogram},
      {"plp", probe_plp},
      {"tempogram_ratio", probe_tempogram_ratio},
      {"nnls_chroma", probe_nnls_chroma},
      {"griffin_lim", probe_griffin_lim},
      {"tone", probe_tone},
      {"chirp", probe_chirp},
      {"clicks", probe_clicks},
      {"downmix", probe_downmix},
      {"audio_from_file", probe_audio_from_file},
      {"chord_functions", probe_chord_functions},
#if defined(SONARE_WITH_PITCH_EDITOR)
      {"note_segments", probe_note_segments},
#endif
      {"spectral_centroid", probe_spectral_centroid},
      {"spectral_bandwidth", probe_spectral_bandwidth},
      {"spectral_rolloff", probe_spectral_rolloff},
      {"spectral_flatness", probe_spectral_flatness},
      {"zero_crossing_rate", probe_zero_crossing_rate},
      {"rms_energy", probe_rms_energy},
      {"onset_strength", probe_onset_strength},
      {"frames_to_time", probe_frames_to_time},
      {"time_to_frames", probe_time_to_frames},
#if defined(SONARE_WITH_PITCH_EDITOR)
      {"polyphonic_analyze", probe_polyphonic_analyze},
#endif
#if defined(SONARE_WITH_ARRANGEMENT)
      {"align_take_to_reference", probe_align_take_to_reference},
      {"project_analyze_tempo", probe_project_analyze_tempo},
      {"project_auto_tempo", probe_project_auto_tempo},
#endif
  };
  return table;
}

/// An analyzer runs on a count of one only degenerately, so an integer whose domain starts at one
/// is probed at 32; a bin grid folded onto twelve pitch classes takes 12, and a window the
/// pitch trackers frame takes the default length.
double analysis_inside_value(const NumericDomainRow& row) {
  const std::string entry = row.entry;
  const std::string param = row.param;
  if ((param == "options.bins_per_octave" && entry.rfind("chroma", 0) == 0) ||
      param == "config.bins_per_octave") {
    return 12.0;
  }
  if (param == "frame_length") return 2048.0;
  if (row.domain.kind == NumberKind::Integer && *row.domain.lo == 1.0) return 32.0;
  if (row.stricter != nullptr && std::string(row.stricter) == "even" && *row.domain.lo < 32.0) {
    return 32.0;
  }
  return lowest_inside_value(row);
}

/// The rows whose entry this build compiles; the pitch-editor and arrangement entries are gated.
std::vector<NumericDomainRow> built_rows() {
  std::vector<NumericDomainRow> rows;
  for (const NumericDomainRow& row : kAnalysisDomainRows) {
    if (probes().count(row.entry) != 0) rows.push_back(row);
  }
  return rows;
}

NumericDomainTable built_table() {
  static const std::vector<NumericDomainRow> rows = built_rows();
  return {"analysis", rows.data(), rows.size()};
}

}  // namespace

TEST_CASE("the analysis numeric-domain rows are sorted", "[numeric_domains][c_api]") {
  require_sorted(numeric_domain_table("analysis", kAnalysisDomainRows));
}

TEST_CASE("an analysis value outside its row's domain is refused under the row's subject",
          "[numeric_domains][c_api]") {
  require_outside_refused(built_table(), probes());
}

TEST_CASE("an analysis value inside its row's domain is not refused under the row's subject",
          "[numeric_domains][c_api]") {
  require_inside_accepted(built_table(), probes(), analysis_inside_value);
}

TEST_CASE("ISTFT keeps the Hann COLA hop refusal on the n_fft and hop_length pair",
          "[numeric_domains][c_api]") {
  constexpr int n_fft = 16;
  constexpr int n_bins = n_fft / 2 + 1;
  const std::vector<float> magnitude(static_cast<size_t>(n_bins) * 2, 1.0f);
  const std::vector<float> phase(magnitude.size(), 0.0f);
  SonareIstftOptions options{};
  sonare_istft_options_init(&options);
  float* out = nullptr;
  size_t out_length = 0;

  CHECK(sonare_istft(magnitude.data(), phase.data(), n_bins, 2, n_fft, n_fft / 2 + 1, &options,
                     &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_last_refusal_subject_count() == 2);
  CHECK(last_refusal_has_subject("n_fft"));
  CHECK(last_refusal_has_subject("hop_length"));
  CHECK(std::string(sonare_last_error_message()).find("hop_length must be at most n_fft / 2") !=
        std::string::npos);
}
