/// @file au_instrument_provider.mm
/// @brief Audio Unit host: wraps AU instances in core ProcessorBase /
///        MidiInstrument adapters. See au_instrument_provider.h.

#include "host/backends/plughost/au_instrument_provider.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "midi/midi_event.h"
#include "midi/ump.h"

namespace sonare::host::backends {
namespace {

constexpr size_t kMaxChannels = 32;
constexpr size_t kEventQueueDepth = 512;

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
bool set_planar_float_format(AudioUnit unit, AudioUnitScope scope, double sample_rate,
                             int channels) {
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
  return AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, scope, 0, &fmt, sizeof(fmt)) ==
         noErr;
}

int query_latency_samples(AudioUnit unit, double sample_rate) {
  Float64 seconds = 0.0;
  UInt32 size = sizeof(seconds);
  if (AudioUnitGetProperty(unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &seconds,
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
  explicit AuMidiInstrument(AudioUnit unit) : unit_(unit) {}
  ~AuMidiInstrument() override {
    if (unit_ != nullptr) {
      AudioUnitUninitialize(unit_);
      AudioComponentInstanceDispose(unit_);
    }
  }

  void prepare(double sample_rate, int max_block_size) override {
    sample_rate_ = sample_rate;
    max_block_ = max_block_size;
    set_planar_float_format(unit_, kAudioUnitScope_Output, sample_rate, 2);
    auto frames = static_cast<UInt32>(max_block_size);
    AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                         &frames, sizeof(frames));
    AudioUnitInitialize(unit_);
    latency_ = query_latency_samples(unit_, sample_rate);
    position_ = 0;
    event_count_ = 0;
  }

  void process(float* const* channels, int num_channels, int num_samples) override {
    if (unit_ == nullptr || num_channels <= 0) return;
    const int chans = num_channels > static_cast<int>(kMaxChannels) ? static_cast<int>(kMaxChannels)
                                                                    : num_channels;
    // Deliver events in non-decreasing intra-block frame order. AUs expect the
    // MusicDeviceMIDIEvent offset to be monotonic within one render cycle, but
    // on_event queues in arrival order, which can interleave across clips routed
    // to the same destination. stable_sort keeps same-frame events (e.g. a
    // note-off before a note-on at the same frame) in their queued order, and is
    // bounded (<= kEventQueueDepth) with no heap allocation.
    std::stable_sort(events_.begin(), events_.begin() + event_count_,
                     [](const midi::MidiEvent& a, const midi::MidiEvent& b) {
                       return a.render_frame < b.render_frame;
                     });
    // Flush queued events at their intra-block sample offset before rendering.
    for (size_t i = 0; i < event_count_; ++i) {
      const int64_t offset = events_[i].render_frame - position_;
      const UInt32 frame = offset < 0              ? 0
                           : offset >= num_samples ? static_cast<UInt32>(num_samples - 1)
                                                   : static_cast<UInt32>(offset);
      // Lower MIDI 2.0 to one or more MIDI 1.0 messages at the same frame. A
      // bank-valid program change expands to CC#0, CC#32, then Program Change so
      // the AU selects the intended bank/patch instead of dropping the bank.
      const midi::Midi1MessageList lowered = midi::midi2_to_midi1_messages(events_[i].ump);
      for (size_t m = 0; m < lowered.count; ++m) {
        uint8_t status = 0, d1 = 0, d2 = 0;
        if (midi1_ump_to_bytes(lowered.messages[m], status, d1, d2)) {
          MusicDeviceMIDIEvent(unit_, status, d1, d2, frame);
        }
      }
    }
    event_count_ = 0;

    AudioBufferList* list = buffers_.list();
    list->mNumberBuffers = static_cast<UInt32>(chans);
    for (int c = 0; c < chans; ++c) {
      list->mBuffers[c].mNumberChannels = 1;
      list->mBuffers[c].mDataByteSize = static_cast<UInt32>(num_samples * sizeof(float));
      list->mBuffers[c].mData = channels[c];
    }
    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp ts{};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    ts.mSampleTime = static_cast<Float64>(position_);
    AudioUnitRender(unit_, &flags, &ts, 0, static_cast<UInt32>(num_samples), list);
    position_ += num_samples;
  }

  void reset() override {
    if (unit_ != nullptr) AudioUnitReset(unit_, kAudioUnitScope_Global, 0);
    event_count_ = 0;
    position_ = 0;
  }

  int latency_samples() const noexcept override { return latency_; }

  void on_event(uint32_t /*destination_id*/, const midi::MidiEvent& event) noexcept override {
    if (event_count_ < events_.size()) events_[event_count_++] = event;
  }

 private:
  AudioUnit unit_ = nullptr;
  double sample_rate_ = 48000.0;
  int max_block_ = 512;
  int latency_ = 0;
  int64_t position_ = 0;
  BufferListStorage buffers_{};
  std::array<midi::MidiEvent, kEventQueueDepth> events_{};
  size_t event_count_ = 0;
};

class AuEffectProcessor final : public rt::ProcessorBase {
 public:
  explicit AuEffectProcessor(AudioUnit unit) : unit_(unit) {}
  ~AuEffectProcessor() override {
    if (unit_ != nullptr) {
      AudioUnitUninitialize(unit_);
      AudioComponentInstanceDispose(unit_);
    }
  }

  void prepare(double sample_rate, int max_block_size) override {
    set_planar_float_format(unit_, kAudioUnitScope_Input, sample_rate, 2);
    set_planar_float_format(unit_, kAudioUnitScope_Output, sample_rate, 2);
    auto frames = static_cast<UInt32>(max_block_size);
    AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                         &frames, sizeof(frames));
    // Supply input via a render callback that copies the host's in-place buffer.
    AURenderCallbackStruct cb{};
    cb.inputProc = &AuEffectProcessor::input_trampoline;
    cb.inputProcRefCon = this;
    AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                         sizeof(cb));
    AudioUnitInitialize(unit_);
    latency_ = query_latency_samples(unit_, sample_rate);
    position_ = 0;
  }

  void process(float* const* channels, int num_channels, int num_samples) override {
    if (unit_ == nullptr || num_channels <= 0) return;
    const int chans = num_channels > static_cast<int>(kMaxChannels) ? static_cast<int>(kMaxChannels)
                                                                    : num_channels;
    in_channels_ = channels;
    in_count_ = chans;
    AudioBufferList* list = buffers_.list();
    list->mNumberBuffers = static_cast<UInt32>(chans);
    for (int c = 0; c < chans; ++c) {
      list->mBuffers[c].mNumberChannels = 1;
      list->mBuffers[c].mDataByteSize = static_cast<UInt32>(num_samples * sizeof(float));
      list->mBuffers[c].mData = channels[c];
    }
    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp ts{};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    ts.mSampleTime = static_cast<Float64>(position_);
    AudioUnitRender(unit_, &flags, &ts, 0, static_cast<UInt32>(num_samples), list);
    position_ += num_samples;
    in_channels_ = nullptr;
  }

  void reset() override {
    if (unit_ != nullptr) AudioUnitReset(unit_, kAudioUnitScope_Global, 0);
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
  int latency_ = 0;
  int64_t position_ = 0;
  BufferListStorage buffers_{};
  float* const* in_channels_ = nullptr;
  int in_count_ = 0;
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

}  // namespace

// ===========================================================================
// AuInstrumentProvider
// ===========================================================================

AuInstrumentProvider::AuInstrumentProvider() = default;
AuInstrumentProvider::~AuInstrumentProvider() = default;

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
  if (!can_create(descriptor)) return nullptr;
  AudioUnit unit = instantiate(descriptor);
  if (unit == nullptr) return nullptr;
  return std::make_unique<AuEffectProcessor>(unit);
}

size_t AuInstrumentProvider::parameter_count(const PluginDescriptor& descriptor) const noexcept {
  AudioUnit unit = instantiate(descriptor);
  if (unit == nullptr) return 0;
  UInt32 size = 0;
  Boolean writable = false;
  const OSStatus status = AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_ParameterList,
                                                   kAudioUnitScope_Global, 0, &size, &writable);
  AudioComponentInstanceDispose(unit);
  if (status != noErr || size == 0) return 0;
  return size / sizeof(AudioUnitParameterID);
}

bool AuInstrumentProvider::parameter_descriptor(const PluginDescriptor& descriptor, size_t index,
                                                PluginParameterDescriptor* out) const noexcept {
  if (out == nullptr) return false;
  AudioUnit unit = instantiate(descriptor);
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
  AudioComponentInstanceDispose(unit);
  return ok;
}

}  // namespace sonare::host::backends
