/// @file feature_core.cpp
/// @brief Embind bindings for core conversion and signal utility APIs.

#ifdef __EMSCRIPTEN__

#include "core/synthesis.h"
#include "wasm/bindings/common/common.h"

namespace {

void validateFiniteVector(const std::vector<float>& values, const char* function) {
  if (values.size() > kMaxAudioBufferSize) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(function) + ": input buffer is too large");
  }
  for (float value : values) {
    if (!std::isfinite(value)) {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string(function) + ": input contains NaN or Inf");
    }
  }
}

void validateMatrix(const std::vector<float>& values, int rows, int columns, const char* function) {
  if (rows <= 0 || columns <= 0 ||
      static_cast<size_t>(rows) > kMaxAudioBufferSize / static_cast<size_t>(columns) ||
      values.size() != static_cast<size_t>(rows) * static_cast<size_t>(columns)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(function) + ": matrix dimensions do not match input");
  }
  validateFiniteVector(values, function);
}

// The tempogram / PLP family takes an onset envelope (not raw audio); its
// sample_rate is only a BPM-scaling factor, so — matching the C ABI oracle,
// which does not band-limit it — we require it to be positive rather than
// inside the [kMin,kMax]AudioSampleRate audio band.
void validatePositiveSampleRate(const char* function, int sample_rate) {
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(function) + ": sample rate must be positive");
  }
}

}  // namespace

// ============================================================================
// Core - Conversion
// ============================================================================

float js_hz_to_mel(float hz) { return hz_to_mel(hz); }
float js_mel_to_hz(float mel) { return mel_to_hz(mel); }
float js_hz_to_midi(float hz) { return hz_to_midi(hz); }
float js_midi_to_hz(float midi) { return midi_to_hz(midi); }
std::string js_hz_to_note(float hz) { return hz_to_note(hz); }
float js_note_to_hz(const std::string& note) { return note_to_hz(note); }
float js_frames_to_time(const val& frames, const val& sr, const val& hop_length) {
  return frames_to_time(checkedIntFromVal(frames, "frames"), checkedIntFromVal(sr, "sr"),
                        checkedIntFromVal(hop_length, "hopLength"));
}
int js_time_to_frames(const val& time_val, const val& sr, const val& hop_length) {
  // time_to_frames saturates a non-finite result to INT_MAX, so a value the f32
  // parameter turned into an infinity returns a frame index nothing downstream
  // can tell from a real one.
  return time_to_frames(checkedFloatFromVal(time_val, "time"), checkedIntFromVal(sr, "sr"),
                        checkedIntFromVal(hop_length, "hopLength"));
}
int js_frames_to_samples(const val& frames, const val& hop_length, const val& n_fft) {
  return frames_to_samples(checkedIntFromVal(frames, "frames"),
                           checkedIntFromVal(hop_length, "hopLength"),
                           checkedIntFromVal(n_fft, "nFft"));
}
int js_samples_to_frames(const val& samples, const val& hop_length, const val& n_fft) {
  return samples_to_frames(checkedIntFromVal(samples, "samples"),
                           checkedIntFromVal(hop_length, "hopLength"),
                           checkedIntFromVal(n_fft, "nFft"));
}

val js_power_to_db(val values, float ref, float amin, float top_db) {
  std::vector<float> data = float32ArrayToVector(values);
  validateFiniteVector(data, "powerToDb");
  return vectorToFloat32Array(power_to_db(data, ref, amin, top_db));
}

val js_amplitude_to_db(val values, float ref, float amin, float top_db) {
  std::vector<float> data = float32ArrayToVector(values);
  validateFiniteVector(data, "amplitudeToDb");
  return vectorToFloat32Array(amplitude_to_db(data, ref, amin, top_db));
}

val js_db_to_power(val values, float ref) {
  std::vector<float> data = float32ArrayToVector(values);
  validateFiniteVector(data, "dbToPower");
  return vectorToFloat32Array(db_to_power(data, ref));
}

val js_db_to_amplitude(val values, float ref) {
  std::vector<float> data = float32ArrayToVector(values);
  validateFiniteVector(data, "dbToAmplitude");
  return vectorToFloat32Array(db_to_amplitude(data, ref));
}

val js_preemphasis(val samples, const val& coef_val, val zi) {
  const float coef = checkedFloatFromVal(coef_val, "coef");
  std::vector<float> data = float32ArrayToVector(samples);
  validateFiniteVector(data, "preemphasis");
  if (zi.isUndefined() || zi.isNull()) {
    return vectorToFloat32Array(preemphasis(data, coef));
  }
  return vectorToFloat32Array(preemphasis(data, coef, checkedFloatFromVal(zi, "zi")));
}

val js_deemphasis(val samples, const val& coef_val, val zi) {
  const float coef = checkedFloatFromVal(coef_val, "coef");
  std::vector<float> data = float32ArrayToVector(samples);
  validateFiniteVector(data, "deemphasis");
  if (zi.isUndefined() || zi.isNull()) {
    return vectorToFloat32Array(deemphasis(data, coef));
  }
  return vectorToFloat32Array(deemphasis(data, coef, checkedFloatFromVal(zi, "zi")));
}

val js_trim_silence(val samples, const val& top_db_val, const val& frame_length_val,
                    const val& hop_length_val) {
  const float top_db = checkedFloatFromVal(top_db_val, "topDb");
  const int frame_length = checkedIntFromVal(frame_length_val, "frameLength");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  std::vector<float> data = float32ArrayToVector(samples);
  validateFiniteVector(data, "trimSilence");
  auto result = trim(data, top_db, frame_length, hop_length);
  val out = val::object();
  out.set("audio", vectorToFloat32Array(result.audio));
  out.set("startSample", result.start_sample);
  out.set("endSample", result.end_sample);
  return out;
}

val js_split_silence(val samples, const val& top_db_val, const val& frame_length_val,
                     const val& hop_length_val) {
  const float top_db = checkedFloatFromVal(top_db_val, "topDb");
  const int frame_length = checkedIntFromVal(frame_length_val, "frameLength");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  std::vector<float> data = float32ArrayToVector(samples);
  validateFiniteVector(data, "splitSilence");
  auto ranges = split(data, top_db, frame_length, hop_length);
  std::vector<int> flat;
  flat.reserve(ranges.size() * 2);
  for (const auto& range : ranges) {
    flat.push_back(range.first);
    flat.push_back(range.second);
  }
  return vectorToInt32Array(flat);
}

val js_frame_signal(val samples, const val& frame_length_val, const val& hop_length_val) {
  const int frame_length = checkedIntFromVal(frame_length_val, "frameLength");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  std::vector<float> data = float32ArrayToVector(samples);
  validateFiniteVector(data, "frameSignal");
  val out = val::object();
  out.set("nFrames", frame_count(data.size(), frame_length, hop_length));
  out.set("frames", vectorToFloat32Array(frame(data, frame_length, hop_length)));
  return out;
}

val js_tone(const val& frequency_val, const val& sample_rate, float duration, const val& phase_val,
            const val& amplitude_val) {
  const float frequency = checkedFloatFromVal(frequency_val, "frequency");
  const float phase = checkedFloatFromVal(phase_val, "phase");
  const float amplitude = checkedFloatFromVal(amplitude_val, "amplitude");
  const Audio audio =
      tone(frequency, checkedIntFromVal(sample_rate, "sampleRate"), duration, phase, amplitude);
  return vectorToFloat32Array(std::vector<float>(audio.data(), audio.data() + audio.size()));
}

val js_chirp(const val& fmin_val, const val& fmax_val, const val& sample_rate, float duration,
             bool linear) {
  const float fmin = checkedFloatFromVal(fmin_val, "fmin");
  const float fmax = checkedFloatFromVal(fmax_val, "fmax");
  const Audio audio =
      chirp(fmin, fmax, checkedIntFromVal(sample_rate, "sampleRate"), duration, linear);
  return vectorToFloat32Array(std::vector<float>(audio.data(), audio.data() + audio.size()));
}

val js_clicks(val times, const val& sample_rate_val, const val& length_val, float frequency,
              float click_duration) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int length = checkedIntFromVal(length_val, "length");
  std::vector<float> values = float32ArrayToVector(times);
  validateFiniteVector(values, "clicks");
  const Audio audio = clicks(values, sample_rate, length, frequency, click_duration);
  return vectorToFloat32Array(std::vector<float>(audio.data(), audio.data() + audio.size()));
}

val js_pad_center(val values, const val& size_val, const val& pad_value_val) {
  const int size = checkedIntFromVal(size_val, "size");
  const float pad_value = checkedFloatFromVal(pad_value_val, "padValue");
  std::vector<float> data = float32ArrayToVector(values);
  validateFiniteVector(data, "padCenter");
  if (size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "padCenter: size must be non-negative");
  }
  return vectorToFloat32Array(pad_center(data, static_cast<size_t>(size), pad_value));
}

val js_fix_length(val values, const val& size_val, const val& pad_value_val) {
  const int size = checkedIntFromVal(size_val, "size");
  const float pad_value = checkedFloatFromVal(pad_value_val, "padValue");
  std::vector<float> data = float32ArrayToVector(values);
  validateFiniteVector(data, "fixLength");
  if (size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "fixLength: size must be non-negative");
  }
  return vectorToFloat32Array(fix_length(data, static_cast<size_t>(size), pad_value));
}

std::vector<int> intArrayToVector(val arr) {
  // The caller-supplied `.length` decides the allocation, so it goes through the
  // shared safe-integer + budget guard rather than a raw as<int>() narrowing.
  const size_t length = wasmArrayLikeLength(arr, "Int32Array");
  std::vector<int> out(length);
  for (size_t index = 0; index < length; ++index) {
    // Each element goes through the same 32-bit-integer guard as any other
    // scalar read, so an out-of-range or fractional element is refused rather
    // than silently narrowed.
    out[index] = checkedIntFromVal(arr[index], "Int32Array element");
  }
  return out;
}

val js_fix_frames(val frames, const val& x_min, const val& x_max, bool pad) {
  return vectorToInt32Array(fix_frames(intArrayToVector(frames), checkedIntFromVal(x_min, "xMin"),
                                       checkedIntFromVal(x_max, "xMax"), pad));
}

val js_onset_backtrack(val events, val energy) {
  std::vector<float> energy_values = float32ArrayToVector(energy);
  validateFiniteVector(energy_values, "onsetBacktrack");
  return vectorToInt32Array(onset_backtrack(intArrayToVector(events), energy_values));
}

val js_peak_pick(val values, const val& pre_max, const val& post_max, const val& pre_avg,
                 const val& post_avg, const val& delta_val, const val& wait) {
  const int pre_max_frames = checkedIntFromVal(pre_max, "preMax");
  const int post_max_frames = checkedIntFromVal(post_max, "postMax");
  const int pre_avg_frames = checkedIntFromVal(pre_avg, "preAvg");
  const int post_avg_frames = checkedIntFromVal(post_avg, "postAvg");
  const float delta = checkedFloatFromVal(delta_val, "delta");
  const int wait_frames = checkedIntFromVal(wait, "wait");
  std::vector<float> data = float32ArrayToVector(values);
  validateFiniteVector(data, "peakPick");
  return vectorToInt32Array(peak_pick(data, pre_max_frames, post_max_frames, pre_avg_frames,
                                      post_avg_frames, delta, wait_frames));
}

val js_vector_normalize(val values, const val& norm_type_val, const val& threshold_val) {
  const int norm_type = checkedIntFromVal(norm_type_val, "normType");
  const float threshold = checkedFloatFromVal(threshold_val, "threshold");
  std::vector<float> data = float32ArrayToVector(values);
  validateFiniteVector(data, "vectorNormalize");
  NormType norm = NormType::Inf;
  if (norm_type == 1) norm = NormType::L1;
  if (norm_type == 2) norm = NormType::L2;
  if (norm_type == 3) norm = NormType::Power;
  return vectorToFloat32Array(normalize(data, norm, threshold));
}

val js_pcen(val values, const val& n_bins_val, const val& n_frames_val, val options) {
  const int n_bins = checkedIntFromVal(n_bins_val, "nBins");
  const int n_frames = checkedIntFromVal(n_frames_val, "nFrames");
  std::vector<float> data = float32ArrayToVector(values);
  validateMatrix(data, n_bins, n_frames, "pcen");
  PcenConfig config;
  if (!options.isUndefined() && !options.isNull()) {
    config.sr = intProperty(options, "sampleRate", config.sr);
    config.hop_length = intProperty(options, "hopLength", config.hop_length);
    config.time_constant = floatProperty(options, "timeConstant", config.time_constant);
    config.gain = floatProperty(options, "gain", config.gain);
    config.bias = floatProperty(options, "bias", config.bias);
    config.power = floatProperty(options, "power", config.power);
    config.eps = floatProperty(options, "eps", config.eps);
  }
  return vectorToFloat32Array(pcen(data, n_bins, n_frames, config));
}

val js_tonnetz(val chromagram, const val& n_chroma_val, const val& n_frames_val) {
  const int n_chroma = checkedIntFromVal(n_chroma_val, "nChroma");
  const int n_frames = checkedIntFromVal(n_frames_val, "nFrames");
  std::vector<float> data = float32ArrayToVector(chromagram);
  validateMatrix(data, n_chroma, n_frames, "tonnetz");
  return vectorToFloat32Array(tonnetz(data.data(), n_chroma, n_frames));
}

TempogramMode tempogramModeFromValue(val mode) {
  if (mode.isUndefined() || mode.isNull()) return TempogramMode::kAutocorrelation;
  if (mode.typeOf().as<std::string>() == "number") {
    // An ordinal names a member, not a quantity: 0.5 used to select
    // autocorrelation and 1.5 cosine, neither of which the caller spelled.
    const int mode_id = checkedIntFromVal(mode, "tempogram mode");
    if (mode_id == SONARE_TEMPOGRAM_AUTOCORRELATION) return TempogramMode::kAutocorrelation;
    if (mode_id == SONARE_TEMPOGRAM_COSINE) return TempogramMode::kCosine;
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "tempogram mode must be 'autocorrelation' or 'cosine'");
  }
  const std::string value = mode.as<std::string>();
  if (value == "autocorrelation" || value == "auto" || value == "ac") {
    return TempogramMode::kAutocorrelation;
  }
  if (value == "cosine") return TempogramMode::kCosine;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "tempogram mode must be 'autocorrelation' or 'cosine'");
}

val js_tempogram(val onset_envelope, const val& sample_rate_val, const val& hop_length_val,
                 const val& win_length_val, val mode, bool center, bool norm) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const int win_length = checkedIntFromVal(win_length_val, "winLength");
  std::vector<float> data = float32ArrayToVector(onset_envelope);
  validateFiniteVector(data, "tempogram");
  validatePositiveSampleRate("tempogram", sample_rate);
  TempogramConfig config;
  config.hop_length = hop_length;
  config.win_length = win_length;
  config.mode = tempogramModeFromValue(mode);
  config.center = center;
  config.norm = norm;
  auto result = tempogram(data, sample_rate, config);
  val out = val::object();
  out.set("nFrames", static_cast<int>(data.size()));
  out.set("winLength", win_length);
  out.set("data", vectorToFloat32Array(result));
  return out;
}

val js_cyclic_tempogram(val onset_envelope, const val& sample_rate_val, const val& hop_length_val,
                        const val& win_length_val, float bpm_min, const val& n_bins_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const int win_length = checkedIntFromVal(win_length_val, "winLength");
  const int n_bins = checkedIntFromVal(n_bins_val, "nBins");
  std::vector<float> data = float32ArrayToVector(onset_envelope);
  validateFiniteVector(data, "cyclicTempogram");
  validatePositiveSampleRate("cyclicTempogram", sample_rate);
  TempogramConfig config;
  config.hop_length = hop_length;
  config.win_length = win_length;
  config.center = true;
  config.norm = false;
  auto result = cyclic_tempogram(data, sample_rate, config, bpm_min, n_bins);
  val out = val::object();
  out.set("nFrames", static_cast<int>(data.size()));
  out.set("nBins", n_bins);
  out.set("data", vectorToFloat32Array(result));
  return out;
}

val js_plp(val onset_envelope, const val& sample_rate_val, const val& hop_length_val,
           float tempo_min, float tempo_max, const val& win_length_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const int win_length = checkedIntFromVal(win_length_val, "winLength");
  std::vector<float> data = float32ArrayToVector(onset_envelope);
  validateFiniteVector(data, "plp");
  validatePositiveSampleRate("plp", sample_rate);
  PlpConfig config;
  config.sr = sample_rate;
  config.hop_length = hop_length;
  config.tempo_min = tempo_min;
  config.tempo_max = tempo_max;
  config.win_length = win_length;
  return vectorToFloat32Array(plp(data, config));
}

val js_onset_envelope(val samples, const val& sample_rate_val, const val& n_fft_val,
                      const val& hop_length_val, const val& n_mels_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const int n_mels = checkedIntFromVal(n_mels_val, "nMels");
  Audio audio = loadValidatedAudio(samples, sample_rate);
  MelConfig mel_config;
  mel_config.n_fft = n_fft;
  mel_config.hop_length = hop_length;
  mel_config.n_mels = n_mels;
  return vectorToFloat32Array(compute_onset_strength(audio, mel_config, OnsetConfig()));
}

val js_onset_strength_multi(val samples, const val& sample_rate_val, const val& n_fft_val,
                            const val& hop_length_val, const val& n_mels_val,
                            const val& n_bands_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const int n_mels = checkedIntFromVal(n_mels_val, "nMels");
  const int n_bands = checkedIntFromVal(n_bands_val, "nBands");
  Audio audio = loadValidatedAudio(samples, sample_rate);
  MelConfig mel_config;
  mel_config.n_fft = n_fft;
  mel_config.hop_length = hop_length;
  mel_config.n_mels = n_mels;
  MelSpectrogram mel = MelSpectrogram::compute(audio, mel_config);
  std::vector<float> env = onset_strength_multi(mel, n_bands, OnsetConfig());

  val out = val::object();
  out.set("nBands", n_bands);
  out.set("nFrames", mel.n_frames());
  out.set("data", vectorToFloat32Array(env));
  return out;
}

val js_fourier_tempogram(val onset_envelope, const val& sample_rate_val, const val& hop_length_val,
                         const val& win_length_val, bool center, bool norm) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const int win_length = checkedIntFromVal(win_length_val, "winLength");
  std::vector<float> data = float32ArrayToVector(onset_envelope);
  validateFiniteVector(data, "fourierTempogram");
  validatePositiveSampleRate("fourierTempogram", sample_rate);
  TempogramConfig config;
  config.hop_length = hop_length;
  config.win_length = win_length;
  config.center = center;
  config.norm = norm;
  auto result = fourier_tempogram(data, sample_rate, config);
  val out = val::object();
  out.set("nBins", win_length / 2 + 1);
  out.set("nFrames", static_cast<int>(data.size()));
  out.set("data", vectorToFloat32Array(result));
  return out;
}

val js_tempogram_ratio(val tempogram_data, const val& win_length_val, const val& sample_rate_val,
                       const val& hop_length_val, val factors) {
  const int win_length = checkedIntFromVal(win_length_val, "winLength");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const bool has_factors = !factors.isUndefined() && !factors.isNull();
  if (has_factors) {
    validateWasmFloat32ArrayPair(tempogram_data, "tempogram data", factors, "factors",
                                 "tempogramRatio input", false);
  }
  std::vector<float> data = float32ArrayToVector(tempogram_data);
  validateFiniteVector(data, "tempogramRatio");
  validatePositiveSampleRate("tempogramRatio", sample_rate);
  // An undefined/null/empty factors argument falls back to the library default
  // {0.5, 1, 2, 3, 4}, matching the C and Node behaviour.
  if (!has_factors) {
    return vectorToFloat32Array(tempogram_ratio(data, win_length, sample_rate, hop_length));
  }
  std::vector<float> factor_values = float32ArrayToVector(factors);
  if (factor_values.empty()) {
    return vectorToFloat32Array(tempogram_ratio(data, win_length, sample_rate, hop_length));
  }
  return vectorToFloat32Array(
      tempogram_ratio(data, win_length, sample_rate, hop_length, factor_values));
}

void registerFeatureCoreBindings() {
  function("hzToMel", &js_hz_to_mel);
  function("melToHz", &js_mel_to_hz);
  function("hzToMidi", &js_hz_to_midi);
  function("midiToHz", &js_midi_to_hz);
  function("hzToNote", &js_hz_to_note);
  function("noteToHz", &js_note_to_hz);
  function("framesToTime", &js_frames_to_time);
  function("timeToFrames", &js_time_to_frames);
  function("framesToSamples", &js_frames_to_samples);
  function("samplesToFrames", &js_samples_to_frames);
  function("powerToDb", &js_power_to_db);
  function("amplitudeToDb", &js_amplitude_to_db);
  function("dbToPower", &js_db_to_power);
  function("dbToAmplitude", &js_db_to_amplitude);
  function("preemphasis", &js_preemphasis);
  function("deemphasis", &js_deemphasis);
  function("trimSilence", &js_trim_silence);
  function("splitSilence", &js_split_silence);
  function("frameSignal", &js_frame_signal);
  function("tone", &js_tone);
  function("chirp", &js_chirp);
  function("clicks", &js_clicks);
  function("padCenter", &js_pad_center);
  function("fixLength", &js_fix_length);
  function("fixFrames", &js_fix_frames);
  function("onsetBacktrack", &js_onset_backtrack);
  function("peakPick", &js_peak_pick);
  function("vectorNormalize", &js_vector_normalize);
  function("pcen", &js_pcen);
  function("tonnetz", &js_tonnetz);
  function("tempogram", &js_tempogram);
  function("cyclicTempogram", &js_cyclic_tempogram);
  function("plp", &js_plp);
  function("onsetEnvelope", &js_onset_envelope);
  function("onsetStrengthMulti", &js_onset_strength_multi);
  function("fourierTempogram", &js_fourier_tempogram);
  function("tempogramRatio", &js_tempogram_ratio);
}

#endif  // __EMSCRIPTEN__
