/// @file au_instrument_provider.mm
/// @brief Audio Unit host: wraps AU instances in core ProcessorBase /
///        MidiInstrument adapters. See au_instrument_provider.h.

#include "host/backends/plughost/au_instrument_provider.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "midi/midi_event.h"
#include "midi/ump.h"
#include "transport/transport_state.h"
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
struct alignas(AudioBufferList) BufferListStorage {
  std::array<uint8_t, sizeof(AudioBufferList) + kMaxChannels * sizeof(AudioBuffer)> bytes{};
  AudioBufferList* list() noexcept { return reinterpret_cast<AudioBufferList*>(bytes.data()); }
};

/// Finalize the host channels after an AU render call so the instrument and
/// effect render paths behave identically (a fix on one must not drift from the
/// other). On a failed render the AU-targeted host channels are silenced; on
/// success any non-finite sample the AU emitted is scrubbed to zero so it cannot
/// circulate through a downstream feedback effect (delay/reverb) and silence the
/// whole session. Host channels beyond the AU's negotiated count are silenced,
/// and any tail past the AU's maximum block size is zeroed so a caller violating
/// the negotiated maximum gets a silent tail rather than a scratch overrun or an
/// AU property change on the render thread. `render_chans` is the AU's negotiated
/// channel count, `chans` the clamped host channel count, `num_channels` the
/// caller's channel count.
void finalize_au_output(float* const* channels, int num_channels, int chans, int render_chans,
                        int render_samples, int num_samples, bool render_ok) noexcept {
  if (!render_ok) {
    for (int c = 0; c < chans; ++c) {
      if (channels[c] != nullptr) {
        std::memset(channels[c], 0, static_cast<size_t>(render_samples) * sizeof(float));
      }
    }
  } else {
    for (int c = 0; c < render_chans && c < chans; ++c) {
      if (channels[c] == nullptr) continue;
      for (int s = 0; s < render_samples; ++s) {
        if (!std::isfinite(channels[c][s])) channels[c][s] = 0.0f;
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
}

}  // namespace

// ===========================================================================
// AU instrument adapter
// ===========================================================================

namespace {

class AuMidiInstrument final : public midi::MidiInstrument, public AuInstrumentTelemetry {
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

  /// Adopts the block's first DEVICE render frame as the basis every queued event
  /// is placed against (see "Event clock domain" in midi/instrument.h). The engine
  /// pushes this snapshot immediately before each process() call. A self-
  /// accumulated position cannot serve as the basis: the engine renders an
  /// instrument only while the transport rolls or one of its notes still sounds,
  /// so an internal counter drifts out of the event basis on the first stop and
  /// never resyncs on a seek or a loop wrap. Without a host transport the
  /// free-running position advanced by process() remains the fallback.
  void set_transport(const transport::TransportState& state) noexcept override {
    position_ = state.render_frame;
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
    // position_ is the block's first DEVICE render frame, taken from the engine's
    // transport snapshot in set_transport(); the negative case below is the
    // stopped-and-silent window in which the engine renders nothing and events
    // queue across more than one block (midi/instrument.h).
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
    finalize_au_output(channels, num_channels, chans, render_chans, render_samples, num_samples,
                       status == noErr);
    position_ += num_samples;
  }

  void reset() override {
    if (unit_ != nullptr) api_->reset(unit_, kAudioUnitScope_Global, 0);
    event_count_ = 0;
    position_ = 0;
  }

  int latency_samples() const noexcept override { return latency_; }

  void on_event(uint32_t /*destination_id*/, const midi::MidiEvent& event) noexcept override {
    if (event_count_ < events_.size()) {
      events_[event_count_++] = event;
    } else {
      // Mirror the fixed MIDI queues' telemetry: a full block-local event buffer
      // drops the event, but the drop is counted rather than silent.
      dropped_events_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  /// Cumulative count of events dropped because the block-local event buffer was
  /// full when on_event() was called. Mirrors FixedMidiInputSource::dropped_count.
  /// Reachable from a plain midi::MidiInstrument* (what create_instrument()
  /// actually returns) via dynamic_cast<const AuInstrumentTelemetry*> — see the
  /// interface's doc comment in au_instrument_provider.h.
  uint32_t dropped_count() const noexcept override {
    return dropped_events_.load(std::memory_order_relaxed);
  }

 private:
  AudioUnit unit_ = nullptr;
  const AuRuntimeApi* api_ = &kSystemAuRuntimeApi;
  double sample_rate_ = 48000.0;
  int max_block_ = 512;
  int latency_ = 0;
  int output_channels_ = 0;
  bool initialized_ = false;
  // Block's first DEVICE render frame: overwritten by set_transport() under a
  // host, self-advanced by process() when there is none.
  int64_t position_ = 0;
  BufferListStorage buffers_{};
  std::vector<float> scratch_{};
  std::array<midi::MidiEvent, kEventQueueDepth> events_{};
  size_t event_count_ = 0;
  std::atomic<uint32_t> dropped_events_{0};
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
    in_samples_ = render_samples;
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
    finalize_au_output(channels, num_channels, chans, render_chans, render_samples, num_samples,
                       status == noErr);
    position_ += num_samples;
    in_channels_ = nullptr;
    in_samples_ = 0;
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
    // The caller's planes are only valid for the num_samples passed to this
    // process() call, which the AU may render in a smaller block than the
    // prepared maximum. Clamp to in_samples_ (the current call's frame count),
    // not max_block_ (the prepared upper bound), or a variable-block-size host
    // reads past the end of the caller's buffer.
    const size_t requested =
        std::min(static_cast<size_t>(frames), static_cast<size_t>(std::max(self->in_samples_, 0)));
    const int buffers = static_cast<int>(data->mNumberBuffers);
    for (int c = 0; c < buffers; ++c) {
      auto* dst = static_cast<float*>(data->mBuffers[c].mData);
      const float* src = c < self->in_count_ ? self->in_channels_[c] : nullptr;
      if (dst == nullptr) continue;
      const size_t capacity_frames = data->mBuffers[c].mDataByteSize / sizeof(float);
      // The AU asked for `frames` and will read all of them back. Clamping the
      // copy to the caller's block leaves [requested, frames) holding whatever
      // was in the AU's own input buffer — most often the previous block, which
      // the AU then processes as if it were current signal: a repeated tail that
      // a reverb or delay smears across the session, and stale rather than
      // silent is the difference between an audible artefact and a render tail.
      // The tail is zeroed for the same reason finalize_au_output() zeroes the
      // output side past render_samples, and the two must not drift apart.
      const size_t fillable = std::min(static_cast<size_t>(frames), capacity_frames);
      const size_t copied = src != nullptr ? std::min(requested, capacity_frames) : 0;
      if (copied > 0) std::memcpy(dst, src, copied * sizeof(float));
      if (fillable > copied) {
        std::memset(dst + copied, 0, (fillable - copied) * sizeof(float));
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
  int in_samples_ = 0;
};

struct AuCallSpyState {
  unsigned set_property_calls = 0;
  unsigned initialize_calls = 0;
  unsigned uninitialize_calls = 0;
  unsigned render_calls = 0;

  // --- effect input-callback probe (H-24 regression) ---
  // spy_set_property captures the AURenderCallbackStruct the effect installs on
  // kAudioUnitScope_Input. When probe_input_frames is non-zero, spy_render then
  // invokes that captured callback directly, requesting probe_input_frames
  // frames regardless of the frame count the outer render() call itself was
  // asked for — reproducing a third-party AU's internal input buffering, which
  // may pull more frames from the input callback than the current process()
  // block. The destination buffers below back the callback's own AudioBufferList
  // (not the caller's planes under test), sized to the largest block a probe
  // test uses.
  AURenderCallbackStruct captured_input_cb{};
  bool has_input_cb = false;
  UInt32 probe_input_frames = 0;
  std::array<float, 1024> probe_dst_left{};
  std::array<float, 1024> probe_dst_right{};

  // --- event-placement probe ---
  // Block-relative frames the instrument adapter handed to MusicDeviceMIDIEvent,
  // and the sample time it stamped on each render timestamp, in call order.
  std::array<UInt32, 8> midi_event_frames{};
  size_t midi_event_count = 0;
  std::array<Float64, 8> render_sample_times{};
  size_t render_sample_time_count = 0;
};

thread_local AuCallSpyState* g_au_call_spy = nullptr;

OSStatus spy_set_property(AudioUnit, AudioUnitPropertyID prop_id, AudioUnitScope scope,
                          AudioUnitElement, const void* value, UInt32 size) {
  ++g_au_call_spy->set_property_calls;
  if (prop_id == kAudioUnitProperty_SetRenderCallback && scope == kAudioUnitScope_Input &&
      value != nullptr && size == sizeof(AURenderCallbackStruct)) {
    g_au_call_spy->captured_input_cb = *static_cast<const AURenderCallbackStruct*>(value);
    g_au_call_spy->has_input_cb = true;
  }
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

OSStatus spy_render(AudioUnit, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts,
                    UInt32 bus, UInt32 /*frames*/, AudioBufferList* /*data*/) {
  ++g_au_call_spy->render_calls;
  if (ts != nullptr &&
      g_au_call_spy->render_sample_time_count < g_au_call_spy->render_sample_times.size()) {
    g_au_call_spy->render_sample_times[g_au_call_spy->render_sample_time_count++] = ts->mSampleTime;
  }
  if (g_au_call_spy->probe_input_frames > 0 && g_au_call_spy->has_input_cb) {
    BufferListStorage storage;
    AudioBufferList* list = storage.list();
    list->mNumberBuffers = 2;
    list->mBuffers[0].mNumberChannels = 1;
    list->mBuffers[0].mDataByteSize =
        static_cast<UInt32>(g_au_call_spy->probe_dst_left.size() * sizeof(float));
    list->mBuffers[0].mData = g_au_call_spy->probe_dst_left.data();
    list->mBuffers[1].mNumberChannels = 1;
    list->mBuffers[1].mDataByteSize =
        static_cast<UInt32>(g_au_call_spy->probe_dst_right.size() * sizeof(float));
    list->mBuffers[1].mData = g_au_call_spy->probe_dst_right.data();
    g_au_call_spy->captured_input_cb.inputProc(g_au_call_spy->captured_input_cb.inputProcRefCon,
                                               flags, ts, bus, g_au_call_spy->probe_input_frames,
                                               list);
  }
  return noErr;
}

OSStatus spy_reset(AudioUnit, AudioUnitScope, AudioUnitElement) { return noErr; }

OSStatus spy_midi_event(MusicDeviceComponent, UInt32, UInt32, UInt32, UInt32 frame) {
  if (g_au_call_spy->midi_event_count < g_au_call_spy->midi_event_frames.size()) {
    g_au_call_spy->midi_event_frames[g_au_call_spy->midi_event_count++] = frame;
  }
  return noErr;
}

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
///
/// noexcept: called from AuInstrumentProvider::parameter_count/descriptor,
/// both noexcept. `*cache_id = descriptor.id` can throw std::bad_alloc (the
/// encoded id string is long enough to exceed libc++'s SSO buffer); an
/// exception must not escape into those noexcept callers and reach
/// std::terminate, so it is caught here and reported as "no cached unit"
/// instead.
AudioUnit cached_param_unit(void** cache_unit, std::string* cache_id,
                            const PluginDescriptor& descriptor) noexcept {
  auto current = static_cast<AudioUnit>(*cache_unit);
  if (current != nullptr && *cache_id == descriptor.id) return current;
  if (current != nullptr) {
    AudioComponentInstanceDispose(current);
    *cache_unit = nullptr;
    cache_id->clear();
  }
  AudioUnit unit = instantiate(descriptor);
  if (unit != nullptr) {
    try {
      *cache_unit = unit;
      *cache_id = descriptor.id;
    } catch (...) {
      AudioComponentInstanceDispose(unit);
      *cache_unit = nullptr;
      cache_id->clear();
      return nullptr;
    }
  }
  return unit;
}

/// Translate the display/automation metadata an AU publishes for one parameter
/// into the seam's SDK-free fields. Only units with an exact seam counterpart
/// are mapped: an approximate one (AU Seconds onto kMilliseconds, LinearGain
/// onto kDecibels) would rescale the number a UI prints next to the value, so
/// anything else stays kGeneric — "no unit" understates, a wrong unit lies.
///
/// kAudioUnitParameterFlag_NonRealTime marks a parameter the AU cannot change
/// mid-render without glitching; the seam's `realtime_safe` is the flag the
/// automation engine already enforces (automation_engine.cpp rejects writes to
/// a non-RT-safe target), so the two are the same statement and must agree.
void apply_au_parameter_metadata(const AudioUnitParameterInfo& info,
                                 PluginParameterDescriptor* out) noexcept {
  switch (info.unit) {
    case kAudioUnitParameterUnit_Decibels:
      out->unit = PluginParameterUnit::kDecibels;
      break;
    case kAudioUnitParameterUnit_Hertz:
      out->unit = PluginParameterUnit::kHertz;
      break;
    case kAudioUnitParameterUnit_Milliseconds:
      out->unit = PluginParameterUnit::kMilliseconds;
      break;
    case kAudioUnitParameterUnit_Percent:
      out->unit = PluginParameterUnit::kPercent;
      break;
    case kAudioUnitParameterUnit_Boolean:
      out->unit = PluginParameterUnit::kBoolean;
      break;
    case kAudioUnitParameterUnit_RelativeSemiTones:
      out->unit = PluginParameterUnit::kSemitones;
      break;
    default:
      out->unit = PluginParameterUnit::kGeneric;
      break;
  }
  out->realtime_safe = (info.flags & kAudioUnitParameterFlag_NonRealTime) == 0;
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

detail::AuUndersizedBlockProbeResult detail::run_au_effect_undersized_block_probe() {
  AuCallSpyState state;
  g_au_call_spy = &state;
  auto fake_unit = reinterpret_cast<AudioUnit>(static_cast<uintptr_t>(1));

  constexpr int kPreparedMaxBlock = 512;
  constexpr int kActualBlock = 64;  // smaller than the prepared maximum
  // Distinguishable from both the host planes' contents and from silence.
  constexpr float kSentinel = -7.5f;

  detail::AuUndersizedBlockProbeResult result;
  {
    // Scoped so ~AuEffectProcessor() (which calls api_->uninitialize via the
    // spy table) runs before g_au_call_spy is cleared below.
    AuEffectProcessor effect(fake_unit, &kSpyAuRuntimeApi);
    effect.prepare(48000.0, kPreparedMaxBlock);
    // Make the fake render() pull from the input callback at the prepared
    // maximum, regardless of the block size the outer process() call below
    // uses, reproducing a third-party AU that internally buffers/looks ahead.
    state.probe_input_frames = static_cast<UInt32>(kPreparedMaxBlock);
    // Stand in for the stale block a real AU's input buffer still holds when it
    // pulls: a zeroed destination cannot tell "left untouched" apart from
    // "deliberately silenced", so the tail check below would pass either way.
    state.probe_dst_left.fill(kSentinel);
    state.probe_dst_right.fill(kSentinel);

    // Heap-allocate the host planes at exactly kActualBlock samples: any read
    // past the end is a heap-buffer-overflow under ASan, since num_samples is
    // the caller's contract for how many frames of each plane are valid.
    std::vector<float> left(static_cast<size_t>(kActualBlock), 0.25f);
    std::vector<float> right(static_cast<size_t>(kActualBlock), -0.25f);
    std::array<float*, 2> channels{left.data(), right.data()};
    effect.process(channels.data(), 2, kActualBlock);

    result.ran = true;
    result.block_samples = static_cast<size_t>(kActualBlock);
    result.probe_frames = static_cast<size_t>(kPreparedMaxBlock);

    const auto region_all = [](const std::array<float, 1024>& buffer, size_t begin, size_t end,
                               float expected) {
      for (size_t i = begin; i < end && i < buffer.size(); ++i) {
        if (buffer[i] != expected) return false;
      }
      return true;
    };
    const size_t block = result.block_samples;
    const size_t requested = result.probe_frames;
    result.input_head_copied = region_all(state.probe_dst_left, 0, block, 0.25f) &&
                               region_all(state.probe_dst_right, 0, block, -0.25f);
    result.input_tail_silent = region_all(state.probe_dst_left, block, requested, 0.0f) &&
                               region_all(state.probe_dst_right, block, requested, 0.0f);
    result.input_beyond_request_untouched =
        region_all(state.probe_dst_left, requested, state.probe_dst_left.size(), kSentinel) &&
        region_all(state.probe_dst_right, requested, state.probe_dst_right.size(), kSentinel);
  }
  g_au_call_spy = nullptr;
  return result;
}

detail::AuInstrumentDroppedEventProbeResult detail::run_au_instrument_dropped_event_probe() {
  AuCallSpyState state;
  g_au_call_spy = &state;
  auto fake_unit = reinterpret_cast<AudioUnit>(static_cast<uintptr_t>(1));

  detail::AuInstrumentDroppedEventProbeResult result;
  {
    // Scoped so ~AuMidiInstrument() (which calls api_->uninitialize/dispose via
    // the spy table) runs before g_au_call_spy is cleared below.
    AuMidiInstrument concrete(fake_unit, &kSpyAuRuntimeApi);
    concrete.prepare(48000.0, 4);

    // Downcast from the SAME interface pointer type create_instrument()
    // actually hands callers (midi::MidiInstrument*), not from the concrete
    // class — that is the reachability this probes for.
    midi::MidiInstrument* base = &concrete;
    const auto* telemetry = dynamic_cast<const AuInstrumentTelemetry*>(base);
    result.telemetry_reachable = telemetry != nullptr;
    if (telemetry != nullptr) {
      result.dropped_before_overflow = telemetry->dropped_count();
      // Queue exactly kEventQueueDepth events (fills the block-local buffer
      // without overflowing it), then one more: that last on_event() must be
      // counted as dropped, not silently discarded.
      for (size_t i = 0; i < kEventQueueDepth; ++i) {
        base->on_event(0, midi::MidiEvent{0, midi::make_midi1_note_on(0, 0, 60, 100)});
      }
      base->on_event(0, midi::MidiEvent{0, midi::make_midi1_note_on(0, 0, 61, 100)});
      result.dropped_after_overflow = telemetry->dropped_count();
    }
  }
  g_au_call_spy = nullptr;
  return result;
}

detail::AuInstrumentTransportPlacementProbeResult
detail::run_au_instrument_transport_placement_probe() {
  constexpr int kProbeBlock = 8;
  constexpr int64_t kFirstBlockFrame = 1000;
  constexpr int64_t kFirstEventOffset = 3;
  // A device frame far past kFirstBlockFrame + kProbeBlock: the engine keeps
  // advancing the render frame through blocks it does not ask this instrument to
  // render, and a seek or loop wrap lands the next rendered block anywhere.
  constexpr int64_t kSecondBlockFrame = 5000;
  constexpr int64_t kSecondEventOffset = 6;

  AuCallSpyState state;
  g_au_call_spy = &state;
  auto fake_unit = reinterpret_cast<AudioUnit>(static_cast<uintptr_t>(1));

  detail::AuInstrumentTransportPlacementProbeResult result;
  {
    // Scoped so ~AuMidiInstrument() runs before g_au_call_spy is cleared.
    AuMidiInstrument instrument(fake_unit, &kSpyAuRuntimeApi);
    instrument.prepare(48000.0, kProbeBlock);
    std::array<float, kProbeBlock> left{};
    std::array<float, kProbeBlock> right{};
    std::array<float*, 2> channels{left.data(), right.data()};

    transport::TransportState block{};
    block.render_frame = kFirstBlockFrame;
    instrument.on_event(0, midi::MidiEvent{kFirstBlockFrame + kFirstEventOffset,
                                           midi::make_midi1_note_on(0, 0, 60, 100)});
    instrument.set_transport(block);
    instrument.process(channels.data(), 2, kProbeBlock);

    block.render_frame = kSecondBlockFrame;
    instrument.on_event(0, midi::MidiEvent{kSecondBlockFrame + kSecondEventOffset,
                                           midi::make_midi1_note_on(0, 0, 62, 100)});
    instrument.set_transport(block);
    instrument.process(channels.data(), 2, kProbeBlock);
  }
  g_au_call_spy = nullptr;

  result.ran = state.midi_event_count == 2 && state.render_sample_time_count == 2;
  if (result.ran) {
    result.first_event_frame = state.midi_event_frames[0];
    result.second_event_frame = state.midi_event_frames[1];
    result.first_sample_time = state.render_sample_times[0];
    result.second_sample_time = state.render_sample_times[1];
  }
  return result;
}

detail::AuParameterMetadataProbeResult detail::run_au_parameter_metadata_probe() {
  // Exercises apply_au_parameter_metadata(), the same translation the provider's
  // parameter_descriptor() applies to a live AU's AudioUnitParameterInfo.
  const auto translate = [](AudioUnitParameterUnit unit, AudioUnitParameterOptions flags) {
    AudioUnitParameterInfo info{};
    info.unit = unit;
    info.flags = flags;
    PluginParameterDescriptor descriptor;
    apply_au_parameter_metadata(info, &descriptor);
    return descriptor;
  };

  detail::AuParameterMetadataProbeResult result;
  result.hertz = translate(kAudioUnitParameterUnit_Hertz, 0).unit;
  result.decibels = translate(kAudioUnitParameterUnit_Decibels, 0).unit;
  result.milliseconds = translate(kAudioUnitParameterUnit_Milliseconds, 0).unit;
  result.percent = translate(kAudioUnitParameterUnit_Percent, 0).unit;
  result.boolean_flag = translate(kAudioUnitParameterUnit_Boolean, 0).unit;
  result.relative_semitones = translate(kAudioUnitParameterUnit_RelativeSemiTones, 0).unit;
  result.without_counterpart = translate(kAudioUnitParameterUnit_Beats, 0).unit;
  result.realtime_safe_without_flag =
      translate(kAudioUnitParameterUnit_Generic, kAudioUnitParameterFlag_IsWritable).realtime_safe;
  result.realtime_safe_with_flag =
      translate(kAudioUnitParameterUnit_Generic,
                kAudioUnitParameterFlag_IsWritable | kAudioUnitParameterFlag_NonRealTime)
          .realtime_safe;
  result.ran = true;
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
  const std::array<OSType, 2> component_types =
      kind == PluginKind::kInstrument
          ? std::array<OSType, 2>{kAudioUnitType_MusicDevice, 0}
          : std::array<OSType, 2>{kAudioUnitType_Effect, kAudioUnitType_MusicEffect};
  for (const OSType type : component_types) {
    if (type == 0) continue;
    AudioComponentDescription query{};
    query.componentType = type;
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
  if (!decode_id(descriptor.id, desc) || (desc.componentType != kAudioUnitType_Effect &&
                                          desc.componentType != kAudioUnitType_MusicEffect))
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
  // The body below allocates (the `ids` vector; PluginParameterDescriptor::name
  // string assignments), which can throw std::bad_alloc. This function is
  // noexcept, so an escaping exception would reach std::terminate — catch and
  // report failure instead.
  try {
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
            apply_au_parameter_metadata(info, out);
            // HasCFNameString signals a CF name is PRESENT; CFNameRelease
            // signals the caller must release it. Gating presence on
            // CFNameRelease (as before) dropped the CF name for AUs that
            // expose a static, non-owned CF string, falling back to the
            // deprecated char[] info.name.
            if ((info.flags & kAudioUnitParameterFlag_HasCFNameString) &&
                info.cfNameString != nullptr) {
              char nbuf[128];
              if (CFStringGetCString(info.cfNameString, nbuf, sizeof(nbuf),
                                     kCFStringEncodingUTF8)) {
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
  } catch (...) {
    return false;
  }
}

}  // namespace sonare::host::backends
