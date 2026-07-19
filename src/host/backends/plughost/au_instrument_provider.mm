/// @file au_instrument_provider.mm
/// @brief Audio Unit host: wraps AU instances in core ProcessorBase /
///        MidiInstrument adapters. See au_instrument_provider.h.

#include "host/backends/plughost/au_instrument_provider.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "midi/midi_event.h"
#include "midi/ump.h"
#include "util/exception.h"

namespace sonare::host::backends {
namespace {

constexpr size_t kMaxChannels = 32;
constexpr size_t kEventQueueDepth = 512;

struct AuRuntimeApi {
  OSStatus (*set_property)(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement,
                           const void*, UInt32);
  OSStatus (*get_property)(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, void*,
                           UInt32*);
  OSStatus (*initialize)(AudioUnit);
  OSStatus (*uninitialize)(AudioUnit);
  OSStatus (*render)(AudioUnit, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32,
                     AudioBufferList*);
  OSStatus (*reset)(AudioUnit, AudioUnitScope, AudioUnitElement);
  OSStatus (*midi_event)(MusicDeviceComponent, UInt32, UInt32, UInt32, UInt32);
  OSStatus (*dispose)(AudioComponentInstance);
};

const AuRuntimeApi kSystemAuRuntimeApi{
    &AudioUnitSetProperty, &AudioUnitGetProperty,
    &AudioUnitInitialize,  &AudioUnitUninitialize,
    &AudioUnitRender,      &AudioUnitReset,
    &MusicDeviceMIDIEvent, &AudioComponentInstanceDispose,
};

/// Encode an AudioComponentDescription into the descriptor id string.
std::string encode_id(const AudioComponentDescription& desc) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%08x:%08x:%08x", static_cast<unsigned>(desc.componentType),
                static_cast<unsigned>(desc.componentSubType),
                static_cast<unsigned>(desc.componentManufacturer));
  return std::string(buf);
}

/// Parse a descriptor id back into an AudioComponentDescription. Returns false
/// on malformed input.
bool decode_id(const std::string& id, AudioComponentDescription& out) {
  unsigned t = 0, s = 0, m = 0;
  if (std::sscanf(id.c_str(), "%08x:%08x:%08x", &t, &s, &m) != 3) return false;
  out = AudioComponentDescription{};
  out.componentType = t;
  out.componentSubType = s;
  out.componentManufacturer = m;
  return true;
}

/// Extract the MIDI 1.0 status/data byte triple from an already-lowered MIDI 1.0
/// channel-voice UMP. Returns false for any other UMP. Lowering from MIDI 2.0
/// (including the multi-message bank-select + program-change expansion) is done
/// by midi2_to_midi1_messages before this is called.
bool midi1_ump_to_bytes(const midi::Ump& ump, uint8_t& status, uint8_t& data1, uint8_t& data2) {
  if (ump.word_count == 0 || ump.message_type() != midi::UmpMessageType::kMidi1ChannelVoice) {
    return false;
  }
  const uint32_t w = ump.words[0];
  status = static_cast<uint8_t>((w >> 16) & 0xFFu);
  data1 = static_cast<uint8_t>((w >> 8) & 0x7Fu);
  data2 = static_cast<uint8_t>(w & 0x7Fu);
  return true;
}

/// Negotiate non-interleaved float32 on a scope so render can point the buffer
/// list straight at the engine's planar channel arrays (zero copy).
bool set_planar_float_format(const AuRuntimeApi& api, AudioUnit unit, AudioUnitScope scope,
                             double sample_rate, int channels) {
  AudioStreamBasicDescription fmt{};
  fmt.mSampleRate = sample_rate;
  fmt.mFormatID = kAudioFormatLinearPCM;
  fmt.mFormatFlags =
      kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
  fmt.mChannelsPerFrame = static_cast<UInt32>(channels);
  fmt.mBitsPerChannel = 32;
  fmt.mFramesPerPacket = 1;
  fmt.mBytesPerFrame = sizeof(float);  // per (non-interleaved) buffer
  fmt.mBytesPerPacket = sizeof(float);
  return api.set_property(unit, kAudioUnitProperty_StreamFormat, scope, 0, &fmt, sizeof(fmt)) ==
         noErr;
}

int query_latency_samples(const AuRuntimeApi& api, AudioUnit unit, double sample_rate) {
  Float64 seconds = 0.0;
  UInt32 size = sizeof(seconds);
  if (api.get_property(unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &seconds,
                       &size) != noErr) {
    return 0;
  }
  return static_cast<int>(seconds * sample_rate + 0.5);
}

/// Storage for a flexible AudioBufferList sized for kMaxChannels.
struct BufferListStorage {
  std::array<uint8_t, sizeof(AudioBufferList) + kMaxChannels * sizeof(AudioBuffer)> bytes{};
  AudioBufferList* list() noexcept { return reinterpret_cast<AudioBufferList*>(bytes.data()); }
};

}  // namespace

// ===========================================================================
// AU instrument adapter
// ===========================================================================

namespace {

class AuMidiInstrument final : public midi::MidiInstrument {
 public:
  explicit AuMidiInstrument(AudioUnit unit, const AuRuntimeApi* api = &kSystemAuRuntimeApi)
      : unit_(unit), api_(api) {}
  ~AuMidiInstrument() override {
    if (unit_ != nullptr) {
      if (initialized_) api_->uninitialize(unit_);
      api_->dispose(unit_);
    }
  }

  void prepare(double sample_rate, int max_block_size) override {
    if (unit_ == nullptr || !std::isfinite(sample_rate) || sample_rate <= 0.0 ||
        max_block_size <= 0) {
      throw SonareException(ErrorCode::InvalidParameter, "invalid Audio Unit prepare config");
    }
    if (initialized_) {
      api_->uninitialize(unit_);
      initialized_ = false;
    }
    sample_rate_ = sample_rate;
    max_block_ = max_block_size;
    // Scratch backing for channels the host does not supply, so process() never
    // allocates. One row per possible channel, each max_block_size long.
    scratch_.assign(kMaxChannels * static_cast<size_t>(max_block_size), 0.0f);
    auto frames = static_cast<UInt32>(max_block_size);
    if (api_->set_property(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                           0, &frames, sizeof(frames)) != noErr ||
        !set_planar_float_format(*api_, unit_, kAudioUnitScope_Output, sample_rate_, 2) ||
        api_->initialize(unit_) != noErr) {
      throw SonareException(ErrorCode::InvalidState, "failed to prepare Audio Unit instrument");
    }
    initialized_ = true;
    // The MidiInstrument::prepare seam carries no channel count, so default to
    // stereo here. process() adapts mismatched host buffers without changing AU
    // properties on the audio thread.
    output_channels_ = 2;
    latency_ = query_latency_samples(*api_, unit_, sample_rate_);
    position_ = 0;
    event_count_ = 0;
  }

  void process(float* const* channels, int num_channels, int num_samples) override {
    if (unit_ == nullptr || !initialized_ || channels == nullptr || num_channels <= 0 ||
        num_samples <= 0) {
      return;
    }
    const int chans = num_channels > static_cast<int>(kMaxChannels) ? static_cast<int>(kMaxChannels)
                                                                    : num_channels;
    const int render_samples = std::min(num_samples, max_block_);
    // Do NOT renegotiate the AU stream format here: an AU format change requires
    // AudioUnitUninitialize/Initialize, and running those on the audio thread
    // violates the process() no-allocation/no-I-O contract (midi/instrument.h).
    // The AU keeps the channel count negotiated in prepare(); the buffer list
    // below is adapted to it (a host channel the AU does not fill is silenced,
    // a channel the host did not supply is backed by pre-sized scratch). The
    // common steady stereo host matches output_channels_ exactly with no scratch.
    // Deliver events in non-decreasing intra-block frame order. AUs expect the
    // MusicDeviceMIDIEvent offset to be monotonic within one render cycle, but
    // on_event queues in arrival order, which can interleave across clips routed
    // to the same destination. Insertion sort is stable (same-frame events, e.g.
    // a note-off before a note-on at the same frame, keep their queued order) and
    // never allocates on the audio thread, unlike std::stable_sort which may grab
    // a temporary buffer. event_count_ is bounded (<= kEventQueueDepth) and is
    // typically a handful per block.
    for (size_t i = 1; i < event_count_; ++i) {
      const midi::MidiEvent key = events_[i];
      size_t j = i;
      while (j > 0 && events_[j - 1].render_frame > key.render_frame) {
        events_[j] = events_[j - 1];
        --j;
      }
      events_[j] = key;
    }
    // Flush queued events at their intra-block sample offset before rendering.
    for (size_t i = 0; i < event_count_; ++i) {
      const int64_t offset = events_[i].render_frame - position_;
      const UInt32 frame = offset < 0                 ? 0
                           : offset >= render_samples ? static_cast<UInt32>(render_samples - 1)
                                                      : static_cast<UInt32>(offset);
      // Lower MIDI 2.0 to one or more MIDI 1.0 messages at the same frame. A
      // bank-valid program change expands to CC#0, CC#32, then Program Change so
      // the AU selects the intended bank/patch instead of dropping the bank.
      const midi::Midi1MessageList lowered = midi::midi2_to_midi1_messages(events_[i].ump);
      for (size_t m = 0; m < lowered.count; ++m) {
        uint8_t status = 0, d1 = 0, d2 = 0;
        if (midi1_ump_to_bytes(lowered.messages[m], status, d1, d2)) {
          api_->midi_event(unit_, status, d1, d2, frame);
        }
      }
    }
    event_count_ = 0;

    // Present exactly the AU's negotiated channel count. Back any channel the
    // host did not supply with pre-sized scratch (never allocated here); num_samples
    // is bounded by the max_block_size passed to prepare(), so the scratch rows fit.
    AudioBufferList* list = buffers_.list();
    const int render_chans = output_channels_;
    list->mNumberBuffers = static_cast<UInt32>(render_chans);
    for (int c = 0; c < render_chans; ++c) {
      float* dst = c < chans && channels[c] != nullptr
                       ? channels[c]
                       : scratch_.data() + static_cast<size_t>(c) * max_block_;
      list->mBuffers[c].mNumberChannels = 1;
      list->mBuffers[c].mDataByteSize = static_cast<UInt32>(render_samples * sizeof(float));
      list->mBuffers[c].mData = dst;
    }
    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp ts{};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    ts.mSampleTime = static_cast<Float64>(position_);
    const OSStatus status =
        api_->render(unit_, &flags, &ts, 0, static_cast<UInt32>(render_samples), list);
    if (status != noErr) {
      for (int c = 0; c < chans; ++c) {
        if (channels[c] != nullptr) {
          std::memset(channels[c], 0, static_cast<size_t>(render_samples) * sizeof(float));
        }
      }
    }
    // Silence host channels the AU did not fill (host supplied more than the AU renders).
    for (int c = render_chans; c < num_channels; ++c) {
      if (channels[c] != nullptr) {
        std::memset(channels[c], 0, static_cast<size_t>(render_samples) * sizeof(float));
      }
    }
    // A caller violating the negotiated maximum gets a silent tail, never a
    // scratch overrun or an AU property change on the render thread.
    if (render_samples < num_samples) {
      for (int c = 0; c < num_channels; ++c) {
        if (channels[c] != nullptr) {
          std::memset(channels[c] + render_samples, 0,
                      static_cast<size_t>(num_samples - render_samples) * sizeof(float));
        }
      }
    }
    position_ += num_samples;
  }

  void reset() override {
    if (unit_ != nullptr) api_->reset(unit_, kAudioUnitScope_Global, 0);
    event_count_ = 0;
    position_ = 0;
  }

  int latency_samples() const noexcept override { return latency_; }

  void on_event(uint32_t /*destination_id*/, const midi::MidiEvent& event) noexcept override {
    if (event_count_ < events_.size()) events_[event_count_++] = event;
  }

 private:
  AudioUnit unit_ = nullptr;
  const AuRuntimeApi* api_ = &kSystemAuRuntimeApi;
  double sample_rate_ = 48000.0;
  int max_block_ = 512;
  int latency_ = 0;
  int output_channels_ = 0;
  bool initialized_ = false;
  int64_t position_ = 0;
  BufferListStorage buffers_{};
  std::vector<float> scratch_{};
  std::array<midi::MidiEvent, kEventQueueDepth> events_{};
  size_t event_count_ = 0;
};

class AuEffectProcessor final : public rt::ProcessorBase {
 public:
  explicit AuEffectProcessor(AudioUnit unit, const AuRuntimeApi* api = &kSystemAuRuntimeApi)
      : unit_(unit), api_(api) {}
  ~AuEffectProcessor() override {
    if (unit_ != nullptr) {
      if (initialized_) api_->uninitialize(unit_);
      api_->dispose(unit_);
    }
  }

  void prepare(double sample_rate, int max_block_size) override {
    if (unit_ == nullptr || !std::isfinite(sample_rate) || sample_rate <= 0.0 ||
        max_block_size <= 0) {
      throw SonareException(ErrorCode::InvalidParameter, "invalid Audio Unit prepare config");
    }
    if (initialized_) {
      api_->uninitialize(unit_);
      initialized_ = false;
    }
    sample_rate_ = sample_rate;
    max_block_ = max_block_size;
    // Scratch backing for channels the host does not supply, so process() never
    // allocates. One row per possible channel, each max_block_size long.
    scratch_.assign(kMaxChannels * static_cast<size_t>(max_block_size), 0.0f);
    auto frames = static_cast<UInt32>(max_block_size);
    if (api_->set_property(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                           0, &frames, sizeof(frames)) != noErr) {
      throw SonareException(ErrorCode::InvalidState,
                            "failed to set Audio Unit maximum render frames");
    }
    // Supply input via a render callback that copies the host's in-place buffer.
    // The callback is a property that survives format re-negotiation, so it is
    // set once here rather than on every configure_channels().
    AURenderCallbackStruct cb{};
    cb.inputProc = &AuEffectProcessor::input_trampoline;
    cb.inputProcRefCon = this;
    if (api_->set_property(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                           &cb, sizeof(cb)) != noErr ||
        !set_planar_float_format(*api_, unit_, kAudioUnitScope_Input, sample_rate_, 2) ||
        !set_planar_float_format(*api_, unit_, kAudioUnitScope_Output, sample_rate_, 2) ||
        api_->initialize(unit_) != noErr) {
      throw SonareException(ErrorCode::InvalidState, "failed to prepare Audio Unit effect");
    }
    initialized_ = true;
    // ProcessorBase::prepare carries no channel count, so negotiate stereo once
    // on this control-thread seam and adapt mismatched process buffers safely.
    channels_ = 2;
    latency_ = query_latency_samples(*api_, unit_, sample_rate_);
    position_ = 0;
  }

  void process(float* const* channels, int num_channels, int num_samples) override {
    if (unit_ == nullptr || !initialized_ || channels == nullptr || num_channels <= 0 ||
        num_samples <= 0) {
      return;
    }
    const int chans = num_channels > static_cast<int>(kMaxChannels) ? static_cast<int>(kMaxChannels)
                                                                    : num_channels;
    const int render_samples = std::min(num_samples, max_block_);
    // Do NOT renegotiate the AU format here: uninitialising/reinitialising the AU
    // on the audio thread violates the process() no-allocation/no-I-O contract
    // (midi/instrument.h). Keep the channel count negotiated in prepare() and
    // adapt the buffer list to it. The input callback already copies min(host,
    // negotiated) channels and silences the rest via in_count_.
    in_channels_ = channels;
    in_count_ = chans;
    // Present exactly the AU's negotiated channel count; back any channel the host
    // did not supply with pre-sized scratch (num_samples is bounded by prepare()'s
    // max_block_size, so the scratch rows fit).
    AudioBufferList* list = buffers_.list();
    const int render_chans = channels_;
    list->mNumberBuffers = static_cast<UInt32>(render_chans);
    for (int c = 0; c < render_chans; ++c) {
      float* dst = c < chans && channels[c] != nullptr
                       ? channels[c]
                       : scratch_.data() + static_cast<size_t>(c) * max_block_;
      list->mBuffers[c].mNumberChannels = 1;
      list->mBuffers[c].mDataByteSize = static_cast<UInt32>(render_samples * sizeof(float));
      list->mBuffers[c].mData = dst;
    }
    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp ts{};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    ts.mSampleTime = static_cast<Float64>(position_);
    const OSStatus status =
        api_->render(unit_, &flags, &ts, 0, static_cast<UInt32>(render_samples), list);
    if (status != noErr) {
      for (int c = 0; c < chans; ++c) {
        if (channels[c] != nullptr) {
          std::memset(channels[c], 0, static_cast<size_t>(render_samples) * sizeof(float));
        }
      }
    }
    // Silence host channels the AU did not fill (host supplied more than the AU renders).
    for (int c = render_chans; c < num_channels; ++c) {
      if (channels[c] != nullptr) {
        std::memset(channels[c], 0, static_cast<size_t>(render_samples) * sizeof(float));
      }
    }
    if (render_samples < num_samples) {
      for (int c = 0; c < num_channels; ++c) {
        if (channels[c] != nullptr) {
          std::memset(channels[c] + render_samples, 0,
                      static_cast<size_t>(num_samples - render_samples) * sizeof(float));
        }
      }
    }
    position_ += num_samples;
    in_channels_ = nullptr;
  }

  void reset() override {
    if (unit_ != nullptr) api_->reset(unit_, kAudioUnitScope_Global, 0);
    position_ = 0;
  }

  int latency_samples() const noexcept override { return latency_; }

 private:
  static OSStatus input_trampoline(void* ref, AudioUnitRenderActionFlags* /*flags*/,
                                   const AudioTimeStamp* /*ts*/, UInt32 /*bus*/, UInt32 frames,
                                   AudioBufferList* data) noexcept {
    auto* self = static_cast<AuEffectProcessor*>(ref);
    if (self->in_channels_ == nullptr || data == nullptr) return noErr;
    const int n = static_cast<int>(frames);
    const int buffers = static_cast<int>(data->mNumberBuffers);
    for (int c = 0; c < buffers; ++c) {
      auto* dst = static_cast<float*>(data->mBuffers[c].mData);
      const float* src = c < self->in_count_ ? self->in_channels_[c] : nullptr;
      if (src != nullptr) {
        std::memcpy(dst, src, static_cast<size_t>(n) * sizeof(float));
      } else {
        std::memset(dst, 0, static_cast<size_t>(n) * sizeof(float));
      }
    }
    return noErr;
  }

  AudioUnit unit_ = nullptr;
  const AuRuntimeApi* api_ = &kSystemAuRuntimeApi;
  double sample_rate_ = 48000.0;
  int max_block_ = 512;
  int latency_ = 0;
  int channels_ = 0;
  bool initialized_ = false;
  int64_t position_ = 0;
  BufferListStorage buffers_{};
  std::vector<float> scratch_{};
  float* const* in_channels_ = nullptr;
  int in_count_ = 0;
};

struct AuCallSpyState {
  unsigned set_property_calls = 0;
  unsigned initialize_calls = 0;
  unsigned uninitialize_calls = 0;
  unsigned render_calls = 0;
};

thread_local AuCallSpyState* g_au_call_spy = nullptr;

OSStatus spy_set_property(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement,
                          const void*, UInt32) {
  ++g_au_call_spy->set_property_calls;
  return noErr;
}

OSStatus spy_get_property(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement,
                          void* value, UInt32* size) {
  if (value != nullptr && size != nullptr && *size >= sizeof(Float64)) {
    *static_cast<Float64*>(value) = 0.0;
  }
  return noErr;
}

OSStatus spy_initialize(AudioUnit) {
  ++g_au_call_spy->initialize_calls;
  return noErr;
}

OSStatus spy_uninitialize(AudioUnit) {
  ++g_au_call_spy->uninitialize_calls;
  return noErr;
}

OSStatus spy_render(AudioUnit, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32,
                    AudioBufferList*) {
  ++g_au_call_spy->render_calls;
  return noErr;
}

OSStatus spy_reset(AudioUnit, AudioUnitScope, AudioUnitElement) { return noErr; }

OSStatus spy_midi_event(MusicDeviceComponent, UInt32, UInt32, UInt32, UInt32) { return noErr; }

OSStatus spy_dispose(AudioComponentInstance) { return noErr; }

const AuRuntimeApi kSpyAuRuntimeApi{
    &spy_set_property, &spy_get_property, &spy_initialize, &spy_uninitialize,
    &spy_render,       &spy_reset,        &spy_midi_event, &spy_dispose,
};

/// Instantiate the AU named by `descriptor`, or nullptr.
AudioUnit instantiate(const PluginDescriptor& descriptor) {
  AudioComponentDescription desc{};
  if (!decode_id(descriptor.id, desc)) return nullptr;
  AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
  if (comp == nullptr) return nullptr;
  AudioUnit unit = nullptr;
  if (AudioComponentInstanceNew(comp, &unit) != noErr) return nullptr;
  return unit;
}

/// Return an AU instance for `descriptor`, reusing the one cached in
/// (*cache_unit, *cache_id) when the descriptor matches. A parameter sweep
/// (parameter_count + parameter_descriptor per index) then instantiates the AU
/// once rather than once per call. The cache owns the instance; callers must NOT
/// dispose it. The AU is left uninitialised, which is all the parameter-list /
/// parameter-info queries require.
AudioUnit cached_param_unit(void** cache_unit, std::string* cache_id,
                            const PluginDescriptor& descriptor) {
  auto current = static_cast<AudioUnit>(*cache_unit);
  if (current != nullptr && *cache_id == descriptor.id) return current;
  if (current != nullptr) {
    AudioComponentInstanceDispose(current);
    *cache_unit = nullptr;
    cache_id->clear();
  }
  AudioUnit unit = instantiate(descriptor);
  if (unit != nullptr) {
    *cache_unit = unit;
    *cache_id = descriptor.id;
  }
  return unit;
}

}  // namespace

detail::AuProcessCallSpyResult detail::run_au_process_call_spy() {
  AuCallSpyState state;
  g_au_call_spy = &state;
  detail::AuProcessCallSpyResult result;
  auto fake_unit = reinterpret_cast<AudioUnit>(static_cast<uintptr_t>(1));

  const auto control_calls = [&state] {
    return state.set_property_calls + state.initialize_calls + state.uninitialize_calls;
  };
  {
    AuMidiInstrument instrument(fake_unit, &kSpyAuRuntimeApi);
    instrument.prepare(48000.0, 4);
    const unsigned before = control_calls();
    std::array<float, 7> left{};
    std::array<float, 7> right{};
    std::array<float*, 1> mono{left.data()};
    std::array<float*, 2> stereo{left.data(), right.data()};
    instrument.process(mono.data(), 1, 4);
    instrument.process(stereo.data(), 2, 4);
    instrument.process(stereo.data(), 2, 7);
    result.instrument_controls_unchanged = control_calls() == before;
  }
  {
    AuEffectProcessor effect(fake_unit, &kSpyAuRuntimeApi);
    effect.prepare(48000.0, 4);
    const unsigned before = control_calls();
    std::array<float, 7> left{};
    std::array<float, 7> right{};
    std::array<float*, 1> mono{left.data()};
    std::array<float*, 2> stereo{left.data(), right.data()};
    effect.process(mono.data(), 1, 4);
    effect.process(stereo.data(), 2, 4);
    effect.process(stereo.data(), 2, 7);
    result.effect_controls_unchanged = control_calls() == before;
  }
  result.render_calls = state.render_calls;
  g_au_call_spy = nullptr;
  return result;
}

// ===========================================================================
// AuInstrumentProvider
// ===========================================================================

AuInstrumentProvider::AuInstrumentProvider() = default;

AuInstrumentProvider::~AuInstrumentProvider() {
  if (param_cache_unit_ != nullptr) {
    AudioComponentInstanceDispose(static_cast<AudioUnit>(param_cache_unit_));
    param_cache_unit_ = nullptr;
  }
}

std::vector<PluginDescriptor> AuInstrumentProvider::enumerate(PluginKind kind) {
  std::vector<PluginDescriptor> out;
  AudioComponentDescription query{};
  query.componentType =
      kind == PluginKind::kInstrument ? kAudioUnitType_MusicDevice : kAudioUnitType_Effect;
  AudioComponent comp = nullptr;
  while ((comp = AudioComponentFindNext(comp, &query)) != nullptr) {
    AudioComponentDescription desc{};
    if (AudioComponentGetDescription(comp, &desc) != noErr) continue;
    CFStringRef name = nullptr;
    PluginDescriptor pd;
    pd.format = "au";
    pd.kind = kind;
    pd.id = encode_id(desc);
    if (AudioComponentCopyName(comp, &name) == noErr && name != nullptr) {
      char nbuf[256];
      if (CFStringGetCString(name, nbuf, sizeof(nbuf), kCFStringEncodingUTF8)) pd.name = nbuf;
      CFRelease(name);
    }
    out.push_back(std::move(pd));
  }
  return out;
}

bool AuInstrumentProvider::can_create(const PluginDescriptor& descriptor) const noexcept {
  if (descriptor.format != "au") return false;
  AudioComponentDescription desc{};
  return decode_id(descriptor.id, desc);
}

std::unique_ptr<midi::MidiInstrument> AuInstrumentProvider::create_instrument(
    const PluginDescriptor& descriptor) {
  if (descriptor.kind != PluginKind::kInstrument || !can_create(descriptor)) return nullptr;
  AudioUnit unit = instantiate(descriptor);
  if (unit == nullptr) return nullptr;
  return std::make_unique<AuMidiInstrument>(unit);
}

std::unique_ptr<rt::ProcessorBase> AuInstrumentProvider::create_effect(
    const PluginDescriptor& descriptor) {
  if (descriptor.kind != PluginKind::kEffect || descriptor.format != "au") return nullptr;
  AudioComponentDescription desc{};
  if (!decode_id(descriptor.id, desc) || desc.componentType != kAudioUnitType_Effect)
    return nullptr;
  AudioUnit unit = instantiate(descriptor);
  if (unit == nullptr) return nullptr;
  return std::make_unique<AuEffectProcessor>(unit);
}

size_t AuInstrumentProvider::parameter_count(const PluginDescriptor& descriptor) const noexcept {
  AudioUnit unit = cached_param_unit(&param_cache_unit_, &param_cache_id_, descriptor);
  if (unit == nullptr) return 0;
  UInt32 size = 0;
  Boolean writable = false;
  const OSStatus status = AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_ParameterList,
                                                   kAudioUnitScope_Global, 0, &size, &writable);
  if (status != noErr || size == 0) return 0;
  return size / sizeof(AudioUnitParameterID);
}

bool AuInstrumentProvider::parameter_descriptor(const PluginDescriptor& descriptor, size_t index,
                                                PluginParameterDescriptor* out) const noexcept {
  if (out == nullptr) return false;
  AudioUnit unit = cached_param_unit(&param_cache_unit_, &param_cache_id_, descriptor);
  if (unit == nullptr) return false;
  UInt32 size = 0;
  Boolean writable = false;
  bool ok = false;
  if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0,
                               &size, &writable) == noErr &&
      size > 0) {
    const size_t count = size / sizeof(AudioUnitParameterID);
    if (index < count) {
      std::vector<AudioUnitParameterID> ids(count);
      if (AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0,
                               ids.data(), &size) == noErr) {
        AudioUnitParameterInfo info{};
        UInt32 info_size = sizeof(info);
        if (AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global,
                                 ids[index], &info, &info_size) == noErr) {
          *out = PluginParameterDescriptor{};
          out->id = ids[index];
          out->min_value = info.minValue;
          out->max_value = info.maxValue;
          out->default_value = info.defaultValue;
          // HasCFNameString signals a CF name is PRESENT; CFNameRelease signals
          // the caller must release it. Gating presence on CFNameRelease (as
          // before) dropped the CF name for AUs that expose a static, non-owned
          // CF string, falling back to the deprecated char[] info.name.
          if ((info.flags & kAudioUnitParameterFlag_HasCFNameString) &&
              info.cfNameString != nullptr) {
            char nbuf[128];
            if (CFStringGetCString(info.cfNameString, nbuf, sizeof(nbuf), kCFStringEncodingUTF8)) {
              out->name = nbuf;
            }
            if (info.flags & kAudioUnitParameterFlag_CFNameRelease) {
              CFRelease(info.cfNameString);
            }
          } else {
            out->name = info.name;
          }
          ok = true;
        }
      }
    }
  }
  return ok;
}

}  // namespace sonare::host::backends
