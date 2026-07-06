/// @file mixing.cpp
/// @brief Embind bindings for scene-based and one-shot mixing APIs.

#ifdef __EMSCRIPTEN__

#include "mixing_wasm.h"

val js_mixing_scene_preset_names() {
  val out = val::array();
  auto names = mixing::api::scene_preset_names();
  for (size_t index = 0; index < names.size(); ++index) {
    out.call<void>("push", names[index]);
  }
  return out;
}

std::string js_mixing_scene_preset_json(std::string preset_name) {
  return mixing::api::scene_to_json(
      mixing::api::scene_preset(mixing::api::scene_preset_from_string(preset_name)));
}

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)
MixerWasm::MixerWasm(SonareMixer* mixer, int sample_rate, int block_size)
    : mixer_(mixer), sample_rate_(sample_rate), block_size_(block_size) {
  if (block_size_ <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "mixer block size must be positive");
  }
  const size_t strip_count = sonare_mixer_strip_count(mixer_);
  left_scratch_.resize(strip_count);
  right_scratch_.resize(strip_count);
  left_ptrs_.resize(strip_count);
  right_ptrs_.resize(strip_count);
  for (size_t index = 0; index < strip_count; ++index) {
    left_scratch_[index].resize(static_cast<size_t>(block_size_));
    right_scratch_[index].resize(static_cast<size_t>(block_size_));
    left_ptrs_[index] = left_scratch_[index].data();
    right_ptrs_[index] = right_scratch_[index].data();
  }
  out_scratch_left_.resize(static_cast<size_t>(block_size_));
  out_scratch_right_.resize(static_cast<size_t>(block_size_));
}

MixerWasm::~MixerWasm() {
  if (mixer_ != nullptr) {
    sonare_mixer_destroy(mixer_);
    mixer_ = nullptr;
  }
}

MixerWasm* MixerWasm::fromSceneJson(std::string json, int sample_rate, int block_size) {
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), sample_rate, block_size);
  if (mixer == nullptr) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to build mixer from scene JSON: ") + sonare_last_error_message());
  }
  // Capture any non-fatal load warning (e.g. insert params no processor read)
  // before any later C-ABI call can overwrite the thread-local message.
  std::string warning = sonare_last_warning_message();
  auto* wrapped = new MixerWasm(mixer, sample_rate, block_size);
  wrapped->scene_warning_ = std::move(warning);
  return wrapped;
}

// Non-fatal warnings captured when this mixer was built from scene JSON, one
// entry per insert handed param keys it does not read; empty when all consumed.
val MixerWasm::sceneWarnings() const {
  val out = val::array();
  if (scene_warning_.empty()) {
    return out;
  }
  size_t start = 0;
  while (start <= scene_warning_.size()) {
    size_t end = scene_warning_.find('\n', start);
    if (end == std::string::npos) {
      out.call<void>("push", scene_warning_.substr(start));
      break;
    }
    out.call<void>("push", scene_warning_.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

std::string MixerWasm::presetJson(std::string name) {
  char* json = nullptr;
  SonareError err = sonare_mixing_scene_preset_json(name.c_str(), &json);
  if (err != SONARE_OK || json == nullptr) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to get mixing scene preset JSON: ") + sonare_error_message(err));
  }
  std::string out(json);
  sonare_free_string(json);
  return out;
}

void MixerWasm::compile() {
  SonareError err = sonare_mixer_compile(mixer_);
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to compile mixer graph: ") + sonare_error_message(err));
  }
}

size_t MixerWasm::stripCount() const { return sonare_mixer_strip_count(mixer_); }

// Borrowed strip handle by index in [0, stripCount()). Throws if out of range.
// The handle is owned by the mixer; do not free it.
SonareStrip* MixerWasm::stripAt(unsigned int strip_index) {
  SonareStrip* strip = sonare_mixer_strip_at(mixer_, static_cast<size_t>(strip_index));
  if (strip == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "mixer strip index out of range");
  }
  return strip;
}

// Resolves a strip's index from its id. Returns -1 when the id is not found;
// the TS wrapper maps -1 to null for cross-binding consistency (Node returns
// number | null).
int MixerWasm::stripById(std::string id) {
  const size_t count = sonare_mixer_strip_count(mixer_);
  SonareStrip* target = sonare_mixer_strip_by_id(mixer_, id.c_str());
  if (target == nullptr) {
    return -1;
  }
  for (size_t index = 0; index < count; ++index) {
    if (sonare_mixer_strip_at(mixer_, index) == target) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

void MixerWasm::checkStripError(SonareError err, const char* what) {
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  std::string(what) + ": " + sonare_error_message(err));
  }
}

void setPerPlaneMeters(val& out, const float* peak_db, const float* rms_db,
                       const float* true_peak_db, int channel_count) {
  out.set("channelCount", channel_count);
  val peak = val::array();
  val rms = val::array();
  val true_peak = val::array();
  for (int ch = 0; ch < channel_count; ++ch) {
    peak.call<void>("push", peak_db[ch]);
    rms.call<void>("push", rms_db[ch]);
    true_peak.call<void>("push", true_peak_db[ch]);
  }
  out.set("peakDb", peak);
  out.set("rmsDb", rms);
  out.set("truePeakDb", true_peak);
}

val MixerWasm::mixMeterSnapshotToVal(const SonareMixMeterSnapshot& snapshot) {
  val out = val::object();
  out.set("peakDbL", snapshot.peak_db_l);
  out.set("peakDbR", snapshot.peak_db_r);
  out.set("rmsDbL", snapshot.rms_db_l);
  out.set("rmsDbR", snapshot.rms_db_r);
  out.set("correlation", snapshot.correlation);
  out.set("monoCompatWidth", snapshot.mono_compat_width);
  out.set("monoCompatPeak", snapshot.mono_compat_peak);
  out.set("monoCompatSideRms", snapshot.mono_compat_side_rms);
  out.set("likelyMonoCompatible", snapshot.likely_mono_compatible != 0);
  out.set("momentaryLufs", snapshot.momentary_lufs);
  out.set("shortTermLufs", snapshot.short_term_lufs);
  out.set("integratedLufs", snapshot.integrated_lufs);
  out.set("gainReductionDb", snapshot.gain_reduction_db);
  out.set("truePeakDbL", snapshot.true_peak_db_l);
  out.set("truePeakDbR", snapshot.true_peak_db_r);
  out.set("maxTruePeakDb", snapshot.max_true_peak_db);
  out.set("seq", static_cast<double>(snapshot.seq));
  setPerPlaneMeters(out, snapshot.peak_db, snapshot.rms_db, snapshot.true_peak_db,
                    snapshot.channel_count);
  return out;
}

MixerWasm* createMixerFromSceneJson(std::string json, int sample_rate, int block_size) {
  return MixerWasm::fromSceneJson(std::move(json), sample_rate, block_size);
}
#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

namespace {

val optionAt(val options, const char* key, int index) {
  if (!hasProperty(options, key)) {
    return val::undefined();
  }
  val value = options[key];
  if (val::global("Array").call<bool>("isArray", value)) {
    return value[index];
  }
  return value;
}

mixing::PanMode panModeFromVal(val value) {
  if (value.isUndefined() || value.isNull()) {
    return mixing::PanMode::Balance;
  }
  if (value.typeOf().as<std::string>() == "number") {
    const int mode = value.as<int>();
    if (mode == 1) return mixing::PanMode::StereoPan;
    if (mode == 2) return mixing::PanMode::DualPan;
    return mixing::PanMode::Balance;
  }
  if (value.typeOf().as<std::string>() != "string") {
    return mixing::PanMode::Balance;
  }
  std::string mode = value.as<std::string>();
  for (char& ch : mode) {
    if (ch == '_') ch = '-';
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (mode == "stereo-pan" || mode == "stereopan" || mode == "pan") {
    return mixing::PanMode::StereoPan;
  }
  if (mode == "dual-pan" || mode == "dualpan") {
    return mixing::PanMode::DualPan;
  }
  return mixing::PanMode::Balance;
}

val meterSnapshotToVal(const mixing::MeterSnapshot& snapshot) {
  val out = val::object();
  out.set("peakDbL", snapshot.peak_db[0]);
  out.set("peakDbR", snapshot.peak_db[1]);
  out.set("rmsDbL", snapshot.rms_db[0]);
  out.set("rmsDbR", snapshot.rms_db[1]);
  out.set("correlation", snapshot.correlation);
  out.set("monoCompatWidth", snapshot.mono_compat_width);
  out.set("monoCompatPeak", snapshot.mono_compat_peak);
  out.set("monoCompatSideRms", snapshot.mono_compat_side_rms);
  out.set("likelyMonoCompatible", snapshot.likely_mono_compatible);
  out.set("momentaryLufs", snapshot.momentary_lufs);
  out.set("shortTermLufs", snapshot.short_term_lufs);
  out.set("integratedLufs", snapshot.integrated_lufs);
  out.set("gainReductionDb", snapshot.gain_reduction_db);
  out.set("truePeakDbL", snapshot.true_peak_db[0]);
  out.set("truePeakDbR", snapshot.true_peak_db[1]);
  out.set("maxTruePeakDb", snapshot.max_true_peak_db);
  out.set("seq", static_cast<double>(snapshot.seq));
  setPerPlaneMeters(out, snapshot.peak_db.data(), snapshot.rms_db.data(),
                    snapshot.true_peak_db.data(), snapshot.channel_count);
  return out;
}

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)
// Resolves a JS pan-mode value (number / string) to the SONARE_PAN_MODE_*
// ordinal accepted by sonare_strip_set_pan. Mirrors Node's PanModeValue so the
// real-graph mixStereo path behaves identically. Omitted / null defaults to
// Balance (mixStereo builds fresh strips, so there is no prior mode to keep).
int panModeOrdinalFromVal(val value) {
  if (value.isUndefined() || value.isNull()) {
    return SONARE_PAN_MODE_BALANCE;
  }
  if (value.typeOf().as<std::string>() == "number") {
    return value.as<int>();
  }
  if (value.typeOf().as<std::string>() != "string") {
    return SONARE_PAN_MODE_BALANCE;
  }
  std::string mode = value.as<std::string>();
  for (char& ch : mode) {
    if (ch == '_') ch = '-';
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (mode == "stereo-pan" || mode == "stereopan" || mode == "pan") {
    return SONARE_PAN_MODE_STEREO_PAN;
  }
  if (mode == "dual-pan" || mode == "dualpan") {
    return SONARE_PAN_MODE_DUAL_PAN;
  }
  return SONARE_PAN_MODE_BALANCE;
}

// Converts a C-ABI mix-meter snapshot to the JS meter object (same shape as
// meterSnapshotToVal / Node's MixMeterToObject).
val mixMeterSnapshotToValFree(const SonareMixMeterSnapshot& snapshot) {
  val out = val::object();
  out.set("peakDbL", snapshot.peak_db_l);
  out.set("peakDbR", snapshot.peak_db_r);
  out.set("rmsDbL", snapshot.rms_db_l);
  out.set("rmsDbR", snapshot.rms_db_r);
  out.set("correlation", snapshot.correlation);
  out.set("monoCompatWidth", snapshot.mono_compat_width);
  out.set("monoCompatPeak", snapshot.mono_compat_peak);
  out.set("monoCompatSideRms", snapshot.mono_compat_side_rms);
  out.set("likelyMonoCompatible", snapshot.likely_mono_compatible != 0);
  out.set("momentaryLufs", snapshot.momentary_lufs);
  out.set("shortTermLufs", snapshot.short_term_lufs);
  out.set("integratedLufs", snapshot.integrated_lufs);
  out.set("gainReductionDb", snapshot.gain_reduction_db);
  out.set("truePeakDbL", snapshot.true_peak_db_l);
  out.set("truePeakDbR", snapshot.true_peak_db_r);
  out.set("maxTruePeakDb", snapshot.max_true_peak_db);
  out.set("seq", static_cast<double>(snapshot.seq));
  setPerPlaneMeters(out, snapshot.peak_db, snapshot.rms_db, snapshot.true_peak_db,
                    snapshot.channel_count);
  return out;
}
#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

}  // namespace

val js_mix_stereo(val left_channels, val right_channels, int sample_rate, val options) {
  const int count = left_channels["length"].as<int>();
  if (count <= 0 || right_channels["length"].as<int>() != count) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "leftChannels and rightChannels must have the same non-zero length");
  }

  std::vector<std::vector<float>> left_inputs;
  std::vector<std::vector<float>> right_inputs;
  left_inputs.reserve(static_cast<size_t>(count));
  right_inputs.reserve(static_cast<size_t>(count));

  size_t length = 0;
  for (int index = 0; index < count; ++index) {
    left_inputs.push_back(float32ArrayToVector(left_channels[index]));
    right_inputs.push_back(float32ArrayToVector(right_channels[index]));
    if (left_inputs.back().size() != right_inputs.back().size()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "left and right channel lengths must match");
    }
    if (index == 0) {
      length = left_inputs.back().size();
    } else if (left_inputs.back().size() != length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "all strips must have the same length");
    }
  }

  std::vector<float> out_left(length, 0.0f);
  std::vector<float> out_right(length, 0.0f);
  val meters = val::array();

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)
  // Build a real mixer and route every input through the compiled routing graph
  // + master bus via sonare_mixer_process_stereo, matching the Node/Python
  // mixStereo path exactly (instead of summing bare ChannelStrip outputs).
  SonareMixer* mixer =
      sonare_mixer_create(sample_rate, static_cast<int>(std::max<size_t>(1, length)));
  if (mixer == nullptr) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to create mixer: ") + sonare_last_error_message());
  }
  std::vector<SonareStrip*> strips;
  std::vector<const float*> left_ptrs(static_cast<size_t>(count));
  std::vector<const float*> right_ptrs(static_cast<size_t>(count));
  try {
    strips.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
      SonareStrip* strip = sonare_mixer_add_strip(mixer, ("strip" + std::to_string(index)).c_str());
      if (strip == nullptr) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to add mixer strip");
      }
      strips.push_back(strip);

      val inputTrim = optionAt(options, "inputTrimDb", index);
      if (!inputTrim.isUndefined() && !inputTrim.isNull() &&
          inputTrim.typeOf().as<std::string>() == "number") {
        sonare_strip_set_input_trim_db(strip, inputTrim.as<float>());
      }
      val fader = optionAt(options, "faderDb", index);
      if (!fader.isUndefined() && !fader.isNull() && fader.typeOf().as<std::string>() == "number") {
        sonare_strip_set_fader_db(strip, fader.as<float>());
      }
      val pan = optionAt(options, "pan", index);
      if (!pan.isUndefined() && !pan.isNull() && pan.typeOf().as<std::string>() == "number") {
        sonare_strip_set_pan(strip, pan.as<float>(),
                             panModeOrdinalFromVal(optionAt(options, "panMode", index)));
      }
      val width = optionAt(options, "width", index);
      if (!width.isUndefined() && !width.isNull() && width.typeOf().as<std::string>() == "number") {
        sonare_strip_set_width(strip, width.as<float>());
      }
      val muted = optionAt(options, "muted", index);
      if (!muted.isUndefined() && !muted.isNull() &&
          muted.typeOf().as<std::string>() == "boolean") {
        sonare_strip_set_muted(strip, muted.as<bool>() ? 1 : 0);
      }

      left_ptrs[static_cast<size_t>(index)] = left_inputs[static_cast<size_t>(index)].data();
      right_ptrs[static_cast<size_t>(index)] = right_inputs[static_cast<size_t>(index)].data();
    }

    SonareError err = sonare_mixer_process_stereo(mixer, left_ptrs.data(), right_ptrs.data(),
                                                  static_cast<size_t>(count), out_left.data(),
                                                  out_right.data(), length);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("mixer process failed: ") + sonare_error_message(err));
    }
    // The per-strip meter snapshots reflect only this single one-shot block.
    // The integrating-meter fields (momentaryLufs/shortTermLufs/integratedLufs
    // and the true-peak fields) require sustained streaming to populate; on a
    // short one-shot mix they read the -120 dB floor sentinel. Use the
    // streaming Mixer path if you need meaningful loudness/true-peak readings.
    for (size_t index = 0; index < strips.size(); ++index) {
      SonareMixMeterSnapshot snapshot{};
      sonare_strip_meter(strips[index], &snapshot);
      meters.call<void>("push", mixMeterSnapshotToValFree(snapshot));
    }
  } catch (...) {
    sonare_mixer_destroy(mixer);
    throw;
  }
  sonare_mixer_destroy(mixer);
#else
  // Fallback for builds without the mixing routing graph: bare per-strip
  // ChannelStrip processing + manual sum. Functionally equivalent for the simple
  // (no-routing) case the graph collapses to.
  for (int index = 0; index < count; ++index) {
    mixing::ChannelStrip strip;
    strip.prepare(sample_rate, static_cast<int>(std::max<size_t>(1, length)));

    val inputTrim = optionAt(options, "inputTrimDb", index);
    if (!inputTrim.isUndefined() && !inputTrim.isNull() &&
        inputTrim.typeOf().as<std::string>() == "number") {
      strip.set_input_trim_db(inputTrim.as<float>());
    }
    val fader = optionAt(options, "faderDb", index);
    if (!fader.isUndefined() && !fader.isNull() && fader.typeOf().as<std::string>() == "number") {
      strip.set_fader_db(fader.as<float>());
    }
    val pan = optionAt(options, "pan", index);
    if (!pan.isUndefined() && !pan.isNull() && pan.typeOf().as<std::string>() == "number") {
      strip.set_pan_mode(panModeFromVal(optionAt(options, "panMode", index)));
      strip.set_pan(pan.as<float>());
    }
    val width = optionAt(options, "width", index);
    if (!width.isUndefined() && !width.isNull() && width.typeOf().as<std::string>() == "number") {
      strip.set_width(width.as<float>());
    }
    val muted = optionAt(options, "muted", index);
    if (!muted.isUndefined() && !muted.isNull() && muted.typeOf().as<std::string>() == "boolean") {
      strip.set_muted(muted.as<bool>());
    }

    float* channels[] = {left_inputs[static_cast<size_t>(index)].data(),
                         right_inputs[static_cast<size_t>(index)].data()};
    strip.process(channels, 2, static_cast<int>(length));
    for (size_t sample = 0; sample < length; ++sample) {
      out_left[sample] += left_inputs[static_cast<size_t>(index)][sample];
      out_right[sample] += right_inputs[static_cast<size_t>(index)][sample];
    }
    meters.call<void>("push", meterSnapshotToVal(strip.meter_snapshot()));
  }
#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

  val out = val::object();
  out.set("left", vectorToFloat32Array(out_left));
  out.set("right", vectorToFloat32Array(out_right));
  out.set("sampleRate", sample_rate);
  out.set("meters", meters);
  return out;
}

void registerMixingBindings() {
  function("mixingScenePresetNames", &js_mixing_scene_preset_names);
  function("mixingScenePresetJson", &js_mixing_scene_preset_json);
  function("mixStereo", &js_mix_stereo);
#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)
  class_<MixerWasm> cls("Mixer");
  cls.function("compile", &MixerWasm::compile)
      .function("stripCount", &MixerWasm::stripCount)
      .function("sceneWarnings", &MixerWasm::sceneWarnings)
      .function("stripById", &MixerWasm::stripById);
  registerMixerStripControls(cls);
  registerMixerAutomationMeters(cls);
  registerMixerProcessing(cls);
  registerMixerTopology(cls);
  function("createMixerFromSceneJson", &createMixerFromSceneJson, allow_raw_pointers());
#endif
}

#endif  // __EMSCRIPTEN__
