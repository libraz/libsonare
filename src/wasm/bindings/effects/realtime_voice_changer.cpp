/// @file realtime_voice_changer.cpp
/// @brief Embind bindings for realtime voice changer streaming APIs.

#ifdef __EMSCRIPTEN__

#include "wasm/bindings/common/common.h"

std::string realtimeVoiceChangerConfigTextFromVal(val config) {
  if (config.isNull() || config.isUndefined()) return "neutral-monitor";
  if (config.typeOf().as<std::string>() == "string") return config.as<std::string>();
  return val::global("JSON").call<std::string>("stringify", config);
}

// Flat POD transport used by the AudioWorklet control plane.  Unlike the
// general config path, this neither stringifies a JS object nor invokes the
// nested JSON parser on the audio rendering thread.
#define SONARE_WASM_VC_POD_FIELDS(X)                   \
  X(input_gain_db, inputGainDb)                        \
  X(output_gain_db, outputGainDb)                      \
  X(wet_mix, wetMix)                                   \
  X(retune.semitones, retuneSemitones)                 \
  X(retune.mix, retuneMix)                             \
  X(retune.grain_size, retuneGrainSize)                \
  X(formant.factor, formantFactor)                     \
  X(formant.amount, formantAmount)                     \
  X(formant.body, formantBody)                         \
  X(formant.brightness, formantBrightness)             \
  X(formant.nasal, formantNasal)                       \
  X(eq.highpass_hz, eqHighpassHz)                      \
  X(eq.body_db, eqBodyDb)                              \
  X(eq.presence_db, eqPresenceDb)                      \
  X(eq.air_db, eqAirDb)                                \
  X(gate.threshold_db, gateThresholdDb)                \
  X(gate.attack_ms, gateAttackMs)                      \
  X(gate.release_ms, gateReleaseMs)                    \
  X(gate.range_db, gateRangeDb)                        \
  X(compressor.threshold_db, compressorThresholdDb)    \
  X(compressor.ratio, compressorRatio)                 \
  X(compressor.attack_ms, compressorAttackMs)          \
  X(compressor.release_ms, compressorReleaseMs)        \
  X(compressor.makeup_gain_db, compressorMakeupGainDb) \
  X(deesser.frequency_hz, deesserFrequencyHz)          \
  X(deesser.threshold_db, deesserThresholdDb)          \
  X(deesser.ratio, deesserRatio)                       \
  X(deesser.range_db, deesserRangeDb)                  \
  X(reverb.mix, reverbMix)                             \
  X(reverb.time_ms, reverbTimeMs)                      \
  X(reverb.damping, reverbDamping)                     \
  X(reverb.seed, reverbSeed)                           \
  X(limiter.ceiling_db, limiterCeilingDb)              \
  X(limiter.release_ms, limiterReleaseMs)              \
  X(limiter.isp_ceiling_dbtp, limiterIspCeilingDbtp)

// Voice transformer fields: float scalars, whole numbers, then the string enums.
#define SONARE_WASM_VC_POD_TRANSFORM_FLOAT_FIELDS(X)           \
  X(voice_engine.f0_min_hz, voiceEngineF0MinHz)                \
  X(pitch_correction.amount, pitchCorrectionAmount)            \
  X(pitch_correction.speed_ms, pitchCorrectionSpeedMs)         \
  X(harmony.level, harmonyLevel)                               \
  X(harmony.lead_level, harmonyLeadLevel)                      \
  X(harmony.detune_cents, harmonyDetuneCents)                  \
  X(harmony.delay_ms, harmonyDelayMs)                          \
  X(vocoder.attack_ms, vocoderAttackMs)                        \
  X(vocoder.release_ms, vocoderReleaseMs)                      \
  X(vocoder.sibilance, vocoderSibilance)                       \
  X(vocoder.ensemble_detune_cents, vocoderEnsembleDetuneCents) \
  X(vocoder.level, vocoderLevel)                               \
  X(character.amount, characterAmount)                         \
  X(character.tone, characterTone)                             \
  X(reverb_variation.delay_ms, reverbVariationDelayMs)         \
  X(reverb_variation.feedback, reverbVariationFeedback)        \
  X(reverb_variation.tempo_bpm, reverbVariationTempoBpm)

#define SONARE_WASM_VC_POD_TRANSFORM_INT_FIELDS(X)             \
  X(pitch_correction.key_root, pitchCorrectionKeyRoot)         \
  X(pitch_correction.robot_octave, pitchCorrectionRobotOctave) \
  X(harmony.voices, harmonyVoices)                             \
  X(harmony.steps[0], harmonyStep1)                            \
  X(harmony.steps[1], harmonyStep2)                            \
  X(harmony.steps[2], harmonyStep3)                            \
  X(vocoder.bands, vocoderBands)

namespace {

constexpr char kVcPodSubject[] = "voice changer POD config";

// Spellings of the string enums in the flat POD: each enumerator in snake_case.
struct VcEnumName {
  int value;
  const char* name;
};

constexpr VcEnumName kVcEngineNames[] = {{0, "grain"}, {1, "psola"}};
constexpr VcEnumName kVcCorrectionModeNames[] = {{0, "off"}, {1, "auto"}, {2, "robot"}};
constexpr VcEnumName kVcScaleNames[] = {{0, "major"}, {1, "natural_minor"}, {2, "chromatic"}};
constexpr VcEnumName kVcCarrierSourceNames[] = {{0, "internal"}, {1, "external"}};
constexpr VcEnumName kVcCarrierWaveNames[] = {{0, "saw"}, {1, "pulse"}, {2, "ensemble"}};
constexpr VcEnumName kVcCharacterNames[] = {
    {0, "none"}, {1, "megaphone"}, {2, "radio"}, {3, "bbd_chorus"}, {4, "strobo"}};
constexpr VcEnumName kVcVariationNames[] = {
    {0, "reverb"}, {1, "echo"}, {2, "tempo_delay"}, {3, "dub_echo"}};
constexpr VcEnumName kVcDivisionNames[] = {
    {0, "quarter"}, {1, "eighth"}, {2, "dotted_eighth"}, {3, "sixteenth"}};
constexpr VcEnumName kVcMidiReleaseNames[] = {{0, "hold"}, {1, "input"}};

template <typename Enum, size_t N>
Enum vcParseEnum(const val& pod, const char* key, const VcEnumName (&names)[N]) {
  const std::string text =
      requireTypedProperty(pod, key, kVcPodSubject, "string", "a string").as<std::string>();
  for (const VcEnumName& row : names) {
    if (text == row.name) return static_cast<Enum>(row.value);
  }
  std::string accepted;
  for (size_t i = 0; i < N; ++i) {
    if (i > 0) accepted += i + 1 == N ? " or " : ", ";
    accepted += std::string("'") + names[i].name + "'";
  }
  throw WasmRangeError(std::string(kVcPodSubject) + "." + key + " must be " + accepted + ", got '" +
                       text + "'");
}

template <typename Enum, size_t N>
const char* vcEnumName(const VcEnumName (&names)[N], Enum value) {
  for (const VcEnumName& row : names) {
    if (row.value == static_cast<int>(value)) return row.name;
  }
  return names[0].name;
}

}  // namespace

// The flat camelCase POD of a config: the object realtimeVoiceChangerPresetConfig() returns.
val realtimeVoiceChangerPodVal(const editing::voice_changer::RealtimeVoiceChangerConfig& cfg) {
  val out = val::object();
#define X(cpp_path, js_key) out.set(#js_key, cfg.cpp_path);
  SONARE_WASM_VC_POD_FIELDS(X)
  SONARE_WASM_VC_POD_TRANSFORM_FLOAT_FIELDS(X)
  SONARE_WASM_VC_POD_TRANSFORM_INT_FIELDS(X)
#undef X
  out.set("limiterEnableIspLimiter", cfg.limiter.enable_isp_limiter);
  out.set("formantMode", std::string(editing::voice_changer::formant_mode_name(cfg.formant_mode)));
  out.set("voiceEngineEngine", std::string(vcEnumName(kVcEngineNames, cfg.voice_engine.engine)));
  out.set("pitchCorrectionMode",
          std::string(vcEnumName(kVcCorrectionModeNames, cfg.pitch_correction.mode)));
  out.set("pitchCorrectionScale",
          std::string(vcEnumName(kVcScaleNames, cfg.pitch_correction.scale)));
  out.set("vocoderEnabled", cfg.vocoder.enabled);
  out.set("vocoderCarrierSource",
          std::string(vcEnumName(kVcCarrierSourceNames, cfg.vocoder.carrier_source)));
  out.set("vocoderCarrierWave",
          std::string(vcEnumName(kVcCarrierWaveNames, cfg.vocoder.carrier_wave)));
  out.set("characterType", std::string(vcEnumName(kVcCharacterNames, cfg.character.type)));
  out.set("reverbVariationVariation",
          std::string(vcEnumName(kVcVariationNames, cfg.reverb_variation.variation)));
  out.set("reverbVariationDivision",
          std::string(vcEnumName(kVcDivisionNames, cfg.reverb_variation.division)));
  out.set("midiRelease", std::string(vcEnumName(kVcMidiReleaseNames, cfg.midi.release)));
  return out;
}

editing::voice_changer::RealtimeVoiceChangerConfig realtimeVoiceChangerConfigFromPodVal(val pod) {
  if (pod.isNull() || pod.isUndefined() || pod.typeOf().as<std::string>() != "object") {
    throw WasmTypeError("voice changer POD config must be an object");
  }
  using namespace editing::voice_changer;
  RealtimeVoiceChangerConfig parsed;
  // Every field is REQUIRED: unlike the general JSON path (which normalizes a
  // partial preset against defaults), the flat POD transport used by the
  // AudioWorklet control plane must reject a partial object rather than
  // silently zero-fill it. A missing limiterEnableIspLimiter used to read as
  // JS `false` here, which can turn the ISP limiter off and let the DAC clip.
#define X(cpp_path, js_key) \
  parsed.cpp_path = requireProperty<decltype(parsed.cpp_path)>(pod, #js_key, kVcPodSubject);
  SONARE_WASM_VC_POD_FIELDS(X)
  SONARE_WASM_VC_POD_TRANSFORM_FLOAT_FIELDS(X)
#undef X
#define X(cpp_path, js_key) parsed.cpp_path = requireIntProperty(pod, #js_key, kVcPodSubject);
  SONARE_WASM_VC_POD_TRANSFORM_INT_FIELDS(X)
#undef X
  parsed.limiter.enable_isp_limiter =
      requireProperty<bool>(pod, "limiterEnableIspLimiter", kVcPodSubject);
  parsed.formant_mode = parse_formant_mode(
      requireTypedProperty(pod, "formantMode", kVcPodSubject, "string", "a string")
          .as<std::string>());
  parsed.voice_engine.engine = vcParseEnum<PitchEngine>(pod, "voiceEngineEngine", kVcEngineNames);
  parsed.pitch_correction.mode =
      vcParseEnum<PitchCorrectionMode>(pod, "pitchCorrectionMode", kVcCorrectionModeNames);
  parsed.pitch_correction.scale =
      vcParseEnum<ScaleKind>(pod, "pitchCorrectionScale", kVcScaleNames);
  parsed.vocoder.enabled = requireProperty<bool>(pod, "vocoderEnabled", kVcPodSubject);
  parsed.vocoder.carrier_source =
      vcParseEnum<VocoderCarrierSource>(pod, "vocoderCarrierSource", kVcCarrierSourceNames);
  parsed.vocoder.carrier_wave =
      vcParseEnum<VocoderCarrierWave>(pod, "vocoderCarrierWave", kVcCarrierWaveNames);
  parsed.character.type = vcParseEnum<CharacterType>(pod, "characterType", kVcCharacterNames);
  parsed.reverb_variation.variation =
      vcParseEnum<ReverbVariation>(pod, "reverbVariationVariation", kVcVariationNames);
  parsed.reverb_variation.division =
      vcParseEnum<NoteDivision>(pod, "reverbVariationDivision", kVcDivisionNames);
  parsed.midi.release = vcParseEnum<MidiRelease>(pod, "midiRelease", kVcMidiReleaseNames);
  std::string error;
  if (!validate_realtime_voice_changer_config(parsed, nullptr, &error)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, error);
  }
  return parsed;
}

// A refused preset id or nested document is InvalidFormat; the flat POD (recognised by its
// `retuneSemitones` key) is a per-call parameter bag and stays InvalidParameter.
editing::voice_changer::RealtimeVoiceChangerConfig realtimeVoiceChangerConfigFromVal(val config) {
  editing::voice_changer::RealtimeVoiceChangerConfig parsed;
  std::string error;
  if (!editing::voice_changer::realtime_voice_changer_config_from_input(
          realtimeVoiceChangerConfigTextFromVal(config), &parsed, &error)) {
    const bool flat_pod = config.typeOf().as<std::string>() == "object" && !config.isNull() &&
                          !config["retuneSemitones"].isUndefined();
    throw sonare::SonareException(
        flat_pod ? sonare::ErrorCode::InvalidParameter : sonare::ErrorCode::InvalidFormat, error);
  }
  return parsed;
}

class RealtimeVoiceChangerWrapper {
 public:
  explicit RealtimeVoiceChangerWrapper(val config)
      : changer_(realtimeVoiceChangerConfigFromVal(config)) {}

  void prepare(double sample_rate, const val& max_block_size_val, const val& channels_val) {
    const int max_block_size = checkedIntFromVal(max_block_size_val, "maxBlockSize");
    const int channels = checkedIntFromVal(channels_val, "channels");
    changer_.prepare(sample_rate, max_block_size, channels);
    // Pre-warm the per-instance scratch buffers so the first process* call
    // does not trigger an allocation. The `ensure_*_capacity` helpers only
    // grow; once warmed up to (channels, max_block_size) they stay that size.
    ensure_mono_capacity(static_cast<size_t>(max_block_size));
    ensure_interleaved_capacity(static_cast<size_t>(max_block_size), channels);
    max_block_size_ = max_block_size;
    prepared_channels_ = channels;
    prepared_ = true;
    ++buffer_generation_;
  }

  void reset() { changer_.reset(); }

  void setConfig(val config) { changer_.set_config(realtimeVoiceChangerConfigFromVal(config)); }

  void setPodConfig(val config) {
    changer_.set_config(realtimeVoiceChangerConfigFromPodVal(config));
  }

  std::string configJson() const {
    return editing::voice_changer::realtime_voice_changer_config_to_json(changer_.config());
  }

  int latencySamples() const { return changer_.latency_samples(); }
  uint32_t nonFiniteDiscardCount() const { return changer_.non_finite_discard_count(); }
  uint32_t bufferGeneration() const { return buffer_generation_; }

  // Element-wise legacy path. NOT RT-safe for high block rates; AudioWorklet
  // consumers should prefer the prepared API below (getMonoInputBuffer /
  // processPreparedMono / getMonoOutputBuffer) which avoids per-sample JS↔C++
  // crossings and per-call allocations entirely.
  val processMono(val samples) { return process_mono(samples, val::undefined()); }

  val processMonoWithCarrier(val samples, val carrier) { return process_mono(samples, carrier); }

  void processMonoInto(val samples, val output) {
    process_mono_into(samples, val::undefined(), output);
  }

  void processMonoWithCarrierInto(val samples, val carrier, val output) {
    process_mono_into(samples, carrier, output);
  }

  val processInterleaved(val samples, const val& channels_val) {
    return process_interleaved(samples, val::undefined(), channels_val);
  }

  val processInterleavedWithCarrier(val samples, val carrier, const val& channels_val) {
    return process_interleaved(samples, carrier, channels_val);
  }

  void processInterleavedInto(val samples, const val& channels_val, val output) {
    process_interleaved_into(samples, val::undefined(), channels_val, output);
  }

  void processInterleavedWithCarrierInto(val samples, val carrier, const val& channels_val,
                                         val output) {
    process_interleaved_into(samples, carrier, channels_val, output);
  }

  // ---- MIDI notes --------------------------------------------------------

  void noteOn(const val& note_val, const val& velocity_val) {
    const int note = checkedMidiNoteFromVal(note_val, "RealtimeVoiceChanger");
    if (!velocity_val.isNumber()) {
      throw WasmTypeError("RealtimeVoiceChanger.noteOn: velocity must be a number");
    }
    const double velocity = velocity_val.as<double>();
    if (!(velocity > 0.0 && velocity <= 1.0)) {
      throw WasmRangeError("RealtimeVoiceChanger.noteOn: velocity must be in (0, 1]");
    }
    require_note_accepted(changer_.note_on(note, static_cast<float>(velocity)), "noteOn");
  }

  void noteOff(const val& note_val) {
    require_note_accepted(
        changer_.note_off(checkedMidiNoteFromVal(note_val, "RealtimeVoiceChanger")), "noteOff");
  }

  void allNotesOff() { changer_.all_notes_off(); }

  // ---- Zero-copy "prepared" API ----------------------------------------
  // Caller fills the input view (returned as a typed_memory_view onto the
  // WASM heap), calls processPrepared*, then reads the output view. No JS↔C++
  // sample-level crossings and no allocations on the audio thread.

  val getMonoInputBuffer(const val& num_samples_val) {
    const int num_samples = checkedIntFromVal(num_samples_val, "numSamples");
    require_prepared();
    if (num_samples <= 0 || num_samples > max_block_size_) {
      throw WasmRangeError("RealtimeVoiceChanger.getMonoInputBuffer: out-of-range length");
    }
    ensure_mono_capacity(static_cast<size_t>(num_samples));
    return val(typed_memory_view(static_cast<size_t>(num_samples), mono_input_.data()));
  }

  val getMonoOutputBuffer(const val& num_samples_val) {
    const int num_samples = checkedIntFromVal(num_samples_val, "numSamples");
    require_prepared();
    if (num_samples <= 0 || num_samples > max_block_size_) {
      throw WasmRangeError("RealtimeVoiceChanger.getMonoOutputBuffer: out-of-range length");
    }
    ensure_mono_capacity(static_cast<size_t>(num_samples));
    return val(typed_memory_view(static_cast<size_t>(num_samples), mono_output_.data()));
  }

  void processPreparedMono(const val& num_samples_val) {
    process_prepared_mono(num_samples_val, false);
  }

  /// Carrier view counterpart to getMonoInputBuffer: fill it, then call
  /// processPreparedMonoWithCarrier / processPreparedInterleavedWithCarrier /
  /// processPreparedPlanarWithCarrier. One mono carrier is shared by every channel.
  val getCarrierBuffer(const val& num_samples_val) {
    const int num_samples = checkedIntFromVal(num_samples_val, "numSamples");
    require_prepared();
    if (num_samples <= 0 || num_samples > max_block_size_) {
      throw WasmRangeError("RealtimeVoiceChanger.getCarrierBuffer: out-of-range length");
    }
    ensure_mono_capacity(static_cast<size_t>(num_samples));
    return val(typed_memory_view(static_cast<size_t>(num_samples), carrier_.data()));
  }

  void processPreparedMonoWithCarrier(const val& num_samples_val) {
    process_prepared_mono(num_samples_val, true);
  }

  val getInterleavedInputBuffer(const val& num_frames_val, const val& num_channels_val) {
    const int num_frames = checkedIntFromVal(num_frames_val, "numFrames");
    const int num_channels = checkedIntFromVal(num_channels_val, "numChannels");
    require_prepared();
    if (num_frames <= 0 || num_channels <= 0) {
      throw WasmRangeError("RealtimeVoiceChanger.getInterleavedInputBuffer: bad dims");
    }
    if (num_frames > max_block_size_) {
      throw WasmRangeError(
          "RealtimeVoiceChanger.getInterleavedInputBuffer: frames exceed max block size");
    }
    require_prepared_channels(num_channels);
    ensure_interleaved_capacity(static_cast<size_t>(num_frames), num_channels);
    const size_t length = static_cast<size_t>(num_frames) * static_cast<size_t>(num_channels);
    return val(typed_memory_view(length, interleaved_input_.data()));
  }

  val getInterleavedOutputBuffer(const val& num_frames_val, const val& num_channels_val) {
    const int num_frames = checkedIntFromVal(num_frames_val, "numFrames");
    const int num_channels = checkedIntFromVal(num_channels_val, "numChannels");
    require_prepared();
    if (num_frames <= 0 || num_channels <= 0) {
      throw WasmRangeError("RealtimeVoiceChanger.getInterleavedOutputBuffer: bad dims");
    }
    if (num_frames > max_block_size_) {
      throw WasmRangeError(
          "RealtimeVoiceChanger.getInterleavedOutputBuffer: frames exceed max block size");
    }
    require_prepared_channels(num_channels);
    ensure_interleaved_capacity(static_cast<size_t>(num_frames), num_channels);
    const size_t length = static_cast<size_t>(num_frames) * static_cast<size_t>(num_channels);
    return val(typed_memory_view(length, interleaved_output_.data()));
  }

  void processPreparedInterleaved(const val& num_frames_val, const val& num_channels_val) {
    process_prepared_interleaved(num_frames_val, num_channels_val, false);
  }

  void processPreparedInterleavedWithCarrier(const val& num_frames_val,
                                             const val& num_channels_val) {
    process_prepared_interleaved(num_frames_val, num_channels_val, true);
  }

  // ---- Planar zero-copy stereo path -----------------------------------
  // Match AudioWorklet's native planar layout: each channel is its own
  // Float32Array, so the worklet can hand the in/out buffers straight
  // through with no interleave/deinterleave passes.

  val getPlanarChannelBuffer(const val& channel_val, const val& num_frames_val) {
    const int channel = checkedIntFromVal(channel_val, "channel");
    const int num_frames = checkedIntFromVal(num_frames_val, "numFrames");
    require_prepared();
    if (num_frames <= 0) {
      throw WasmRangeError("RealtimeVoiceChanger.getPlanarChannelBuffer: bad frames");
    }
    if (num_frames > max_block_size_) {
      throw WasmRangeError(
          "RealtimeVoiceChanger.getPlanarChannelBuffer: frames exceed max block size");
    }
    if (channel < 0 || channel >= prepared_channels_) {
      throw WasmRangeError("RealtimeVoiceChanger.getPlanarChannelBuffer: channel out of range");
    }
    ensure_interleaved_capacity(static_cast<size_t>(num_frames), prepared_channels_);
    return val(typed_memory_view(static_cast<size_t>(num_frames),
                                 planar_[static_cast<size_t>(channel)].data()));
  }

  void processPreparedPlanar(const val& num_frames_val) {
    process_prepared_planar(num_frames_val, false);
  }

  void processPreparedPlanarWithCarrier(const val& num_frames_val) {
    process_prepared_planar(num_frames_val, true);
  }

 private:
  static void require_note_accepted(bool accepted, const char* method) {
    if (!accepted) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("RealtimeVoiceChanger.") + method +
              ": the event was not accepted (grain engine, unprepared chain or full event queue)");
    }
  }

  static void require_processed(bool processed) {
    if (!processed) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          "RealtimeVoiceChanger: block not processed; the configuration takes an external vocoder "
          "carrier, so use the WithCarrier process methods");
    }
  }

  /// Copies a JS carrier into the scratch and returns it; undefined/null means none.
  const float* load_carrier(const val& carrier, int length) {
    if (carrier.isUndefined() || carrier.isNull()) return nullptr;
    if (static_cast<int>(wasmArrayLikeLength(carrier, "carrier")) != length) {
      throw WasmRangeError("RealtimeVoiceChanger: carrier length must equal the input length");
    }
    ensure_mono_capacity(static_cast<size_t>(length));
    copyFloat32Array(carrier, carrier_.data(), static_cast<size_t>(length));
    return carrier_.data();
  }

  val process_mono(val samples, const val& carrier) {
    require_prepared();
    const int length = static_cast<int>(wasmArrayLikeLength(samples, "samples"));
    require_block_within_max(length);
    ensure_mono_capacity(static_cast<size_t>(length));
    copyFloat32Array(samples, mono_input_.data(), static_cast<size_t>(length));
    const float* carrier_ptr = load_carrier(carrier, length);
    require_processed(
        changer_.process_block(mono_input_.data(), carrier_ptr, mono_output_.data(), length));
    val output = val::global("Float32Array").new_(length);
    val view = val(typed_memory_view(static_cast<size_t>(length), mono_output_.data()));
    output.call<void>("set", view);
    return output;
  }

  void process_mono_into(val samples, const val& carrier, val output) {
    require_prepared();
    const int length = static_cast<int>(wasmArrayLikeLength(samples, "samples"));
    require_block_within_max(length);
    if (static_cast<int>(wasmArrayLikeLength(output, "output")) < length) {
      throw WasmRangeError("output buffer is too small");
    }
    ensure_mono_capacity(static_cast<size_t>(length));
    copyFloat32Array(samples, mono_input_.data(), static_cast<size_t>(length));
    const float* carrier_ptr = load_carrier(carrier, length);
    require_processed(
        changer_.process_block(mono_input_.data(), carrier_ptr, mono_output_.data(), length));
    val view = val(typed_memory_view(static_cast<size_t>(length), mono_output_.data()));
    output.call<void>("set", view);
  }

  val process_interleaved(val samples, const val& carrier, const val& channels_val) {
    require_prepared();
    const int length = static_cast<int>(wasmArrayLikeLength(samples, "samples"));
    val output = val::global("Float32Array").new_(length);
    // Forwarded unnarrowed: process_interleaved_into narrows it under the same key.
    process_interleaved_into(samples, carrier, channels_val, output);
    return output;
  }

  void process_interleaved_into(val samples, const val& carrier, const val& channels_val,
                                val output) {
    const int channels = checkedIntFromVal(channels_val, "channels");
    require_prepared();
    const int length = static_cast<int>(wasmArrayLikeLength(samples, "samples"));
    if (channels <= 0 || length % channels != 0) {
      throw WasmRangeError("invalid interleaved channel count");
    }
    require_prepared_channels(channels);
    if (static_cast<int>(wasmArrayLikeLength(output, "output")) < length) {
      throw WasmRangeError("output buffer is too small");
    }
    const size_t frames = static_cast<size_t>(length / channels);
    require_block_within_max(static_cast<int>(frames));
    ensure_interleaved_capacity(frames, channels);
    copyFloat32Array(samples, interleaved_input_.data(), static_cast<size_t>(length));
    // The carrier is mono: one sample per frame, shared by every channel.
    const float* carrier_ptr = load_carrier(carrier, static_cast<int>(frames));
    for (int ch = 0; ch < channels; ++ch) {
      for (size_t i = 0; i < frames; ++i) {
        planar_[static_cast<size_t>(ch)][i] =
            interleaved_input_[i * static_cast<size_t>(channels) + static_cast<size_t>(ch)];
      }
    }
    require_processed(changer_.process_block(channel_ptrs_.data(), channels, carrier_ptr,
                                             static_cast<int>(frames)));
    for (size_t i = 0; i < frames; ++i) {
      for (int ch = 0; ch < channels; ++ch) {
        interleaved_output_[i * static_cast<size_t>(channels) + static_cast<size_t>(ch)] =
            planar_[static_cast<size_t>(ch)][i];
      }
    }
    output.call<void>(
        "set", val(typed_memory_view(static_cast<size_t>(length), interleaved_output_.data())));
  }

  void process_prepared_mono(const val& num_samples_val, bool with_carrier) {
    const char* method = with_carrier ? "processPreparedMonoWithCarrier" : "processPreparedMono";
    const int num_samples = checkedIntFromVal(num_samples_val, "numSamples");
    require_prepared();
    if (num_samples <= 0 || num_samples > max_block_size_) {
      throw WasmRangeError(std::string("RealtimeVoiceChanger.") + method + ": out-of-range length");
    }
    if (mono_input_.size() < static_cast<size_t>(num_samples) ||
        mono_output_.size() < static_cast<size_t>(num_samples) ||
        carrier_.size() < static_cast<size_t>(num_samples)) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          std::string("RealtimeVoiceChanger.") + method +
              ": getMonoInputBuffer/getMonoOutputBuffer must be called first");
    }
    require_processed(changer_.process_block(mono_input_.data(),
                                             with_carrier ? carrier_.data() : nullptr,
                                             mono_output_.data(), num_samples));
  }

  void process_prepared_interleaved(const val& num_frames_val, const val& num_channels_val,
                                    bool with_carrier) {
    const char* method =
        with_carrier ? "processPreparedInterleavedWithCarrier" : "processPreparedInterleaved";
    const int num_frames = checkedIntFromVal(num_frames_val, "numFrames");
    const int num_channels = checkedIntFromVal(num_channels_val, "numChannels");
    require_prepared();
    if (num_frames <= 0 || num_channels <= 0) {
      throw WasmRangeError(std::string("RealtimeVoiceChanger.") + method + ": bad dims");
    }
    if (num_frames > max_block_size_) {
      throw WasmRangeError(std::string("RealtimeVoiceChanger.") + method +
                           ": frames exceed max block size");
    }
    require_prepared_channels(num_channels);
    const size_t frames = static_cast<size_t>(num_frames);
    const size_t channel_count = static_cast<size_t>(num_channels);
    const size_t length = frames * channel_count;
    if (interleaved_input_.size() < length || interleaved_output_.size() < length ||
        planar_.size() < channel_count || carrier_.size() < frames) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          std::string("RealtimeVoiceChanger.") + method +
              ": getInterleavedInputBuffer/getInterleavedOutputBuffer must be called first with "
              "matching dims");
    }
    for (size_t ch = 0; ch < channel_count; ++ch) {
      float* dst = planar_[ch].data();
      const float* src = interleaved_input_.data() + ch;
      for (size_t i = 0; i < frames; ++i) {
        dst[i] = src[i * channel_count];
      }
    }
    require_processed(changer_.process_block(channel_ptrs_.data(), num_channels,
                                             with_carrier ? carrier_.data() : nullptr, num_frames));
    for (size_t ch = 0; ch < channel_count; ++ch) {
      const float* src = planar_[ch].data();
      float* dst = interleaved_output_.data() + ch;
      for (size_t i = 0; i < frames; ++i) {
        dst[i * channel_count] = src[i];
      }
    }
  }

  void process_prepared_planar(const val& num_frames_val, bool with_carrier) {
    const char* method =
        with_carrier ? "processPreparedPlanarWithCarrier" : "processPreparedPlanar";
    const int num_frames = checkedIntFromVal(num_frames_val, "numFrames");
    require_prepared();
    if (num_frames <= 0) {
      throw WasmRangeError(std::string("RealtimeVoiceChanger.") + method + ": bad frames");
    }
    if (num_frames > max_block_size_) {
      throw WasmRangeError(std::string("RealtimeVoiceChanger.") + method +
                           ": frames exceed max block size");
    }
    const size_t channel_count = static_cast<size_t>(prepared_channels_);
    if (planar_.size() < channel_count || carrier_.size() < static_cast<size_t>(num_frames)) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          std::string("RealtimeVoiceChanger.") + method +
              ": getPlanarChannelBuffer must be called for each channel before processing");
    }
    for (size_t ch = 0; ch < channel_count; ++ch) {
      if (planar_[ch].size() < static_cast<size_t>(num_frames)) {
        throw WasmRangeError(std::string("RealtimeVoiceChanger.") + method +
                             ": planar buffer too small for requested frames");
      }
    }
    require_processed(changer_.process_block(channel_ptrs_.data(), prepared_channels_,
                                             with_carrier ? carrier_.data() : nullptr, num_frames));
  }

  static void copyFloat32Array(val source, float* destination, size_t length) {
    val(typed_memory_view(length, destination)).call<void>("set", source);
  }

  void ensure_mono_capacity(size_t samples) {
    if (mono_input_.size() < samples) {
      mono_input_.resize(samples);
      mono_output_.resize(samples);
    }
    if (carrier_.size() < samples) carrier_.resize(samples);
  }

  void ensure_interleaved_capacity(size_t frames, int channels) {
    const size_t channel_count = static_cast<size_t>(channels);
    if (planar_.size() < channel_count) planar_.resize(channel_count);
    if (channel_ptrs_.size() < channel_count) channel_ptrs_.resize(channel_count, nullptr);
    for (size_t ch = 0; ch < channel_count; ++ch) {
      if (planar_[ch].size() < frames) planar_[ch].resize(frames);
      channel_ptrs_[ch] = planar_[ch].data();
    }
    const size_t length = frames * channel_count;
    if (interleaved_input_.size() < length) interleaved_input_.resize(length);
    if (interleaved_output_.size() < length) interleaved_output_.resize(length);
  }

  void require_prepared() const {
    if (!prepared_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.prepare() must be called before processing");
    }
  }

  /// Reject a per-call block that exceeds the prepared max_block_size. The core
  /// process_block early-returns (emitting stale/garbage scratch) for an
  /// oversized block; the prepared API already guards this, so the legacy
  /// element-wise paths must too — mirroring the C-ABI behaviour of throwing.
  void require_block_within_max(int block) const {
    if (block > max_block_size_) {
      throw WasmRangeError(
          "RealtimeVoiceChanger: block size exceeds the prepared "
          "max block size");
    }
  }

  /// Accept any channel count in [1, prepared_channels_], matching the C-ABI
  /// oracle (sonare_realtime_voice_changer_process_interleaved rejects only
  /// num_channels < 1 || num_channels > handle->num_channels) and the Node and
  /// Python surfaces. The changer is prepared for prepared_channels_, and the
  /// grow-only scratch is already sized for that maximum, so processing a
  /// narrower layout (e.g. a mono block on a stereo-prepared instance) is safe.
  /// A count above the prepared maximum is rejected: it would exceed the
  /// allocated planar state the changer was configured for.
  void require_prepared_channels(int channels) const {
    if (channels < 1 || channels > prepared_channels_) {
      throw WasmRangeError(
          "RealtimeVoiceChanger: channel count must be between 1 and the prepared layout");
    }
  }

  editing::voice_changer::RealtimeVoiceChanger changer_;
  std::vector<float> mono_input_;
  std::vector<float> mono_output_;
  std::vector<float> carrier_;
  std::vector<std::vector<float>> planar_;
  std::vector<float*> channel_ptrs_;
  std::vector<float> interleaved_input_;
  std::vector<float> interleaved_output_;
  int max_block_size_ = 0;
  int prepared_channels_ = 0;
  uint32_t buffer_generation_ = 0;
  bool prepared_ = false;
};

RealtimeVoiceChangerWrapper* createRealtimeVoiceChanger(val config) {
  return new RealtimeVoiceChangerWrapper(config);
}

val realtimeVoiceChangerPresetNames() {
  return stringVectorToVal(editing::voice_changer::realtime_voice_changer_preset_names());
}

std::string realtimeVoiceChangerPresetJson(const std::string& id) {
  wasmRefuseEmbeddedNul(id, "presetId");
  return editing::voice_changer::realtime_voice_changer_preset_json(
      editing::voice_changer::realtime_voice_changer_preset_from_id(id));
}

val validateRealtimeVoiceChangerPresetJson(const std::string& json) {
  wasmRefuseEmbeddedNul(json, "json");
  // Full schema-level validation (schemaVersion, id/name string limits,
  // unknown-key rejection, every value range) — must match the C/Node/
  // Python contract. Earlier this only did a from_json→to_json roundtrip,
  // which silently accepted incomplete presets.
  val out = val::object();
  try {
    std::string normalized;
    std::string error;
    if (editing::voice_changer::validate_realtime_voice_changer_preset_json(json, &normalized,
                                                                            &error)) {
      out.set("ok", true);
      out.set("normalizedJson", normalized);
    } else {
      out.set("ok", false);
      out.set("error", error.empty() ? std::string("invalid preset JSON") : error);
    }
  } catch (const std::exception& ex) {
    out.set("ok", false);
    out.set("error", std::string(ex.what()));
  }
  return out;
}

void registerRealtimeVoiceChangerStreamingBindings() {
  class_<RealtimeVoiceChangerWrapper>("RealtimeVoiceChanger")
      .function("prepare", &RealtimeVoiceChangerWrapper::prepare)
      .function("reset", &RealtimeVoiceChangerWrapper::reset)
      .function("setConfig", &RealtimeVoiceChangerWrapper::setConfig)
      .function("setPodConfig", &RealtimeVoiceChangerWrapper::setPodConfig)
      .function("configJson", &RealtimeVoiceChangerWrapper::configJson)
      .function("latencySamples", &RealtimeVoiceChangerWrapper::latencySamples)
      .function("nonFiniteDiscardCount", &RealtimeVoiceChangerWrapper::nonFiniteDiscardCount)
      .function("bufferGeneration", &RealtimeVoiceChangerWrapper::bufferGeneration)
      .function("processMono", &RealtimeVoiceChangerWrapper::processMono)
      .function("processMonoInto", &RealtimeVoiceChangerWrapper::processMonoInto)
      .function("processInterleaved", &RealtimeVoiceChangerWrapper::processInterleaved)
      .function("processInterleavedInto", &RealtimeVoiceChangerWrapper::processInterleavedInto)
      .function("processMonoWithCarrier", &RealtimeVoiceChangerWrapper::processMonoWithCarrier)
      .function("processMonoWithCarrierInto",
                &RealtimeVoiceChangerWrapper::processMonoWithCarrierInto)
      .function("processInterleavedWithCarrier",
                &RealtimeVoiceChangerWrapper::processInterleavedWithCarrier)
      .function("processInterleavedWithCarrierInto",
                &RealtimeVoiceChangerWrapper::processInterleavedWithCarrierInto)
      .function("noteOn", &RealtimeVoiceChangerWrapper::noteOn)
      .function("noteOff", &RealtimeVoiceChangerWrapper::noteOff)
      .function("allNotesOff", &RealtimeVoiceChangerWrapper::allNotesOff)
      .function("getCarrierBuffer", &RealtimeVoiceChangerWrapper::getCarrierBuffer)
      .function("processPreparedMonoWithCarrier",
                &RealtimeVoiceChangerWrapper::processPreparedMonoWithCarrier)
      .function("processPreparedInterleavedWithCarrier",
                &RealtimeVoiceChangerWrapper::processPreparedInterleavedWithCarrier)
      .function("processPreparedPlanarWithCarrier",
                &RealtimeVoiceChangerWrapper::processPreparedPlanarWithCarrier)
      .function("getMonoInputBuffer", &RealtimeVoiceChangerWrapper::getMonoInputBuffer)
      .function("getMonoOutputBuffer", &RealtimeVoiceChangerWrapper::getMonoOutputBuffer)
      .function("processPreparedMono", &RealtimeVoiceChangerWrapper::processPreparedMono)
      .function("getInterleavedInputBuffer",
                &RealtimeVoiceChangerWrapper::getInterleavedInputBuffer)
      .function("getInterleavedOutputBuffer",
                &RealtimeVoiceChangerWrapper::getInterleavedOutputBuffer)
      .function("processPreparedInterleaved",
                &RealtimeVoiceChangerWrapper::processPreparedInterleaved)
      .function("getPlanarChannelBuffer", &RealtimeVoiceChangerWrapper::getPlanarChannelBuffer)
      .function("processPreparedPlanar", &RealtimeVoiceChangerWrapper::processPreparedPlanar);
  function("createRealtimeVoiceChanger", &createRealtimeVoiceChanger, allow_raw_pointers());
  function("realtimeVoiceChangerPresetNames", &realtimeVoiceChangerPresetNames);
  function("realtimeVoiceChangerPresetJson", &realtimeVoiceChangerPresetJson);
  function("validateRealtimeVoiceChangerPresetJson", &validateRealtimeVoiceChangerPresetJson);
}

#endif  // __EMSCRIPTEN__
