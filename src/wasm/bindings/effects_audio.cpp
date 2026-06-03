/// @file effects_audio.cpp
/// @brief Embind bindings for offline audio effects APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

// ============================================================================
// Effects
// ============================================================================

// HPSS - Harmonic/Percussive Source Separation
val js_hpss(val samples, int sample_rate, int kernel_harmonic, int kernel_percussive) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  HpssConfig config;
  config.kernel_size_harmonic = kernel_harmonic;
  config.kernel_size_percussive = kernel_percussive;

  HpssAudioResult result = hpss(audio, config);

  val out = val::object();

  // Harmonic audio
  std::vector<float> harmonic_vec(result.harmonic.data(),
                                  result.harmonic.data() + result.harmonic.size());
  out.set("harmonic", vectorToFloat32Array(harmonic_vec));

  // Percussive audio
  std::vector<float> percussive_vec(result.percussive.data(),
                                    result.percussive.data() + result.percussive.size());
  out.set("percussive", vectorToFloat32Array(percussive_vec));

  out.set("sampleRate", result.harmonic.sample_rate());

  return out;
}

// Get harmonic component only
val js_harmonic(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio result = harmonic(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Get percussive component only
val js_percussive(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio result = percussive(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Time stretch
val js_time_stretch(val samples, int sample_rate, float rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio result = time_stretch(audio, rate);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Pitch shift
val js_pitch_shift(val samples, int sample_rate, float semitones) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio result = pitch_shift(audio, semitones);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_pitch_correct_to_midi(val samples, int sample_rate, float current_midi, float target_midi) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  editing::pitch_editor::PitchCorrector corrector;
  editing::pitch_editor::F0Track track;
  track.sample_rate = sample_rate;
  track.hop_length = 512;
  track.f0_hz = {editing::pitch_editor::PitchCorrector::midi_to_hz(current_midi)};
  track.voiced = {true};
  track.voiced_prob = {1.0f};
  Audio result = corrector.correct_to_midi(audio, track, target_midi);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_note_stretch(val samples, int sample_rate, int onset_sample, int offset_sample,
                    float stretch_ratio) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  editing::pitch_editor::NoteRegion region;
  region.onset_sample = onset_sample;
  region.offset_sample = offset_sample;
  editing::pitch_editor::NoteEditor editor;
  Audio result = editor.stretch_note(audio, region, stretch_ratio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_voice_change(val samples, int sample_rate, float pitch_semitones, float formant_factor) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  editing::voice_changer::VoiceChangerConfig config;
  config.pitch_semitones = pitch_semitones;
  config.formant_factor = formant_factor;
  editing::voice_changer::VoiceChanger changer(config);
  Audio result = changer.process(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// NMF decomposition of a non-negative spectrogram. Mirrors the C ABI
// sonare_decompose / librosa.decompose.decompose. Returns the two factor
// matrices as { w, h }: w is [n_features x n_components] row-major and h is
// [n_components x n_frames] row-major (both flat Float32Array buffers).
val js_decompose(val s, int n_features, int n_frames, int n_components, int n_iter, float beta) {
  std::vector<float> data = float32ArrayToVector(s);
  DecomposeResult result =
      decompose(data.data(), n_features, n_frames, n_components, n_iter, "mu", beta);

  val out = val::object();
  out.set("w", vectorToFloat32Array(result.W));
  out.set("h", vectorToFloat32Array(result.H));
  return out;
}

// NMF decomposition with a selectable initialiser. Mirrors the C ABI
// sonare_decompose_with_init / librosa.decompose.decompose (init). Identical to
// js_decompose but exposes the initialisation strategy: "random" (default,
// deterministic seed) or "nndsvd" (SVD-based warm start). Returns { w, h }.
val js_decompose_with_init(val s, int n_features, int n_frames, int n_components, int n_iter,
                           float beta, std::string init) {
  std::vector<float> data = float32ArrayToVector(s);
  if (init.empty()) init = "random";
  DecomposeResult result =
      decompose(data.data(), n_features, n_frames, n_components, n_iter, "mu", beta, init);

  val out = val::object();
  out.set("w", vectorToFloat32Array(result.W));
  out.set("h", vectorToFloat32Array(result.H));
  return out;
}

// Nearest-neighbour spectrogram filter. Mirrors the C ABI sonare_nn_filter /
// librosa.decompose.nn_filter. Returns the smoothed spectrogram
// [n_features x n_frames] as { data, rows, cols }.
val js_nn_filter(val s, int n_features, int n_frames, std::string aggregate, int k, int width) {
  std::vector<float> data = float32ArrayToVector(s);
  if (aggregate.empty()) aggregate = "mean";
  std::vector<float> filtered = nn_filter(data.data(), n_features, n_frames, aggregate, k, width);

  val out = val::object();
  out.set("data", vectorToFloat32Array(filtered));
  out.set("rows", n_features);
  out.set("cols", n_frames);
  return out;
}

// Time-domain remix: reorders / concatenates a signal by (start, end) interval
// slices. Mirrors the C ABI sonare_remix / librosa.effects.remix. @p intervals
// is a flat Int32Array of (start, end) pairs.
val js_remix(val samples, val intervals, int sample_rate, bool align_zeros) {
  std::vector<float> data = float32ArrayToVector(samples);
  // Sample indices must survive as exact integers: converting through float32
  // would round any boundary above 2^24 (16,777,216) and silently misalign the
  // slice. Read the Int32Array straight into int32 storage instead.
  std::vector<int32_t> interval_ints = int32ArrayToVector(intervals);
  if (interval_ints.size() % 2 != 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "remix intervals must be (start, end) pairs");
  }
  std::vector<std::pair<int, int>> pairs;
  pairs.reserve(interval_ints.size() / 2);
  for (size_t i = 0; i + 1 < interval_ints.size(); i += 2) {
    pairs.emplace_back(static_cast<int>(interval_ints[i]), static_cast<int>(interval_ints[i + 1]));
  }
  // sample_rate is validated for API symmetry with the C ABI but not consumed
  // by the time-domain remix itself.
  (void)sample_rate;
  std::vector<float> remixed = remix(data.data(), data.size(), pairs, align_zeros);
  return vectorToFloat32Array(remixed);
}

// HPSS with residual: separates audio into harmonic, percussive and residual
// signals (residual = original - harmonic - percussive). Mirrors the C ABI
// sonare_hpss_with_residual. Returns { harmonic, percussive, residual,
// sampleRate } where all three buffers share the same length and sample rate.
val js_hpss_with_residual(val samples, int sample_rate, int kernel_harmonic,
                          int kernel_percussive) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  HpssConfig config;
  config.kernel_size_harmonic = kernel_harmonic;
  config.kernel_size_percussive = kernel_percussive;

  HpssAudioResultWithResidual result = hpss_with_residual(audio, config);

  std::vector<float> harmonic_vec(result.harmonic.data(),
                                  result.harmonic.data() + result.harmonic.size());
  std::vector<float> percussive_vec(result.percussive.data(),
                                    result.percussive.data() + result.percussive.size());
  std::vector<float> residual_vec(result.residual.data(),
                                  result.residual.data() + result.residual.size());

  val out = val::object();
  out.set("harmonic", vectorToFloat32Array(harmonic_vec));
  out.set("percussive", vectorToFloat32Array(percussive_vec));
  out.set("residual", vectorToFloat32Array(residual_vec));
  out.set("sampleRate", result.harmonic.sample_rate());
  return out;
}

// Phase-vocoder time-scale modification (STFT -> phase_vocoder -> iSTFT).
// Mirrors the C ABI sonare_phase_vocoder. rate < 1.0 = slower, > 1.0 = faster.
val js_phase_vocoder(val samples, int sample_rate, float rate, int n_fft, int hop_length) {
  // Guard the time-scale rate before deriving the output length: a non-finite
  // or non-positive rate yields an opaque embind exception, and a tiny rate
  // would request an enormous output buffer. Mirrors the C ABI rate > 0 check
  // (sonare_phase_vocoder) and adds a sane upper bound.
  if (!std::isfinite(rate) || rate <= 0.0f || rate > 100.0f) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "phaseVocoder: rate must be finite and within (0, 100]");
  }
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = hop_length;
  Spectrogram stretched = phase_vocoder(spec, rate, pv_config);

  const int expected_length = static_cast<int>(std::ceil(static_cast<float>(audio.size()) / rate));
  Audio result = stretched.to_audio(expected_length);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Normalize
val js_normalize(val samples, int sample_rate, float target_db) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio result = normalize(audio, target_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Trim silence
val js_trim(val samples, int sample_rate, float threshold_db) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio result = trim_absolute(audio, threshold_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

void registerEffectsAudioBindings() {
  function("hpss", &js_hpss);
  function("harmonic", &js_harmonic);
  function("percussive", &js_percussive);
  function("timeStretch", &js_time_stretch);
  function("pitchShift", &js_pitch_shift);
  function("pitchCorrectToMidi", &js_pitch_correct_to_midi);
  function("noteStretch", &js_note_stretch);
  function("voiceChange", &js_voice_change);
  function("decompose", &js_decompose);
  function("decomposeWithInit", &js_decompose_with_init);
  function("nnFilter", &js_nn_filter);
  function("remix", &js_remix);
  function("hpssWithResidual", &js_hpss_with_residual);
  function("phaseVocoder", &js_phase_vocoder);
  function("normalize", &js_normalize);
  function("trim", &js_trim);
}

#endif  // __EMSCRIPTEN__
