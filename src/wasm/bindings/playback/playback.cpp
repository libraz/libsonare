/// @file playback.cpp
/// @brief Embind bindings for the playback renderer, HRTF sets and the
///        program loudness meter.
///
/// Every call goes through the playback C ABI, so the WASM surface inherits its
/// argument checks and error codes. The build embeds no HRTF data: a headphones
/// renderer needs an HRTF set built from bytes the caller supplies.
///
/// The renderer handle carries two paths. `processPlanar` / `processInterleaved`
/// copy through embind arrays for main-thread use. The AudioWorklet path writes
/// into heap planes allocated once at construction (`inputPlane` /
/// `outputPlane`) and calls `processPrepared`, which allocates nothing and
/// returns the C ABI code instead of throwing.

#ifdef __EMSCRIPTEN__

#include <cfloat>
#include <memory>
#include <string>
#include <vector>

#include "c_api/sonare_c_error_mapping.h"
#include "wasm/bindings/common/common.h"

#if defined(SONARE_WITH_PLAYBACK)

namespace {

/// Largest input channel count any layout accepts (7.1).
constexpr int kMaxInputChannels = 8;

[[noreturn]] void throwPlaybackError(SonareError err, const std::string& context) {
  const char* detail = sonare_last_error_message();
  std::string message = context + ": ";
  message += detail != nullptr && detail[0] != '\0' ? detail : sonare_error_message(err);
  throw SonareException(sonare_c_detail::error_code_from_c_error(err), message);
}

void checkPlayback(SonareError err, const char* context) {
  if (err != SONARE_OK) throwPlaybackError(err, context);
}

// The realtime entry points leave the thread-local message alone, so a rejected
// block is described here rather than read back.
[[noreturn]] void throwRejectedBlock(const char* entry, int in_channels, int frames) {
  throw SonareException(ErrorCode::InvalidParameter,
                        std::string(entry) + ": the renderer rejected " +
                            std::to_string(in_channels) + " input channels x " +
                            std::to_string(frames) +
                            " frames (a fixed input layout takes its own channel count, "
                            "\"auto\" takes 1, 2, 6 or 8; at most maxBlockSize frames)");
}

// The renderer's own documented contract is looser than
// loadValidatedChannelSet's (an offline-analysis policy): a 0-frame block is a
// no-op and a non-finite sample is replaced with 0 and counted by the C ABI
// itself, not rejected up front. This loader checks structure only -- array
// shape and matching channel lengths -- and leaves content (emptiness,
// finiteness) to the renderer, matching processInterleaved's own contract.
std::vector<Audio> loadPlaybackChannelSet(const val& channels, int sample_rate, const char* entry) {
  const std::string subject(entry);
  if (channels.isUndefined() || channels.isNull()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          subject + ": channels must be an array of Float32Array");
  }
  const std::size_t count = wasmArrayLikeLength(channels, "channels");
  if (count == 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          subject + ": channels must hold at least one channel");
  }
  const std::string budget = subject + " input";
  std::vector<Audio> loaded;
  loaded.reserve(std::min(count, kMaxWasmObjectArrayReserve));
  std::size_t cumulative = 0;
  std::size_t length = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const val channel = channels[index];
    if (channel.isUndefined() || channel.isNull()) {
      throw SonareException(
          ErrorCode::InvalidParameter,
          subject + ": channels[" + std::to_string(index) + "] must be a Float32Array");
    }
    const std::size_t frames =
        accumulateWasmFloat32ArrayLength(channel, "channels entry", budget.c_str(), &cumulative);
    if (index == 0) {
      length = frames;
    } else if (frames != length) {
      throw SonareException(ErrorCode::InvalidParameter, subject + ": channel lengths must match");
    }
    const std::vector<float> data = float32ArrayToVector(channel);
    loaded.push_back(Audio::from_buffer(data.data(), data.size(), sample_rate));
  }
  return loaded;
}

// Structural sibling of the loop above, for the offline (interleaved) render
// path: no emptiness or finiteness policy, matching processInterleaved's.
std::vector<float> loadPlaybackInterleaved(val samples, int channels, const char* entry,
                                           std::size_t* out_frames) {
  if (channels <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(entry) + ": channels must be positive");
  }
  std::vector<float> data = float32ArrayToVector(samples);
  if (data.size() % static_cast<std::size_t>(channels) != 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        std::string(entry) + ": interleaved sample count must be a whole number of frames");
  }
  if (out_frames != nullptr) *out_frames = data.size() / static_cast<std::size_t>(channels);
  return data;
}

std::string takeCString(char* text) {
  std::string out = text != nullptr ? text : "";
  sonare_free_string(text);
  return out;
}

class HrtfSetWasm {
 public:
  explicit HrtfSetWasm(val bytes) {
    const std::vector<uint8_t> data = uint8ArrayToVector(bytes);
    checkPlayback(sonare_hrtf_set_create_from_memory(data.data(), data.size(), &set_),
                  "HrtfSet.fromBytes");
  }
  ~HrtfSetWasm() { sonare_hrtf_set_destroy(set_); }
  HrtfSetWasm(const HrtfSetWasm&) = delete;
  HrtfSetWasm& operator=(const HrtfSetWasm&) = delete;

  const SonareHrtfSet* handle() const { return set_; }

 private:
  SonareHrtfSet* set_ = nullptr;
};

const SonareHrtfSet* hrtfHandleFromVal(const val& hrtf) {
  if (hrtf.isNull() || hrtf.isUndefined()) return nullptr;
  return hrtf.as<HrtfSetWasm*>(allow_raw_pointers())->handle();
}

class PlaybackRendererWasm {
 public:
  PlaybackRendererWasm(const std::string& config_json, const val& hrtf, const val& sample_rate_val,
                       const val& max_block_size_val) {
    sample_rate_ = checkedIntFromVal(sample_rate_val, "sampleRate");
    max_block_ = checkedIntFromVal(max_block_size_val, "maxBlockSize");
    SonarePlaybackRenderer* created = nullptr;
    checkPlayback(sonare_playback_renderer_create_json(config_json.c_str(), hrtfHandleFromVal(hrtf),
                                                       sample_rate_, max_block_, &created),
                  "PlaybackRenderer");
    renderer_.reset(created);
    checkPlayback(sonare_playback_renderer_output_channel_count(renderer_.get(), &out_channels_),
                  "PlaybackRenderer");
    const auto block = static_cast<size_t>(max_block_);
    in_planes_.assign(kMaxInputChannels, std::vector<float>(block, 0.0f));
    out_planes_.assign(static_cast<size_t>(out_channels_), std::vector<float>(block, 0.0f));
    for (auto& plane : in_planes_) in_ptrs_.push_back(plane.data());
    for (auto& plane : out_planes_) out_ptrs_.push_back(plane.data());
  }

  val processPlanar(const val& planes) {
    const std::vector<Audio> channels =
        loadPlaybackChannelSet(planes, sample_rate_, "PlaybackRenderer.processPlanar");
    const int in_channels = static_cast<int>(channels.size());
    const int frames = static_cast<int>(channels.front().size());
    if (in_channels > kMaxInputChannels || frames > max_block_) {
      throwRejectedBlock("PlaybackRenderer.processPlanar", in_channels, frames);
    }
    std::vector<const float*> in(channels.size());
    for (size_t ch = 0; ch < channels.size(); ++ch) in[ch] = channels[ch].data();
    if (sonare_playback_renderer_process_planar(renderer_.get(), in.data(), in_channels,
                                                out_ptrs_.data(), out_channels_,
                                                frames) != SONARE_OK) {
      throwRejectedBlock("PlaybackRenderer.processPlanar", in_channels, frames);
    }
    val out = val::array();
    for (int ch = 0; ch < out_channels_; ++ch) {
      const float* plane = out_planes_[static_cast<size_t>(ch)].data();
      out.call<void>("push", val::global("Float32Array")
                                 .new_(val(typed_memory_view(static_cast<size_t>(frames), plane))));
    }
    return out;
  }

  val processInterleaved(const val& samples, const val& in_channels_val) {
    const int in_channels = checkedIntFromVal(in_channels_val, "inChannels");
    if (in_channels <= 0) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "PlaybackRenderer.processInterleaved: inChannels must be positive");
    }
    const std::vector<float> input = float32ArrayToVector(samples);
    if (input.size() % static_cast<size_t>(in_channels) != 0) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "PlaybackRenderer.processInterleaved: interleaved sample count must "
                            "be a whole number of frames");
    }
    const size_t frames = input.size() / static_cast<size_t>(in_channels);
    if (frames > static_cast<size_t>(max_block_)) {
      throwRejectedBlock("PlaybackRenderer.processInterleaved", in_channels,
                         static_cast<int>(std::min<size_t>(frames, INT32_MAX)));
    }
    std::vector<float> output(frames * static_cast<size_t>(out_channels_));
    if (sonare_playback_renderer_process_interleaved(renderer_.get(), input.data(), in_channels,
                                                     output.data(), out_channels_,
                                                     static_cast<int>(frames)) != SONARE_OK) {
      throwRejectedBlock("PlaybackRenderer.processInterleaved", in_channels,
                         static_cast<int>(frames));
    }
    return vectorToFloat32Array(output);
  }

  void setConfig(const std::string& config_json) {
    checkPlayback(sonare_playback_renderer_set_config_json(renderer_.get(), config_json.c_str()),
                  "PlaybackRenderer.setConfig");
  }

  std::string configJson() const {
    char* text = nullptr;
    checkPlayback(sonare_playback_renderer_config_json(renderer_.get(), &text),
                  "PlaybackRenderer.config");
    return takeCString(text);
  }

  // Realtime: the C ABI ignores a non-finite angle, as on every surface. An
  // angle past the float range arrives as an infinity rather than through an
  // undefined narrowing.
  void setHeadOrientation(double yaw_deg, double pitch_deg, double roll_deg) {
    const auto angle = [](double deg) {
      return std::abs(deg) <= FLT_MAX ? static_cast<float>(deg)
                                      : static_cast<float>(deg > 0.0 ? HUGE_VAL : -HUGE_VAL);
    };
    sonare_playback_renderer_set_head_orientation(renderer_.get(), angle(yaw_deg), angle(pitch_deg),
                                                  angle(roll_deg));
  }

  void reset() {
    checkPlayback(sonare_playback_renderer_reset(renderer_.get()), "PlaybackRenderer.reset");
  }

  int latencySamples() const {
    int out = 0;
    checkPlayback(sonare_playback_renderer_latency_samples(renderer_.get(), &out),
                  "PlaybackRenderer.latencySamples");
    return out;
  }

  int inputChannels() const {
    int out = 0;
    checkPlayback(sonare_playback_renderer_input_channel_count(renderer_.get(), &out),
                  "PlaybackRenderer.inputChannels");
    return out;
  }

  int outputChannels() const { return out_channels_; }

  std::string diagnosticsJson() const {
    char* text = nullptr;
    checkPlayback(sonare_playback_renderer_diagnostics_json(renderer_.get(), &text),
                  "PlaybackRenderer.diagnostics");
    return takeCString(text);
  }

  uint32_t nonFiniteDiscardCount() const {
    uint32_t out = 0;
    checkPlayback(sonare_playback_renderer_non_finite_discard_count(renderer_.get(), &out),
                  "PlaybackRenderer.nonFiniteDiscardCount");
    return out;
  }

  val inputPlane(const val& channel_val) {
    const int channel = checkedIntFromVal(channel_val, "channel");
    if (channel < 0 || channel >= kMaxInputChannels) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "PlaybackRenderer.inputPlane: channel out of range");
    }
    auto& plane = in_planes_[static_cast<size_t>(channel)];
    return val(typed_memory_view(plane.size(), plane.data()));
  }

  val outputPlane(const val& channel_val) {
    const int channel = checkedIntFromVal(channel_val, "channel");
    if (channel < 0 || channel >= out_channels_) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "PlaybackRenderer.outputPlane: channel out of range");
    }
    auto& plane = out_planes_[static_cast<size_t>(channel)];
    return val(typed_memory_view(plane.size(), plane.data()));
  }

  int processPrepared(const val& in_channels_val, const val& frames_val) {
    const int in_channels = checkedIntFromVal(in_channels_val, "inChannels");
    const int frames = checkedIntFromVal(frames_val, "frames");
    if (in_channels < 1 || in_channels > kMaxInputChannels) return SONARE_ERROR_INVALID_PARAMETER;
    const int err = sonare_playback_renderer_process_planar(
        renderer_.get(), in_ptrs_.data(), in_channels, out_ptrs_.data(), out_channels_, frames);
    // A rejected call does not advance any state (per the C ABI's own
    // contract), so the cache below only updates on SONARE_OK -- at which
    // point the renderer's active input layout is exactly `in_channels`,
    // whether this block just switched it (an "auto" layout) or confirmed it
    // (a fixed one).
    if (err == SONARE_OK) last_input_channels_ = in_channels;
    return err;
  }

  /// Renders @p frames of silence on the active input layout, so the timeline
  /// advances without switching the layout.
  ///
  /// Reads the active input channel count from `last_input_channels_` rather
  /// than sonare_playback_renderer_input_channel_count: that C-ABI entry is
  /// SONARE_C_API_ENTRY-guarded (it clears the thread-local last-error string),
  /// not one of the three entries sonare_c_playback.h documents as realtime-
  /// safe, and this method runs on the AudioWorklet's real-time callback.
  int processPreparedSilence(const val& frames_val) {
    const int frames = checkedIntFromVal(frames_val, "frames");
    if (frames < 0 || frames > max_block_) return SONARE_ERROR_INVALID_PARAMETER;
    for (int ch = 0; ch < last_input_channels_; ++ch) {
      std::fill_n(in_planes_[static_cast<size_t>(ch)].begin(), frames, 0.0f);
    }
    return sonare_playback_renderer_process_planar(renderer_.get(), in_ptrs_.data(),
                                                   last_input_channels_, out_ptrs_.data(),
                                                   out_channels_, frames);
  }

 private:
  struct RendererDeleter {
    void operator()(SonarePlaybackRenderer* renderer) const {
      sonare_playback_renderer_destroy(renderer);
    }
  };

  std::unique_ptr<SonarePlaybackRenderer, RendererDeleter> renderer_;
  int sample_rate_ = 0;
  int max_block_ = 0;
  int out_channels_ = 0;
  // Mirrors sonare_playback_renderer_input_channel_count's own documented
  // default ("2 before the first call"); kept current by processPrepared so
  // processPreparedSilence never has to ask the C ABI from the realtime path.
  int last_input_channels_ = 2;
  std::vector<std::vector<float>> in_planes_;
  std::vector<std::vector<float>> out_planes_;
  std::vector<const float*> in_ptrs_;
  std::vector<float*> out_ptrs_;
};

class PlaybackLoudnessMeterWasm {
 public:
  PlaybackLoudnessMeterWasm(const val& channels_val, const val& sample_rate_val) {
    channels_ = checkedIntFromVal(channels_val, "channels");
    const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
    checkPlayback(sonare_playback_loudness_meter_create(channels_, sample_rate, &meter_),
                  "PlaybackLoudnessMeter");
  }
  ~PlaybackLoudnessMeterWasm() { sonare_playback_loudness_meter_destroy(meter_); }
  PlaybackLoudnessMeterWasm(const PlaybackLoudnessMeterWasm&) = delete;
  PlaybackLoudnessMeterWasm& operator=(const PlaybackLoudnessMeterWasm&) = delete;

  void pushInterleaved(const val& samples) {
    const std::vector<float> input = float32ArrayToVector(samples);
    if (input.size() % static_cast<size_t>(channels_) != 0) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "PlaybackLoudnessMeter.pushInterleaved: interleaved sample count "
                            "must be a whole number of frames");
    }
    checkPlayback(sonare_playback_loudness_meter_push_interleaved(
                      meter_, input.data(), input.size() / static_cast<size_t>(channels_)),
                  "PlaybackLoudnessMeter.pushInterleaved");
  }

  float integratedLufs() const {
    float out = 0.0f;
    checkPlayback(sonare_playback_loudness_meter_integrated_lufs(meter_, &out),
                  "PlaybackLoudnessMeter.integratedLufs");
    return out;
  }

 private:
  SonarePlaybackLoudnessMeter* meter_ = nullptr;
  int channels_ = 0;
};

HrtfSetWasm* createHrtfSet(val bytes) { return new HrtfSetWasm(bytes); }

PlaybackRendererWasm* createPlaybackRenderer(const std::string& config_json, val hrtf,
                                             val sample_rate, val max_block_size) {
  return new PlaybackRendererWasm(config_json, hrtf, sample_rate, max_block_size);
}

PlaybackLoudnessMeterWasm* createPlaybackLoudnessMeter(val channels, val sample_rate) {
  return new PlaybackLoudnessMeterWasm(channels, sample_rate);
}

val renderPlayback(val samples, val channels_val, val sample_rate_val,
                   const std::string& config_json, val hrtf) {
  const int channels = checkedIntFromVal(channels_val, "channels");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  size_t frames = 0;
  const std::vector<float> input =
      loadPlaybackInterleaved(samples, channels, "renderPlayback", &frames);
  float* rendered = nullptr;
  size_t out_frames = 0;
  int out_channels = 0;
  checkPlayback(sonare_playback_render_interleaved(input.data(), frames, channels, sample_rate,
                                                   config_json.c_str(), hrtfHandleFromVal(hrtf),
                                                   &rendered, &out_frames, &out_channels),
                "renderPlayback");
  std::unique_ptr<float, void (*)(float*)> owned(rendered, &sonare_free_playback_render);
  val out = val::object();
  out.set("samples", val::global("Float32Array")
                         .new_(val(typed_memory_view(out_frames * static_cast<size_t>(out_channels),
                                                     owned.get()))));
  out.set("channels", out_channels);
  return out;
}

}  // namespace

void registerPlaybackBindings() {
  class_<HrtfSetWasm>("HrtfSet");
  class_<PlaybackRendererWasm>("PlaybackRenderer")
      .function("processPlanar", &PlaybackRendererWasm::processPlanar)
      .function("processInterleaved", &PlaybackRendererWasm::processInterleaved)
      .function("setConfig", &PlaybackRendererWasm::setConfig)
      .function("configJson", &PlaybackRendererWasm::configJson)
      .function("setHeadOrientation", &PlaybackRendererWasm::setHeadOrientation)
      .function("reset", &PlaybackRendererWasm::reset)
      .function("latencySamples", &PlaybackRendererWasm::latencySamples)
      .function("inputChannels", &PlaybackRendererWasm::inputChannels)
      .function("outputChannels", &PlaybackRendererWasm::outputChannels)
      .function("diagnosticsJson", &PlaybackRendererWasm::diagnosticsJson)
      .function("nonFiniteDiscardCount", &PlaybackRendererWasm::nonFiniteDiscardCount)
      .function("inputPlane", &PlaybackRendererWasm::inputPlane)
      .function("outputPlane", &PlaybackRendererWasm::outputPlane)
      .function("processPrepared", &PlaybackRendererWasm::processPrepared)
      .function("processPreparedSilence", &PlaybackRendererWasm::processPreparedSilence);
  class_<PlaybackLoudnessMeterWasm>("PlaybackLoudnessMeter")
      .function("pushInterleaved", &PlaybackLoudnessMeterWasm::pushInterleaved)
      .function("integratedLufs", &PlaybackLoudnessMeterWasm::integratedLufs);
  function("createHrtfSet", &createHrtfSet, allow_raw_pointers());
  function("createPlaybackRenderer", &createPlaybackRenderer, allow_raw_pointers());
  function("createPlaybackLoudnessMeter", &createPlaybackLoudnessMeter, allow_raw_pointers());
  function("renderPlayback", &renderPlayback, allow_raw_pointers());
}

#endif  // SONARE_WITH_PLAYBACK

#endif  // __EMSCRIPTEN__
