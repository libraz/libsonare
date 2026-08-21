/// @file effects_audio.cpp
/// @brief Embind bindings for offline audio effects APIs.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/constants.h"
#include "wasm/bindings/common/common.h"

// ============================================================================
// Effects
// ============================================================================

// HPSS - Harmonic/Percussive Source Separation
val js_hpss_ex(val samples, int sample_rate, int kernel_harmonic, int kernel_percussive, int n_fft,
               int hop_length, bool hard_mask) {
  Audio audio = loadValidatedAudio(samples, sample_rate);

  HpssConfig config;
  config.kernel_size_harmonic = kernel_harmonic;
  config.kernel_size_percussive = kernel_percussive;
  config.use_soft_mask = !hard_mask;

  StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;

  HpssAudioResult result = hpss(audio, config, stft_config);

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

val js_hpss(val samples, int sample_rate, int kernel_harmonic, int kernel_percussive) {
  return js_hpss_ex(samples, sample_rate, kernel_harmonic, kernel_percussive,
                    constants::kDefaultNFft, constants::kDefaultHopLength, false);
}

// Get harmonic component only
val js_harmonic(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  Audio result = harmonic(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Get percussive component only
val js_percussive(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  Audio result = percussive(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Time stretch
val js_time_stretch_ex(val samples, int sample_rate, float rate, int n_fft, int hop_length) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  TimeStretchConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.backend = StretchBackend::NativeSpectral;
  Audio result = time_stretch(audio, rate, config);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_time_stretch(val samples, int sample_rate, float rate) {
  return js_time_stretch_ex(samples, sample_rate, rate, constants::kDefaultNFft,
                            constants::kDefaultHopLength);
}

// Pitch shift
val js_pitch_shift_ex(val samples, int sample_rate, float semitones, int n_fft, int hop_length) {
  PitchShiftPlan plan;
  if (!make_pitch_shift_plan(samples["length"].as<size_t>(), sample_rate, semitones, &plan)) {
    throw SonareException(ErrorCode::InvalidParameter, "unsupported pitch-shift expansion");
  }
  Audio audio = loadValidatedAudio(samples, sample_rate);
  PitchShiftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.backend = StretchBackend::NativeSpectral;
  Audio result = pitch_shift(audio, semitones, config);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_pitch_shift(val samples, int sample_rate, float semitones) {
  return js_pitch_shift_ex(samples, sample_rate, semitones, constants::kDefaultNFft,
                           constants::kDefaultHopLength);
}

// Pitch-editor bindings (pitch-correct / note stretch / note move).
//
// The C ABI gates its pitch-editor entry points behind
// `#if defined(SONARE_WITH_PITCH_EDITOR)` and returns a NOT_SUPPORTED stub when
// the feature is compiled out. These WASM bindings deliberately call the
// editing::pitch_editor core unconditionally: the top-level CMake forces
// BUILD_PITCH_EDITOR ON for every configuration (it is a hard dependency of the
// C editing API), so the feature is always linked and the `#else` stub branch is
// unreachable on this surface — adding the guard here would compile the stub and
// break the binding rather than mirror the C ABI.
val js_pitch_correct_to_midi(val samples, int sample_rate, float current_midi, float target_midi) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  editing::pitch_editor::PitchCorrector corrector;
  Audio result = corrector.correct_to_midi(audio, current_midi, target_midi);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Per-frame ("time-varying") correction toward target_midi following a
// caller-supplied F0 contour. f0_hz is required; voiced / voiced_prob are
// optional (undefined/null -> every frame voiced). Companion arrays are passed
// as Float32Array (voiced uses 0.0/1.0) so a single conversion path suffices.
val js_pitch_correct_to_midi_timevarying(val samples, int sample_rate, val f0_hz, float target_midi,
                                         int hop_length, val voiced, val voiced_prob) {
  const bool has_voiced = !voiced.isUndefined() && !voiced.isNull();
  const bool has_prob = !voiced_prob.isUndefined() && !voiced_prob.isNull();
  std::size_t cumulative_count = 0;
  accumulateWasmFloat32ArrayLength(samples, "samples", "pitchCorrectToMidiTimevarying input",
                                   &cumulative_count);
  accumulateWasmFloat32ArrayLength(f0_hz, "f0Hz", "pitchCorrectToMidiTimevarying input",
                                   &cumulative_count);
  if (has_voiced) {
    accumulateWasmFloat32ArrayLength(voiced, "voiced", "pitchCorrectToMidiTimevarying input",
                                     &cumulative_count);
  }
  if (has_prob) {
    accumulateWasmFloat32ArrayLength(voiced_prob, "voicedProb",
                                     "pitchCorrectToMidiTimevarying input", &cumulative_count);
  }
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<float> f0 = float32ArrayToVector(f0_hz);
  const size_t n_frames = f0.size();
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

  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  editing::pitch_editor::PitchCorrector corrector;
  Audio result = corrector.correct_to_midi_timevarying(audio, track, target_midi);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_pitch_correct_timevarying(val samples, int sample_rate, val f0_hz, int hop_length,
                                 val options) {
  val voiced = val::undefined();
  val voiced_prob = val::undefined();
  bool has_voiced = false;
  bool has_prob = false;
  if (!options.isUndefined() && !options.isNull()) {
    voiced = options["voiced"];
    voiced_prob = options["voicedProb"];
    has_voiced = !voiced.isUndefined() && !voiced.isNull();
    has_prob = !voiced_prob.isUndefined() && !voiced_prob.isNull();
  }
  std::size_t cumulative_count = 0;
  accumulateWasmFloat32ArrayLength(samples, "samples", "pitchCorrectTimevarying input",
                                   &cumulative_count);
  accumulateWasmFloat32ArrayLength(f0_hz, "f0Hz", "pitchCorrectTimevarying input",
                                   &cumulative_count);
  if (has_voiced) {
    accumulateWasmFloat32ArrayLength(voiced, "voiced", "pitchCorrectTimevarying input",
                                     &cumulative_count);
  }
  if (has_prob) {
    accumulateWasmFloat32ArrayLength(voiced_prob, "voicedProb", "pitchCorrectTimevarying input",
                                     &cumulative_count);
  }
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<float> f0 = float32ArrayToVector(f0_hz);
  const size_t n_frames = f0.size();

  editing::pitch_editor::PitchCorrectionConfig config{};
  bool scale_mode = false;
  float target_midi = constants::kMidiA4;
  std::vector<float> voiced_vec;
  std::vector<float> prob_vec;
  if (!options.isUndefined() && !options.isNull()) {
    if (hasProperty(options, "mode")) {
      const val mode_value = options["mode"];
      if (mode_value.typeOf().as<std::string>() != "string") {
        throw SonareException(ErrorCode::InvalidParameter,
                              "pitch correction mode must be 'midi' or 'scale'");
      }
      const std::string mode = mode_value.as<std::string>();
      if (mode == "scale") {
        scale_mode = true;
      } else if (mode != "midi") {
        throw SonareException(ErrorCode::InvalidParameter, "unknown pitch correction mode");
      }
    }
    target_midi = floatProperty(options, "targetMidi", target_midi);
    config.scale.root = intProperty(options, "scaleRoot", config.scale.root);
    const int scale_mode_mask =
        intProperty(options, "scaleModeMask", static_cast<int>(config.scale.mode_mask));
    if (scale_mode_mask < 0 || scale_mode_mask > 0x0FFF) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "scaleModeMask must be a non-zero 12-bit mask");
    }
    config.scale.mode_mask = static_cast<uint16_t>(scale_mode_mask);
    config.scale.reference_midi =
        floatProperty(options, "referenceMidi", config.scale.reference_midi);
    config.retune_amount = floatProperty(options, "retuneAmount", config.retune_amount);
    config.max_correction_semitones =
        floatProperty(options, "maxCorrectionSemitones", config.max_correction_semitones);
    config.retune_speed_ms = floatProperty(options, "retuneSpeedMs", config.retune_speed_ms);
    config.vibrato_threshold_cents =
        floatProperty(options, "vibratoThresholdCents", config.vibrato_threshold_cents);
    if (has_voiced) voiced_vec = float32ArrayToVector(voiced);
    if (has_prob) prob_vec = float32ArrayToVector(voiced_prob);
  }
  if ((has_voiced && voiced_vec.size() != n_frames) || (has_prob && prob_vec.size() != n_frames)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "voiced and voicedProb must match f0Hz length");
  }
  if (!scale_mode && (!std::isfinite(target_midi) || target_midi < 0.0f || target_midi > 127.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "targetMidi must be finite and in [0, 127]");
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

  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  editing::pitch_editor::PitchCorrector corrector(config);
  Audio result = scale_mode ? corrector.correct_to_scale_timevarying(audio, track)
                            : corrector.correct_to_midi_timevarying(audio, track, target_midi);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_note_stretch(val samples, int sample_rate, int onset_sample, int offset_sample,
                    float stretch_ratio) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  editing::pitch_editor::NoteRegion region;
  region.onset_sample = onset_sample;
  region.offset_sample = offset_sample;
  editing::pitch_editor::NoteEditor editor;
  Audio result = editor.stretch_note(audio, region, stretch_ratio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_note_move(val samples, int sample_rate, int onset_sample, int offset_sample,
                 int target_onset_sample) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  editing::pitch_editor::NoteRegion region;
  region.onset_sample = onset_sample;
  region.offset_sample = offset_sample;
  editing::pitch_editor::NoteEditor editor;
  Audio result = editor.move_note(audio, region, target_onset_sample);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_voice_change(val samples, int sample_rate, float pitch_semitones, float formant_factor) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  editing::voice_changer::VoiceChangerConfig config;
  config.pitch_semitones = pitch_semitones;
  config.formant_factor = formant_factor;
  editing::voice_changer::VoiceChanger changer(config);
  Audio result = changer.process(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_voice_change_realtime(val samples, int sample_rate, std::string preset, int channels) {
  std::vector<float> input = float32ArrayToVector(samples);
  float* output = nullptr;
  size_t output_length = 0;
  const SonareError err = sonare_voice_change_realtime(
      input.data(), input.size(), sample_rate, preset.c_str(), channels, &output, &output_length);
  if (err != SONARE_OK) {
    sonare_free_floats(output);
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string("voiceChangeRealtime failed: ") + sonare_error_message(err));
  }
  std::vector<float> result(output, output + output_length);
  sonare_free_floats(output);
  return vectorToFloat32Array(result);
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

// Phase-carrying NMF separation. Mirrors the C ABI sonare_decompose_stems.
// Unlike decompose(), which returns W/H factors of a magnitude spectrogram,
// this applies a per-component soft mask to the ORIGINAL complex spectrogram,
// so every returned component keeps the source's phase and is directly
// listenable. Returns { components: Float32Array[], w, h }.
val js_decompose_stems(val samples, int sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  DecomposeStemsConfig config;
  // 0 is the "use the built-in default" sentinel the C ABI documents on
  // SonareDecomposeStemsConfig, so an explicit 0 must land on the same
  // effective value here as it does through sonare_decompose_stems. Reading
  // each field with 0 as its fallback lets an absent key and an explicit 0
  // take that one path. Everything else is left to validate_config, which
  // decompose_stems applies to the config it is handed, so a negative count or
  // a sub-unit mask power is rejected with the same error the C ABI returns.
  const int n_components = intProperty(options, "nComponents", 0);
  const int n_fft = intProperty(options, "nFft", 0);
  const int hop_length = intProperty(options, "hopLength", 0);
  const int n_iter = intProperty(options, "nIter", 0);
  const float beta = floatProperty(options, "beta", 0.0f);
  const float mask_power = floatProperty(options, "maskPower", 0.0f);
  const std::string init = stringProperty(options, "init", config.init);
  // Rejected before the sentinel promotion, exactly as the C ABI does: a
  // negative value must not be quietly swallowed by the "0 or less keeps the
  // default" rule that follows.
  if (n_components < 0 || n_fft < 0 || hop_length < 0 || n_iter < 0 || !std::isfinite(beta) ||
      !std::isfinite(mask_power) || mask_power < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "decomposeStems: counts must not be negative, beta and maskPower must "
                          "be finite, and maskPower must not be negative");
  }
  if (n_components > 0) config.n_components = n_components;
  if (n_fft > 0) config.n_fft = n_fft;
  if (hop_length > 0) config.hop_length = hop_length;
  if (n_iter > 0) config.n_iter = n_iter;
  if (beta != 0.0f) config.beta = beta;
  if (mask_power > 0.0f) config.mask_power = mask_power;
  config.init = init.empty() ? std::string("random") : init;

  DecomposeStemsResult result = decompose_stems(audio.data(), audio.size(), sample_rate, config);
  val components = val::array();
  for (const std::vector<float>& component : result.components) {
    components.call<void>("push", vectorToFloat32Array(component));
  }
  val out = val::object();
  out.set("components", components);
  out.set("w", vectorToFloat32Array(result.W));
  out.set("h", vectorToFloat32Array(result.H));
  out.set("sampleRate", sample_rate);
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
  // Validate finite samples, non-empty input, and the sample-rate range up front
  // so js_remix rejects exactly what the C ABI's run_offline (sonare_remix) does,
  // rather than copying NaN/Inf through or silently accepting a bad rate.
  Audio audio = loadValidatedAudio(samples, sample_rate);
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
  std::vector<float> remixed = remix(audio.data(), audio.size(), pairs, align_zeros);
  return vectorToFloat32Array(remixed);
}

// Resolves the cut points remix() would use, without cutting. Mirrors the C ABI
// sonare_remix_aligned_intervals. Returns a flat Int32Array of (start, end)
// pairs so a host can apply ONE cut set to every channel of a multichannel
// take; calling remix() per channel snaps each channel independently.
val js_remix_aligned_intervals(val samples, val intervals, int sample_rate, bool align_zeros) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
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
  const std::vector<std::pair<int, int>> resolved =
      align_remix_intervals(audio.data(), audio.size(), pairs, align_zeros);
  std::vector<int> flat;
  flat.reserve(resolved.size() * 2);
  for (const auto& pair : resolved) {
    flat.push_back(pair.first);
    flat.push_back(pair.second);
  }
  return vectorToInt32Array(flat);
}

// HPSS with residual: separates audio into harmonic, percussive and residual
// signals (residual = original - harmonic - percussive). Mirrors the C ABI
// sonare_hpss_with_residual. Returns { harmonic, percussive, residual,
// sampleRate } where all three buffers share the same length and sample rate.
val js_hpss_with_residual_ex(val samples, int sample_rate, int kernel_harmonic,
                             int kernel_percussive, int n_fft, int hop_length, bool hard_mask) {
  Audio audio = loadValidatedAudio(samples, sample_rate);

  HpssConfig config;
  config.kernel_size_harmonic = kernel_harmonic;
  config.kernel_size_percussive = kernel_percussive;
  config.use_soft_mask = !hard_mask;

  StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;

  HpssAudioResultWithResidual result = hpss_with_residual(audio, config, stft_config);

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

val js_hpss_with_residual(val samples, int sample_rate, int kernel_harmonic,
                          int kernel_percussive) {
  return js_hpss_with_residual_ex(samples, sample_rate, kernel_harmonic, kernel_percussive,
                                  constants::kDefaultNFft, constants::kDefaultHopLength, false);
}

// Phase-vocoder time-scale modification (STFT -> phase_vocoder -> iSTFT).
// Mirrors the C ABI sonare_phase_vocoder. rate < 1.0 = slower, > 1.0 = faster.
val js_phase_vocoder(val samples, int sample_rate, float rate, int n_fft, int hop_length) {
  // Guard the time-scale rate before deriving the output length: a non-finite
  // rate would request an enormous (Inf) or garbage (NaN) output buffer, and a
  // non-positive rate is rejected. Mirrors the C ABI rate > 0 check
  // (sonare_phase_vocoder); the finite guard is a WASM-heap safeguard that never
  // rejects a valid finite rate, so no upper cap is imposed on fast rates.
  if (!std::isfinite(rate) || rate <= 0.0f) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "phaseVocoder: rate must be a finite positive number");
  }
  Audio audio = loadValidatedAudio(samples, sample_rate);

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
val js_normalize_ex(val samples, int sample_rate, float target_db, const std::string& mode) {
  if (mode != "peak" && mode != "rms") {
    throw SonareException(ErrorCode::InvalidParameter, "normalize: mode must be 'peak' or 'rms'");
  }
  Audio audio = loadValidatedAudio(samples, sample_rate);
  Audio result =
      mode == "rms" ? normalize_rms(audio, target_db, true) : normalize(audio, target_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_normalize(val samples, int sample_rate, float target_db) {
  return js_normalize_ex(samples, sample_rate, target_db, "peak");
}

// Trim silence
val js_trim_ex(val samples, int sample_rate, float threshold_db, int frame_length, int hop_length) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  Audio result = trim_absolute(audio, threshold_db, frame_length, hop_length);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_trim(val samples, int sample_rate, float threshold_db) {
  return js_trim_ex(samples, sample_rate, threshold_db, constants::kDefaultNFft,
                    constants::kDefaultHopLength);
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
  if (s == "rectangular" || s == "rect") return WindowType::Rectangular;
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
  Audio audio = loadValidatedAudio(samples, sample_rate);

  SpectralEditConfig config;
  if (!options.isUndefined() && !options.isNull()) {
    // Mirror the C-ABI oracle (sonare_spectral_edit): a value of 0 means "keep
    // the core default" rather than forcing an invalid 0 into the core, which
    // requires n_fft/hop_length >= 1 and heal_radius_frames >= 1. Previously the
    // WASM path passed 0 through verbatim and threw where Node/Python succeeded.
    if (hasProperty(options, "nFft")) {
      const int n_fft = options["nFft"].as<int>();
      if (n_fft != 0) config.n_fft = n_fft;
    }
    if (hasProperty(options, "hopLength")) {
      const int hop_length = options["hopLength"].as<int>();
      if (hop_length != 0) config.hop_length = hop_length;
    }
    if (hasProperty(options, "window")) {
      config.window = parseSpectralEditWindow(options["window"]);
    }
    if (hasProperty(options, "healRadiusFrames")) {
      const int heal_radius = options["healRadiusFrames"].as<int>();
      if (heal_radius != 0) config.heal_radius_frames = heal_radius;
    }
  }

  std::vector<SpectralRegionOp> region_ops;
  if (!ops.isUndefined() && !ops.isNull()) {
    // The op count comes from an untrusted JS `.length`: validate it through the
    // shared safe-integer + budget guard, and cap the pre-reserve so a
    // fabricated length cannot allocate storage the array does not back. A
    // longer genuine array still works — the vector grows as ops are read.
    const std::size_t n = wasmArrayLikeLength(ops, "spectral edit ops");
    region_ops.reserve(std::min(n, kMaxWasmObjectArrayReserve));
    for (std::size_t i = 0; i < n; ++i) {
      val op = ops[i];
      SpectralRegionOp region;
      // An omitted endSample defaults to the whole signal (matches the Node
      // facade, sonare_wrap_effects.cpp). The core defaults end_sample to 0,
      // which would otherwise make an omitted endSample a silent no-op here while
      // Node processes the full region.
      region.end_sample = static_cast<int64_t>(audio.size());
      // Sample positions arrive as plain JS numbers; read as double and cast to
      // int64 (mirrors project.cpp's totalFrames) so callers need not pass BigInt.
      if (hasProperty(op, "startSample")) {
        region.start_sample = static_cast<int64_t>(op["startSample"].as<double>());
      }
      if (hasProperty(op, "endSample")) {
        region.end_sample = static_cast<int64_t>(op["endSample"].as<double>());
      }
      if (hasProperty(op, "lowHz")) region.low_hz = op["lowHz"].as<float>();
      if (hasProperty(op, "highHz")) region.high_hz = op["highHz"].as<float>();
      if (hasProperty(op, "gainDb")) region.gain_db = op["gainDb"].as<float>();
      region.mode =
          hasProperty(op, "mode") ? parseSpectralEditMode(op["mode"]) : SpectralEditMode::Gain;
      region_ops.push_back(region);
    }
  }

  Audio result = spectral_edit(audio, config, region_ops.data(), region_ops.size());
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

void registerEffectsAudioBindings() {
  function("hpss", &js_hpss);
  function("hpssEx", &js_hpss_ex);
  function("harmonic", &js_harmonic);
  function("percussive", &js_percussive);
  function("timeStretch", &js_time_stretch);
  function("timeStretchEx", &js_time_stretch_ex);
  function("pitchShift", &js_pitch_shift);
  function("pitchShiftEx", &js_pitch_shift_ex);
  function("pitchCorrectToMidi", &js_pitch_correct_to_midi);
  function("pitchCorrectToMidiTimevarying", &js_pitch_correct_to_midi_timevarying);
  function("pitchCorrectTimevarying", &js_pitch_correct_timevarying);
  function("noteStretch", &js_note_stretch);
  function("noteMove", &js_note_move);
  function("voiceChange", &js_voice_change);
  function("voiceChangeRealtime", &js_voice_change_realtime);
  function("decompose", &js_decompose);
  function("decomposeWithInit", &js_decompose_with_init);
  function("decomposeStems", &js_decompose_stems);
  function("nnFilter", &js_nn_filter);
  function("remix", &js_remix);
  function("remixAlignedIntervals", &js_remix_aligned_intervals);
  function("hpssWithResidual", &js_hpss_with_residual);
  function("hpssWithResidualEx", &js_hpss_with_residual_ex);
  function("phaseVocoder", &js_phase_vocoder);
  function("normalize", &js_normalize);
  function("normalizeEx", &js_normalize_ex);
  function("trim", &js_trim);
  function("trimEx", &js_trim_ex);
  function("spectralEdit", &js_spectral_edit);
}

#endif  // __EMSCRIPTEN__
