/// @file realtime_engine_processing.cpp
/// @brief Embind realtime-engine facade: audio processing, graph & offline.

#ifdef __EMSCRIPTEN__

#include <array>
#include <cmath>
#include <string>

#include "c_api/engine_config_domains.h"
#include "realtime_engine_wasm.h"
#include "sonare/sonare_c_types_engine.h"
#include "util/refusal_predicates.h"
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
  const int count = static_cast<int>(wasmPlaneListLength(channels_val, "channels"));
  if (count <= 0) {
    throw WasmRangeError("channels must not be empty");
  }
  ChannelBlock block;
  block.storage.reserve(static_cast<size_t>(count));
  block.pointers.reserve(static_cast<size_t>(count));
  for (int ch = 0; ch < count; ++ch) {
    const std::string channel_name = "channels[" + std::to_string(ch) + "]";
    std::vector<float> channel = float32ArrayToVector(channels_val[ch], channel_name.c_str());
    if (ch == 0) {
      block.frames = static_cast<int>(channel.size());
      if (block.frames <= 0) {
        throw WasmRangeError("channels must not be empty");
      }
    } else if (static_cast<int>(channel.size()) != block.frames) {
      throw WasmRangeError(channel_name + " must have the same length as channels[0]");
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
  const int node_count = static_cast<int>(wasmArrayLength(nodes, "nodes"));
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
    // An omitted numPorts takes the spec's channel count; a written one is read in its domain.
    const std::string ports_subject = "spec.nodes[" + std::to_string(i) + "].numPorts";
    const int ports = optionalDomainValue<int>(node["numPorts"], ports_subject.c_str(),
                                               sonare_c_detail::kEngineGraphNodePortsDomain)
                          .value_or(num_channels);
    if (!graph->add_node(id, std::move(processor), ports)) {
      throw WasmRangeError("failed to add graph node");
    }
  }
  val connections = spec["connections"];
  const int connection_count = static_cast<int>(wasmArrayLength(connections, "connections"));
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
    const int binding_count = static_cast<int>(wasmArrayLength(bindings, "parameterBindings"));
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

val RealtimeEngineWasm::process(val channels_val, const val& with_monitor_val) {
  ChannelBlock block = readChannels(channels_val);
  if (optionalBool(with_monitor_val, "withMonitor").value_or(false)) {
    return processBlockWithMonitor(block);
  }
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
  sonare::util::check::integer_in_range(num_channels, 1, kMaxPreparedChannels, "numChannels");
  sonare::util::check::integer_at_least(max_frames, 1, "maxFrames");
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

void RealtimeEngineWasm::processPrepared(const val& num_frames_val, const val& with_monitor_val) {
  const int num_frames = checkedIntFromVal(num_frames_val, "numFrames");
  if (optionalBool(with_monitor_val, "withMonitor").value_or(false)) {
    processPreparedWithMonitor(num_frames);
    return;
  }
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
// as a SEPARATE AudioWorklet output must call processPrepared(numFrames, true)
// instead: the program plane stays untouched and the cue lands in its own
// planes. Call prepareMonitorChannels() once, off the audio thread, after
// prepareChannels().
void RealtimeEngineWasm::prepareMonitorChannels(const val& num_channels_val,
                                                const val& max_frames_val) {
  const int num_channels = checkedIntFromVal(num_channels_val, "numChannels");
  const int max_frames = checkedIntFromVal(max_frames_val, "maxFrames");
  sonare::util::check::integer_in_range(num_channels, 1, kMaxPreparedChannels, "numChannels");
  sonare::util::check::integer_at_least(max_frames, 1, "maxFrames");
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

void RealtimeEngineWasm::processPreparedWithMonitor(int num_frames) {
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

val RealtimeEngineWasm::processBlockWithMonitor(ChannelBlock& block) {
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
                       : domainNumberFromVal<int>(block_size_val, "blockSize",
                                                  sonare_c_detail::kEngineRenderBlockSizeDomain);
  // A never-prepared engine renders nothing and cannot signal through telemetry.
  requirePrepared();
  // 0 selects the default offline block.
  block_size = resolveOfflineBlock(block_size);
  ChannelBlock block = readChannels(channels_val);
  // Planes past the prepared count would be written as silence rather than refused.
  checkRenderChannels(static_cast<int>(block.storage.size()), "numChannels");
  checkPreparedBlock(block_size, "blockSize");
  engine_.render_offline(block.pointers.data(), static_cast<int>(block.storage.size()),
                         block.frames, block_size, finalize);
  return channelsToJs(block);
}

void RealtimeEngineWasm::finishOfflineRender() {
  // Mirror the C-ABI oracle (sonare_engine_finish_offline_render): a
  // never-prepared engine has nothing to release and no way to report, so fail
  // closed rather than returning as though a render had been finalized.
  requirePrepared();
  engine_.finish_offline_render();
}

val RealtimeEngineWasm::bounceOffline(val options_val) {
  const int64_t total_frames = int64Property(options_val, "totalFrames", 0);
  // An omitted blockSize is the automatic offline block (0 below); a written one is read in its
  // domain, so a typed 0 is refused as on the C ABI.
  int block_size = optionalDomainValue<int>(options_val["blockSize"], "blockSize",
                                            sonare_c_detail::kEngineOfflineBlockDomain)
                       .value_or(0);
  const int num_channels = requireIntProperty(options_val, "numChannels", "");
  // An omitted rate selects the prepared rate for the source and the source rate for the target.
  const int requested_source_rate =
      optionalDomainValue<int>(options_val["sourceSampleRate"], "sourceSampleRate",
                               sonare_c_detail::kEngineSampleRateDomain)
          .value_or(0);
  const int requested_target_rate =
      optionalDomainValue<int>(options_val["targetSampleRate"], "targetSampleRate",
                               sonare_c_detail::kEngineSampleRateDomain)
          .value_or(0);
  const int source_sample_rate = requested_source_rate == 0
                                     ? static_cast<int>(std::lround(engine_.sample_rate()))
                                     : requested_source_rate;
  const int target_sample_rate =
      requested_target_rate == 0 ? source_sample_rate : requested_target_rate;
  const int dither_bits = intProperty(options_val, "ditherBits", 16);
  const int dither = intProperty(options_val, "dither", 0);
  const bool normalize_lufs = boolProperty(options_val, "normalizeLufs", false);
  // As in the C ABI, every finite target is literal and only absence selects the default.
  const float target_lufs =
      floatProperty(options_val, "targetLufs", SONARE_DEFAULT_BOUNCE_TARGET_LUFS);
  const uint32_t dither_seed =
      uintProperty(options_val, "ditherSeed", SONARE_ENGINE_DEFAULT_DITHER_SEED);
  // Refusals follow the C-ABI oracle's order (sonare_engine_bounce_offline).
  sonare::util::check::integer_within(total_frames, sonare_c_detail::kEngineBounceFramesDomain,
                                      "totalFrames");
  requirePrepared();
  checkRenderChannels(num_channels, "numChannels");
  // Widths with no speaker layout (3, 4, 5, 7) would leave their extra planes unpanned.
  if (sonare::channel_count(sonare::layout_from_channel_count(num_channels)) != num_channels) {
    sonare::util::check::refuse(
        "numChannels", sonare::util::refusal::one_of_predicate(std::array<int, 4>{1, 2, 6, 8}));
  }
  sonare::util::check::finite_float(target_lufs, "targetLufs");
  sonare::util::check::one_of(dither, std::array<int, 4>{0, 1, 2, 3}, "dither");
  if (dither != 0) {
    sonare::util::check::integer_within(dither_bits, sonare_c_detail::kEngineDitherBitsDomain,
                                        "ditherBits");
  }
  if (source_sample_rate <= 0) {
    sonare::util::check::refuse("sourceSampleRate",
                                "must name a rate when the engine has no prepared sample rate");
  }
  if (!sonare::resource::engine_bounce_shape_fits(total_frames, num_channels, source_sample_rate,
                                                  target_sample_rate, dither != 0)) {
    sonare::util::check::refuse_relation(
        {"totalFrames", "numChannels", "sourceSampleRate", "targetSampleRate"},
        "{0}, {1}, {2} and {3} give a result larger than the supported size");
  }
  block_size = resolveOfflineBlock(block_size);
  checkPreparedBlock(block_size, "blockSize");
  // The render runs at the prepared rate, which an explicit sourceSampleRate must name exactly.
  if (requested_source_rate > 0 &&
      engine_.sample_rate() != static_cast<double>(requested_source_rate)) {
    sonare::util::check::refuse("sourceSampleRate",
                                "must equal the prepared sample rate " +
                                    std::to_string(static_cast<long long>(engine_.sample_rate())));
  }

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
  int block_size = optionalDomainValue<int>(options_val["blockSize"], "blockSize",
                                            sonare_c_detail::kEngineOfflineBlockDomain)
                       .value_or(0);
  const int num_channels = requireIntProperty(options_val, "numChannels", "");
  uint32_t clip_id = uintProperty(options_val, "clipId", 1);
  if (clip_id == 0) clip_id = 1;
  // Read startPpq at full double precision to match setClips() and the
  // double-typed ClipSchedule.start_ppq field; a Float32 read would quantize a
  // frozen clip at a large PPQ position to a different sample than the same
  // clip placed via setClips.
  const double start_ppq = hasProperty(options_val, "startPpq")
                               ? numberFromVal(objectProperty(options_val, "startPpq"), "startPpq")
                               : 0.0;
  // A non-finite position is refused at the read, as the Node reader does.
  sonare::util::check::finite(start_ppq, "startPpq");
  const float gain = floatProperty(options_val, "gain", 1.0f);
  // Refusals follow the C-ABI oracle's order (sonare_engine_freeze_offline).
  sonare::util::check::integer_within(total_frames, sonare_c_detail::kEngineBounceFramesDomain,
                                      "totalFrames");
  requirePrepared();
  checkRenderChannels(num_channels, "numChannels");
  // A position past the public timeline has no sample a clip can start at, as setClips refuses.
  sonare::util::check::in_range(start_ppq, 0.0, sonare::transport::kMaxPublicPpq, "startPpq");
  sonare::util::check::at_least(gain, 0.0, "gain");
  if (!sonare::resource::engine_offline_shape_fits(total_frames, num_channels, 1)) {
    sonare::util::check::refuse_relation({"totalFrames", "numChannels"},
                                         "{0} times {1} exceeds the supported size");
  }
  block_size = resolveOfflineBlock(block_size);
  checkPreparedBlock(block_size, "blockSize");

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
      .function("renderOffline", &RealtimeEngineWasm::renderOffline)
      .function("finishOfflineRender", &RealtimeEngineWasm::finishOfflineRender)
      .function("bounceOffline", &RealtimeEngineWasm::bounceOffline)
      .function("freezeOffline", &RealtimeEngineWasm::freezeOffline);
}

#endif  // __EMSCRIPTEN__
