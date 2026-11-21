/// @file audio.cpp
/// @brief Embind bindings for offline audio effects APIs.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <cmath>
#include <limits>

#include "analysis/key_profiles.h"
#include "c_api/sonare_c_error_mapping.h"
#include "editing/pitch_editor/auto_tune.h"
#include "editing/pitch_editor/f0_provider.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/note_segmenter.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "util/constants.h"
#include "wasm/bindings/common/common.h"

// ============================================================================
// Effects
// ============================================================================

// HPSS - Harmonic/Percussive Source Separation
val js_hpss_ex(val samples, const val& sample_rate, const val& kernel_harmonic,
               const val& kernel_percussive, const val& n_fft, const val& hop_length,
               bool hard_mask) {
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  HpssConfig config;
  config.kernel_size_harmonic = checkedIntFromVal(kernel_harmonic, "kernelHarmonic");
  config.kernel_size_percussive = checkedIntFromVal(kernel_percussive, "kernelPercussive");
  config.use_soft_mask = !hard_mask;

  StftConfig stft_config;
  stft_config.n_fft = checkedIntFromVal(n_fft, "nFft");
  stft_config.hop_length = resynthesisHopFromVal(hop_length);

  HpssAudioResult result = hpss(audio, config, stft_config);

  val out = val::object();

  // Harmonic audio
  out.set("harmonic", vectorToFloat32Array(result.harmonic.data(), result.harmonic.size()));

  // Percussive audio
  out.set("percussive", vectorToFloat32Array(result.percussive.data(), result.percussive.size()));

  out.set("sampleRate", result.harmonic.sample_rate());

  return out;
}

val js_hpss(val samples, const val& sample_rate, const val& kernel_harmonic,
            const val& kernel_percussive) {
  return js_hpss_ex(samples, sample_rate, kernel_harmonic, kernel_percussive,
                    val(constants::kDefaultNFft), val(constants::kDefaultHopLength), false);
}

// Get harmonic component only
val js_harmonic(val samples, const val& sample_rate) {
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));
  Audio result = harmonic(audio);
  return vectorToFloat32Array(result.data(), result.size());
}

// Get percussive component only
val js_percussive(val samples, const val& sample_rate) {
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));
  Audio result = percussive(audio);
  return vectorToFloat32Array(result.data(), result.size());
}

// Time stretch
val js_time_stretch_ex(val samples, const val& sample_rate, const val& rate_val, const val& n_fft,
                       const val& hop_length) {
  const float rate = checkedFloatFromVal(rate_val, "speedRatio");
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));
  TimeStretchConfig config;
  config.n_fft = checkedIntFromVal(n_fft, "nFft");
  config.hop_length = resynthesisHopFromVal(hop_length);
  config.backend = StretchBackend::NativeSpectral;
  Audio result = time_stretch(audio, rate, config);
  return vectorToFloat32Array(result.data(), result.size());
}

val js_time_stretch(val samples, const val& sample_rate, const val& rate) {
  return js_time_stretch_ex(samples, sample_rate, rate, val(constants::kDefaultNFft),
                            val(constants::kDefaultHopLength));
}

// Pitch shift
val js_pitch_shift_ex(val samples, const val& sample_rate_val, const val& semitones_val,
                      const val& n_fft, const val& hop_length) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const float semitones = checkedFloatFromVal(semitones_val, "semitones");
  PitchShiftPlan plan;
  if (!make_pitch_shift_plan(samples["length"].as<size_t>(), sample_rate, semitones, &plan)) {
    throw WasmRangeError("unsupported pitch-shift expansion");
  }
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  PitchShiftConfig config;
  config.n_fft = checkedIntFromVal(n_fft, "nFft");
  config.hop_length = resynthesisHopFromVal(hop_length);
  config.backend = StretchBackend::NativeSpectral;
  Audio result = pitch_shift(audio, semitones, config);
  return vectorToFloat32Array(result.data(), result.size());
}

val js_pitch_shift(val samples, const val& sample_rate, const val& semitones) {
  return js_pitch_shift_ex(samples, sample_rate, semitones, val(constants::kDefaultNFft),
                           val(constants::kDefaultHopLength));
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
val js_pitch_correct_to_midi(val samples, const val& sample_rate, const val& current_midi_val,
                             const val& target_midi_val) {
  const float current_midi = checkedFloatFromVal(current_midi_val, "currentMidi");
  const float target_midi = checkedFloatFromVal(target_midi_val, "targetMidi");
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));
  editing::pitch_editor::PitchCorrector corrector;
  Audio result = corrector.correct_to_midi(audio, current_midi, target_midi);
  return vectorToFloat32Array(result.data(), result.size());
}

// Per-frame ("time-varying") correction toward target_midi following a
// caller-supplied F0 contour. f0_hz is required; voiced / voiced_prob are
// optional (undefined/null -> every frame voiced). Companion arrays are passed
// as Float32Array (voiced uses 0.0/1.0) so a single conversion path suffices.
namespace {

// voiced / voicedProb are already checked against f0's length.
editing::pitch_editor::F0Track buildF0Track(int sample_rate, int hop_length,
                                            const std::vector<float>& f0, bool has_voiced,
                                            const std::vector<float>& voiced_vec, bool has_prob,
                                            const std::vector<float>& prob_vec) {
  const size_t n_frames = f0.size();
  editing::pitch_editor::F0Track track;
  track.sample_rate = sample_rate;
  track.hop_length = hop_length;
  track.f0_hz = f0;
  track.voiced.resize(n_frames);
  track.voiced_prob.resize(n_frames);
  for (size_t i = 0; i < n_frames; ++i) {
    // Absent both voiced and voicedProb, every frame defaults to voiced,
    // matching the C ABI's is_voiced_frame fallback chain.
    const bool is_voiced =
        has_voiced ? (voiced_vec[i] != 0.0f) : (has_prob ? (prob_vec[i] >= 0.5f) : true);
    track.voiced[i] = is_voiced;
    track.voiced_prob[i] = has_prob ? prob_vec[i] : (is_voiced ? 1.0f : 0.0f);
  }
  return track;
}

}  // namespace

val js_pitch_correct_to_midi_timevarying(val samples, const val& sample_rate_val, val f0_hz,
                                         const val& target_midi_val, const val& hop_length_val,
                                         val voiced, val voiced_prob) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const float target_midi = checkedFloatFromVal(target_midi_val, "targetMidi");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const bool has_voiced = !voiced.isUndefined() && !voiced.isNull();
  // Explicit voiced flags own the decision; do not inspect or copy the
  // companion probability array when they are present.
  const bool has_prob = !has_voiced && !voiced_prob.isUndefined() && !voiced_prob.isNull();
  std::size_t cumulative_count = 0;
  accumulateWasmFloat32ArrayLength(samples, "samples", &cumulative_count);
  accumulateWasmFloat32ArrayLength(f0_hz, "f0Hz", &cumulative_count);
  if (has_voiced) {
    accumulateWasmFloat32ArrayLength(voiced, "voiced", &cumulative_count);
  }
  if (has_prob) {
    accumulateWasmFloat32ArrayLength(voiced_prob, "voicedProb", &cumulative_count);
  }
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<float> f0 = float32ArrayToVector(f0_hz);
  const size_t n_frames = f0.size();
  std::vector<float> voiced_vec = has_voiced ? float32ArrayToVector(voiced) : std::vector<float>{};
  std::vector<float> prob_vec = has_prob ? float32ArrayToVector(voiced_prob) : std::vector<float>{};
  if (has_voiced && voiced_vec.size() != n_frames) {
    throw WasmRangeError("voiced must have the same length as f0Hz");
  }
  if (has_prob && prob_vec.size() != n_frames) {
    throw WasmRangeError("voicedProb must have the same length as f0Hz");
  }

  const editing::pitch_editor::F0Track track =
      buildF0Track(sample_rate, hop_length, f0, has_voiced, voiced_vec, has_prob, prob_vec);

  validate_offline_audio_input("samples", data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  editing::pitch_editor::PitchCorrector corrector;
  Audio result = corrector.correct_to_midi_timevarying(audio, track, target_midi);
  return vectorToFloat32Array(result.data(), result.size());
}

val js_pitch_correct_timevarying(val samples, const val& sample_rate_val, val f0_hz,
                                 const val& hop_length_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  val voiced = val::undefined();
  val voiced_prob = val::undefined();
  bool has_voiced = false;
  bool has_prob = false;
  if (!options.isUndefined() && !options.isNull()) {
    voiced = options["voiced"];
    voiced_prob = options["voicedProb"];
    has_voiced = !voiced.isUndefined() && !voiced.isNull();
    has_prob = !has_voiced && !voiced_prob.isUndefined() && !voiced_prob.isNull();
  }
  std::size_t cumulative_count = 0;
  accumulateWasmFloat32ArrayLength(samples, "samples", &cumulative_count);
  accumulateWasmFloat32ArrayLength(f0_hz, "f0Hz", &cumulative_count);
  if (has_voiced) {
    accumulateWasmFloat32ArrayLength(voiced, "voiced", &cumulative_count);
  }
  if (has_prob) {
    accumulateWasmFloat32ArrayLength(voiced_prob, "voicedProb", &cumulative_count);
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
    if (hasProperty(options, "targetMode")) {
      const val mode_value = options["targetMode"];
      if (mode_value.typeOf().as<std::string>() != "string") {
        throw WasmTypeError("targetMode must be one of 'midi', 'scale' (a name, not a number)");
      }
      const std::string mode = mode_value.as<std::string>();
      if (mode == "scale") {
        scale_mode = true;
      } else if (mode != "midi") {
        throw WasmRangeError("targetMode must be one of 'midi', 'scale', got '" + mode + "'");
      }
    }
    target_midi = floatProperty(options, "targetMidi", target_midi);
    if (hasProperty(options, "scaleRoot")) {
      config.scale.root = domainNumberFromVal<int>(options["scaleRoot"], "scaleRoot",
                                                   editing::pitch_editor::kScaleRootDomain);
    }
    const val mask_value =
        hasProperty(options, "scaleModeMask") ? options["scaleModeMask"] : val::undefined();
    if (mask_value.isString()) {
      config.scale.mode_mask = scale_mask_for_mode(modeFromVal(mask_value, "scaleModeMask"));
    } else if (hasProperty(options, "scaleModeMask")) {
      config.scale.mode_mask = domainNumberFromVal<uint16_t>(
          mask_value, "scaleModeMask", editing::pitch_editor::kScaleModeMaskDomain);
    }
    config.scale.reference_midi =
        floatProperty(options, "scaleReferenceMidi", config.scale.reference_midi);
    config.retune_amount = floatProperty(options, "retuneAmount", config.retune_amount);
    config.max_correction_semitones =
        floatProperty(options, "maxCorrectionSemitones", config.max_correction_semitones);
    config.retune_speed_ms = floatProperty(options, "retuneSpeedMs", config.retune_speed_ms);
    config.vibrato_threshold_cents =
        floatProperty(options, "vibratoThresholdCents", config.vibrato_threshold_cents);
    if (has_voiced) voiced_vec = float32ArrayToVector(voiced);
    if (has_prob) prob_vec = float32ArrayToVector(voiced_prob);
  }
  if (has_voiced && voiced_vec.size() != n_frames) {
    throw WasmRangeError("voiced must have the same length as f0Hz");
  }
  if (has_prob && prob_vec.size() != n_frames) {
    throw WasmRangeError("voicedProb must have the same length as f0Hz");
  }
  if (!scale_mode && (!std::isfinite(target_midi) || target_midi < 0.0f || target_midi > 127.0f)) {
    throw WasmRangeError("targetMidi must be finite and in [0, 127]");
  }
  const editing::pitch_editor::F0Track track =
      buildF0Track(sample_rate, hop_length, f0, has_voiced, voiced_vec, has_prob, prob_vec);

  validate_offline_audio_input("samples", data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  editing::pitch_editor::PitchCorrector corrector(config);
  Audio result = scale_mode ? corrector.correct_to_scale_timevarying(audio, track)
                            : corrector.correct_to_midi_timevarying(audio, track, target_midi);
  return vectorToFloat32Array(result.data(), result.size());
}

val js_auto_tune(val samples, const val& sample_rate_val, const val& key_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::optional<Key> named;
  if (!key_val.isUndefined() && !key_val.isNull()) {
    const int root = checkedIntFromVal(key_val["root"], "key.root");
    if (root < static_cast<int>(PitchClass::C) || root > static_cast<int>(PitchClass::B)) {
      throw WasmRangeError("key.root must be in [0, 11]");
    }
    named = Key{static_cast<PitchClass>(root), modeFromVal(key_val["mode"], "key.mode"), 1.0f};
  }

  editing::pitch_editor::PitchCorrectionConfig config{};
  if (!options.isUndefined() && !options.isNull()) {
    config.retune_amount = floatProperty(options, "retuneAmount", config.retune_amount);
    config.scale.reference_midi =
        floatProperty(options, "scaleReferenceMidi", config.scale.reference_midi);
    config.max_correction_semitones =
        floatProperty(options, "maxCorrectionSemitones", config.max_correction_semitones);
    config.retune_speed_ms = floatProperty(options, "retuneSpeedMs", config.retune_speed_ms);
    config.vibrato_threshold_cents =
        floatProperty(options, "vibratoThresholdCents", config.vibrato_threshold_cents);
  }

  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  const editing::pitch_editor::AutoTuneResult result =
      editing::pitch_editor::auto_tune(audio, named, config);
  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.audio.data(), result.audio.size()));
  val key = val::object();
  key.set("root", static_cast<int>(result.key.root));
  key.set("mode", static_cast<int>(result.key.mode));
  key.set("confidence", result.key.confidence);
  key.set("name", result.key.to_string());
  key.set("shortName", result.key.to_short_string());
  out.set("key", key);
  return out;
}

val js_note_stretch(val samples, const val& sample_rate, const val& onset_sample,
                    const val& offset_sample, const val& stretch_ratio_val) {
  const float stretch_ratio = checkedFloatFromVal(stretch_ratio_val, "stretchRatio");
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));
  editing::pitch_editor::NoteRegion region;
  region.onset_sample = checkedIntFromVal(onset_sample, "onsetSample");
  region.offset_sample = checkedIntFromVal(offset_sample, "offsetSample");
  editing::pitch_editor::NoteEditor editor;
  Audio result = editor.stretch_note(audio, region, stretch_ratio);
  return vectorToFloat32Array(result.data(), result.size());
}

val js_note_move(val samples, const val& sample_rate, const val& onset_sample,
                 const val& offset_sample, const val& target_onset_sample) {
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));
  editing::pitch_editor::NoteRegion region;
  region.onset_sample = checkedIntFromVal(onset_sample, "onsetSample");
  region.offset_sample = checkedIntFromVal(offset_sample, "offsetSample");
  editing::pitch_editor::NoteEditor editor;
  Audio result =
      editor.move_note(audio, region, checkedIntFromVal(target_onset_sample, "targetOnsetSample"));
  return vectorToFloat32Array(result.data(), result.size());
}

val js_voice_change(val samples, const val& sample_rate, const val& pitch_semitones_val,
                    const val& formant_factor_val, const std::string& formant_mode) {
  const float pitch_semitones = checkedFloatFromVal(pitch_semitones_val, "pitchSemitones");
  const float formant_factor = checkedFloatFromVal(formant_factor_val, "formantFactor");
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));
  editing::voice_changer::VoiceChangerConfig config;
  config.pitch_semitones = pitch_semitones;
  config.formant_factor = formant_factor;
  config.formant_mode = editing::voice_changer::parse_formant_mode(formant_mode);
  editing::voice_changer::VoiceChanger changer(config);
  const Audio result = changer.process(audio);
  return vectorToFloat32Array(result.data(), result.size());
}

val js_voice_change_realtime(val samples, const val& sample_rate, std::string preset,
                             const val& channels) {
  const int rate = checkedIntFromVal(sample_rate, "sampleRate");
  const int channel_count = checkedIntFromVal(channels, "channels");
  std::vector<float> input = float32ArrayToVector(samples);
  float* output = nullptr;
  size_t output_length = 0;
  const SonareError err =
      sonare_voice_change_realtime(input.data(), input.size(), rate, wasmCString(preset, "preset"),
                                   channel_count, &output, &output_length);
  if (err != SONARE_OK) {
    sonare_free_floats(output);
    // The C code and its recorded detail, as on Node, so a host can branch on `err.code`.
    throwCodedCError(err);
  }
  std::vector<float> result(output, output + output_length);
  sonare_free_floats(output);
  return vectorToFloat32Array(result);
}

// HPSS with residual: separates audio into harmonic, percussive and residual
// signals (residual = original - harmonic - percussive). Mirrors the C ABI
// sonare_hpss_with_residual. Returns { harmonic, percussive, residual,
// sampleRate } where all three buffers share the same length and sample rate.
val js_hpss_with_residual_ex(val samples, const val& sample_rate, const val& kernel_harmonic,
                             const val& kernel_percussive, const val& n_fft, const val& hop_length,
                             bool hard_mask) {
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  HpssConfig config;
  config.kernel_size_harmonic = checkedIntFromVal(kernel_harmonic, "kernelHarmonic");
  config.kernel_size_percussive = checkedIntFromVal(kernel_percussive, "kernelPercussive");
  config.use_soft_mask = !hard_mask;

  StftConfig stft_config;
  stft_config.n_fft = checkedIntFromVal(n_fft, "nFft");
  stft_config.hop_length = resynthesisHopFromVal(hop_length);

  HpssAudioResultWithResidual result = hpss_with_residual(audio, config, stft_config);

  val out = val::object();
  out.set("harmonic", vectorToFloat32Array(result.harmonic.data(), result.harmonic.size()));
  out.set("percussive", vectorToFloat32Array(result.percussive.data(), result.percussive.size()));
  out.set("residual", vectorToFloat32Array(result.residual.data(), result.residual.size()));
  out.set("sampleRate", result.harmonic.sample_rate());
  return out;
}

val js_hpss_with_residual(val samples, const val& sample_rate, const val& kernel_harmonic,
                          const val& kernel_percussive) {
  return js_hpss_with_residual_ex(samples, sample_rate, kernel_harmonic, kernel_percussive,
                                  val(constants::kDefaultNFft), val(constants::kDefaultHopLength),
                                  false);
}

// Phase-vocoder time-scale modification (STFT -> phase_vocoder -> iSTFT).
// Mirrors the C ABI sonare_phase_vocoder. rate < 1.0 = slower, > 1.0 = faster.
val js_phase_vocoder(val samples, const val& sample_rate_val, const val& rate_val,
                     const val& n_fft_val, const val& hop_length_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const float rate = checkedFloatFromVal(rate_val, "speedRatio");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  // An automatic hop runs a quarter of nFft, as the C entry resolves it.
  const int hop_length = resolve_resynthesis_hop(resynthesisHopFromVal(hop_length_val), n_fft);
  // Guard the time-scale rate before deriving the output length. Mirrors the C
  // ABI rate > 0 check (sonare_phase_vocoder); no upper cap is imposed on fast
  // rates.
  if (rate <= 0.0f) {
    throw WasmRangeError("rate must be greater than 0");
  }
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);

  StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = hop_length;
  Spectrogram stretched = phase_vocoder(spec, rate, pv_config);

  const int expected_length = static_cast<int>(std::ceil(static_cast<float>(audio.size()) / rate));
  Audio result = stretched.to_audio(expected_length);
  return vectorToFloat32Array(result.data(), result.size());
}

// Normalize
val js_normalize_ex(val samples, const val& sample_rate, const val& target_db_val,
                    const std::string& mode) {
  if (mode != "peak" && mode != "rms") {
    throw WasmRangeError("mode must be 'peak' or 'rms'");
  }
  const float target_db = checkedFloatFromVal(target_db_val, "targetDb");
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));
  Audio result =
      mode == "rms" ? normalize_rms(audio, target_db, true) : normalize(audio, target_db);
  return vectorToFloat32Array(result.data(), result.size());
}

val js_normalize(val samples, const val& sample_rate, const val& target_db) {
  return js_normalize_ex(samples, sample_rate, target_db, "peak");
}

// Normalizing the two channels separately would lift the quieter one until the
// peaks matched, which changes the balance rather than the level. The gain is
// measured across the pair and applied to both. Calls the core directly rather
// than the C ABI, matching every other wrapper in this file.
val js_normalize_stereo(val left_samples, val right_samples, const val& sample_rate_val,
                        const val& target_db_val, const std::string& mode) {
  if (mode != "peak" && mode != "rms") {
    throw WasmRangeError("mode must be 'peak' or 'rms'");
  }
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const float target_db = checkedFloatFromVal(target_db_val, "targetDb");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples", true);
  Audio left = loadValidatedAudio("left", left_samples, sample_rate);
  Audio right = loadValidatedAudio("right", right_samples, sample_rate);
  const NormalizeStereoResult result = mode == "rms"
                                           ? normalize_rms_stereo(left, right, target_db, true)
                                           : normalize_stereo(left, right, target_db);
  val out = val::object();
  out.set("left", vectorToFloat32Array(result.left.data(), result.left.size()));
  out.set("right", vectorToFloat32Array(result.right.data(), result.right.size()));
  out.set("appliedGainDb", result.applied_gain_db);
  return out;
}

// Trim silence
val js_trim_ex(val samples, const val& sample_rate, const val& threshold_db,
               const val& frame_length, const val& hop_length) {
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));
  Audio result = trim_absolute(
      audio, checkedFloatFromVal(threshold_db, "thresholdDb"),
      domainNumberFromVal<int>(frame_length, "frameLength", sonare::kSilenceFrameLengthDomain),
      domainNumberFromVal<int>(hop_length, "hopLength", sonare::kSilenceHopLengthDomain));
  return vectorToFloat32Array(result.data(), result.size());
}

val js_trim(val samples, const val& sample_rate, const val& threshold_db) {
  return js_trim_ex(samples, sample_rate, threshold_db, val(constants::kDefaultNFft),
                    val(constants::kDefaultHopLength));
}

// Fade and biquad filter: audio is read unvalidated so the C side's refusal is the one reported.
val js_fade(val samples, const val& sample_rate, const val& fade_in_sec, const val& fade_out_sec) {
  const int rate = checkedIntFromVal(sample_rate, "sampleRate");
  const float fade_in = checkedFloatFromVal(fade_in_sec, "fadeInSec");
  const float fade_out = checkedFloatFromVal(fade_out_sec, "fadeOutSec");
  std::vector<float> input = float32ArrayToVector(samples);
  float* output = nullptr;
  size_t output_length = 0;
  const SonareError err =
      sonare_fade(input.data(), input.size(), rate, fade_in, fade_out, &output, &output_length);
  if (err != SONARE_OK) {
    sonare_free_floats(output);
    throwCodedCError(err);
  }
  std::vector<float> result(output, output + output_length);
  sonare_free_floats(output);
  return vectorToFloat32Array(result);
}

val js_biquad_filter(val samples, const val& sample_rate, const val& type, const val& order,
                     bool zero_phase, const val& cutoff_hz, const val& center_hz,
                     const val& bandwidth_hz) {
  const int rate = checkedIntFromVal(sample_rate, "sampleRate");
  SonareBiquadFilterConfig config;
  sonare_biquad_filter_config_init(&config);
  config.type = checkedIntFromVal(type, "type");
  config.order = checkedIntFromVal(order, "order");
  config.zero_phase = zero_phase ? 1 : 0;
  // A frequency the type does not read stays NaN (not given), as the init function left it.
  config.cutoff_hz = optionalDomainNumber(cutoff_hz, "cutoffHz").value_or(config.cutoff_hz);
  config.center_hz = optionalDomainNumber(center_hz, "centerHz").value_or(config.center_hz);
  config.bandwidth_hz =
      optionalDomainNumber(bandwidth_hz, "bandwidthHz").value_or(config.bandwidth_hz);
  std::vector<float> input = float32ArrayToVector(samples);
  float* output = nullptr;
  size_t output_length = 0;
  const SonareError err =
      sonare_biquad_filter(input.data(), input.size(), rate, &config, &output, &output_length);
  if (err != SONARE_OK) {
    sonare_free_floats(output);
    throwCodedCErrorFlattening(err, "config");
  }
  std::vector<float> result(output, output + output_length);
  sonare_free_floats(output);
  return vectorToFloat32Array(result);
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
  function("autoTune", &js_auto_tune);
  function("noteStretch", &js_note_stretch);
  function("noteMove", &js_note_move);
  function("voiceChange", &js_voice_change);
  function("voiceChangeRealtime", &js_voice_change_realtime);
  function("hpssWithResidual", &js_hpss_with_residual);
  function("hpssWithResidualEx", &js_hpss_with_residual_ex);
  function("phaseVocoder", &js_phase_vocoder);
  function("normalize", &js_normalize);
  function("normalizeEx", &js_normalize_ex);
  function("normalizeStereo", &js_normalize_stereo);
  function("trim", &js_trim);
  function("trimEx", &js_trim_ex);
  function("fade", &js_fade);
  function("biquadFilter", &js_biquad_filter);
}

#endif  // __EMSCRIPTEN__
