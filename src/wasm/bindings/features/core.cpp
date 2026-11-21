/// @file feature_core.cpp
/// @brief Embind bindings for core conversion and signal utility APIs.

#ifdef __EMSCRIPTEN__

#include "core/convert.h"
#include "core/spectrum.h"
#include "core/synthesis.h"
#include "feature/onset.h"
#include "feature/rhythm.h"
#include "filters/mel.h"
#include "util/frame.h"
#include "wasm/bindings/common/common.h"

namespace {

void validateFiniteVector(const std::vector<float>& values, const std::string& name,
                          const char* noun = "value") {
  if (values.size() > kMaxAudioBufferSize) {
    throw WasmRangeError("input buffer is too large");
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (!std::isfinite(values[i])) {
      throw WasmRangeError(name + " contains a non-finite " + noun + " at index " +
                           std::to_string(i));
    }
  }
}

void validateMatrix(const std::vector<float>& values, const std::string& name, int rows,
                    int columns) {
  if (rows <= 0 || columns <= 0 ||
      static_cast<size_t>(rows) > kMaxAudioBufferSize / static_cast<size_t>(columns) ||
      values.size() != static_cast<size_t>(rows) * static_cast<size_t>(columns)) {
    throw WasmRangeError("matrix dimensions do not match input");
  }
  validateFiniteVector(values, name);
}

// The tempogram / PLP family takes an onset envelope (not raw audio); its
// sample_rate is only a BPM-scaling factor, so — matching the C ABI oracle,
// which does not band-limit it — we require it to be positive rather than
// inside the [kMin,kMax]AudioSampleRate audio band.
void validatePositiveSampleRate(int sample_rate) {
  if (sample_rate <= 0) {
    throw WasmRangeError("sampleRate must be an integer in [1, 2147483647]");
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
float js_note_to_hz(const std::string& note) {
  wasmRefuseEmbeddedNul(note, "note");
  return note_to_hz(note);
}
float js_frames_to_time(const val& frames, const val& sr, const val& hop_length) {
  return frames_to_time(checkedIntFromVal(frames, "frames"),
                        domainNumberFromVal<int>(sr, "sr", kFrameTimeSampleRateDomain),
                        domainNumberFromVal<int>(hop_length, "hopLength", kFrameTimeHopDomain));
}
int js_time_to_frames(const val& time_val, const val& sr, const val& hop_length) {
  // time_to_frames saturates a non-finite result to INT_MAX, so a value the f32
  // parameter turned into an infinity returns a frame index nothing downstream
  // can tell from a real one.
  return time_to_frames(checkedFloatFromVal(time_val, "time"),
                        domainNumberFromVal<int>(sr, "sr", kFrameTimeSampleRateDomain),
                        domainNumberFromVal<int>(hop_length, "hopLength", kFrameTimeHopDomain));
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

val js_power_to_db(val values, const val& ref_val, const val& amin_val, const val& top_db_val) {
  const float ref = checkedFloatFromVal(ref_val, "ref");
  const float amin = checkedFloatFromVal(amin_val, "amin");
  const float top_db = checkedFloatFromVal(top_db_val, "topDb");
  std::vector<float> data = float32ArrayToVector(values, "values");
  validateFiniteVector(data, "values");
  return vectorToFloat32Array(power_to_db(data, ref, amin, top_db));
}

val js_amplitude_to_db(val values, const val& ref_val, const val& amin_val, const val& top_db_val) {
  const float ref = checkedFloatFromVal(ref_val, "ref");
  const float amin = checkedFloatFromVal(amin_val, "amin");
  const float top_db = checkedFloatFromVal(top_db_val, "topDb");
  std::vector<float> data = float32ArrayToVector(values, "values");
  validateFiniteVector(data, "values");
  return vectorToFloat32Array(amplitude_to_db(data, ref, amin, top_db));
}

val js_db_to_power(val values, const val& ref_val) {
  const float ref = checkedFloatFromVal(ref_val, "ref");
  std::vector<float> data = float32ArrayToVector(values, "values");
  validateFiniteVector(data, "values");
  return vectorToFloat32Array(db_to_power(data, ref));
}

val js_db_to_amplitude(val values, const val& ref_val) {
  const float ref = checkedFloatFromVal(ref_val, "ref");
  std::vector<float> data = float32ArrayToVector(values, "values");
  validateFiniteVector(data, "values");
  return vectorToFloat32Array(db_to_amplitude(data, ref));
}

val js_preemphasis(val samples, const val& coef_val, val zi) {
  const float coef = checkedFloatFromVal(coef_val, "coef");
  std::vector<float> data = float32ArrayToVector(samples, "samples");
  if (zi.isUndefined() || zi.isNull()) {
    return vectorToFloat32Array(preemphasis(data, coef));
  }
  return vectorToFloat32Array(preemphasis(data, coef, checkedFloatFromVal(zi, "zi")));
}

val js_deemphasis(val samples, const val& coef_val, val zi) {
  const float coef = checkedFloatFromVal(coef_val, "coef");
  std::vector<float> data = float32ArrayToVector(samples, "samples");
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
  std::vector<float> data = float32ArrayToVector(samples, "samples");
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
  std::vector<float> data = float32ArrayToVector(samples, "samples");
  auto ranges = split(data, top_db, frame_length, hop_length);
  std::vector<int> flat;
  flat.reserve(ranges.size() * 2);
  for (const auto& range : ranges) {
    flat.push_back(range.first);
    flat.push_back(range.second);
  }
  return vectorToInt32Array(flat);
}

namespace {

// What several signals share is their silence, not their sound, and the union
// rule that follows from it lives in sonare::split_common_with_report -- a merge
// rule held in two places is one place for them to disagree.
CommonSplitReport splitSilenceCommonImpl(val signals, const val& top_db_val,
                                         const val& frame_length_val, const val& hop_length_val) {
  const float top_db = checkedFloatFromVal(top_db_val, "topDb");
  const int frame_length = checkedIntFromVal(frame_length_val, "frameLength");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");

  const std::size_t signal_count = wasmArrayLikeLength(signals, "signals");
  if (signal_count == 0) {
    throw WasmRangeError("signals must not be empty");
  }

  std::vector<std::vector<float>> buffers(signal_count);
  std::vector<const float*> pointers(signal_count);
  std::vector<std::size_t> lengths(signal_count);
  std::size_t budget = 0;
  for (std::size_t index = 0; index < signal_count; ++index) {
    const val signal = signals[index];
    if (signal.isUndefined() || signal.isNull()) {
      throw WasmTypeError("signals[" + std::to_string(index) + "] must be a Float32Array");
    }
    accumulateWasmFloat32ArrayLength(signal, "signals", &budget);
    buffers[index] = float32ArrayToVector(signal, "signals");
    validateFiniteVector(buffers[index], "signals[" + std::to_string(index) + "]", "sample");
    pointers[index] = buffers[index].data();
    lengths[index] = buffers[index].size();
  }

  return split_common_with_report(pointers.data(), lengths.data(), signal_count, top_db,
                                  frame_length, hop_length);
}

val flattenIntervals(const std::vector<std::pair<int, int>>& intervals) {
  std::vector<int> flat;
  flat.reserve(intervals.size() * 2);
  for (const auto& interval : intervals) {
    flat.push_back(interval.first);
    flat.push_back(interval.second);
  }
  return vectorToInt32Array(flat);
}

}  // namespace

val js_split_silence_common(val signals, const val& top_db_val, const val& frame_length_val,
                            const val& hop_length_val, const val& with_report_val) {
  const bool with_report = optionalBool(with_report_val, "withReport").value_or(false);
  const CommonSplitReport result =
      splitSilenceCommonImpl(signals, top_db_val, frame_length_val, hop_length_val);
  if (!with_report) return flattenIntervals(result.intervals);
  val report = val::object();
  report.set("silenceCeilingDb", result.silence_ceiling_db);
  report.set("maxSignalIntervals", result.max_signal_intervals);
  report.set("minSignalIntervals", result.min_signal_intervals);
  val out = val::object();
  out.set("intervals", flattenIntervals(result.intervals));
  out.set("report", report);
  return out;
}

val js_frame_signal(val samples, const val& frame_length_val, const val& hop_length_val) {
  const int frame_length =
      domainNumberFromVal<int>(frame_length_val, "frameLength", kFrameLengthDomain);
  const int hop_length =
      domainNumberFromVal<int>(hop_length_val, "hopLength", kFrameHopLengthDomain);
  std::vector<float> data = float32ArrayToVector(samples, "samples");
  val out = val::object();
  out.set("nFrames", frame_count(data.size(), frame_length, hop_length));
  out.set("frames", vectorToFloat32Array(frame(data, frame_length, hop_length)));
  return out;
}

val js_tone(const val& frequency_val, const val& sample_rate, const val& duration_val,
            const val& phase_val, const val& amplitude_val) {
  const float frequency = checkedFloatFromVal(frequency_val, "frequency");
  const float duration = checkedFloatFromVal(duration_val, "duration");
  const float phase = checkedFloatFromVal(phase_val, "phase");
  const float amplitude = checkedFloatFromVal(amplitude_val, "amplitude");
  const Audio audio = tone(
      frequency, domainNumberFromVal<int>(sample_rate, "sampleRate", kSynthesisSampleRateDomain),
      duration, phase, amplitude);
  return vectorToFloat32Array(audio.data(), audio.size());
}

val js_chirp(const val& fmin_val, const val& fmax_val, const val& sample_rate,
             const val& duration_val, bool linear) {
  const float fmin = checkedFloatFromVal(fmin_val, "fmin");
  const float fmax = checkedFloatFromVal(fmax_val, "fmax");
  const float duration = checkedFloatFromVal(duration_val, "duration");
  const Audio audio = chirp(
      fmin, fmax, domainNumberFromVal<int>(sample_rate, "sampleRate", kSynthesisSampleRateDomain),
      duration, linear);
  return vectorToFloat32Array(audio.data(), audio.size());
}

val js_clicks(val times, const val& sample_rate_val, const val& length_val,
              const val& frequency_val, const val& click_duration_val) {
  const int sample_rate =
      domainNumberFromVal<int>(sample_rate_val, "sampleRate", kSynthesisSampleRateDomain);
  const int length =
      optionalDomainValue<int>(length_val, "length", sonare::kClicksLengthDomain).value_or(0);
  const float frequency = checkedFloatFromVal(frequency_val, "frequency");
  const float click_duration = checkedFloatFromVal(click_duration_val, "clickDuration");
  std::vector<float> values = float32ArrayToVector(times, "times");
  validateFiniteVector(values, "times");
  const Audio audio = clicks(values, sample_rate, length, frequency, click_duration);
  return vectorToFloat32Array(audio.data(), audio.size());
}

val js_pad_center(val values, const val& size_val, const val& pad_value_val) {
  const int size = checkedIntFromVal(size_val, "size");
  const float pad_value = checkedFloatFromVal(pad_value_val, "padValue");
  std::vector<float> data = float32ArrayToVector(values, "values");
  validateFiniteVector(data, "values");
  if (size < 0) {
    throw WasmRangeError("size must be an integer in [0, 2147483647]");
  }
  return vectorToFloat32Array(pad_center(data, static_cast<size_t>(size), pad_value));
}

val js_fix_length(val values, const val& size_val, const val& pad_value_val) {
  const int size = checkedIntFromVal(size_val, "size");
  const float pad_value = checkedFloatFromVal(pad_value_val, "padValue");
  std::vector<float> data = float32ArrayToVector(values, "values");
  validateFiniteVector(data, "values");
  if (size < 0) {
    throw WasmRangeError("size must be an integer in [0, 2147483647]");
  }
  return vectorToFloat32Array(fix_length(data, static_cast<size_t>(size), pad_value));
}

std::vector<int> intArrayToVector(val arr, const char* subject) {
  // The caller-supplied `.length` decides the allocation, so it goes through the
  // shared safe-integer + budget guard rather than a raw as<int>() narrowing.
  const size_t length = wasmArrayLikeLength(arr, subject);
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
  return vectorToInt32Array(fix_frames(intArrayToVector(frames, "frames"),
                                       checkedIntFromVal(x_min, "xMin"),
                                       checkedIntFromVal(x_max, "xMax"), pad));
}

val js_onset_backtrack(val events, val energy) {
  std::vector<float> energy_values = float32ArrayToVector(energy, "energy");
  validateFiniteVector(energy_values, "energy");
  return vectorToInt32Array(onset_backtrack(intArrayToVector(events, "events"), energy_values));
}

val js_peak_pick(val values, const val& pre_max, const val& post_max, const val& pre_avg,
                 const val& post_avg, const val& delta_val, const val& wait) {
  const int pre_max_frames = checkedIntFromVal(pre_max, "preMax");
  const int post_max_frames = checkedIntFromVal(post_max, "postMax");
  const int pre_avg_frames = checkedIntFromVal(pre_avg, "preAvg");
  const int post_avg_frames = checkedIntFromVal(post_avg, "postAvg");
  const float delta = checkedFloatFromVal(delta_val, "delta");
  const int wait_frames = checkedIntFromVal(wait, "wait");
  std::vector<float> data = float32ArrayToVector(values, "values");
  validateFiniteVector(data, "values");
  return vectorToInt32Array(peak_pick(data, pre_max_frames, post_max_frames, pre_avg_frames,
                                      post_avg_frames, delta, wait_frames));
}

val js_vector_normalize(val values, const val& norm_type_val, const val& threshold_val) {
  const int norm_type = checkedIntFromVal(norm_type_val, "normType");
  const float threshold = checkedFloatFromVal(threshold_val, "threshold");
  std::vector<float> data = float32ArrayToVector(values, "values");
  validateFiniteVector(data, "values");
  requireOrdinalInRange(norm_type, static_cast<int>(NormType::Inf),
                        static_cast<int>(NormType::Power), "normType");
  return vectorToFloat32Array(normalize(data, static_cast<NormType>(norm_type), threshold));
}

val js_pcen(val values, const val& n_bins_val, const val& n_frames_val, val options) {
  const int n_bins = checkedIntFromVal(n_bins_val, "nBins");
  const int n_frames = checkedIntFromVal(n_frames_val, "nFrames");
  std::vector<float> data = float32ArrayToVector(values, "values");
  validateMatrix(data, "values", n_bins, n_frames);
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
  std::vector<float> data = float32ArrayToVector(chromagram, "chromagram");
  validateMatrix(data, "chromagram", n_chroma, n_frames);
  return vectorToFloat32Array(tonnetz(data.data(), n_chroma, n_frames));
}

TempogramMode tempogramModeFromValue(val mode) {
  if (mode.isUndefined() || mode.isNull()) return TempogramMode::kAutocorrelation;
  if (mode.typeOf().as<std::string>() == "number") {
    // An ordinal names a member, not a quantity: 0.5 used to select
    // autocorrelation and 1.5 cosine, neither of which the caller spelled.
    const int mode_id = checkedIntFromVal(mode, "mode");
    if (mode_id == SONARE_TEMPOGRAM_AUTOCORRELATION) return TempogramMode::kAutocorrelation;
    if (mode_id == SONARE_TEMPOGRAM_COSINE) return TempogramMode::kCosine;
    throw WasmRangeError("mode must be 'autocorrelation' or 'cosine'");
  }
  if (!mode.isString()) throw WasmTypeError("mode must be a string or a number");
  const std::string value = mode.as<std::string>();
  if (value == "autocorrelation" || value == "auto" || value == "ac") {
    return TempogramMode::kAutocorrelation;
  }
  if (value == "cosine") return TempogramMode::kCosine;
  throw WasmRangeError("mode must be 'autocorrelation' or 'cosine'");
}

val js_tempogram(val onset_envelope, const val& sample_rate_val, const val& hop_length_val,
                 const val& win_length_val, val mode, bool center, bool norm) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kRhythmCountDomain);
  const int win_length =
      domainNumberFromVal<int>(win_length_val, "winLength", kTempogramWinLengthDomain);
  std::vector<float> data = float32ArrayToVector(onset_envelope, "onsetEnvelope");
  validateFiniteVector(data, "onsetEnvelope");
  validatePositiveSampleRate(sample_rate);
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
                        const val& win_length_val, const val& bpm_min_val, const val& n_bins_val) {
  const int sample_rate =
      domainNumberFromVal<int>(sample_rate_val, "sampleRate", kRhythmCountDomain);
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kRhythmCountDomain);
  const int win_length =
      domainNumberFromVal<int>(win_length_val, "winLength", kTempogramWinLengthDomain);
  float bpm_min = kCyclicTempogramBpmMin;
  if (const auto v = optionalNumber(bpm_min_val, "bpmMin")) bpm_min = *v;
  const int n_bins = n_bins_val.isUndefined() || n_bins_val.isNull()
                         ? kCyclicTempogramBins
                         : domainNumberFromVal<int>(n_bins_val, "nBins", kRhythmCountDomain);
  std::vector<float> data = float32ArrayToVector(onset_envelope, "onsetEnvelope");
  validateFiniteVector(data, "onsetEnvelope");
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
           const val& bpm_min_val, const val& bpm_max_val, const val& win_length_val) {
  const int sample_rate =
      domainNumberFromVal<int>(sample_rate_val, "sampleRate", kRhythmCountDomain);
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kRhythmCountDomain);
  PlpConfig config;
  if (const auto v = optionalNumber(bpm_min_val, "bpmMin")) config.tempo_min = *v;
  if (const auto v = optionalNumber(bpm_max_val, "bpmMax")) config.tempo_max = *v;
  const int win_length =
      domainNumberFromVal<int>(win_length_val, "winLength", kTempogramWinLengthDomain);
  std::vector<float> data = float32ArrayToVector(onset_envelope, "onsetEnvelope");
  validateFiniteVector(data, "onsetEnvelope");
  config.sr = sample_rate;
  config.hop_length = hop_length;
  config.win_length = win_length;
  return vectorToFloat32Array(plp(data, config));
}

val js_onset_envelope(val samples, const val& sample_rate_val, const val& n_fft_val,
                      const val& hop_length_val, const val& n_mels_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  const int n_mels = domainNumberFromVal<int>(n_mels_val, "nMels", kMelBandsDomain);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
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
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  const int n_mels = domainNumberFromVal<int>(n_mels_val, "nMels", kMelBandsDomain);
  const int n_bands = domainNumberFromVal<int>(n_bands_val, "nBands", kOnsetCountDomain);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
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
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kRhythmCountDomain);
  const int win_length =
      domainNumberFromVal<int>(win_length_val, "winLength", kTempogramWinLengthDomain);
  std::vector<float> data = float32ArrayToVector(onset_envelope, "onsetEnvelope");
  validatePositiveSampleRate(sample_rate);
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
  const int win_length = domainNumberFromVal<int>(win_length_val, "winLength", kRhythmCountDomain);
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kRhythmCountDomain);
  const bool has_factors = !factors.isUndefined() && !factors.isNull();
  if (has_factors) {
    validateWasmFloat32ArrayPair(tempogram_data, "tempogram", factors, "factors", false);
  }
  std::vector<float> data = float32ArrayToVector(tempogram_data, "tempogram");
  validatePositiveSampleRate(sample_rate);
  // An undefined/null/empty factors argument falls back to the library default
  // {0.5, 1, 2, 3, 4}, matching the C and Node behaviour.
  if (!has_factors) {
    return vectorToFloat32Array(tempogram_ratio(data, win_length, sample_rate, hop_length));
  }
  std::vector<float> factor_values = float32ArrayToVector(factors, "factors");
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
  function("splitSilenceCommon", &js_split_silence_common);
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
