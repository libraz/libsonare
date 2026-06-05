/// @file realtime_engine.cpp
/// @brief Embind bindings for the realtime engine facade.

#ifdef __EMSCRIPTEN__

#include "c_api/synth_patch_common.h"
#include "common.h"
#include "midi/midi_fx.h"
#include "synth_patch_val.h"
#if defined(SONARE_WITH_ARRANGEMENT)
#include "midi/synth/sf2_player.h"
#endif
#include "util/json.h"

namespace wasm_json = sonare::util::json;

// Canonical AutomationCurve ordinals (Linear=0, Exp=1, Hold=2, SCurve=3) are
// shared with the C ABI and other bindings; conversion is a direct cast.
sonare::automation::CurveType automationCurveFromInt(int curve) {
  if (curve < 0 || curve > 3) {
    return sonare::automation::CurveType::Linear;
  }
  return static_cast<sonare::automation::CurveType>(curve);
}

int automationCurveToInt(sonare::automation::CurveType curve) { return static_cast<int>(curve); }

int waveformFromName(const std::string& name) {
  if (name == "sine") return SONARE_SYNTH_WAVEFORM_SINE;
  if (name == "saw" || name == "sawtooth") return SONARE_SYNTH_WAVEFORM_SAW;
  if (name == "square") return SONARE_SYNTH_WAVEFORM_SQUARE;
  if (name == "triangle") return SONARE_SYNTH_WAVEFORM_TRIANGLE;
  return -1;
}

#if defined(SONARE_WITH_ARRANGEMENT)
double wasmJsonNumberOr(const wasm_json::Value& obj, const char* key, double fallback) {
  const wasm_json::Value* value = obj.find(key);
  return value != nullptr && value->is_number() ? value->as_number() : fallback;
}

bool wasmJsonHasNumber(const wasm_json::Value& obj, const char* key) {
  const wasm_json::Value* value = obj.find(key);
  return value != nullptr && value->is_number();
}

int wasmJsonIntOr(const wasm_json::Value& obj, const char* key, int fallback) {
  const double value = wasmJsonNumberOr(obj, key, static_cast<double>(fallback));
  if (!std::isfinite(value)) return fallback;
  return static_cast<int>(std::lround(value));
}

void wasmMidiFxChainFromJson(const std::string& config_json, sonare::midi::MidiFxChain* chain) {
  if (chain == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "MIDI-FX chain output is null");
  }
  wasm_json::Value root;
  try {
    root = wasm_json::parse_strict(config_json);
  } catch (const wasm_json::JsonError&) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, "invalid MIDI-FX JSON");
  }
  if (!root.is_object()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "MIDI-FX config must be an object");
  }

  sonare::midi::TransposeConfig transpose;
  if (wasmJsonHasNumber(root, "transpose_semitones")) {
    transpose.enabled = true;
    transpose.semitones = wasmJsonIntOr(root, "transpose_semitones", 0);
    chain->set_transpose(transpose);
  }

  sonare::midi::VelocityCurveConfig velocity;
  const bool has_velocity = wasmJsonHasNumber(root, "velocity_scale") ||
                            wasmJsonHasNumber(root, "velocity_offset") ||
                            wasmJsonHasNumber(root, "velocity_gamma");
  if (has_velocity) {
    velocity.enabled = true;
    velocity.scale = static_cast<float>(wasmJsonNumberOr(root, "velocity_scale", 1.0));
    velocity.offset = static_cast<float>(wasmJsonNumberOr(root, "velocity_offset", 0.0));
    velocity.gamma = static_cast<float>(wasmJsonNumberOr(root, "velocity_gamma", 1.0));
    if (!std::isfinite(velocity.scale) || !std::isfinite(velocity.offset) ||
        !std::isfinite(velocity.gamma) || velocity.gamma <= 0.0f) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "invalid MIDI-FX velocity curve");
    }
    chain->set_velocity_curve(velocity);
  }

  if (wasmJsonHasNumber(root, "quantize_ppq")) {
    const double grid_ppq = wasmJsonNumberOr(root, "quantize_ppq", 0.0);
    const double strength = wasmJsonNumberOr(root, "quantize_strength", 1.0);
    if (!std::isfinite(grid_ppq) || grid_ppq <= 0.0 || !std::isfinite(strength) || strength < 0.0 ||
        strength > 1.0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "invalid MIDI-FX quantize config");
    }
    sonare::midi::QuantizeConfig quantize;
    quantize.enabled = true;
    quantize.grid_frames = std::max<int64_t>(
        1, static_cast<int64_t>(std::llround(grid_ppq * sonare::midi::kMidiFxPpqScale)));
    quantize.strength = static_cast<float>(strength);
    chain->set_quantize(quantize);
  }

  if (const wasm_json::Value* intervals = root.find("chord_intervals")) {
    if (!intervals->is_array()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "MIDI-FX chord intervals must be an array");
    }
    sonare::midi::ChordConfig chord;
    chord.enabled = true;
    const auto& values = intervals->as_array();
    if (values.empty() || values.size() > sonare::midi::ChordConfig::kMaxChordNotes) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "invalid MIDI-FX chord interval count");
    }
    chord.count = values.size();
    for (size_t i = 0; i < values.size(); ++i) {
      if (!values[i].is_number()) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "MIDI-FX chord intervals must be numeric");
      }
      chord.intervals[i] = static_cast<int>(std::lround(values[i].as_number()));
    }
    chain->set_chord(chord);
  }

  const bool has_humanize = wasmJsonHasNumber(root, "humanize_ppq") ||
                            wasmJsonHasNumber(root, "humanize_velocity") ||
                            wasmJsonHasNumber(root, "seed");
  if (has_humanize) {
    const double timing_ppq = wasmJsonNumberOr(root, "humanize_ppq", 0.0);
    const int velocity_amount = wasmJsonIntOr(root, "humanize_velocity", 0);
    const int seed = wasmJsonIntOr(root, "seed", 0);
    if (!std::isfinite(timing_ppq) || timing_ppq < 0.0 || velocity_amount < 0 ||
        velocity_amount > 127 || seed < 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "invalid MIDI-FX humanize config");
    }
    sonare::midi::HumanizeConfig humanize;
    humanize.enabled = true;
    humanize.seed = static_cast<uint32_t>(seed);
    humanize.timing_frames =
        static_cast<int64_t>(std::llround(timing_ppq * sonare::midi::kMidiFxPpqScale));
    humanize.velocity_amount = velocity_amount;
    chain->set_humanize(humanize);
  }

  chain->prepare();
}
#endif

#if defined(SONARE_WITH_GRAPH)

std::unique_ptr<rt::ProcessorBase> makeWasmGraphProcessor(val node) {
  const int type = intProperty(node, "type", 0);
  switch (type) {
    case 0:
      return std::make_unique<rt::PassProcessor>();
    case 1:
      return std::make_unique<rt::GainProcessor>(floatProperty(node, "gainDb", 0.0f));
    default:
      return nullptr;
  }
}
#endif

class RealtimeEngineWasm {
 public:
  RealtimeEngineWasm(double sample_rate, int max_block_size, int command_capacity,
                     int telemetry_capacity) {
    engine_.prepare(sample_rate, max_block_size, capacity(command_capacity),
                    capacity(telemetry_capacity));
  }

  void prepare(double sample_rate, int max_block_size, int command_capacity,
               int telemetry_capacity) {
    engine_.prepare(sample_rate, max_block_size, capacity(command_capacity),
                    capacity(telemetry_capacity));
  }

  void play(int64_t render_frame) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kTransportPlay;
    command.sample_time = render_frame;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue play command");
    }
  }

  void stop(int64_t render_frame) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kTransportStop;
    command.sample_time = render_frame;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue stop command");
    }
  }

  void seekSample(int64_t timeline_sample, int64_t render_frame) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kTransportSeekSample;
    command.sample_time = render_frame;
    command.arg.i = timeline_sample;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue seek command");
    }
  }

  void seekPpq(double ppq, int64_t render_frame) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kTransportSeekPpq;
    command.sample_time = render_frame;
    // Engine reads the PPQ scalar from the full-precision double slot
    // (kTransportSeekPpq -> transport_.seek_ppq(command.arg.d)); writing the
    // float slot of the union would surface as garbage. Match the C API.
    command.arg.d = ppq;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue seek command");
    }
  }

  void setTempo(double bpm) { engine_.set_tempo(bpm); }
  void setTimeSignature(int numerator, int denominator) {
    engine_.set_time_signature(numerator, denominator);
  }
  void setLoop(double start_ppq, double end_ppq, bool enabled) {
    engine_.set_loop(start_ppq, end_ppq, enabled);
  }

  void addParameter(val info) {
    const uint32_t id = static_cast<uint32_t>(intProperty(info, "id", 0));
    if (id == 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "parameter id must be non-zero");
    }
    parameter_strings_.push_back(stringProperty(info, "name", ""));
    parameter_strings_.push_back(stringProperty(info, "unit", ""));
    sonare::automation::ParameterInfo parameter{};
    parameter.id = id;
    parameter.name = parameter_strings_[parameter_strings_.size() - 2].c_str();
    parameter.unit = parameter_strings_[parameter_strings_.size() - 1].c_str();
    parameter.min_value = floatProperty(info, "minValue", 0.0f);
    parameter.max_value = floatProperty(info, "maxValue", 1.0f);
    parameter.default_value = floatProperty(info, "defaultValue", 0.0f);
    parameter.rt_safe = boolProperty(info, "rtSafe", true);
    parameter.default_curve = automationCurveFromInt(intProperty(info, "defaultCurve", 1));
    if (!parameters_.add(parameter)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "duplicate parameter id");
    }
    publishParameterMetadata();
  }

  int parameterCount() const { return static_cast<int>(parameters_.parameter_count()); }

  val parameterInfoByIndex(int index) const {
    sonare::automation::ParameterInfo info{};
    if (index < 0 || !parameters_.parameter_info_by_index(static_cast<size_t>(index), &info)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "parameter index out of range");
    }
    return parameterToVal(info);
  }

  val parameterInfo(int id) const {
    sonare::automation::ParameterInfo info{};
    if (!parameters_.parameter_info(static_cast<uint32_t>(id), &info)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown parameter id");
    }
    return parameterToVal(info);
  }

  void setAutomationLane(int param_id, val points) {
    // NOTE: this surfaces a non-RT-safe parameter synchronously (a throw),
    // whereas setParameter/setParameterSmoothed and the canonical C API
    // (sonare_engine_set_automation_lane) report the same misuse asynchronously
    // via kNonRealtimeSafeParameter telemetry. The synchronous throw is kept
    // here intentionally because setAutomationLane is a control-thread (offline)
    // setter, so an immediate, actionable error is preferable to a deferred
    // telemetry record; the queued realtime writes keep the telemetry contract.
    if (!parameters_.parameter_is_realtime_safe(static_cast<uint32_t>(param_id))) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "parameter is not realtime safe");
    }
    sonare::automation::AutomationLane lane(static_cast<uint32_t>(param_id));
    std::vector<sonare::automation::Breakpoint> breakpoints;
    const int count = points["length"].as<int>();
    breakpoints.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      val point = points[i];
      breakpoints.push_back({objectProperty(point, "ppq").as<double>(),
                             floatProperty(point, "value", 0.0f),
                             automationCurveFromInt(intProperty(point, "curveToNext", 1))});
    }
    lane.set_points(std::move(breakpoints));
    bool replaced = false;
    for (auto& existing : automation_lanes_) {
      if (existing.target_param_id() == static_cast<uint32_t>(param_id)) {
        existing = std::move(lane);
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      automation_lanes_.push_back(std::move(lane));
    }
    engine_.automation().set_lanes(automation_lanes_);
  }

  int automationLaneCount() const { return static_cast<int>(engine_.automation().lane_count()); }

  void setMarkers(val markers) {
    marker_strings_.clear();
    std::vector<sonare::transport::Marker> prepared;
    const int count = markers["length"].as<int>();
    prepared.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      val marker = markers[i];
      marker_strings_.push_back(stringProperty(marker, "name", ""));
      prepared.push_back({objectProperty(marker, "ppq").as<double>(),
                          static_cast<uint32_t>(intProperty(marker, "id", i + 1)),
                          marker_strings_.back().c_str()});
    }
    engine_.set_markers(std::move(prepared));
  }

  int markerCount() const { return static_cast<int>(engine_.marker_count()); }

  val markerByIndex(int index) const {
    sonare::transport::Marker marker{};
    if (index < 0 || !engine_.marker_by_index(static_cast<size_t>(index), &marker)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "marker index out of range");
    }
    return markerToVal(marker);
  }

  val marker(int id) const {
    sonare::transport::Marker marker{};
    if (!engine_.marker_by_id(static_cast<uint32_t>(id), &marker)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown marker id");
    }
    return markerToVal(marker);
  }

  void seekMarker(int id, int64_t render_frame) {
    // Mirror the C API (sonare_engine_seek_marker): a sample-accurate seek is
    // queued as a kSeekMarker command so it lands at the requested render frame
    // instead of mutating transport state immediately.
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kSeekMarker;
    command.target_id = static_cast<uint32_t>(id);
    command.sample_time = render_frame;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue seek marker command");
    }
  }

  void setParameter(int param_id, float value, int64_t render_frame) {
    if (registeredParameterRejectsRealtime(static_cast<uint32_t>(param_id))) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "parameter is not realtime safe");
    }
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kSetParam;
    command.target_id = static_cast<uint32_t>(param_id);
    command.sample_time = render_frame;
    command.arg.f = value;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue set parameter command");
    }
  }

  void setParameterSmoothed(int param_id, float value, int64_t render_frame) {
    if (registeredParameterRejectsRealtime(static_cast<uint32_t>(param_id))) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "parameter is not realtime safe");
    }
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kSetParamSmoothed;
    command.target_id = static_cast<uint32_t>(param_id);
    command.sample_time = render_frame;
    command.arg.f = value;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue set parameter command");
    }
  }

  void setBuiltinInstrument(uint32_t destination_id, val config) {
#if defined(SONARE_WITH_ARRANGEMENT)
    sonare::midi::BuiltinSynthConfig cfg;
    if (!config.isUndefined() && !config.isNull()) {
      if (hasProperty(config, "waveform")) {
        val wf = config["waveform"];
        if (wf.typeOf().as<std::string>() == "string") {
          const std::string name = wf.as<std::string>();
          const int mapped = waveformFromName(name);
          if (mapped < 0) {
            throw sonare::SonareException(
                sonare::ErrorCode::InvalidParameter,
                "Unknown synth waveform name: '" + name +
                    "' (expected sine, saw, sawtooth, square, or triangle)");
          }
          cfg.waveform = static_cast<sonare::midi::SynthWaveform>(mapped);
        } else {
          cfg.waveform = static_cast<sonare::midi::SynthWaveform>(wf.as<int>());
        }
      }
      cfg.gain = floatProperty(config, "gain", 0.0f);
      cfg.attack_ms = floatProperty(config, "attackMs", 0.0f);
      cfg.decay_ms = floatProperty(config, "decayMs", 0.0f);
      cfg.sustain = floatProperty(config, "sustain", 0.0f);
      cfg.release_ms = floatProperty(config, "releaseMs", 0.0f);
      cfg.polyphony = intProperty(config, "polyphony", 0);
    }
    auto synth =
        std::make_unique<sonare::midi::BuiltinSynth>(sonare::midi::clamp_synth_config(cfg));
    bindInstrument(destination_id, std::move(synth));
#else
    (void)destination_id;
    (void)config;
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "arrangement/MIDI engine is not available in this build");
#endif
  }

  // Binds the patch-driven NativeSynth (the full synthesizer) on a realtime
  // MIDI destination. patch is a SynthPatch object or a preset-name string
  // ("saw-lead" / "va:saw-lead"), resolving exactly like
  // Project.bounceWithSynthInstrument. Unknown preset names throw.
  void setSynthInstrument(uint32_t destination_id, val patch) {
#if defined(SONARE_WITH_ARRANGEMENT)
    const SonareSynthPatch c_patch = sonare_wasm_synth::synthPatchFromVal(patch);
    sonare::midi::synth::NativeSynthConfig cfg;
    const char* error = nullptr;
    if (!sonare_c_detail::synth_config_from_patch_c(c_patch, &cfg, &error)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    error != nullptr ? error : "invalid synth patch");
    }
    bindInstrument(destination_id, std::make_unique<sonare::midi::synth::NativeSynth>(cfg));
#else
    (void)destination_id;
    (void)patch;
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "arrangement/MIDI engine is not available in this build");
#endif
  }

  // Loads (parses) SoundFont 2 bytes into the engine so SF2 instruments can be
  // bound with setSf2Instrument. The host copies the .sf2 bytes into linear
  // memory as a Uint8Array; they are not referenced after the call. Replaces
  // any previously loaded SoundFont (already-bound SF2 players keep the
  // SoundFont they were created with).
  void loadSoundFont(val data) {
#if defined(SONARE_WITH_ARRANGEMENT)
    std::vector<uint8_t> bytes = uint8ArrayToVector(data);
    auto soundfont = std::make_shared<sonare::midi::synth::Sf2File>();
    std::string error;
    if (bytes.empty() || !soundfont->parse(bytes.data(), bytes.size(), &error)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "failed to load SoundFont: " + error);
    }
    soundfont_ = std::move(soundfont);
#else
    (void)data;
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "arrangement/MIDI engine is not available in this build");
#endif
  }

  // Binds/replaces a GS-compatible SoundFont player on a realtime MIDI
  // destination, fed by the engine's loaded SoundFont. Without a loaded
  // SoundFont the player's NativeSynth GM fallback is the data-free floor
  // (live MIDI stays audible). config is { gain?, polyphony? }
  // ("0 / omit => default").
  void setSf2Instrument(uint32_t destination_id, val config) {
#if defined(SONARE_WITH_ARRANGEMENT)
    sonare::midi::synth::Sf2PlayerConfig cfg;
    if (!config.isUndefined() && !config.isNull()) {
      const float gain = floatProperty(config, "gain", 0.0f);
      if (gain > 0.0f) cfg.gain = gain;
      const int polyphony = intProperty(config, "polyphony", 0);
      if (polyphony > 0) cfg.polyphony = polyphony;
    }
    auto player = std::make_unique<sonare::midi::synth::Sf2Player>(cfg);
    player->set_soundfont(soundfont_);
    bindInstrument(destination_id, std::move(player));
#else
    (void)destination_id;
    (void)config;
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "arrangement/MIDI engine is not available in this build");
#endif
  }

#if defined(SONARE_WITH_ARRANGEMENT)
  // Binds (or replaces) an engine-owned instrument on a destination, keeping
  // the ownership table and the engine's instrument rack in sync. Shared by
  // the built-in synth and SF2 instrument entries.
  void bindInstrument(uint32_t destination_id,
                      std::unique_ptr<sonare::midi::MidiInstrument> instrument) {
    for (auto& entry : builtin_instruments_) {
      if (entry.first == destination_id) {
        sonare::midi::MidiInstrument* raw = instrument.get();
        if (!engine_.set_midi_instrument(destination_id, raw)) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                        "failed to bind MIDI instrument");
        }
        entry.second = std::move(instrument);
        return;
      }
    }
    builtin_instruments_.emplace_back(destination_id, std::move(instrument));
    sonare::midi::MidiInstrument* raw = builtin_instruments_.back().second.get();
    if (!engine_.set_midi_instrument(destination_id, raw)) {
      builtin_instruments_.pop_back();
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to bind MIDI instrument");
    }
  }
#endif

  void clearMidiInstrument(uint32_t destination_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
    engine_.set_midi_instrument(destination_id, nullptr);
    builtin_instruments_.erase(
        std::remove_if(builtin_instruments_.begin(), builtin_instruments_.end(),
                       [&](const auto& entry) { return entry.first == destination_id; }),
        builtin_instruments_.end());
#else
    (void)destination_id;
#endif
  }

  size_t midiInstrumentCount() const {
#if defined(SONARE_WITH_ARRANGEMENT)
    return engine_.midi_instrument_count();
#else
    return 0;
#endif
  }

  void bindMidiCc(int channel, int controller, uint32_t param_id, float min_value,
                  float max_value) {
#if defined(SONARE_WITH_ARRANGEMENT)
    if (channel < 0 || channel > 15 || controller < 0 || controller > 127 || param_id == 0 ||
        !std::isfinite(min_value) || !std::isfinite(max_value) || max_value < min_value) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "bindMidiCc: channel in [0,15], controller in [0,127], paramId non-zero, range finite");
    }
    if (!engine_.bind_midi_cc(static_cast<uint8_t>(controller), static_cast<uint8_t>(channel),
                              param_id, min_value, max_value)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to bind MIDI CC");
    }
#else
    (void)channel;
    (void)controller;
    (void)param_id;
    (void)min_value;
    (void)max_value;
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "arrangement/MIDI engine is not available in this build");
#endif
  }

  void clearMidiCcBindings() {
#if defined(SONARE_WITH_ARRANGEMENT)
    engine_.clear_midi_cc_bindings();
#endif
  }

  size_t midiCcBindingCount() const {
#if defined(SONARE_WITH_ARRANGEMENT)
    return engine_.midi_cc_binding_count();
#else
    return 0;
#endif
  }

  void setMidiFx(uint32_t destination_id, const std::string& config_json) {
#if defined(SONARE_WITH_ARRANGEMENT)
    sonare::midi::MidiFxChain chain;
    wasmMidiFxChainFromJson(config_json, &chain);
    if (!engine_.set_midi_fx(destination_id, chain)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to install MIDI-FX insert");
    }
#else
    (void)destination_id;
    (void)config_json;
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "arrangement/MIDI engine is not available in this build");
#endif
  }

  void clearMidiFx(uint32_t destination_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
    engine_.clear_midi_fx(destination_id);
#else
    (void)destination_id;
#endif
  }

  void setMidiInputSource(uint32_t destination_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
    engine_.set_midi_input_source(&midi_input_source_, destination_id);
    midi_input_source_enabled_ = true;
#else
    (void)destination_id;
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "arrangement/MIDI engine is not available in this build");
#endif
  }

  void clearMidiInputSource() {
#if defined(SONARE_WITH_ARRANGEMENT)
    engine_.set_midi_input_source(nullptr, 0);
    midi_input_source_enabled_ = false;
#endif
  }

  size_t midiInputPendingCount() const {
#if defined(SONARE_WITH_ARRANGEMENT)
    return midi_input_source_.pending_count();
#else
    return 0;
#endif
  }

  void pushMidiInputNoteOn(int group, int channel, int note, int velocity,
                           int64_t port_time_samples) {
    pushMidiInputEvent(group, channel, note, velocity, port_time_samples, true);
  }

  void pushMidiInputNoteOff(int group, int channel, int note, int velocity,
                            int64_t port_time_samples) {
    pushMidiInputEvent(group, channel, note, velocity, port_time_samples, false);
  }

  void pushMidiInputCc(int group, int channel, int controller, int value,
                       int64_t port_time_samples) {
#if defined(SONARE_WITH_ARRANGEMENT)
    if (!midi_input_source_enabled_ || group < 0 || group > 15 || channel < 0 || channel > 15 ||
        controller < 0 || controller > 127 || value < 0 || value > 127) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "pushMidiInputCc: source enabled, group/channel in [0,15], controller/value in [0,127]");
    }
    if (!midi_input_source_.push_event(
            sonare::midi::make_midi1_control_change(
                static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                static_cast<uint8_t>(controller), static_cast<uint8_t>(value)),
            port_time_samples)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to enqueue MIDI input CC");
    }
#else
    (void)group;
    (void)channel;
    (void)controller;
    (void)value;
    (void)port_time_samples;
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "arrangement/MIDI engine is not available in this build");
#endif
  }

  void pushMidiNoteOn(uint32_t destination_id, int group, int channel, int note, int velocity,
                      int64_t render_frame) {
    pushMidiNote(destination_id, group, channel, note, velocity, render_frame,
                 sonare::rt::CommandType::kMidiNoteOnImmediate);
  }

  void pushMidiNoteOff(uint32_t destination_id, int group, int channel, int note, int velocity,
                       int64_t render_frame) {
    pushMidiNote(destination_id, group, channel, note, velocity, render_frame,
                 sonare::rt::CommandType::kMidiNoteOffImmediate);
  }

  // Queues an immediate (live) MIDI control change to a MIDI destination. Mirrors
  // the C ABI sonare_engine_push_midi_cc: the synthesized MIDI 1.0 CC reaches the
  // registered host instrument at @p render_frame (-1 = immediate). Values are
  // 7-bit; channel 0..15, group 0..15. The scalar fields are packed into arg.i
  // using the encoding documented in rt/command.h (kMidiCcImmediate).
  void pushMidiCc(uint32_t destination_id, int group, int channel, int controller, int value,
                  int64_t render_frame) {
    if (group < 0 || group > 15 || channel < 0 || channel > 15 || controller < 0 ||
        controller > 127 || value < 0 || value > 127) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "pushMidiCc: group/channel in [0,15], controller/value in [0,127]");
    }
    const uint64_t packed =
        static_cast<uint64_t>(value) | (static_cast<uint64_t>(controller) << 8) |
        (static_cast<uint64_t>(channel) << 16) | (static_cast<uint64_t>(group) << 24);
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kMidiCcImmediate;
    command.target_id = destination_id;
    command.sample_time = render_frame;
    command.arg.i = static_cast<int64_t>(packed);
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue MIDI CC command");
    }
  }

  // Queues a MIDI panic (all-notes-off) releasing every sounding note at
  // @p render_frame (-1 = immediate). Mirrors the C ABI
  // sonare_engine_push_midi_panic.
  void pushMidiPanic(int64_t render_frame) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kMidiAllNotesOff;
    command.target_id = 0;
    command.sample_time = render_frame;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue MIDI panic command");
    }
  }

  // Removes all registered parameters and releases their backing strings (plus
  // the mirrored automation lanes). Control-thread only; not realtime-safe.
  // Mirrors the C ABI sonare_engine_clear_parameters.
  void clearParameters() {
    parameters_.clear();
    parameter_strings_.clear();
    automation_lanes_.clear();
    publishParameterMetadata();
    engine_.automation().set_lanes(automation_lanes_);
  }

  val getTransportState() const {
    const sonare::transport::TransportState state = engine_.transport_state_control();
    val out = val::object();
    out.set("playing", state.playing);
    out.set("looping", state.looping);
    out.set("renderFrame", static_cast<double>(state.render_frame));
    out.set("samplePosition", static_cast<double>(state.sample_position));
    out.set("ppq", state.ppq_position);
    out.set("bpm", state.bpm);
    out.set("barStartPpq", state.bar_start_ppq);
    out.set("barCount", static_cast<double>(state.bar_count));
    val time_signature = val::object();
    time_signature.set("numerator", state.time_sig.numerator);
    time_signature.set("denominator", state.time_sig.denominator);
    // The transport TimeSignature carries no confidence; mirror the C ABI which
    // reports a fixed 1.0 for the engine-driven (authoritative) time signature.
    time_signature.set("confidence", 1.0f);
    out.set("timeSignature", time_signature);
    out.set("loopStartPpq", state.loop_start_ppq);
    out.set("loopEndPpq", state.loop_end_ppq);
    out.set("sampleRate", state.sample_rate);
    return out;
  }

  void setLoopFromMarkers(int start_marker_id, int end_marker_id) {
    if (!engine_.set_loop_from_markers(static_cast<uint32_t>(start_marker_id),
                                       static_cast<uint32_t>(end_marker_id))) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown loop marker id");
    }
  }

  void setMetronome(val config) {
    sonare::engine::MetronomeConfig metronome{};
    metronome.enabled = boolProperty(config, "enabled", false);
    metronome.beat_gain = floatProperty(config, "beatGain", 0.35f);
    metronome.accent_gain = floatProperty(config, "accentGain", 0.7f);
    metronome.click_samples = intProperty(config, "clickSamples", 96);
    // clickSeconds is optional: a value > 0 overrides the engine's 2 ms default
    // click length (parity with the C-ABI/Python/Node click_seconds field). A
    // missing or 0 value leaves the struct default in place.
    const double click_seconds = hasProperty(config, "clickSeconds")
                                     ? objectProperty(config, "clickSeconds").as<double>()
                                     : 0.0;
    if (click_seconds > 0.0) {
      metronome.click_seconds = click_seconds;
    }
    engine_.set_metronome_config(metronome);
  }

  val metronome() const {
    const sonare::engine::MetronomeConfig& config = engine_.metronome_config();
    val out = val::object();
    out.set("enabled", config.enabled);
    out.set("beatGain", config.beat_gain);
    out.set("accentGain", config.accent_gain);
    out.set("clickSamples", config.click_samples);
    out.set("clickSeconds", config.click_seconds);
    return out;
  }

  int64_t countInEndSample(int64_t start_sample, int bars) const {
    return engine_.count_in_end_sample(start_sample, bars);
  }

  void setGraph(val spec) {
#if defined(SONARE_WITH_GRAPH)
    auto graph = std::make_unique<sonare::graph::Graph>();
    val nodes = spec["nodes"];
    const int node_count = nodes["length"].as<int>();
    if (node_count <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "graph nodes must not be empty");
    }
    const int num_channels = intProperty(spec, "numChannels", 2);
    for (int i = 0; i < node_count; ++i) {
      val node = nodes[i];
      auto processor = makeWasmGraphProcessor(node);
      if (!processor) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "unsupported graph node type");
      }
      const std::string id = stringProperty(node, "id", "");
      const int ports = intProperty(node, "numPorts", num_channels);
      if (!graph->add_node(id, std::move(processor), ports)) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "failed to add graph node");
      }
    }
    val connections = spec["connections"];
    const int connection_count = connections["length"].as<int>();
    for (int i = 0; i < connection_count; ++i) {
      val connection = connections[i];
      sonare::graph::Connection graph_connection{};
      graph_connection.source_node = stringProperty(connection, "sourceNode", "");
      graph_connection.source_port = intProperty(connection, "sourcePort", 0);
      graph_connection.dest_node = stringProperty(connection, "destNode", "");
      graph_connection.dest_port = intProperty(connection, "destPort", 0);
      graph_connection.mix = intProperty(connection, "mix", 1) == 0
                                 ? sonare::graph::Connection::Mix::Replace
                                 : sonare::graph::Connection::Mix::Add;
      if (!graph->connect(std::move(graph_connection))) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "failed to connect graph");
      }
    }
    if (!graph->compile()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to compile graph");
    }
    const auto state = engine_.transport().snapshot_control();
    graph->prepare(state.sample_rate, engine_.max_block_size());
    const std::string input_node = stringProperty(spec, "inputNode", "");
    const std::string output_node = stringProperty(spec, "outputNode", "");
    if (!engine_.swap_graph(std::move(graph), input_node.c_str(), output_node.c_str(),
                            num_channels)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to swap graph");
    }
    if (hasProperty(spec, "parameterBindings")) {
      val bindings = spec["parameterBindings"];
      const int binding_count = bindings["length"].as<int>();
      for (int i = 0; i < binding_count; ++i) {
        val binding = bindings[i];
        if (!engine_.bind_graph_parameter(static_cast<uint32_t>(intProperty(binding, "paramId", 0)),
                                          stringProperty(binding, "nodeId", "").c_str())) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "failed to bind graph parameter");
        }
      }
    }
#else
    (void)spec;
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "graph support is not enabled");
#endif
  }

  int graphNodeCount() const {
#if defined(SONARE_WITH_GRAPH)
    return static_cast<int>(engine_.graph_node_count());
#else
    return 0;
#endif
  }

  int graphConnectionCount() const {
#if defined(SONARE_WITH_GRAPH)
    return static_cast<int>(engine_.graph_connection_count());
#else
    return 0;
#endif
  }

  void setClips(val clips) {
    const int count = clips["length"].as<int>();
    clip_storage_.clear();
    clip_ptrs_.clear();
    clip_storage_.reserve(static_cast<size_t>(count));
    clip_ptrs_.reserve(static_cast<size_t>(count));
    std::vector<sonare::engine::ClipSchedule> schedules;
    schedules.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
      val clip_val = clips[i];
      val channels_val = clip_val["channels"];
      const int channel_count = channels_val["length"].as<int>();
      if (channel_count <= 0) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "clip channels must not be empty");
      }
      clip_storage_.emplace_back();
      clip_ptrs_.emplace_back();
      auto& storage = clip_storage_.back();
      auto& pointers = clip_ptrs_.back();
      storage.reserve(static_cast<size_t>(channel_count));
      pointers.reserve(static_cast<size_t>(channel_count));
      int64_t num_samples = 0;
      for (int ch = 0; ch < channel_count; ++ch) {
        std::vector<float> channel = float32ArrayToVector(channels_val[ch]);
        if (ch == 0) {
          num_samples = static_cast<int64_t>(channel.size());
          if (num_samples <= 0) {
            throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                          "clip channels must not be empty");
          }
        } else if (static_cast<int64_t>(channel.size()) != num_samples) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "all clip channels must have the same length");
        }
        storage.push_back(std::move(channel));
        pointers.push_back(storage.back().data());
      }

      sonare::engine::ClipSchedule schedule{};
      schedule.id = static_cast<uint32_t>(intProperty(clip_val, "id", i + 1));
      schedule.buffer = {pointers.data(), channel_count, num_samples};
      schedule.start_ppq = objectProperty(clip_val, "startPpq").as<double>();
      // clip_offset_samples / fade_*_samples are int64_t in ClipSchedule; read
      // them at full 64-bit precision (like length_samples below) so large
      // offsets above 2^31 samples do not silently truncate/sign-flip.
      schedule.clip_offset_samples =
          hasProperty(clip_val, "clipOffsetSamples")
              ? objectProperty(clip_val, "clipOffsetSamples").as<int64_t>()
              : 0;
      schedule.length_samples = hasProperty(clip_val, "lengthSamples")
                                    ? objectProperty(clip_val, "lengthSamples").as<int64_t>()
                                    : num_samples;
      schedule.loop = boolProperty(clip_val, "loop", false);
      schedule.gain = floatProperty(clip_val, "gain", 1.0f);
      schedule.fade_in_samples = hasProperty(clip_val, "fadeInSamples")
                                     ? objectProperty(clip_val, "fadeInSamples").as<int64_t>()
                                     : 0;
      schedule.fade_out_samples = hasProperty(clip_val, "fadeOutSamples")
                                      ? objectProperty(clip_val, "fadeOutSamples").as<int64_t>()
                                      : 0;
      schedules.push_back(schedule);
    }
    engine_.set_clips(std::move(schedules));
  }

  int clipCount() const { return static_cast<int>(engine_.clip_count()); }

  void setCaptureBuffer(int num_channels, int capacity_frames) {
    if (num_channels <= 0 || capacity_frames <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "capture buffer dimensions must be positive");
    }
    capture_storage_.assign(static_cast<size_t>(num_channels),
                            std::vector<float>(static_cast<size_t>(capacity_frames), 0.0f));
    capture_ptrs_.clear();
    capture_ptrs_.reserve(capture_storage_.size());
    for (auto& channel : capture_storage_) {
      capture_ptrs_.push_back(channel.data());
    }
    engine_.set_capture_segment(
        {capture_ptrs_.data(), num_channels, static_cast<int64_t>(capacity_frames)});
  }

  void armCapture(bool armed) { engine_.set_capture_armed(armed); }
  void setCapturePunch(int64_t start_sample, int64_t end_sample, bool enabled) {
    engine_.set_capture_punch(start_sample, end_sample, enabled);
  }
  void resetCapture() { engine_.reset_capture(); }

  val captureStatus() const {
    val out = val::object();
    out.set("capturedFrames", static_cast<double>(engine_.captured_frames()));
    out.set("overflowCount", engine_.capture_overflow_count());
    out.set("armed", engine_.capture_armed());
    out.set("punchEnabled", engine_.capture_punch_enabled());
    return out;
  }

  val capturedAudio() const {
    const int64_t frames = engine_.captured_frames();
    val out = val::array();
    for (size_t ch = 0; ch < capture_storage_.size(); ++ch) {
      const size_t count =
          static_cast<size_t>(std::min<int64_t>(frames, capture_storage_[ch].size()));
      std::vector<float> channel(capture_storage_[ch].begin(),
                                 capture_storage_[ch].begin() + count);
      out.set(static_cast<int>(ch), vectorToFloat32Array(channel));
    }
    return out;
  }

  val process(val channels_val) {
    ChannelBlock block = readChannels(channels_val);
    engine_.process(block.pointers.data(), static_cast<int>(block.storage.size()), block.frames);
    return channelsToJs(block);
  }

  // ---- Zero-copy "prepared" realtime path ------------------------------
  // The AudioWorklet render thread fills the per-channel input views (returned
  // as typed_memory_views onto persistent WASM-heap storage), calls
  // processPrepared(numFrames) which runs engine_.process() IN PLACE, then reads
  // the same views back. No std::vector or JS Float32Array is allocated per
  // quantum, so process() never touches the C++/JS heap allocators on the audio
  // thread (mirrors RealtimeVoiceChanger's prepared API). Call
  // prepareChannels(numChannels, maxFrames) once on the main thread first.
  void prepareChannels(int num_channels, int max_frames) {
    if (num_channels <= 0 || max_frames <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeEngine.prepareChannels: dimensions must be positive");
    }
    prepared_channels_ = num_channels;
    prepared_capacity_ = max_frames;
    prepared_storage_.assign(static_cast<size_t>(num_channels),
                             std::vector<float>(static_cast<size_t>(max_frames), 0.0f));
    prepared_ptrs_.clear();
    prepared_ptrs_.reserve(prepared_storage_.size());
    for (auto& channel : prepared_storage_) {
      prepared_ptrs_.push_back(channel.data());
    }
  }

  val getChannelBuffer(int channel, int num_frames) {
    if (channel < 0 || channel >= prepared_channels_) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeEngine.getChannelBuffer: channel out of range; call "
                                    "prepareChannels() first");
    }
    if (num_frames <= 0 || num_frames > prepared_capacity_) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeEngine.getChannelBuffer: out-of-range frame count");
    }
    return val(typed_memory_view(static_cast<size_t>(num_frames),
                                 prepared_storage_[static_cast<size_t>(channel)].data()));
  }

  void processPrepared(int num_frames) {
    if (prepared_channels_ <= 0 || prepared_storage_.empty()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "RealtimeEngine.processPrepared: prepareChannels() must be "
                                    "called first");
    }
    if (num_frames <= 0 || num_frames > prepared_capacity_) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeEngine.processPrepared: out-of-range frame count");
    }
    engine_.process(prepared_ptrs_.data(), prepared_channels_, num_frames);
  }

  val processWithMonitor(val channels_val) {
    ChannelBlock block = readChannels(channels_val);
    ChannelBlock monitor;
    monitor.frames = block.frames;
    monitor.storage.assign(block.storage.size(),
                           std::vector<float>(static_cast<size_t>(block.frames), 0.0f));
    monitor.pointers.reserve(monitor.storage.size());
    for (auto& channel : monitor.storage) {
      monitor.pointers.push_back(channel.data());
    }
    engine_.process_with_monitor(block.pointers.data(), monitor.pointers.data(),
                                 static_cast<int>(block.storage.size()), block.frames);
    val out = val::object();
    out.set("output", channelsToJs(block));
    out.set("monitor", channelsToJs(monitor));
    return out;
  }

  val renderOffline(val channels_val, int block_size) {
    ChannelBlock block = readChannels(channels_val);
    engine_.render_offline(block.pointers.data(), static_cast<int>(block.storage.size()),
                           block.frames, block_size);
    return channelsToJs(block);
  }

  val bounceOffline(val options_val) {
    const int64_t total_frames = objectProperty(options_val, "totalFrames").as<int64_t>();
    const int block_size = intProperty(options_val, "blockSize", 128);
    const int num_channels = intProperty(options_val, "numChannels", 2);
    const int source_sample_rate = intProperty(options_val, "sourceSampleRate", 48000);
    const int target_sample_rate = intProperty(options_val, "targetSampleRate", 48000);
    if (total_frames <= 0 || block_size <= 0 || num_channels <= 0 || source_sample_rate <= 0 ||
        target_sample_rate <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid bounce options");
    }

    std::vector<std::vector<float>> channels(static_cast<size_t>(num_channels),
                                             std::vector<float>(static_cast<size_t>(total_frames)));
    std::vector<float*> pointers;
    pointers.reserve(channels.size());
    for (auto& channel : channels) {
      pointers.push_back(channel.data());
    }
    engine_.render_offline(pointers.data(), num_channels, total_frames, block_size);

    if (source_sample_rate != target_sample_rate) {
      for (auto& channel : channels) {
        channel = resample(channel.data(), channel.size(), source_sample_rate, target_sample_rate);
      }
    }

    std::vector<float> interleaved = interleave(channels);
    const size_t frames = channels.empty() ? 0 : channels[0].size();
    if (boolProperty(options_val, "normalizeLufs", false)) {
      // Pull the canonical fallback target from the C API so the WASM facade
      // never drifts away from the C/Node/Python bounce normalization target.
      // See SONARE_DEFAULT_BOUNCE_TARGET_LUFS in src/sonare_c_types.h and the
      // sentinel handling in sonare_engine_bounce_offline.
      const float target_lufs =
          floatProperty(options_val, "targetLufs", SONARE_DEFAULT_BOUNCE_TARGET_LUFS);
      metering::normalize_interleaved_to_lufs(interleaved, frames, num_channels, target_sample_rate,
                                              target_lufs);
    }

    const int dither = intProperty(options_val, "dither", 0);
    if (dither != 0) {
      mastering::final::DitherConfig config{};
      config.type = ditherTypeFromInt(dither);
      config.target_bits = intProperty(options_val, "ditherBits", 16);
      if (config.target_bits <= 0) config.target_bits = 16;
      // Match the C API: seed == 0 means "keep the library default seed".
      const auto requested_seed = static_cast<uint32_t>(intProperty(options_val, "ditherSeed", 0));
      if (requested_seed != 0) config.seed = requested_seed;
      Audio dithered = mastering::final::dither(
          Audio::from_buffer(interleaved.data(), interleaved.size(), target_sample_rate), config);
      interleaved.assign(dithered.data(), dithered.data() + dithered.size());
    }

    const auto loudness =
        metering::lufs_interleaved(interleaved.data(), frames, num_channels, target_sample_rate);
    val out = val::object();
    out.set("interleaved", vectorToFloat32Array(interleaved));
    out.set("frames", static_cast<double>(frames));
    out.set("numChannels", num_channels);
    out.set("sampleRate", target_sample_rate);
    out.set("integratedLufs", loudness.integrated_lufs);
    return out;
  }

  val freezeOffline(val options_val) {
    const int64_t total_frames = objectProperty(options_val, "totalFrames").as<int64_t>();
    const int block_size = intProperty(options_val, "blockSize", 128);
    const int num_channels = intProperty(options_val, "numChannels", 2);
    if (total_frames <= 0 || block_size <= 0 || num_channels <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid freeze options");
    }

    std::vector<std::vector<float>> frozen(static_cast<size_t>(num_channels),
                                           std::vector<float>(static_cast<size_t>(total_frames)));
    std::vector<float*> render_pointers;
    render_pointers.reserve(frozen.size());
    for (auto& channel : frozen) {
      render_pointers.push_back(channel.data());
    }
    engine_.render_offline(render_pointers.data(), num_channels, total_frames, block_size);

    clip_storage_.clear();
    clip_ptrs_.clear();
    clip_storage_.push_back(std::move(frozen));
    clip_ptrs_.emplace_back();
    clip_ptrs_.back().reserve(clip_storage_.back().size());
    for (const auto& channel : clip_storage_.back()) {
      clip_ptrs_.back().push_back(channel.data());
    }

    sonare::engine::ClipSchedule schedule{};
    schedule.id = static_cast<uint32_t>(intProperty(options_val, "clipId", 1));
    if (schedule.id == 0) schedule.id = 1;
    schedule.buffer = {clip_ptrs_.back().data(), num_channels, total_frames};
    // Read startPpq at full double precision to match setClips() and the
    // double-typed ClipSchedule.start_ppq field; a Float32 read would quantize a
    // frozen clip at a large PPQ position to a different sample than the same
    // clip placed via setClips.
    schedule.start_ppq = hasProperty(options_val, "startPpq")
                             ? objectProperty(options_val, "startPpq").as<double>()
                             : 0.0;
    schedule.clip_offset_samples = 0;
    schedule.length_samples = total_frames;
    schedule.loop = false;
    schedule.gain = floatProperty(options_val, "gain", 1.0f);
    engine_.set_clips({schedule});

    val out = val::object();
    out.set("clipId", schedule.id);
    out.set("frames", static_cast<double>(total_frames));
    out.set("numChannels", num_channels);
    return out;
  }

  val drainTelemetry(int max_records) {
    val out = val::array();
    if (max_records <= 0) return out;
    sonare::engine::Telemetry telemetry{};
    int count = 0;
    while (count < max_records && engine_.pop_telemetry(telemetry)) {
      val item = val::object();
      item.set("type", static_cast<int>(telemetry.type));
      item.set("error", static_cast<int>(telemetry.error));
      item.set("renderFrame", static_cast<double>(telemetry.render_frame));
      item.set("timelineSample", static_cast<double>(telemetry.timeline_sample));
      item.set("audibleTimelineSample", static_cast<double>(telemetry.audible_timeline_sample));
      item.set("graphLatencySamplesQ8", telemetry.graph_latency_samples_q8);
      item.set("value", telemetry.value);
      out.set(count++, item);
    }
    return out;
  }

  val drainMeterTelemetry(int max_records) {
    val out = val::array();
    if (max_records <= 0) return out;
    sonare::engine::MeterTelemetryRecord meter{};
    int count = 0;
    while (count < max_records && engine_.pop_meter_telemetry(meter)) {
      val item = val::object();
      item.set("targetId", meter.target_id);
      item.set("renderFrame", static_cast<double>(meter.render_frame));
      item.set("seq", static_cast<double>(meter.seq));
      item.set("peakDbL", meter.peak_db[0]);
      item.set("peakDbR", meter.peak_db[1]);
      item.set("rmsDbL", meter.rms_db[0]);
      item.set("rmsDbR", meter.rms_db[1]);
      item.set("truePeakDbL", meter.true_peak_db[0]);
      item.set("truePeakDbR", meter.true_peak_db[1]);
      item.set("maxTruePeakDb", meter.max_true_peak_db);
      item.set("correlation", meter.correlation);
      item.set("monoCompatWidth", meter.mono_compat_width);
      item.set("momentaryLufs", meter.momentary_lufs);
      item.set("shortTermLufs", meter.short_term_lufs);
      item.set("integratedLufs", meter.integrated_lufs);
      item.set("gainReductionDb", meter.gain_reduction_db);
      item.set("droppedRecords", meter.dropped_records);
      out.set(count++, item);
    }
    return out;
  }

 private:
  // Maps a JS-supplied queue depth to the engine's size_t capacity. A value <= 0
  // selects the engine default (1024), matching the Node/Python bindings.
  static size_t capacity(int requested) {
    return requested > 0 ? static_cast<size_t>(requested) : 1024;
  }

  struct ChannelBlock {
    std::vector<std::vector<float>> storage;
    std::vector<float*> pointers;
    int frames = 0;
  };

  static ChannelBlock readChannels(val channels_val) {
    const int count = channels_val["length"].as<int>();
    if (count <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "channels must not be empty");
    }
    ChannelBlock block;
    block.storage.reserve(static_cast<size_t>(count));
    block.pointers.reserve(static_cast<size_t>(count));
    for (int ch = 0; ch < count; ++ch) {
      std::vector<float> channel = float32ArrayToVector(channels_val[ch]);
      if (ch == 0) {
        block.frames = static_cast<int>(channel.size());
        if (block.frames <= 0) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "channels must not be empty");
        }
      } else if (static_cast<int>(channel.size()) != block.frames) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "all channels must have the same length");
      }
      block.storage.push_back(std::move(channel));
    }
    for (auto& channel : block.storage) {
      block.pointers.push_back(channel.data());
    }
    return block;
  }

  static val channelsToJs(const ChannelBlock& block) {
    val out = val::array();
    for (size_t ch = 0; ch < block.storage.size(); ++ch) {
      out.set(static_cast<int>(ch), vectorToFloat32Array(block.storage[ch]));
    }
    return out;
  }

  static std::vector<float> interleave(const std::vector<std::vector<float>>& channels) {
    if (channels.empty()) return {};
    const size_t frames = channels[0].size();
    std::vector<float> out(frames * channels.size());
    for (size_t frame = 0; frame < frames; ++frame) {
      for (size_t ch = 0; ch < channels.size(); ++ch) {
        out[frame * channels.size() + ch] = channels[ch][frame];
      }
    }
    return out;
  }

  static mastering::final::DitherType ditherTypeFromInt(int value) {
    switch (value) {
      case 1:
        return mastering::final::DitherType::Rpdf;
      case 2:
        return mastering::final::DitherType::Tpdf;
      case 3:
        return mastering::final::DitherType::NoiseShaped;
      default:
        return mastering::final::DitherType::None;
    }
  }

  static val parameterToVal(const sonare::automation::ParameterInfo& info) {
    val out = val::object();
    out.set("id", info.id);
    out.set("name", std::string(info.name ? info.name : ""));
    out.set("unit", std::string(info.unit ? info.unit : ""));
    out.set("minValue", info.min_value);
    out.set("maxValue", info.max_value);
    out.set("defaultValue", info.default_value);
    out.set("rtSafe", info.rt_safe);
    out.set("defaultCurve", automationCurveToInt(info.default_curve));
    return out;
  }

  static val markerToVal(const sonare::transport::Marker& marker) {
    val out = val::object();
    out.set("id", marker.id);
    out.set("ppq", marker.ppq);
    out.set("name", std::string(marker.name ? marker.name : ""));
    return out;
  }

  void publishParameterMetadata() {
    std::vector<sonare::automation::ParameterInfo> parameters;
    parameters.reserve(parameters_.parameter_count());
    for (size_t i = 0; i < parameters_.parameter_count(); ++i) {
      sonare::automation::ParameterInfo info{};
      if (parameters_.parameter_info_by_index(i, &info)) {
        parameters.push_back(info);
      }
    }
    engine_.automation().set_parameter_metadata(std::move(parameters));
  }

  bool registeredParameterRejectsRealtime(uint32_t param_id) const {
    sonare::automation::ParameterInfo info{};
    return parameters_.parameter_info(param_id, &info) && !info.rt_safe;
  }

  sonare::engine::RealtimeEngine engine_{};
  /// Engine-owned instrument per destination (built-in synth or SF2 player).
  std::vector<std::pair<uint32_t, std::unique_ptr<sonare::midi::MidiInstrument>>>
      builtin_instruments_{};
#if defined(SONARE_WITH_ARRANGEMENT)
  /// Loaded SoundFont (loadSoundFont); shared read-only with the SF2 players
  /// bound through setSf2Instrument.
  std::shared_ptr<const sonare::midi::synth::Sf2File> soundfont_;
#endif
  sonare::host::FixedMidiInputSource<512> midi_input_source_{};
  bool midi_input_source_enabled_ = false;
  sonare::automation::ParameterRegistry parameters_{};
  std::vector<sonare::automation::AutomationLane> automation_lanes_;
  std::deque<std::string> parameter_strings_;
  std::deque<std::string> marker_strings_;
  std::vector<std::vector<std::vector<float>>> clip_storage_;
  std::vector<std::vector<const float*>> clip_ptrs_;
  std::vector<std::vector<float>> capture_storage_;
  std::vector<float*> capture_ptrs_;
  // Persistent per-channel scratch for the zero-copy prepared process() path.
  std::vector<std::vector<float>> prepared_storage_;
  std::vector<float*> prepared_ptrs_;
  int prepared_channels_ = 0;
  int prepared_capacity_ = 0;

  void pushMidiNote(uint32_t destination_id, int group, int channel, int note, int velocity,
                    int64_t render_frame, sonare::rt::CommandType type) {
    if (group < 0 || group > 15 || channel < 0 || channel > 15 || note < 0 || note > 127 ||
        velocity < 0 || velocity > 127) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "pushMidiNote: group/channel in [0,15], note/velocity in [0,127]");
    }
    const uint64_t packed = static_cast<uint64_t>(velocity) | (static_cast<uint64_t>(note) << 8) |
                            (static_cast<uint64_t>(channel) << 16) |
                            (static_cast<uint64_t>(group) << 24);
    sonare::rt::Command command{};
    command.type = type;
    command.target_id = destination_id;
    command.sample_time = render_frame;
    command.arg.i = static_cast<int64_t>(packed);
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue MIDI note command");
    }
  }

  void pushMidiInputEvent(int group, int channel, int note, int velocity, int64_t port_time_samples,
                          bool note_on) {
#if defined(SONARE_WITH_ARRANGEMENT)
    if (!midi_input_source_enabled_ || group < 0 || group > 15 || channel < 0 || channel > 15 ||
        note < 0 || note > 127 || velocity < 0 || velocity > 127) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "pushMidiInputNote: source enabled, group/channel in [0,15], note/velocity in [0,127]");
    }
    const sonare::midi::Ump ump =
        note_on ? sonare::midi::make_midi1_note_on(
                      static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                      static_cast<uint8_t>(note), static_cast<uint8_t>(velocity))
                : sonare::midi::make_midi1_note_off(
                      static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                      static_cast<uint8_t>(note), static_cast<uint8_t>(velocity));
    if (!midi_input_source_.push_event(ump, port_time_samples)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to enqueue MIDI input note");
    }
#else
    (void)group;
    (void)channel;
    (void)note;
    (void)velocity;
    (void)port_time_samples;
    (void)note_on;
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "arrangement/MIDI engine is not available in this build");
#endif
  }
};

void registerRealtimeEngineBindings() {
  class_<RealtimeEngineWasm>("RealtimeEngine")
      .constructor<double, int, int, int>()
      .function("prepare", &RealtimeEngineWasm::prepare)
      .function("setParameter", &RealtimeEngineWasm::setParameter)
      .function("setParameterSmoothed", &RealtimeEngineWasm::setParameterSmoothed)
      .function("setBuiltinInstrument", &RealtimeEngineWasm::setBuiltinInstrument)
      .function("setSynthInstrument", &RealtimeEngineWasm::setSynthInstrument)
      .function("loadSoundFont", &RealtimeEngineWasm::loadSoundFont)
      .function("setSf2Instrument", &RealtimeEngineWasm::setSf2Instrument)
      .function("clearMidiInstrument", &RealtimeEngineWasm::clearMidiInstrument)
      .function("midiInstrumentCount", &RealtimeEngineWasm::midiInstrumentCount)
      .function("bindMidiCc", &RealtimeEngineWasm::bindMidiCc)
      .function("clearMidiCcBindings", &RealtimeEngineWasm::clearMidiCcBindings)
      .function("midiCcBindingCount", &RealtimeEngineWasm::midiCcBindingCount)
      .function("setMidiFx", &RealtimeEngineWasm::setMidiFx)
      .function("clearMidiFx", &RealtimeEngineWasm::clearMidiFx)
      .function("setMidiInputSource", &RealtimeEngineWasm::setMidiInputSource)
      .function("clearMidiInputSource", &RealtimeEngineWasm::clearMidiInputSource)
      .function("midiInputPendingCount", &RealtimeEngineWasm::midiInputPendingCount)
      .function("pushMidiInputNoteOn", &RealtimeEngineWasm::pushMidiInputNoteOn)
      .function("pushMidiInputNoteOff", &RealtimeEngineWasm::pushMidiInputNoteOff)
      .function("pushMidiInputCc", &RealtimeEngineWasm::pushMidiInputCc)
      .function("pushMidiNoteOn", &RealtimeEngineWasm::pushMidiNoteOn)
      .function("pushMidiNoteOff", &RealtimeEngineWasm::pushMidiNoteOff)
      .function("pushMidiCc", &RealtimeEngineWasm::pushMidiCc)
      .function("pushMidiPanic", &RealtimeEngineWasm::pushMidiPanic)
      .function("clearParameters", &RealtimeEngineWasm::clearParameters)
      .function("getTransportState", &RealtimeEngineWasm::getTransportState)
      .function("play", &RealtimeEngineWasm::play)
      .function("stop", &RealtimeEngineWasm::stop)
      .function("seekSample", &RealtimeEngineWasm::seekSample)
      .function("seekPpq", &RealtimeEngineWasm::seekPpq)
      .function("setTempo", &RealtimeEngineWasm::setTempo)
      .function("setTimeSignature", &RealtimeEngineWasm::setTimeSignature)
      .function("setLoop", &RealtimeEngineWasm::setLoop)
      .function("addParameter", &RealtimeEngineWasm::addParameter)
      .function("parameterCount", &RealtimeEngineWasm::parameterCount)
      .function("parameterInfoByIndex", &RealtimeEngineWasm::parameterInfoByIndex)
      .function("parameterInfo", &RealtimeEngineWasm::parameterInfo)
      .function("setAutomationLane", &RealtimeEngineWasm::setAutomationLane)
      .function("automationLaneCount", &RealtimeEngineWasm::automationLaneCount)
      .function("setMarkers", &RealtimeEngineWasm::setMarkers)
      .function("markerCount", &RealtimeEngineWasm::markerCount)
      .function("markerByIndex", &RealtimeEngineWasm::markerByIndex)
      .function("marker", &RealtimeEngineWasm::marker)
      .function("seekMarker", &RealtimeEngineWasm::seekMarker)
      .function("setLoopFromMarkers", &RealtimeEngineWasm::setLoopFromMarkers)
      .function("setMetronome", &RealtimeEngineWasm::setMetronome)
      .function("metronome", &RealtimeEngineWasm::metronome)
      .function("countInEndSample", &RealtimeEngineWasm::countInEndSample)
      .function("setGraph", &RealtimeEngineWasm::setGraph)
      .function("graphNodeCount", &RealtimeEngineWasm::graphNodeCount)
      .function("graphConnectionCount", &RealtimeEngineWasm::graphConnectionCount)
      .function("setClips", &RealtimeEngineWasm::setClips)
      .function("clipCount", &RealtimeEngineWasm::clipCount)
      .function("setCaptureBuffer", &RealtimeEngineWasm::setCaptureBuffer)
      .function("armCapture", &RealtimeEngineWasm::armCapture)
      .function("setCapturePunch", &RealtimeEngineWasm::setCapturePunch)
      .function("resetCapture", &RealtimeEngineWasm::resetCapture)
      .function("captureStatus", &RealtimeEngineWasm::captureStatus)
      .function("capturedAudio", &RealtimeEngineWasm::capturedAudio)
      .function("process", &RealtimeEngineWasm::process)
      .function("prepareChannels", &RealtimeEngineWasm::prepareChannels)
      .function("getChannelBuffer", &RealtimeEngineWasm::getChannelBuffer)
      .function("processPrepared", &RealtimeEngineWasm::processPrepared)
      .function("processWithMonitor", &RealtimeEngineWasm::processWithMonitor)
      .function("renderOffline", &RealtimeEngineWasm::renderOffline)
      .function("bounceOffline", &RealtimeEngineWasm::bounceOffline)
      .function("freezeOffline", &RealtimeEngineWasm::freezeOffline)
      .function("drainTelemetry", &RealtimeEngineWasm::drainTelemetry)
      .function("drainMeterTelemetry", &RealtimeEngineWasm::drainMeterTelemetry);
}

#endif  // __EMSCRIPTEN__
