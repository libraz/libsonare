/// @file realtime_engine_processing.cpp
/// @brief Embind realtime-engine facade: audio processing, graph & offline.

#ifdef __EMSCRIPTEN__

#include "realtime_engine_wasm.h"
#include "util/resource_limits.h"

namespace {

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

}  // namespace

RealtimeEngineWasm::ChannelBlock RealtimeEngineWasm::readChannels(val channels_val) {
  const int count = static_cast<int>(wasmArrayLikeLength(channels_val, "channels"));
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

val RealtimeEngineWasm::channelsToJs(const ChannelBlock& block) {
  val out = val::array();
  for (size_t ch = 0; ch < block.storage.size(); ++ch) {
    out.set(static_cast<int>(ch), vectorToFloat32Array(block.storage[ch]));
  }
  return out;
}

std::vector<float> RealtimeEngineWasm::interleave(const std::vector<std::vector<float>>& channels) {
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

mastering::final::DitherType RealtimeEngineWasm::ditherTypeFromInt(int value) {
  switch (value) {
    case 0:
      return mastering::final::DitherType::None;
    case 1:
      return mastering::final::DitherType::Rpdf;
    case 2:
      return mastering::final::DitherType::Tpdf;
    case 3:
      return mastering::final::DitherType::NoiseShaped;
    default:
      // Callers validate the ordinal before rendering; falling through to None
      // here would return undithered audio with no way to tell the request was
      // ignored, which is what the C-ABI oracle rejects.
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unsupported dither type");
  }
}

void RealtimeEngineWasm::setGraph(val spec) {
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
    // Match the C ABI (sonare_engine_set_graph): a non-positive numPorts — which
    // includes an explicit `numPorts: 0` that intProperty passes through as-is —
    // falls back to num_channels rather than reaching add_node with 0 and throwing.
    const int requested_ports = intProperty(node, "numPorts", num_channels);
    const int ports = requested_ports > 0 ? requested_ports : num_channels;
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
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to connect graph");
    }
  }
  if (!graph->compile()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to compile graph");
  }
  const auto state = engine_.transport().snapshot_control();
  graph->prepare(state.sample_rate, engine_.max_block_size());
  const std::string input_node = stringProperty(spec, "inputNode", "");
  const std::string output_node = stringProperty(spec, "outputNode", "");
  std::vector<sonare::engine::GraphRuntime::ParameterBinding> parameter_bindings;
  if (hasProperty(spec, "parameterBindings")) {
    val bindings = spec["parameterBindings"];
    const int binding_count = bindings["length"].as<int>();
    parameter_bindings.reserve(static_cast<size_t>(std::max(binding_count, 0)));
    for (int i = 0; i < binding_count; ++i) {
      val binding = bindings[i];
      parameter_bindings.push_back({static_cast<uint32_t>(intProperty(binding, "paramId", 0)),
                                    stringProperty(binding, "nodeId", "")});
    }
  }
  if (!engine_.swap_graph(std::move(graph), input_node.c_str(), output_node.c_str(), num_channels,
                          std::move(parameter_bindings))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to swap graph");
  }
#else
  (void)spec;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "graph support is not enabled");
#endif
}

int RealtimeEngineWasm::graphNodeCount() const {
#if defined(SONARE_WITH_GRAPH)
  return static_cast<int>(engine_.graph_node_count());
#else
  return 0;
#endif
}

int RealtimeEngineWasm::graphConnectionCount() const {
#if defined(SONARE_WITH_GRAPH)
  return static_cast<int>(engine_.graph_connection_count());
#else
  return 0;
#endif
}

val RealtimeEngineWasm::process(val channels_val) {
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
void RealtimeEngineWasm::prepareChannels(int num_channels, int max_frames) {
  if (num_channels <= 0 || num_channels > 64 || max_frames <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "RealtimeEngine.prepareChannels: channels must be within 1..64; "
                                  "max_frames must be positive");
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

val RealtimeEngineWasm::getChannelBuffer(int channel, int num_frames) {
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

void RealtimeEngineWasm::processPrepared(int num_frames) {
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

// Cue-bus companion to the prepared path. process() folds the monitor bus into
// the program output for historical compatibility, so a host that wants PFL/AFL
// as a SEPARATE AudioWorklet output must drive processPreparedWithMonitor()
// instead: the program plane stays untouched and the cue lands in its own
// planes. Call prepareMonitorChannels() once, off the audio thread, after
// prepareChannels().
void RealtimeEngineWasm::prepareMonitorChannels(int num_channels, int max_frames) {
  if (num_channels <= 0 || num_channels > 64 || max_frames <= 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "RealtimeEngine.prepareMonitorChannels: channels must be within 1..64; "
        "max_frames must be positive");
  }
  monitor_channels_ = num_channels;
  monitor_capacity_ = max_frames;
  monitor_storage_.assign(static_cast<size_t>(num_channels),
                          std::vector<float>(static_cast<size_t>(max_frames), 0.0f));
  monitor_ptrs_.clear();
  monitor_ptrs_.reserve(monitor_storage_.size());
  for (auto& channel : monitor_storage_) {
    monitor_ptrs_.push_back(channel.data());
  }
}

val RealtimeEngineWasm::getMonitorChannelBuffer(int channel, int num_frames) {
  if (channel < 0 || channel >= monitor_channels_) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "RealtimeEngine.getMonitorChannelBuffer: channel out of range; call "
        "prepareMonitorChannels() first");
  }
  if (num_frames <= 0 || num_frames > monitor_capacity_) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "RealtimeEngine.getMonitorChannelBuffer: out-of-range frame "
                                  "count");
  }
  return val(typed_memory_view(static_cast<size_t>(num_frames),
                               monitor_storage_[static_cast<size_t>(channel)].data()));
}

void RealtimeEngineWasm::processPreparedWithMonitor(int num_frames) {
  if (prepared_channels_ <= 0 || prepared_storage_.empty()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "RealtimeEngine.processPreparedWithMonitor: prepareChannels() "
                                  "must be called first");
  }
  if (monitor_channels_ <= 0 || monitor_storage_.empty()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "RealtimeEngine.processPreparedWithMonitor: "
                                  "prepareMonitorChannels() must be called first");
  }
  // The engine writes one cue plane per program channel, so a narrower monitor
  // plane would be written past its end.
  if (monitor_channels_ < prepared_channels_) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "RealtimeEngine.processPreparedWithMonitor: monitor channel "
                                  "count must be at least the prepared channel count");
  }
  if (num_frames <= 0 || num_frames > prepared_capacity_ || num_frames > monitor_capacity_) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "RealtimeEngine.processPreparedWithMonitor: out-of-range frame "
                                  "count");
  }
  engine_.process_with_monitor(prepared_ptrs_.data(), monitor_ptrs_.data(), prepared_channels_,
                               num_frames);
}

val RealtimeEngineWasm::processWithMonitor(val channels_val) {
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

val RealtimeEngineWasm::renderOffline(val channels_val, int block_size, bool finalize) {
  // Mirror the C-ABI oracle (sonare_engine_render_offline): a never-prepared
  // engine renders nothing and cannot signal through telemetry, so fail closed
  // instead of handing back a silent buffer that reads as a completed render.
  if (engine_.max_block_size() <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "engine not prepared");
  }
  // Match the C-ABI oracle (sonare_engine_render_offline): a non-positive block
  // size is an error, not silently clamped to 1 as the core would do (WASM
  // bypasses the C-ABI guard, and the sibling bounce/freeze paths reject it).
  if (block_size <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "renderOffline block size must be positive");
  }
  ChannelBlock block = readChannels(channels_val);
  engine_.render_offline(block.pointers.data(), static_cast<int>(block.storage.size()),
                         block.frames, block_size, finalize);
  return channelsToJs(block);
}

void RealtimeEngineWasm::finishOfflineRender() {
  // Mirror the C-ABI oracle (sonare_engine_finish_offline_render): a
  // never-prepared engine has nothing to release and no way to report, so fail
  // closed rather than returning as though a render had been finalized.
  if (engine_.max_block_size() <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "engine not prepared");
  }
  engine_.finish_offline_render();
}

val RealtimeEngineWasm::bounceOffline(val options_val) {
  const int64_t total_frames = objectProperty(options_val, "totalFrames").as<int64_t>();
  const int block_size = intProperty(options_val, "blockSize", 128);
  const int num_channels = intProperty(options_val, "numChannels", 2);
  const int source_sample_rate = intProperty(options_val, "sourceSampleRate", 48000);
  const int target_sample_rate = intProperty(options_val, "targetSampleRate", 48000);
  // Read ditherBits up front so a negative value is rejected exactly as the
  // C-ABI oracle does (sonare_engine_bounce_offline: dither_bits < 0 ->
  // SONARE_ERROR_INVALID_PARAMETER) instead of being silently clamped to 16.
  const int dither_bits = intProperty(options_val, "ditherBits", 16);
  // Same for the dither type: the oracle rejects an out-of-range ordinal before
  // it renders anything rather than silently mapping it to None, which would
  // hand back undithered audio with no way to tell the request was ignored.
  const int dither = intProperty(options_val, "dither", 0);
  if (total_frames <= 0 || block_size <= 0 || num_channels <= 0 || source_sample_rate <= 0 ||
      target_sample_rate <= 0 || dither_bits < 0 || dither < 0 || dither > 3 ||
      !sonare::resource::engine_bounce_shape_fits(total_frames, num_channels, source_sample_rate,
                                                  target_sample_rate)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid bounce options");
  }
  // The bounce width must map to a supported speaker layout (1 mono, 2 stereo,
  // 6 = 5.1, 8 = 7.1); counts like 3/4/5/7 have no layout and would silently
  // leave their extra planes unpanned. Mirror the C-ABI oracle round-trip
  // (sonare_c_engine.cpp) so WASM rejects them instead of writing garbage planes.
  if (sonare::channel_count(sonare::layout_from_channel_count(num_channels)) != num_channels) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "unsupported bounce channel count");
  }
  // Mirror the C-ABI oracle (sonare_engine_bounce_offline): a never-prepared
  // engine renders silence with no telemetry channel, so fail closed rather than
  // return a bounce result the caller would read as a valid silent render.
  if (engine_.max_block_size() <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "engine not prepared");
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
    float target_lufs = floatProperty(options_val, "targetLufs", SONARE_DEFAULT_BOUNCE_TARGET_LUFS);
    if (target_lufs == 0.0f || !std::isfinite(target_lufs)) {
      target_lufs = SONARE_DEFAULT_BOUNCE_TARGET_LUFS;
    }
    metering::normalize_interleaved_to_lufs(interleaved, frames, num_channels, target_sample_rate,
                                            target_lufs);
  }

  if (dither != 0) {
    mastering::final::DitherConfig config{};
    config.type = ditherTypeFromInt(dither);
    // A negative ditherBits was already rejected above; 0 is not an error for
    // the oracle, so keep the "0 -> library default (16)" promotion here.
    config.target_bits = dither_bits;
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

val RealtimeEngineWasm::freezeOffline(val options_val) {
  const int64_t total_frames = objectProperty(options_val, "totalFrames").as<int64_t>();
  const int block_size = intProperty(options_val, "blockSize", 128);
  const int num_channels = intProperty(options_val, "numChannels", 2);
  if (total_frames <= 0 || block_size <= 0 || num_channels <= 0 ||
      !sonare::resource::engine_offline_shape_fits(total_frames, num_channels, 1)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid freeze options");
  }
  // Mirror the C-ABI oracle (sonare_engine_freeze_offline): freezing a
  // never-prepared engine would capture pure silence with no error channel.
  if (engine_.max_block_size() <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "engine not prepared");
  }

  std::vector<std::vector<float>> frozen(static_cast<size_t>(num_channels),
                                         std::vector<float>(static_cast<size_t>(total_frames)));
  std::vector<float*> render_pointers;
  render_pointers.reserve(frozen.size());
  for (auto& channel : frozen) {
    render_pointers.push_back(channel.data());
  }
  engine_.render_offline(render_pointers.data(), num_channels, total_frames, block_size);

  std::vector<std::vector<std::vector<float>>> new_storage;
  std::vector<std::vector<const float*>> new_ptrs;
  new_storage.push_back(std::move(frozen));
  new_ptrs.emplace_back();
  new_ptrs.back().reserve(new_storage.back().size());
  for (const auto& channel : new_storage.back()) {
    new_ptrs.back().push_back(channel.data());
  }

  sonare::engine::ClipSchedule schedule{};
  schedule.id = static_cast<uint32_t>(intProperty(options_val, "clipId", 1));
  if (schedule.id == 0) schedule.id = 1;
  schedule.buffer = {new_ptrs.back().data(), num_channels, total_frames};
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
  // Mirror the C-ABI oracle (sonare_c_engine.cpp): a non-finite or negative
  // startPpq yields an undefined clip position, and a non-finite/negative gain
  // fills NaN or phase-inverts the frozen clip. Reject up front instead of
  // letting WASM produce a corrupt freeze where C/Node/Python error.
  if (!std::isfinite(schedule.start_ppq) ||
      !sonare::transport::valid_public_ppq(schedule.start_ppq) || !std::isfinite(schedule.gain) ||
      schedule.gain < 0.0f) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid freeze startPpq or gain");
  }
  clip_storage_ = std::move(new_storage);
  clip_ptrs_ = std::move(new_ptrs);
  engine_.set_clips({schedule});

  val out = val::object();
  out.set("clipId", schedule.id);
  out.set("frames", static_cast<double>(total_frames));
  out.set("numChannels", num_channels);
  return out;
}

void registerRealtimeEngineProcessing(class_<RealtimeEngineWasm>& cls) {
  cls.function("setGraph", &RealtimeEngineWasm::setGraph)
      .function("graphNodeCount", &RealtimeEngineWasm::graphNodeCount)
      .function("graphConnectionCount", &RealtimeEngineWasm::graphConnectionCount)
      .function("process", &RealtimeEngineWasm::process)
      .function("prepareChannels", &RealtimeEngineWasm::prepareChannels)
      .function("getChannelBuffer", &RealtimeEngineWasm::getChannelBuffer)
      .function("processPrepared", &RealtimeEngineWasm::processPrepared)
      .function("prepareMonitorChannels", &RealtimeEngineWasm::prepareMonitorChannels)
      .function("getMonitorChannelBuffer", &RealtimeEngineWasm::getMonitorChannelBuffer)
      .function("processPreparedWithMonitor", &RealtimeEngineWasm::processPreparedWithMonitor)
      .function("processWithMonitor", &RealtimeEngineWasm::processWithMonitor)
      .function("renderOffline", &RealtimeEngineWasm::renderOffline)
      .function("finishOfflineRender", &RealtimeEngineWasm::finishOfflineRender)
      .function("bounceOffline", &RealtimeEngineWasm::bounceOffline)
      .function("freezeOffline", &RealtimeEngineWasm::freezeOffline);
}

#endif  // __EMSCRIPTEN__
