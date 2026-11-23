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
#include "playback/renderer.h"
#include "wasm/bindings/common/common.h"

#if defined(SONARE_WITH_PLAYBACK)

namespace {

/// Largest input channel count any layout accepts (7.1).
constexpr int kMaxInputChannels = 8;

[[noreturn]] void throwPlaybackError(SonareError err) {
  rethrowLastCRefusal();
  const char* detail = sonare_last_error_message();
  std::string message;
  message += detail != nullptr && detail[0] != '\0' ? detail : sonare_error_message(err);
  throw SonareException(sonare_c_detail::error_code_from_c_error(err), message);
}

void checkPlayback(SonareError err) {
  if (err != SONARE_OK) throwPlaybackError(err);
}

// The realtime entry points leave the thread-local message alone, so a rejected
// block is described here rather than read back.
[[noreturn]] void throwRejectedBlock(int in_channels, int frames) {
  throw SonareException(ErrorCode::InvalidParameter,
                        "the renderer rejected " + std::to_string(in_channels) +
                            " input channels x " + std::to_string(frames) +
                            " frames (a fixed input layout takes its own channel count, "
                            "\"auto\" takes 1, 2, 6 or 8; at most maxBlockSize frames)");
}

// The renderer's own documented contract is looser than
// loadValidatedChannelSet's (an offline-analysis policy): a 0-frame block is a
// no-op and a non-finite sample is replaced with 0 and counted by the C ABI
// itself, not rejected up front. This loader checks structure only -- array
// shape and matching channel lengths -- and leaves content (emptiness,
// finiteness) to the renderer, matching processInterleaved's own contract.
std::vector<Audio> loadPlaybackChannelSet(const val& planes, int sample_rate) {
  return loadChannelSet("planes", planes, sample_rate, ChannelSetValidation::StructureOnly);
}

// Structural sibling of the channel loader above, for the offline (interleaved) render
// path: no emptiness or finiteness policy, matching processInterleaved's.
std::vector<float> loadPlaybackInterleaved(val samples, int channels, const char* channels_name,
                                           std::size_t* out_frames) {
  if (channels <= 0) {
    throw WasmRangeError(std::string(channels_name) + " must be an integer in [1, 2147483647]");
  }
  wasmFloat32ArrayLength(samples, "samples");
  std::vector<float> data = float32ArrayToVector(samples, "samples");
  if (data.size() % static_cast<std::size_t>(channels) != 0) {
    throw WasmRangeError("interleaved sample count must be a whole number of frames");
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
    const std::vector<uint8_t> data = uint8ArrayToVector(bytes, "bytes");
    checkPlayback(sonare_hrtf_set_create_from_memory(elementData(data), data.size(), &set_));
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
    max_block_ = max_block_size_val.isUndefined() || max_block_size_val.isNull()
                     ? sonare::playback::kPlaybackRendererDefaultMaxBlock
                     : checkedIntFromVal(max_block_size_val, "maxBlockSize");
    SonarePlaybackRenderer* created = nullptr;
    checkPlayback(sonare_playback_renderer_create_json(wasmCString(config_json, "configJson"),
                                                       hrtfHandleFromVal(hrtf), sample_rate_,
                                                       max_block_, &created));
    renderer_.reset(created);
    checkPlayback(sonare_playback_renderer_num_output_channels(renderer_.get(), &out_channels_));
    const auto block = static_cast<size_t>(max_block_);
    in_planes_.assign(kMaxInputChannels, std::vector<float>(block, 0.0f));
    out_planes_.assign(static_cast<size_t>(out_channels_), std::vector<float>(block, 0.0f));
    for (auto& plane : in_planes_) in_ptrs_.push_back(plane.data());
    for (auto& plane : out_planes_) out_ptrs_.push_back(plane.data());
  }

  val processPlanar(const val& planes) {
    const std::vector<Audio> channels = loadPlaybackChannelSet(planes, sample_rate_);
    const int in_channels = static_cast<int>(channels.size());
    const int frames = static_cast<int>(channels.front().size());
    if (in_channels > kMaxInputChannels || frames > max_block_) {
      throwRejectedBlock(in_channels, frames);
    }
    std::vector<const float*> in(channels.size());
    for (size_t ch = 0; ch < channels.size(); ++ch) in[ch] = channels[ch].data();
    if (sonare_playback_renderer_process_planar(renderer_.get(), elementData(in), in_channels,
                                                elementData(out_ptrs_), out_channels_,
                                                frames) != SONARE_OK) {
      throwRejectedBlock(in_channels, frames);
    }
    return planesToVal(frames);
  }

  val processSilence(const val& frames_val) {
    const int frames = checkedIntFromVal(frames_val, "frames");
    if (frames < 0 || frames > max_block_) {
      throw WasmRangeError("frames must be an integer in [0, " + std::to_string(max_block_) + "]");
    }
    if (sonare_playback_renderer_process_silence(renderer_.get(), elementData(out_ptrs_),
                                                 out_channels_, frames) != SONARE_OK) {
      throw SonareException(
          ErrorCode::InvalidParameter,
          "the renderer rejected a silent block of " + std::to_string(frames) + " frames");
    }
    return planesToVal(frames);
  }

  val processInterleaved(const val& samples, const val& in_channels_val) {
    const int in_channels = checkedIntFromVal(in_channels_val, "inChannels");
    if (in_channels <= 0) {
      throw WasmRangeError("inChannels must be an integer in [1, 2147483647]");
    }
    wasmFloat32ArrayLength(samples, "samples");
    const std::vector<float> input = float32ArrayToVector(samples, "samples");
    if (input.size() % static_cast<size_t>(in_channels) != 0) {
      throw WasmRangeError("interleaved sample count must be a whole number of frames");
    }
    const size_t frames = input.size() / static_cast<size_t>(in_channels);
    if (frames > static_cast<size_t>(max_block_)) {
      throwRejectedBlock(in_channels, static_cast<int>(std::min<size_t>(frames, INT32_MAX)));
    }
    std::vector<float> output(frames * static_cast<size_t>(out_channels_));
    if (sonare_playback_renderer_process_interleaved(
            renderer_.get(), elementData(input), in_channels, elementData(output), out_channels_,
            static_cast<int>(frames)) != SONARE_OK) {
      throwRejectedBlock(in_channels, static_cast<int>(frames));
    }
    return vectorToFloat32Array(output);
  }

  val renderOffline(const val& samples, const val& in_channels_val) {
    const int in_channels = checkedIntFromVal(in_channels_val, "inChannels");
    std::size_t frames = 0;
    const std::vector<float> input =
        loadPlaybackInterleaved(samples, in_channels, "inChannels", &frames);
    float* rendered = nullptr;
    std::size_t out_frames = 0;
    int out_channels = 0;
    checkPlayback(sonare_playback_renderer_render_offline(renderer_.get(), elementData(input),
                                                          frames, in_channels, &rendered,
                                                          &out_frames, &out_channels));
    std::unique_ptr<float, void (*)(float*)> owned(rendered, &sonare_free_floats);
    return vectorToFloat32Array(owned.get(), out_frames * static_cast<std::size_t>(out_channels));
  }

  void setConfig(const std::string& config_json) {
    checkPlayback(sonare_playback_renderer_set_config_json(renderer_.get(),
                                                           wasmCString(config_json, "configJson")));
  }

  std::string configJson() const {
    char* text = nullptr;
    checkPlayback(sonare_playback_renderer_config_json(renderer_.get(), &text));
    return takeCString(text);
  }

  // Realtime low-level behavior follows the C ABI: a non-finite angle is
  // ignored and an out-of-range double narrows to an infinity before the C
  // setter sees it. The public TypeScript facade validates type, finiteness
  // and float32 range before reaching this method.
  void setHeadOrientation(double yaw_deg, double pitch_deg, double roll_deg) {
    const auto angle = [](double deg) {
      return std::abs(deg) <= FLT_MAX ? static_cast<float>(deg)
                                      : static_cast<float>(deg > 0.0 ? HUGE_VAL : -HUGE_VAL);
    };
    sonare_playback_renderer_set_head_orientation(renderer_.get(), angle(yaw_deg), angle(pitch_deg),
                                                  angle(roll_deg));
  }

  void reset() { checkPlayback(sonare_playback_renderer_reset(renderer_.get())); }

  int latencySamples() const {
    int out = 0;
    checkPlayback(sonare_playback_renderer_latency_samples(renderer_.get(), &out));
    return out;
  }

  int maxBlockSize() const {
    int out = 0;
    checkPlayback(sonare_playback_renderer_max_block_size(renderer_.get(), &out));
    return out;
  }

  int inputChannels() const {
    int out = 0;
    checkPlayback(sonare_playback_renderer_num_input_channels(renderer_.get(), &out));
    return out;
  }

  int maxInputChannels() const {
    int out = 0;
    checkPlayback(sonare_playback_renderer_max_input_channels(renderer_.get(), &out));
    return out;
  }

  int outputChannels() const { return out_channels_; }

  std::string diagnosticsJson() const {
    char* text = nullptr;
    checkPlayback(sonare_playback_renderer_diagnostics_json(renderer_.get(), &text));
    return takeCString(text);
  }

  size_t nonFiniteDiscardCount() const {
    size_t out = 0;
    checkPlayback(sonare_playback_renderer_non_finite_discard_count(renderer_.get(), &out));
    return out;
  }

  val inputPlane(const val& channel_val) {
    const int channel = checkedIntFromVal(channel_val, "channel");
    if (channel < 0 || channel >= kMaxInputChannels) {
      throw WasmRangeError("channel out of range");
    }
    auto& plane = in_planes_[static_cast<size_t>(channel)];
    return val(typed_memory_view(plane.size(), plane.data()));
  }

  val outputPlane(const val& channel_val) {
    const int channel = checkedIntFromVal(channel_val, "channel");
    if (channel < 0 || channel >= out_channels_) {
      throw WasmRangeError("channel out of range");
    }
    auto& plane = out_planes_[static_cast<size_t>(channel)];
    return val(typed_memory_view(plane.size(), plane.data()));
  }

  int processPrepared(const val& in_channels_val, const val& frames_val) {
    const int in_channels = checkedIntFromVal(in_channels_val, "inChannels");
    const int frames = checkedIntFromVal(frames_val, "frames");
    if (in_channels < 1 || in_channels > kMaxInputChannels) return SONARE_ERROR_INVALID_PARAMETER;
    const int err =
        sonare_playback_renderer_process_planar(renderer_.get(), elementData(in_ptrs_), in_channels,
                                                elementData(out_ptrs_), out_channels_, frames);
    return err;
  }

  /// Renders @p frames of silence into the output planes without fixing or switching the input
  /// layout; returns the C ABI code instead of throwing, as the worklet path requires.
  int processPreparedSilence(const val& frames_val) {
    const int frames = checkedIntFromVal(frames_val, "frames");
    return sonare_playback_renderer_process_silence(renderer_.get(), elementData(out_ptrs_),
                                                    out_channels_, frames);
  }

 private:
  val planesToVal(int frames) const {
    val out = val::array();
    for (int ch = 0; ch < out_channels_; ++ch) {
      out.call<void>("push", vectorToFloat32Array(out_planes_[static_cast<size_t>(ch)].data(),
                                                  static_cast<size_t>(frames)));
    }
    return out;
  }

  struct RendererDeleter {
    void operator()(SonarePlaybackRenderer* renderer) const {
      sonare_playback_renderer_destroy(renderer);
    }
  };

  std::unique_ptr<SonarePlaybackRenderer, RendererDeleter> renderer_;
  int sample_rate_ = 0;
  int max_block_ = 0;
  int out_channels_ = 0;
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
    checkPlayback(sonare_playback_loudness_meter_create(channels_, sample_rate, &meter_));
  }
  ~PlaybackLoudnessMeterWasm() { sonare_playback_loudness_meter_destroy(meter_); }
  PlaybackLoudnessMeterWasm(const PlaybackLoudnessMeterWasm&) = delete;
  PlaybackLoudnessMeterWasm& operator=(const PlaybackLoudnessMeterWasm&) = delete;

  void pushInterleaved(const val& samples) {
    wasmFloat32ArrayLength(samples, "samples");
    const std::vector<float> input = float32ArrayToVector(samples, "samples");
    if (input.size() % static_cast<size_t>(channels_) != 0) {
      throw WasmRangeError("interleaved sample count must be a whole number of frames");
    }
    checkPlayback(sonare_playback_loudness_meter_push_interleaved(
        meter_, elementData(input), input.size() / static_cast<size_t>(channels_)));
  }

  float integratedLufs() const {
    float out = 0.0f;
    checkPlayback(sonare_playback_loudness_meter_integrated_lufs(meter_, &out));
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
  const int channels = checkedIntFromVal(channels_val, "numChannels");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  size_t frames = 0;
  const std::vector<float> input =
      loadPlaybackInterleaved(samples, channels, "numChannels", &frames);
  float* rendered = nullptr;
  size_t out_frames = 0;
  int out_channels = 0;
  checkPlayback(sonare_playback_render_interleaved(
      elementData(input), frames, channels, sample_rate, wasmCString(config_json, "configJson"),
      hrtfHandleFromVal(hrtf), &rendered, &out_frames, &out_channels));
  std::unique_ptr<float, void (*)(float*)> owned(rendered, &sonare_free_floats);
  val out = val::object();
  out.set("samples",
          vectorToFloat32Array(owned.get(), out_frames * static_cast<size_t>(out_channels)));
  out.set("channels", out_channels);
  return out;
}

int playbackConfigOutputChannels(const std::string& config_json) {
  int out = 0;
  checkPlayback(
      sonare_playback_config_num_output_channels(wasmCString(config_json, "configJson"), &out));
  return out;
}

}  // namespace

void registerPlaybackBindings() {
  class_<HrtfSetWasm>("HrtfSet");
  class_<PlaybackRendererWasm>("PlaybackRenderer")
      .function("processPlanar", &PlaybackRendererWasm::processPlanar)
      .function("processInterleaved", &PlaybackRendererWasm::processInterleaved)
      .function("processSilence", &PlaybackRendererWasm::processSilence)
      .function("renderOffline", &PlaybackRendererWasm::renderOffline)
      .function("setConfig", &PlaybackRendererWasm::setConfig)
      .function("configJson", &PlaybackRendererWasm::configJson)
      .function("setHeadOrientation", &PlaybackRendererWasm::setHeadOrientation)
      .function("reset", &PlaybackRendererWasm::reset)
      .function("latencySamples", &PlaybackRendererWasm::latencySamples)
      .function("maxBlockSize", &PlaybackRendererWasm::maxBlockSize)
      .function("inputChannels", &PlaybackRendererWasm::inputChannels)
      .function("maxInputChannels", &PlaybackRendererWasm::maxInputChannels)
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
  function("playbackConfigOutputChannels", &playbackConfigOutputChannels);
}

#endif  // SONARE_WITH_PLAYBACK

#endif  // __EMSCRIPTEN__
