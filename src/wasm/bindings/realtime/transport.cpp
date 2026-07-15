/// @file realtime_engine_transport.cpp
/// @brief Embind realtime-engine facade: transport, tempo, markers, metronome.

#ifdef __EMSCRIPTEN__

#include "realtime_engine_wasm.h"

void RealtimeEngineWasm::play(int64_t render_frame) {
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kTransportPlay;
  command.sample_time = render_frame;
  if (!engine_.push_command(command)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to queue play command");
  }
}

void RealtimeEngineWasm::stop(int64_t render_frame) {
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kTransportStop;
  command.sample_time = render_frame;
  if (!engine_.push_command(command)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to queue stop command");
  }
}

/// Snaps every in-flight parameter ramp (engine-level smoothed params, mixer
/// lane fader/pan/gate, bus gains) to its target. For offline rendering:
/// call after a priming process() block so the first audible block renders
/// at settled values instead of ramping in from defaults. Matches
/// sonare_engine_settle_parameters.
void RealtimeEngineWasm::settleParameters() { engine_.settle_parameters(); }

void RealtimeEngineWasm::seekSample(int64_t timeline_sample, int64_t render_frame) {
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kTransportSeekSample;
  command.sample_time = render_frame;
  command.arg.i = timeline_sample;
  if (!engine_.push_command(command)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to queue seek command");
  }
}

void RealtimeEngineWasm::seekPpq(double ppq, int64_t render_frame) {
  if (!std::isfinite(ppq) || !sonare::transport::valid_public_ppq(ppq)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "seekPpq: ppq is outside the public timeline range");
  }
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kTransportSeekPpq;
  command.sample_time = render_frame;
  // Engine reads the PPQ scalar from the full-precision double slot
  // (kTransportSeekPpq -> transport_.seek_ppq(command.arg.d)); writing the
  // float slot of the union would surface as garbage. Match the C API.
  command.arg.d = ppq;
  if (!engine_.push_command(command)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to queue seek command");
  }
}

void RealtimeEngineWasm::setTempo(double bpm) { engine_.set_tempo(bpm); }
void RealtimeEngineWasm::setTempoSegments(val segments) {
  std::vector<sonare::transport::TempoSegment> parsed;
  if (!segments.isUndefined() && !segments.isNull()) {
    const unsigned count = segments["length"].as<unsigned>();
    parsed.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
      val entry = segments[i];
      sonare::transport::TempoSegment segment{};
      segment.start_ppq = doubleProperty(entry, "startPpq", 0.0);
      segment.bpm = doubleProperty(entry, "bpm", 0.0);
      segment.end_bpm = doubleProperty(entry, "endBpm", 0.0);
      if (!std::isfinite(segment.start_ppq) || segment.start_ppq < 0.0 ||
          !std::isfinite(segment.bpm) || segment.bpm <= 0.0 ||
          (segment.end_bpm != 0.0 && (!std::isfinite(segment.end_bpm) || segment.end_bpm <= 0.0))) {
        throw sonare::SonareException(
            sonare::ErrorCode::InvalidParameter,
            "setTempoSegments: segments require finite startPpq and positive bpm/endBpm");
      }
      parsed.push_back(segment);
    }
  }
  engine_.set_tempo_segments(std::move(parsed));
}
void RealtimeEngineWasm::setTimeSignature(int numerator, int denominator) {
  // Mirror the C-ABI guard (sonare_engine_set_time_signature): reject a
  // non-positive numerator/denominator instead of silently collapsing to 1/1
  // (tempo_map clamps with std::max(...,1)).
  if (numerator <= 0 || denominator <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "setTimeSignature: numerator and denominator must be positive");
  }
  engine_.set_time_signature(numerator, denominator);
}
void RealtimeEngineWasm::setTimeSignatureSegments(val segments) {
  std::vector<sonare::transport::TimeSignatureSegment> parsed;
  if (!segments.isUndefined() && !segments.isNull()) {
    const unsigned count = segments["length"].as<unsigned>();
    parsed.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
      val entry = segments[i];
      sonare::transport::TimeSignatureSegment segment{};
      segment.start_ppq = doubleProperty(entry, "startPpq", 0.0);
      segment.time_sig.numerator = intProperty(entry, "numerator", 0);
      segment.time_sig.denominator = intProperty(entry, "denominator", 0);
      if (!std::isfinite(segment.start_ppq) || segment.start_ppq < 0.0 ||
          segment.time_sig.numerator <= 0 || segment.time_sig.denominator <= 0) {
        throw sonare::SonareException(
            sonare::ErrorCode::InvalidParameter,
            "setTimeSignatureSegments: segments require finite startPpq and positive signature");
      }
      parsed.push_back(segment);
    }
  }
  engine_.set_time_signature_segments(std::move(parsed));
}
int64_t RealtimeEngineWasm::sampleAtPpq(double ppq) {
  if (!std::isfinite(ppq) || !sonare::transport::valid_public_ppq(ppq)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "sampleAtPpq: ppq is outside the public timeline range");
  }
  return engine_.sample_at_ppq(ppq);
}
void RealtimeEngineWasm::setLoop(double start_ppq, double end_ppq, bool enabled) {
  // Mirror the C-ABI guard (sonare_engine_set_loop): reject non-finite or
  // negative bounds, and an empty/inverted range when enabling the loop.
  if (!std::isfinite(start_ppq) || !std::isfinite(end_ppq) ||
      !sonare::transport::valid_public_ppq(start_ppq) ||
      !sonare::transport::valid_public_ppq(end_ppq) || (enabled && end_ppq <= start_ppq)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "setLoop: bounds must be finite, non-negative, and end > start");
  }
  engine_.set_loop(start_ppq, end_ppq, enabled);
}

void RealtimeEngineWasm::setMarkers(val markers) {
  marker_strings_.clear();
  std::vector<sonare::transport::Marker> prepared;
  const int count = markers["length"].as<int>();
  prepared.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    val marker = markers[i];
    marker_strings_.push_back(stringProperty(marker, "name", ""));
    prepared.push_back({objectProperty(marker, "ppq").as<double>(),
                        static_cast<uint32_t>(intProperty(marker, "id", i + 1)),
                        marker_strings_.back().c_str(),
                        static_cast<uint8_t>(intProperty(marker, "kind", 0)),
                        static_cast<int8_t>(intProperty(marker, "keyFifths", 0)),
                        boolProperty(marker, "keyMinor", false)});
  }
  engine_.set_markers(std::move(prepared));
}

int RealtimeEngineWasm::markerCount() const { return static_cast<int>(engine_.marker_count()); }

val RealtimeEngineWasm::markerByIndex(int index) const {
  sonare::transport::Marker marker{};
  if (index < 0 || !engine_.marker_by_index(static_cast<size_t>(index), &marker)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "marker index out of range");
  }
  return markerToVal(marker);
}

val RealtimeEngineWasm::marker(int id) const {
  sonare::transport::Marker marker{};
  if (!engine_.marker_by_id(static_cast<uint32_t>(id), &marker)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown marker id");
  }
  return markerToVal(marker);
}

void RealtimeEngineWasm::seekMarker(int id, int64_t render_frame) {
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

val RealtimeEngineWasm::getTransportState() const {
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

void RealtimeEngineWasm::setLoopFromMarkers(int start_marker_id, int end_marker_id) {
  if (!engine_.set_loop_from_markers(static_cast<uint32_t>(start_marker_id),
                                     static_cast<uint32_t>(end_marker_id))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown loop marker id");
  }
}

void RealtimeEngineWasm::setMetronome(val config) {
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

val RealtimeEngineWasm::metronome() const {
  const sonare::engine::MetronomeConfig& config = engine_.metronome_config();
  val out = val::object();
  out.set("enabled", config.enabled);
  out.set("beatGain", config.beat_gain);
  out.set("accentGain", config.accent_gain);
  out.set("clickSamples", config.click_samples);
  out.set("clickSeconds", config.click_seconds);
  return out;
}

int64_t RealtimeEngineWasm::countInEndSample(int64_t start_sample, int bars) const {
  return engine_.count_in_end_sample(start_sample, bars);
}

val RealtimeEngineWasm::markerToVal(const sonare::transport::Marker& marker) {
  val out = val::object();
  out.set("id", marker.id);
  out.set("ppq", marker.ppq);
  out.set("name", std::string(marker.name ? marker.name : ""));
  out.set("kind", static_cast<int>(marker.kind));
  out.set("keyFifths", static_cast<int>(marker.key_fifths));
  out.set("keyMinor", marker.key_minor);
  return out;
}

void registerRealtimeEngineTransport(class_<RealtimeEngineWasm>& cls) {
  cls.function("getTransportState", &RealtimeEngineWasm::getTransportState)
      .function("play", &RealtimeEngineWasm::play)
      .function("stop", &RealtimeEngineWasm::stop)
      .function("seekSample", &RealtimeEngineWasm::seekSample)
      .function("settleParameters", &RealtimeEngineWasm::settleParameters)
      .function("seekPpq", &RealtimeEngineWasm::seekPpq)
      .function("setTempo", &RealtimeEngineWasm::setTempo)
      .function("setTempoSegments", &RealtimeEngineWasm::setTempoSegments)
      .function("setTimeSignature", &RealtimeEngineWasm::setTimeSignature)
      .function("setTimeSignatureSegments", &RealtimeEngineWasm::setTimeSignatureSegments)
      .function("sampleAtPpq", &RealtimeEngineWasm::sampleAtPpq)
      .function("setLoop", &RealtimeEngineWasm::setLoop)
      .function("setMarkers", &RealtimeEngineWasm::setMarkers)
      .function("markerCount", &RealtimeEngineWasm::markerCount)
      .function("markerByIndex", &RealtimeEngineWasm::markerByIndex)
      .function("marker", &RealtimeEngineWasm::marker)
      .function("seekMarker", &RealtimeEngineWasm::seekMarker)
      .function("setLoopFromMarkers", &RealtimeEngineWasm::setLoopFromMarkers)
      .function("setMetronome", &RealtimeEngineWasm::setMetronome)
      .function("metronome", &RealtimeEngineWasm::metronome)
      .function("countInEndSample", &RealtimeEngineWasm::countInEndSample);
}

#endif  // __EMSCRIPTEN__
