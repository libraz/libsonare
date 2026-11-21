/// @file realtime_engine_processing.cpp
/// @brief Embind realtime-engine facade: audio processing, graph & offline.

#ifdef __EMSCRIPTEN__

#include <cmath>
#include <string>

#include "realtime_engine_wasm.h"
#include "sonare/sonare_c_types_engine.h"
#include "util/resource_limits.h"

namespace {

/// Channel ceiling for the prepared scratch rows, taken from the engine's own
/// constant so the guards and the messages below move with it.
constexpr int kMaxPreparedChannels =
    static_cast<int>(sonare::engine::RealtimeEngine::kMaxAudioChannels);

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
    throw WasmRangeError("channels must not be empty");
  }
  ChannelBlock block;
  block.storage.reserve(static_cast<size_t>(count));
  block.pointers.reserve(static_cast<size_t>(count));
  for (int ch = 0; ch < count; ++ch) {
    std::vector<float> channel = float32ArrayToVector(channels_val[ch]);
    if (ch == 0) {
      block.frames = static_cast<int>(channel.size());
      if (block.frames <= 0) {
        throw WasmRangeError("channels must not be empty");
      }
    } else if (static_cast<int>(channel.size()) != block.frames) {
      throw WasmRangeError("all channels must have the same length");
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
      throw WasmRangeError("unsupported dither type");
  }
}

void RealtimeEngineWasm::setGraph(val spec) {
#if defined(SONARE_WITH_GRAPH)
  auto graph = std::make_unique<sonare::graph::Graph>();
  val nodes = spec["nodes"];
  const int node_count = static_cast<int>(wasmArrayLikeLength(nodes, "nodes"));
  if (node_count <= 0) {
    throw WasmRangeError("graph nodes must not be empty");
  }
  const int num_channels = intProperty(spec, "numChannels", 2);
  for (int i = 0; i < node_count; ++i) {
    val node = nodes[i];
    auto processor = makeWasmGraphProcessor(node);
    if (!processor) {
      throw WasmRangeError("unsupported graph node type");
    }
    const std::string id = stringProperty(node, "id", "");
    // The C ABI holds a node id in a fixed field; the same id is refused on every surface.
    constexpr size_t kMaxNodeIdBytes = sizeof(SonareEngineGraphNode{}.id) - 1;
    if (id.size() > kMaxNodeIdBytes) {
      throw WasmRangeError("graph node id must be at most " + std::to_string(kMaxNodeIdBytes) +
                           " bytes");
    }
    // Match the C ABI (sonare_engine_set_graph): an explicit `numPorts: 0`, which
    // intProperty passes through as-is, asks for num_channels. A negative count
    // is refused there and must be refused here for the same value.
    const int requested_ports = intProperty(node, "numPorts", num_channels);
    if (requested_ports < 0) {
      throw WasmRangeError("graph node numPorts must not be negative");
    }
    const int ports = requested_ports == 0 ? num_channels : requested_ports;
    if (!graph->add_node(id, std::move(processor), ports)) {
      throw WasmRangeError("failed to add graph node");
    }
  }
  val connections = spec["connections"];
  const int connection_count = static_cast<int>(wasmArrayLikeLength(connections, "connections"));
  for (int i = 0; i < connection_count; ++i) {
    val connection = connections[i];
    sonare::graph::Connection graph_connection{};
    graph_connection.source_node = stringProperty(connection, "sourceNode", "");
    graph_connection.source_port = intProperty(connection, "sourcePort", 0);
    graph_connection.dest_node = stringProperty(connection, "destNode", "");
    graph_connection.dest_port = intProperty(connection, "destPort", 0);
    // 0 = Replace, 1 = Add (the default when omitted); anything else is refused, not read as Add.
    const int mix = intProperty(connection, "mix", 1);
    if (mix != 0 && mix != 1) {
      throw WasmRangeError("graph connection mix must be 0 (replace) or 1 (add)");
    }
    graph_connection.mix =
        mix == 0 ? sonare::graph::Connection::Mix::Replace : sonare::graph::Connection::Mix::Add;
    if (!graph->connect(std::move(graph_connection))) {
      throw WasmRangeError("failed to connect graph");
    }
  }
  if (!graph->compile()) {
    throw WasmRangeError("failed to compile graph");
  }
  const auto state = engine_.transport().snapshot_control();
  graph->prepare(state.sample_rate, engine_.max_block_size());
  const std::string input_node = stringProperty(spec, "inputNode", "");
  const std::string output_node = stringProperty(spec, "outputNode", "");
  std::vector<sonare::engine::GraphRuntime::ParameterBinding> parameter_bindings;
  if (hasProperty(spec, "parameterBindings")) {
    val bindings = spec["parameterBindings"];
    const int binding_count = static_cast<int>(wasmArrayLikeLength(bindings, "parameterBindings"));
    parameter_bindings.reserve(static_cast<size_t>(std::max(binding_count, 0)));
    for (int i = 0; i < binding_count; ++i) {
      val binding = bindings[i];
      parameter_bindings.push_back(
          {uintProperty(binding, "paramId", 0), stringProperty(binding, "nodeId", "")});
    }
  }
  if (!engine_.swap_graph(std::move(graph), wasmCString(input_node, "inputNode"),
                          wasmCString(output_node, "outputNode"), num_channels,
                          std::move(parameter_bindings))) {
    throw WasmRangeError("failed to swap graph");
  }
#else
  (void)spec;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "graph support is not compiled in");
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

void RealtimeEngineWasm::allocatePlanes(std::vector<std::vector<float>>& storage,
                                        std::vector<float*>& ptrs, int num_channels,
                                        int num_frames) {
  storage.assign(static_cast<size_t>(num_channels),
                 std::vector<float>(static_cast<size_t>(num_frames), 0.0f));
  ptrs.clear();
  ptrs.reserve(storage.size());
  for (auto& channel : storage) ptrs.push_back(channel.data());
}

// ---- Zero-copy "prepared" realtime path ------------------------------
// The AudioWorklet render thread fills the per-channel input views (returned
// as typed_memory_views onto persistent WASM-heap storage), calls
// processPrepared(numFrames) which runs engine_.process() IN PLACE, then reads
// the same views back. No std::vector or JS Float32Array is allocated per
// quantum, so process() never touches the C++/JS heap allocators on the audio
// thread (mirrors RealtimeVoiceChanger's prepared API). Call
// prepareChannels(numChannels, maxFrames) once on the main thread first.
void RealtimeEngineWasm::prepareChannels(const val& num_channels_val, const val& max_frames_val) {
  const int num_channels = checkedIntFromVal(num_channels_val, "numChannels");
  const int max_frames = checkedIntFromVal(max_frames_val, "maxFrames");
  if (num_channels <= 0 || num_channels > kMaxPreparedChannels || max_frames <= 0) {
    throw WasmRangeError("channels must be within 1.." + std::to_string(kMaxPreparedChannels) +
                         "; maxFrames must be an integer of at least 1");
  }
  prepared_channels_ = num_channels;
  prepared_capacity_ = max_frames;
  allocatePlanes(prepared_storage_, prepared_ptrs_, num_channels, max_frames);
}

val RealtimeEngineWasm::getChannelBuffer(const val& channel_val, const val& num_frames_val) {
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int num_frames = checkedIntFromVal(num_frames_val, "numFrames");
  if (channel < 0 || channel >= prepared_channels_) {
    throw WasmRangeError(
        "channel out of range; call "
        "prepareChannels() first");
  }
  if (num_frames <= 0 || num_frames > prepared_capacity_) {
    throw WasmRangeError("out-of-range frame count");
  }
  return val(typed_memory_view(static_cast<size_t>(num_frames),
                               prepared_storage_[static_cast<size_t>(channel)].data()));
}

void RealtimeEngineWasm::processPrepared(const val& num_frames_val) {
  const int num_frames = checkedIntFromVal(num_frames_val, "numFrames");
  if (prepared_channels_ <= 0 || prepared_storage_.empty()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "prepareChannels() must be "
                                  "called first");
  }
  if (num_frames <= 0 || num_frames > prepared_capacity_) {
    throw WasmRangeError("out-of-range frame count");
  }
  engine_.process(prepared_ptrs_.data(), prepared_channels_, num_frames);
}

// Cue-bus companion to the prepared path. process() folds the monitor bus into
// the program output for historical compatibility, so a host that wants PFL/AFL
// as a SEPARATE AudioWorklet output must drive processPreparedWithMonitor()
// instead: the program plane stays untouched and the cue lands in its own
// planes. Call prepareMonitorChannels() once, off the audio thread, after
// prepareChannels().
void RealtimeEngineWasm::prepareMonitorChannels(const val& num_channels_val,
                                                const val& max_frames_val) {
  const int num_channels = checkedIntFromVal(num_channels_val, "numChannels");
  const int max_frames = checkedIntFromVal(max_frames_val, "maxFrames");
  if (num_channels <= 0 || num_channels > kMaxPreparedChannels || max_frames <= 0) {
    throw WasmRangeError(
        "channels must be "
        "within 1.." +
        std::to_string(kMaxPreparedChannels) + "; maxFrames must be an integer of at least 1");
  }
  monitor_channels_ = num_channels;
  monitor_capacity_ = max_frames;
  allocatePlanes(monitor_storage_, monitor_ptrs_, num_channels, max_frames);
}

val RealtimeEngineWasm::getMonitorChannelBuffer(const val& channel_val, const val& num_frames_val) {
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int num_frames = checkedIntFromVal(num_frames_val, "numFrames");
  if (channel < 0 || channel >= monitor_channels_) {
    throw WasmRangeError(
        "channel out of range; call "
        "prepareMonitorChannels() first");
  }
  if (num_frames <= 0 || num_frames > monitor_capacity_) {
    throw WasmRangeError(
        "out-of-range frame "
        "count");
  }
  return val(typed_memory_view(static_cast<size_t>(num_frames),
                               monitor_storage_[static_cast<size_t>(channel)].data()));
}

void RealtimeEngineWasm::processPreparedWithMonitor(const val& num_frames_val) {
  const int num_frames = checkedIntFromVal(num_frames_val, "numFrames");
  if (prepared_channels_ <= 0 || prepared_storage_.empty()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "prepareChannels() "
                                  "must be called first");
  }
  if (monitor_channels_ <= 0 || monitor_storage_.empty()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  ""
                                  "prepareMonitorChannels() must be called first");
  }
  // The engine writes one cue plane per program channel, so a narrower monitor
  // plane would be written past its end.
  if (monitor_channels_ < prepared_channels_) {
    throw WasmRangeError(
        "monitor channel "
        "count must be at least the prepared channel count");
  }
  if (num_frames <= 0 || num_frames > prepared_capacity_ || num_frames > monitor_capacity_) {
    throw WasmRangeError(
        "out-of-range frame "
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

val RealtimeEngineWasm::renderOffline(val channels_val, const val& block_size_val, bool finalize) {
  int block_size = (block_size_val.isUndefined() || block_size_val.isNull())
                       ? 0
                       : checkedIntFromVal(block_size_val, "blockSize");
  // Mirror the C-ABI oracle (sonare_engine_render_offline): a never-prepared
  // engine renders nothing and cannot signal through telemetry, so fail closed
  // instead of handing back a silent buffer that reads as a completed render.
  if (engine_.max_block_size() <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "engine not prepared");
  }
  // Match the C-ABI oracle (sonare_engine_render_offline): a negative block
  // size is an error, and 0 selects the default offline block.
  if (block_size < 0) {
    throw WasmRangeError("block size must be an integer of at least 0");
  }
  block_size = resolveOfflineBlock(block_size);
  ChannelBlock block = readChannels(channels_val);
  // More channels or a larger block than the engine was prepared for would write zeros past the
  // bound, as the C ABI refuses.
  if (static_cast<int>(block.storage.size()) > engine_.prepared_channels() ||
      block_size > engine_.max_block_size()) {
    throw WasmRangeError("channel count or block size exceeds the prepared engine");
  }
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
  const int64_t total_frames = int64Property(options_val, "totalFrames", 0);
  int block_size = intProperty(options_val, "blockSize", 0);
  const int num_channels = requireIntProperty(options_val, "numChannels", "options");
  // 0 (or absent) selects the prepared rate for the source and the source rate for the target.
  const int requested_source_rate = intProperty(options_val, "sourceSampleRate", 0);
  const int requested_target_rate = intProperty(options_val, "targetSampleRate", 0);
  const int source_sample_rate = requested_source_rate == 0
                                     ? static_cast<int>(std::lround(engine_.sample_rate()))
                                     : requested_source_rate;
  const int target_sample_rate =
      requested_target_rate == 0 ? source_sample_rate : requested_target_rate;
  // Read ditherBits up front so a negative value is rejected exactly as the
  // C-ABI oracle does (sonare_engine_bounce_offline: dither_bits < 0 ->
  // SONARE_ERROR_INVALID_PARAMETER) instead of being silently clamped to 16.
  const int dither_bits = intProperty(options_val, "ditherBits", 16);
  // Same for the dither type: the oracle rejects an out-of-range ordinal before
  // it renders anything rather than silently mapping it to None, which would
  // hand back undithered audio with no way to tell the request was ignored.
  const int dither = intProperty(options_val, "dither", 0);
  if (total_frames <= 0) throw WasmRangeError("totalFrames must be positive");
  if (block_size < 0) throw WasmRangeError("blockSize must not be negative");
  if (num_channels <= 0) throw WasmRangeError("numChannels must be positive");
  if (requested_target_rate < 0) throw WasmRangeError("targetSampleRate must not be negative");
  if (requested_source_rate < 0) {
    throw WasmRangeError("source_sample_rate must be an integer in [0, 2147483647]");
  }
  if (dither_bits < 0) throw WasmRangeError("ditherBits must not be negative");
  if (source_sample_rate <= 0) {
    throw WasmRangeError("source_sample_rate is 0 and the engine has no prepared sample rate");
  }
  if (dither < 0 || dither > 3) throw WasmRangeError("dither must be an ordinal in [0, 3]");
  if (!sonare::resource::engine_bounce_shape_fits(total_frames, num_channels, source_sample_rate,
                                                  target_sample_rate, dither != 0)) {
    throw WasmRangeError(
        "totalFrames, numChannels and the sample rates give a result larger than the supported "
        "size");
  }
  // The bounce width must map to a supported speaker layout (1 mono, 2 stereo,
  // 6 = 5.1, 8 = 7.1); counts like 3/4/5/7 have no layout and would silently
  // leave their extra planes unpanned. Mirror the C-ABI oracle round-trip
  // (sonare_c_engine.cpp) so WASM rejects them instead of writing garbage planes.
  if (sonare::channel_count(sonare::layout_from_channel_count(num_channels)) != num_channels) {
    throw WasmRangeError("unsupported bounce channel count");
  }
  // Mirror the C-ABI oracle (sonare_engine_bounce_offline): a never-prepared
  // engine renders silence with no telemetry channel, so fail closed rather than
  // return a bounce result the caller would read as a valid silent render.
  if (engine_.max_block_size() <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "engine not prepared");
  }
  block_size = resolveOfflineBlock(block_size);
  // Bouncing more channels than the engine was prepared for would write zeros for every plane
  // past the bound.
  if (num_channels > engine_.prepared_channels() || block_size > engine_.max_block_size()) {
    throw WasmRangeError("bounce channel count or block size exceeds the prepared engine");
  }
  // The render runs at the prepared rate, which an explicit sourceSampleRate must name exactly.
  if (requested_source_rate > 0 &&
      engine_.sample_rate() != static_cast<double>(requested_source_rate)) {
    throw WasmRangeError("source_sample_rate must equal the prepared sample rate " +
                         std::to_string(static_cast<long long>(engine_.sample_rate())));
  }
  if (dither != 0 && (dither_bits < mastering::final::kMinDitherTargetBits ||
                      dither_bits > mastering::final::kMaxDitherTargetBits)) {
    throw WasmRangeError("ditherBits out of range");
  }
  // Every option is read before the render, which consumes engine state.
  const bool normalize_lufs = boolProperty(options_val, "normalizeLufs", false);
  // As in the C ABI, every finite target is literal and only absence selects the default.
  const float target_lufs =
      floatProperty(options_val, "targetLufs", SONARE_DEFAULT_BOUNCE_TARGET_LUFS);
  const uint32_t dither_seed =
      uintProperty(options_val, "ditherSeed", SONARE_ENGINE_DEFAULT_DITHER_SEED);

  // Offline pre-roll, as in the C-ABI oracle (sonare_engine_bounce_offline): a
  // bounce is a one-shot render, so the first audible block must open at the
  // settled fader/pan/gate values instead of ramping in from the defaults.
  engine_.prime_offline_parameters(num_channels, block_size);
  std::vector<std::vector<float>> channels =
      engine_.render_offline_aligned(num_channels, total_frames, block_size);

  if (source_sample_rate != target_sample_rate) {
    for (auto& channel : channels) {
      channel = resample(channel.data(), channel.size(), source_sample_rate, target_sample_rate);
    }
  }

  std::vector<float> interleaved = interleave(channels);
  const size_t frames = channels.empty() ? 0 : channels[0].size();
  if (normalize_lufs) {
    metering::normalize_interleaved_to_lufs(interleaved, frames, num_channels, target_sample_rate,
                                            target_lufs);
  }

  if (dither != 0) {
    mastering::final::DitherConfig config{};
    config.type = ditherTypeFromInt(dither);
    config.target_bits = dither_bits;
    config.seed = dither_seed;
    Audio dithered = mastering::final::dither_interleaved(
        Audio::from_buffer(interleaved.data(), interleaved.size(), target_sample_rate),
        static_cast<size_t>(num_channels), config);
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
  const int64_t total_frames = int64Property(options_val, "totalFrames", 0);
  int block_size = intProperty(options_val, "blockSize", 0);
  const int num_channels = requireIntProperty(options_val, "numChannels", "options");
  if (total_frames <= 0 || block_size < 0 || num_channels <= 0 ||
      !sonare::resource::engine_offline_shape_fits(total_frames, num_channels, 1)) {
    throw WasmRangeError("invalid freeze options");
  }
  // Mirror the C-ABI oracle (sonare_engine_freeze_offline): freezing a
  // never-prepared engine would capture pure silence with no error channel.
  if (engine_.max_block_size() <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "engine not prepared");
  }
  block_size = resolveOfflineBlock(block_size);
  // Freezing more channels than the engine was prepared for would write zeros for every plane
  // past the bound.
  if (num_channels > engine_.prepared_channels() || block_size > engine_.max_block_size()) {
    throw WasmRangeError("freeze channel count or block size exceeds the prepared engine");
  }

  uint32_t clip_id = uintProperty(options_val, "clipId", 1);
  if (clip_id == 0) clip_id = 1;
  // Read startPpq at full double precision to match setClips() and the
  // double-typed ClipSchedule.start_ppq field; a Float32 read would quantize a
  // frozen clip at a large PPQ position to a different sample than the same
  // clip placed via setClips.
  const double start_ppq = hasProperty(options_val, "startPpq")
                               ? numberFromVal(objectProperty(options_val, "startPpq"), "startPpq")
                               : 0.0;
  const float gain = floatProperty(options_val, "gain", 1.0f);
  // Mirror the C-ABI oracle (sonare_c_engine.cpp): a non-finite or negative
  // startPpq yields an undefined clip position, and a non-finite/negative gain
  // fills NaN or phase-inverts the frozen clip. Reject up front instead of
  // letting WASM produce a corrupt freeze where C/Node/Python error.
  if (!std::isfinite(start_ppq) || !sonare::transport::valid_public_ppq(start_ppq) ||
      !std::isfinite(gain) || gain < 0.0f) {
    throw WasmRangeError("invalid freeze startPpq or gain");
  }

  // Offline pre-roll, as in the C-ABI oracle (sonare_engine_freeze_offline): the
  // frozen clip captures the lane at its settled values rather than a fade-in
  // the live lane never had.
  engine_.prime_offline_parameters(num_channels, block_size);
  // The engine bakes the source layer, installs the clip and withdraws the MIDI clips.
  const sonare::engine::ClipSchedule schedule =
      engine_.freeze_offline(num_channels, total_frames, block_size, clip_id, start_ppq, gain);
  std::vector<std::shared_ptr<const sonare::engine::ClipAudioStorage>> new_storage;
  new_storage.push_back(schedule.storage);
  std::vector<uint32_t> new_clip_ids{schedule.id};
  // The frozen audio is engine-owned, so prebakedClipChannels() can hand it back.
  std::vector<uint8_t> new_clip_tempo_baked{1};
  clip_storage_ = std::move(new_storage);
  clip_ids_ = std::move(new_clip_ids);
  clip_tempo_baked_ = std::move(new_clip_tempo_baked);

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
