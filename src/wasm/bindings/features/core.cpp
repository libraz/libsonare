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

void validateSampleRate(const char* function, int sample_rate) {
  if (sample_rate < kMinAudioSampleRate || sample_rate > kMaxAudioSampleRate) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(function) + ": sample rate is out of range");
  }
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
float js_frames_to_time(int frames, int sr, int hop_length) {
  return frames_to_time(frames, sr, hop_length);
}
int js_time_to_frames(float time, int sr, int hop_length) {
  return time_to_frames(time, sr, hop_length);
}
int js_frames_to_samples(int frames, int hop_length, int n_fft) {
  return frames_to_samples(frames, hop_length, n_fft);
}
int js_samples_to_frames(int samples, int hop_length, int n_fft) {
  return samples_to_frames(samples, hop_length, n_fft);
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

val js_preemphasis(val samples, float coef, val zi) {
  std::vector<float> data = float32ArrayToVector(samples);
  validateFiniteVector(data, "preemphasis");
  if (zi.isUndefined() || zi.isNull()) {
    return vectorToFloat32Array(preemphasis(data, coef));
  }
  return vectorToFloat32Array(preemphasis(data, coef, zi.as<float>()));
}

val js_deemphasis(val samples, float coef, val zi) {
  std::vector<float> data = float32ArrayToVector(samples);
  validateFiniteVector(data, "deemphasis");
  if (zi.isUndefined() || zi.isNull()) {
    return vectorToFloat32Array(deemphasis(data, coef));
  }
  return vectorToFloat32Array(deemphasis(data, coef, zi.as<float>()));
}

val js_trim_silence(val samples, float top_db, int frame_length, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  validateFiniteVector(data, "trimSilence");
  auto result = trim(data, top_db, frame_length, hop_length);
  val out = val::object();
  out.set("audio", vectorToFloat32Array(result.audio));
  out.set("startSample", result.start_sample);
  out.set("endSample", result.end_sample);
  return out;
}

val js_split_silence(val samples, float top_db, int frame_length, int hop_length) {
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

val js_frame_signal(val samples, int frame_length, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  validateFiniteVector(data, "frameSignal");
  val out = val::object();
  out.set("nFrames", frame_count(data.size(), frame_length, hop_length));
  out.set("frames", vectorToFloat32Array(frame(data, frame_length, hop_length)));
  return out;
}

val js_tone(float frequency, int sample_rate, float duration, float phase, float amplitude) {
  const Audio audio = tone(frequency, sample_rate, duration, phase, amplitude);
  return vectorToFloat32Array(std::vector<float>(audio.data(), audio.data() + audio.size()));
}

val js_chirp(float fmin, float fmax, int sample_rate, float duration, bool linear) {
  const Audio audio = chirp(fmin, fmax, sample_rate, duration, linear);
  return vectorToFloat32Array(std::vector<float>(audio.data(), audio.data() + audio.size()));
}

val js_clicks(val times, int sample_rate, int length, float frequency, float click_duration) {
  std::vector<float> values = float32ArrayToVector(times);
  validateFiniteVector(values, "clicks");
  const Audio audio = clicks(values, sample_rate, length, frequency, click_duration);
  return vectorToFloat32Array(std::vector<float>(audio.data(), audio.data() + audio.size()));
}

val js_pad_center(val values, int size, float pad_value) {
  std::vector<float> data = float32ArrayToVector(values);
  validateFiniteVector(data, "padCenter");
  if (size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "padCenter: size must be non-negative");
  }
  return vectorToFloat32Array(pad_center(data, static_cast<size_t>(size), pad_value));
}

val js_fix_length(val values, int size, float pad_value) {
  std::vector<float> data = float32ArrayToVector(values);
  validateFiniteVector(data, "fixLength");
  if (size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "fixLength: size must be non-negative");
  }
  return vectorToFloat32Array(fix_length(data, static_cast<size_t>(size), pad_value));
}

std::vector<int> intArrayToVector(val arr) {
  const int length = arr["length"].as<int>();
  std::vector<int> out(static_cast<size_t>(length));
  for (int index = 0; index < length; ++index) {
    out[static_cast<size_t>(index)] = arr[index].as<int>();
  }
  return out;
}

val js_fix_frames(val frames, int x_min, int x_max, bool pad) {
  return vectorToInt32Array(fix_frames(intArrayToVector(frames), x_min, x_max, pad));
}

val js_onset_backtrack(val events, val energy) {
  std::vector<float> energy_values = float32ArrayToVector(energy);
  validateFiniteVector(energy_values, "onsetBacktrack");
  return vectorToInt32Array(onset_backtrack(intArrayToVector(events), energy_values));
}

val js_peak_pick(val values, int pre_max, int post_max, int pre_avg, int post_avg, float delta,
                 int wait) {
  std::vector<float> data = float32ArrayToVector(values);
  validateFiniteVector(data, "peakPick");
  return vectorToInt32Array(peak_pick(data, pre_max, post_max, pre_avg, post_avg, delta, wait));
}

val js_vector_normalize(val values, int norm_type, float threshold) {
  std::vector<float> data = float32ArrayToVector(values);
  validateFiniteVector(data, "vectorNormalize");
  NormType norm = NormType::Inf;
  if (norm_type == 1) norm = NormType::L1;
  if (norm_type == 2) norm = NormType::L2;
  if (norm_type == 3) norm = NormType::Power;
  return vectorToFloat32Array(normalize(data, norm, threshold));
}

val js_pcen(val values, int n_bins, int n_frames, val options) {
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

val js_tonnetz(val chromagram, int n_chroma, int n_frames) {
  std::vector<float> data = float32ArrayToVector(chromagram);
  validateMatrix(data, n_chroma, n_frames, "tonnetz");
  return vectorToFloat32Array(tonnetz(data.data(), n_chroma, n_frames));
}

TempogramMode tempogramModeFromValue(val mode) {
  if (mode.isUndefined() || mode.isNull()) return TempogramMode::kAutocorrelation;
  if (mode.typeOf().as<std::string>() == "number") {
    const int mode_id = mode.as<int>();
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

val js_tempogram(val onset_envelope, int sample_rate, int hop_length, int win_length, val mode,
                 bool center, bool norm) {
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

val js_cyclic_tempogram(val onset_envelope, int sample_rate, int hop_length, int win_length,
                        float bpm_min, int n_bins) {
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

val js_plp(val onset_envelope, int sample_rate, int hop_length, float tempo_min, float tempo_max,
           int win_length) {
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

val js_onset_envelope(val samples, int sample_rate, int n_fft, int hop_length, int n_mels) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  MelConfig mel_config;
  mel_config.n_fft = n_fft;
  mel_config.hop_length = hop_length;
  mel_config.n_mels = n_mels;
  return vectorToFloat32Array(compute_onset_strength(audio, mel_config, OnsetConfig()));
}

val js_onset_strength_multi(val samples, int sample_rate, int n_fft, int hop_length, int n_mels,
                            int n_bands) {
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

val js_fourier_tempogram(val onset_envelope, int sample_rate, int hop_length, int win_length,
                         bool center, bool norm) {
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

val js_tempogram_ratio(val tempogram_data, int win_length, int sample_rate, int hop_length,
                       val factors) {
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
