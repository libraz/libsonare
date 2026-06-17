/// @file effects_audio.cpp
/// @brief Embind bindings for offline audio effects APIs.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <limits>

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

// Per-frame ("time-varying") correction toward target_midi following a
// caller-supplied F0 contour. f0_hz is required; voiced / voiced_prob are
// optional (undefined/null -> every frame voiced). Companion arrays are passed
// as Float32Array (voiced uses 0.0/1.0) so a single conversion path suffices.
val js_pitch_correct_to_midi_timevarying(val samples, int sample_rate, val f0_hz, float target_midi,
                                         int hop_length, val voiced, val voiced_prob) {
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<float> f0 = float32ArrayToVector(f0_hz);
  const size_t n_frames = f0.size();
  const bool has_voiced = !voiced.isUndefined() && !voiced.isNull();
  const bool has_prob = !voiced_prob.isUndefined() && !voiced_prob.isNull();
  std::vector<float> voiced_vec = has_voiced ? float32ArrayToVector(voiced) : std::vector<float>{};
  std::vector<float> prob_vec = has_prob ? float32ArrayToVector(voiced_prob) : std::vector<float>{};
  if ((has_voiced && voiced_vec.size() != n_frames) || (has_prob && prob_vec.size() != n_frames)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "voiced and voicedProb must match f0Hz length");
  }

  editing::pitch_editor::F0Track track;
  track.sample_rate = sample_rate;
  track.hop_length = hop_length;
  track.f0_hz = f0;
  track.voiced.resize(n_frames);
  track.voiced_prob.resize(n_frames);
  for (size_t i = 0; i < n_frames; ++i) {
    const bool is_voiced = has_voiced ? (voiced_vec[i] != 0.0f) : true;
    track.voiced[i] = is_voiced;
    track.voiced_prob[i] = has_prob ? prob_vec[i] : (is_voiced ? 1.0f : 0.0f);
  }

  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  editing::pitch_editor::PitchCorrector corrector;
  Audio result = corrector.correct_to_midi_timevarying(audio, track, target_midi);
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
  if (n_components <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "n_components must be positive");
  }
  if (n_iter <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "n_iter must be positive");
  }
  if (n_features <= 0 || n_frames <= 0 ||
      static_cast<size_t>(n_features) >
          std::numeric_limits<size_t>::max() / static_cast<size_t>(std::max(1, n_frames)) ||
      static_cast<size_t>(n_features) * static_cast<size_t>(n_frames) > data.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "spectrogram dimensions exceed input");
  }
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
  if (n_components <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "n_components must be positive");
  }
  if (n_iter <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "n_iter must be positive");
  }
  if (n_features <= 0 || n_frames <= 0 ||
      static_cast<size_t>(n_features) >
          std::numeric_limits<size_t>::max() / static_cast<size_t>(std::max(1, n_frames)) ||
      static_cast<size_t>(n_features) * static_cast<size_t>(n_frames) > data.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "spectrogram dimensions exceed input");
  }
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
  if (n_features <= 0 || n_frames <= 0 ||
      static_cast<size_t>(n_features) >
          std::numeric_limits<size_t>::max() / static_cast<size_t>(std::max(1, n_frames)) ||
      static_cast<size_t>(n_features) * static_cast<size_t>(n_frames) > data.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "spectrogram dimensions exceed input");
  }
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

namespace {

// Map a spectral-edit mode string ('gain'|'attenuate'|'mute'|'heal') to the
// SpectralEditMode enum. Defaults to Gain when the value is absent.
SpectralEditMode parseSpectralEditMode(val mode) {
  if (mode.isUndefined() || mode.isNull()) return SpectralEditMode::Gain;
  std::string s = mode.as<std::string>();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "gain") return SpectralEditMode::Gain;
  if (s == "attenuate") return SpectralEditMode::Attenuate;
  if (s == "mute") return SpectralEditMode::Mute;
  if (s == "heal") return SpectralEditMode::Heal;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "spectralEdit: unknown mode: " + s);
}

// Map a window string ('hann'|'hamming'|'blackman'|'rectangular') to WindowType.
WindowType parseSpectralEditWindow(val window) {
  std::string s = window.as<std::string>();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "hann") return WindowType::Hann;
  if (s == "hamming") return WindowType::Hamming;
  if (s == "blackman") return WindowType::Blackman;
  if (s == "rectangular") return WindowType::Rectangular;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "spectralEdit: unknown window: " + s);
}

}  // namespace

// Region-based spectral editing (STFT -> per-op bin/frame masking -> iSTFT).
// Mirrors the core sonare::spectral_edit. @p ops is a JS array of region objects
// { startSample, endSample, lowHz, highHz, gainDb, mode } and @p options is an
// optional config bag { nFft, hopLength, window, healRadiusFrames }. Returns the
// edited audio (same length/sample rate as the input) as a Float32Array.
val js_spectral_edit(val samples, int sample_rate, val ops, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  SpectralEditConfig config;
  if (!options.isUndefined() && !options.isNull()) {
    // Mirror the C-ABI oracle (sonare_spectral_edit): a value of 0 means "keep
    // the core default" rather than forcing an invalid 0 into the core, which
    // requires n_fft/hop_length >= 1 and heal_radius_frames >= 1. Previously the
    // WASM path passed 0 through verbatim and threw where Node/Python succeeded.
    if (options.hasOwnProperty("nFft")) {
      const int n_fft = options["nFft"].as<int>();
      if (n_fft != 0) config.n_fft = n_fft;
    }
    if (options.hasOwnProperty("hopLength")) {
      const int hop_length = options["hopLength"].as<int>();
      if (hop_length != 0) config.hop_length = hop_length;
    }
    if (options.hasOwnProperty("window")) {
      config.window = parseSpectralEditWindow(options["window"]);
    }
    if (options.hasOwnProperty("healRadiusFrames")) {
      const int heal_radius = options["healRadiusFrames"].as<int>();
      if (heal_radius != 0) config.heal_radius_frames = heal_radius;
    }
  }

  std::vector<SpectralRegionOp> region_ops;
  if (!ops.isUndefined() && !ops.isNull()) {
    const uint32_t n = ops["length"].as<uint32_t>();
    region_ops.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
      val op = ops[i];
      SpectralRegionOp region;
      // An omitted endSample defaults to the whole signal (matches the Node
      // facade, sonare_wrap_effects.cpp). The core defaults end_sample to 0,
      // which would otherwise make an omitted endSample a silent no-op here while
      // Node processes the full region.
      region.end_sample = static_cast<int64_t>(data.size());
      // Sample positions arrive as plain JS numbers; read as double and cast to
      // int64 (mirrors project.cpp's totalFrames) so callers need not pass BigInt.
      if (op.hasOwnProperty("startSample")) {
        region.start_sample = static_cast<int64_t>(op["startSample"].as<double>());
      }
      if (op.hasOwnProperty("endSample")) {
        region.end_sample = static_cast<int64_t>(op["endSample"].as<double>());
      }
      if (op.hasOwnProperty("lowHz")) region.low_hz = op["lowHz"].as<float>();
      if (op.hasOwnProperty("highHz")) region.high_hz = op["highHz"].as<float>();
      if (op.hasOwnProperty("gainDb")) region.gain_db = op["gainDb"].as<float>();
      region.mode =
          op.hasOwnProperty("mode") ? parseSpectralEditMode(op["mode"]) : SpectralEditMode::Gain;
      region_ops.push_back(region);
    }
  }

  Audio result = spectral_edit(audio, config, region_ops.data(), region_ops.size());
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
  function("pitchCorrectToMidiTimevarying", &js_pitch_correct_to_midi_timevarying);
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
  function("spectralEdit", &js_spectral_edit);
}

#endif  // __EMSCRIPTEN__
