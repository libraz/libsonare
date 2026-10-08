#include "midi/layered_instrument.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <new>

#include "midi/channel_voice_decode.h"
#include "midi/source_residual.h"
#include "midi/ump.h"
#include "rt/pan_law.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::midi {

namespace {

/// 7-bit velocity of a note-on in either protocol.
uint8_t velocity7(const Ump& u) noexcept {
  ChannelVoiceEvent ev;
  if (!decode_channel_voice(u, &ev)) return 0;
  return ev.velocity.u7();
}

bool is_note_addressed(ChannelVoiceKind kind) noexcept {
  switch (kind) {
    case ChannelVoiceKind::PolyPressure:
    case ChannelVoiceKind::RegisteredPerNote:
    case ChannelVoiceKind::AssignablePerNote:
    case ChannelVoiceKind::PerNotePitchBend:
    case ChannelVoiceKind::PerNoteManagement:
      return true;
    default:
      return false;
  }
}

bool transpose_note(uint8_t source_note, int transpose, uint8_t* out) noexcept {
  if (out == nullptr) return false;
  const int64_t result = static_cast<int64_t>(source_note) + static_cast<int64_t>(transpose);
  if (result < 0 || result > 127) return false;
  *out = static_cast<uint8_t>(result);
  return true;
}

}  // namespace

bool LayeredInstrument::add_layer(std::unique_ptr<MidiInstrument> instrument,
                                  const InstrumentLayerSpec& spec) {
  if (instrument == nullptr || layers_.size() >= kMaxInstrumentLayers) return false;

  Layer layer;
  layer.instrument = std::move(instrument);
  layer.spec = spec;
  const rt::PanGains gains = rt::compute_pan_gains(
      std::clamp(spec.pan, -1.0f, 1.0f), rt::PanLaw::Const3dB, rt::PanNormalization::NearUnity);
  layer.gain_left = gains.left * spec.level;
  layer.gain_right = gains.right * spec.level;
  layers_.push_back(std::move(layer));
  prepared_ = false;
  invalidate_prepared_domain();
  return true;
}

MidiInstrument* LayeredInstrument::layer_at(size_t index) noexcept {
  return index < layers_.size() ? layers_[index].instrument.get() : nullptr;
}

void LayeredInstrument::prepare(double sample_rate, int max_block_size) {
  for (Layer& layer : layers_) layer.instrument->prepare(sample_rate, max_block_size);

  // Unequal latencies would sum misaligned, and nothing downstream could tell.
  if (!layers_.empty()) {
    const int first = layers_.front().instrument->latency_samples();
    for (const Layer& layer : layers_) {
      if (layer.instrument->latency_samples() != first) {
        throw SonareException(ErrorCode::InvalidState,
                              "LayeredInstrument layers must report the same latency");
      }
    }
  }

  max_block_size_ = std::max(0, max_block_size);
  scratch_.assign(static_cast<size_t>(max_block_size_) * 2u, 0.0f);
  source_scratch_.assign(kMaxResidualSources * 2u * static_cast<size_t>(max_block_size_), 0.0f);
  note_ledger_.assign(kNoteLedgerCapacity, ActiveNote{});
  if (prepared_identity_.get() == nullptr) {
    prepared_identity_ = std::make_shared<const PreparedLayeredSysExIdentity>();
  }
  prepared_ = true;
  invalidate_prepared_domain();
}

void LayeredInstrument::reset() {
  for (Layer& layer : layers_) layer.instrument->reset();
  std::fill(note_ledger_.begin(), note_ledger_.end(), ActiveNote{});
}

bool LayeredInstrument::covers(const Layer& layer, uint8_t note, uint8_t velocity) const noexcept {
  const InstrumentLayerSpec& s = layer.spec;
  return note >= s.key_lo && note <= s.key_hi && velocity >= s.vel_lo && velocity <= s.vel_hi;
}

bool LayeredInstrument::retune(const Ump& ump, uint8_t note, int transpose, Ump* out) noexcept {
  if (out == nullptr) return false;
  Ump copy = ump;
  copy.words[0] = (copy.words[0] & ~(0x7Fu << 8)) | (static_cast<uint32_t>(note & 0x7Fu) << 8);

  if (copy.message_type() == UmpMessageType::kMidi2ChannelVoice) {
    const uint8_t status = copy.status_nibble();
    if (status == 0x9u && note_attribute_type(copy) == 3u) {
      const int64_t pitch =
          static_cast<int64_t>(note_attribute_data(copy)) + static_cast<int64_t>(transpose) * 512;
      if (pitch < 0 || pitch > std::numeric_limits<uint16_t>::max()) return false;
      copy.words[1] = (copy.words[1] & ~uint32_t{0xFFFFu}) | static_cast<uint32_t>(pitch);
    } else if (status == 0x0u && (copy.words[0] & 0xFFu) == 3u) {
      const int64_t pitch = static_cast<int64_t>(copy.words[1]) +
                            static_cast<int64_t>(transpose) * (int64_t{1} << 25);
      if (pitch < 0 || pitch > std::numeric_limits<uint32_t>::max()) return false;
      copy.words[1] = static_cast<uint32_t>(pitch);
    }
  }
  *out = copy;
  return true;
}

bool LayeredInstrument::send_note(size_t layer_index, const MidiEvent& event, uint8_t note,
                                  int transpose, uint32_t destination_id,
                                  const PreparedLayeredSysEx* prepared) noexcept {
  MidiEvent copy = event;
  if (!retune(event.ump, note, transpose, &copy.ump)) return false;
  if (prepared != nullptr) {
    copy.prepared_sysex = prepared->children[layer_index].get();
  }
  layers_[layer_index].instrument->on_event(destination_id, copy);
  return true;
}

size_t LayeredInstrument::find_note_ledger_entry(uint32_t destination_id,
                                                 const MidiEvent& event) const noexcept {
  const Ump& ump = event.ump;
  const uint8_t group = ump_group_from_word0(ump.words[0]);
  const uint8_t channel = ump.channel();
  const uint8_t source_note = ump.note_number();
  for (size_t i = 0; i < note_ledger_.size(); ++i) {
    const ActiveNote& note = note_ledger_[i];
    if (note.active && note.group == group && note.channel == channel &&
        note.source_note == source_note && note.source_track_id == event.source_track_id &&
        note.destination_id == destination_id) {
      return i;
    }
  }
  return kNoNoteLedgerEntry;
}

void LayeredInstrument::retire_note_ledger_channel(uint32_t destination_id, uint8_t group,
                                                   uint8_t channel) noexcept {
  for (ActiveNote& note : note_ledger_) {
    if (note.active && note.destination_id == destination_id && note.group == group &&
        note.channel == channel) {
      note = ActiveNote{};
    }
  }
}

const LayeredInstrument::PreparedLayeredSysEx* LayeredInstrument::valid_prepared_sysex(
    const PreparedMidiSysEx* prepared) const noexcept {
  if (prepared == nullptr) return nullptr;
  const auto* composite = dynamic_cast<const PreparedLayeredSysEx*>(prepared);
  if (composite == nullptr || composite->identity.get() == nullptr ||
      prepared_identity_.get() == nullptr ||
      composite->identity.get() != prepared_identity_.get() ||
      composite->domain != prepared_domain_ || !prepared_ ||
      composite->children.size() != layers_.size()) {
    return nullptr;
  }
  return composite;
}

void LayeredInstrument::on_event(uint32_t destination_id, const MidiEvent& event) noexcept {
  const PreparedLayeredSysEx* prepared = valid_prepared_sysex(event.prepared_sysex);
  if (event.prepared_sysex != nullptr && prepared == nullptr) return;

  const Ump& u = event.ump;
  const bool note_on = u.is_note_on();
  const bool note_off = u.is_note_off();
  ChannelVoiceEvent decoded;
  const bool is_channel_voice = decode_channel_voice(u, &decoded);
  if (!note_on && !note_off) {
    if (is_channel_voice && decoded.kind == ChannelVoiceKind::ControlChange &&
        (decoded.note == 120u || (decoded.note >= 123u && decoded.note <= 125u))) {
      retire_note_ledger_channel(destination_id, ump_group_from_word0(u.words[0]), u.channel());
    }
    if (is_channel_voice && is_note_addressed(decoded.kind)) {
      const uint8_t source_note = decoded.note;
      for (size_t i = 0; i < layers_.size(); ++i) {
        const Layer& layer = layers_[i];
        if (source_note < layer.spec.key_lo || source_note > layer.spec.key_hi) continue;
        uint8_t transposed = 0;
        if (!transpose_note(source_note, layer.spec.transpose, &transposed)) continue;
        send_note(i, event, transposed, layer.spec.transpose, destination_id, nullptr);
      }
      return;
    }
    if (prepared == nullptr) {
      // Unprepared events broadcast unchanged, including borrowed SysEx views.
      for (Layer& layer : layers_) layer.instrument->on_event(destination_id, event);
    } else {
      // A layered token is a lease bundle, not a child token. Each child must
      // receive its own operation domain; forwarding the composite to Sf2 (or
      // another child) makes its owner check reject the event.
      for (size_t i = 0; i < layers_.size(); ++i) {
        MidiEvent copy = event;
        copy.prepared_sysex = prepared->children[i].get();
        layers_[i].instrument->on_event(destination_id, copy);
      }
    }
    return;
  }

  if (note_on) {
    const uint8_t note = u.note_number();
    const uint8_t vel = velocity7(u);
    size_t entry = find_note_ledger_entry(destination_id, event);
    if (entry == kNoNoteLedgerEntry) {
      for (size_t i = 0; i < note_ledger_.size(); ++i) {
        if (!note_ledger_[i].active) {
          entry = i;
          break;
        }
      }
      // Full ledger: drop and count a new identity; an existing one stays retriggerable.
      if (entry == kNoNoteLedgerEntry) {
        ledger_overflow_count_.bump();
        return;
      }
    }

    uint32_t owners = 0;
    for (size_t i = 0; i < layers_.size(); ++i) {
      if (!covers(layers_[i], note, vel)) continue;
      uint8_t transposed = 0;
      if (!transpose_note(note, layers_[i].spec.transpose, &transposed)) continue;
      if (send_note(i, event, transposed, layers_[i].spec.transpose, destination_id, prepared)) {
        owners |= 1u << i;
      }
    }
    if (owners == 0) return;

    ActiveNote& active = note_ledger_[entry];
    if (!active.active) {
      active.active = true;
      active.group = ump_group_from_word0(u.words[0]);
      active.channel = u.channel();
      active.source_note = note;
      active.source_track_id = event.source_track_id;
      active.destination_id = destination_id;
      active.owners = owners;
    } else {
      // A retrigger may open another velocity split; keep every owner until note-off.
      active.owners |= owners;
    }
    return;
  }

  const size_t entry = find_note_ledger_entry(destination_id, event);
  if (entry == kNoNoteLedgerEntry) return;
  const ActiveNote active = note_ledger_[entry];
  note_ledger_[entry] = ActiveNote{};
  const uint32_t owners = active.owners;
  const uint8_t note = u.note_number();
  for (size_t i = 0; i < layers_.size(); ++i) {
    if ((owners & (1u << i)) == 0) continue;
    uint8_t transposed = 0;
    if (!transpose_note(note, layers_[i].spec.transpose, &transposed)) continue;
    send_note(i, event, transposed, layers_[i].spec.transpose, destination_id, prepared);
  }
}

void LayeredInstrument::add_layer_output(float* const* target, int num_channels, int legs,
                                         const float* left, const float* right, const Layer& layer,
                                         int num_samples) noexcept {
  // A mono target averages the panned pair, so mirrored pans give the same level
  // and a centred layer keeps its own.
  if (num_channels == 1) {
    if (target[0] == nullptr) return;
    for (int i = 0; i < num_samples; ++i) {
      target[0][i] += 0.5f * (left[i] * layer.gain_left + right[i] * layer.gain_right);
    }
    return;
  }
  if (target[0] != nullptr) {
    for (int i = 0; i < num_samples; ++i) target[0][i] += left[i] * layer.gain_left;
  }
  if (legs > 1 && target[1] != nullptr) {
    for (int i = 0; i < num_samples; ++i) target[1][i] += right[i] * layer.gain_right;
  }
  // Anything past the stereo pair takes a mono fold-down, as every built-in
  // instrument gives it.
  for (int ch = 2; ch < num_channels; ++ch) {
    if (target[ch] == nullptr) continue;
    for (int i = 0; i < num_samples; ++i) {
      target[ch][i] +=
          constants::kInvSqrt2 * (left[i] * layer.gain_left + right[i] * layer.gain_right);
    }
  }
}

void LayeredInstrument::process(float* const* channels, int num_channels, int num_samples) {
  ensure_prepared(prepared_, "LayeredInstrument");
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) return;
  if (num_samples > max_block_size_) return;

  // Layers always render their stereo pair; a mono output folds it after the pan.
  constexpr int legs = 2;
  float* scratch_left = scratch_.data();
  float* scratch_right = scratch_.data() + max_block_size_;
  float* scratch_chans[2] = {scratch_left, scratch_right};

  // Read before the layer loop and compared after, so this block bumps at
  // most once however many layers discarded during it.
  const uint64_t layer_discards_before = layer_discard_sum();
  for (Layer& layer : layers_) {
    std::fill_n(scratch_left, num_samples, 0.0f);
    std::fill_n(scratch_right, num_samples, 0.0f);
    layer.instrument->process(scratch_chans, legs, num_samples);
    add_layer_output(channels, num_channels, legs, scratch_left, scratch_right, layer, num_samples);
  }
  if (layer_discard_sum() != layer_discards_before) note_non_finite_discard();
}

bool LayeredInstrument::supports_source_track_rendering() const noexcept {
  for (const Layer& layer : layers_) {
    if (!layer.instrument->supports_source_track_rendering()) return false;
  }
  return true;
}

bool LayeredInstrument::process_source_tracks(const MidiInstrumentSourceOutput* outputs,
                                              size_t output_count, int num_channels,
                                              int num_samples) noexcept {
  if (outputs == nullptr || output_count == 0 || output_count > kMaxResidualSources ||
      num_channels <= 0 || num_samples <= 0 || num_samples > max_block_size_) {
    return false;
  }
  for (const Layer& layer : layers_) {
    if (!layer.instrument->supports_source_track_rendering()) return false;
  }

  constexpr int legs = 2;
  const size_t block = static_cast<size_t>(max_block_size_);
  const auto scratch_channel = [&](size_t slot, int leg) noexcept -> float* {
    return source_scratch_.data() + (slot * 2u + static_cast<size_t>(leg)) * block;
  };

  // One scratch target per caller output slot, reused for every layer below
  // (re-zeroed and re-rendered into each time) rather than allocated per layer.
  std::array<std::array<float*, 2>, kMaxResidualSources> scratch_chans{};
  std::array<MidiInstrumentSourceOutput, kMaxResidualSources> layer_outputs{};
  for (size_t s = 0; s < output_count; ++s) {
    scratch_chans[s] = {scratch_channel(s, 0), scratch_channel(s, 1)};
    layer_outputs[s] = {outputs[s].source_track_id, scratch_chans[s].data()};
  }

  const uint64_t layer_discards_before = layer_discard_sum();
  for (Layer& layer : layers_) {
    for (size_t s = 0; s < output_count; ++s) {
      std::fill_n(scratch_channel(s, 0), num_samples, 0.0f);
      std::fill_n(scratch_channel(s, 1), num_samples, 0.0f);
    }
    if (!layer.instrument->process_source_tracks(layer_outputs.data(), output_count, legs,
                                                 num_samples)) {
      return false;
    }
    for (size_t s = 0; s < output_count; ++s) {
      float* const* target = outputs[s].channels;
      if (target == nullptr) continue;
      add_layer_output(target, num_channels, legs, scratch_channel(s, 0), scratch_channel(s, 1),
                       layer, num_samples);
    }
  }
  if (layer_discard_sum() != layer_discards_before) note_non_finite_discard();
  return true;
}

void LayeredInstrument::set_transport(const transport::TransportState& state) noexcept {
  for (Layer& layer : layers_) layer.instrument->set_transport(state);
}

void LayeredInstrument::on_control_sysex(const uint8_t* data, size_t size) noexcept {
  for (Layer& layer : layers_) layer.instrument->on_control_sysex(data, size);
}

bool LayeredInstrument::prepare_sysex(const uint8_t* data, size_t size,
                                      std::shared_ptr<const PreparedMidiSysEx>& out) {
  out.reset();
  if (!prepared_) return false;

  try {
    // A moved-from LayeredInstrument retains its prepared flag but loses the
    // shared identity along with the move. Stage a replacement locally so an
    // allocation or child-preparation failure leaves the object unchanged.
    std::shared_ptr<const PreparedLayeredSysExIdentity> identity = prepared_identity_;
    if (identity.get() == nullptr) {
      identity = std::make_shared<const PreparedLayeredSysExIdentity>();
    }
    auto composite = std::make_shared<PreparedLayeredSysEx>();
    composite->identity = identity;
    composite->domain = prepared_domain_;
    composite->children.reserve(layers_.size());
    for (const Layer& layer : layers_) {
      std::shared_ptr<const PreparedMidiSysEx> child;
      if (!layer.instrument->prepare_sysex(data, size, child)) {
        out.reset();
        return false;
      }
      // Keep one slot per layer even when a child deliberately accepts the
      // payload without an opaque operation. The composite remains non-null,
      // and the null slot selects the child's raw-byte path on acceptance.
      composite->children.push_back(std::move(child));
    }
    out = std::shared_ptr<const PreparedMidiSysEx>(std::move(composite));
    if (prepared_identity_.get() == nullptr) prepared_identity_ = std::move(identity);
    return true;
  } catch (const std::bad_alloc&) {
    out.reset();
    throw;
  } catch (...) {
    out.reset();
    return false;
  }
}

void LayeredInstrument::on_prepared_sysex_accepted(const uint8_t* data, size_t size,
                                                   const PreparedMidiSysEx* prepared) noexcept {
  if (prepared == nullptr) {
    on_control_sysex(data, size);
    return;
  }
  const PreparedLayeredSysEx* composite = valid_prepared_sysex(prepared);
  if (composite == nullptr) return;
  for (size_t i = 0; i < layers_.size(); ++i) {
    layers_[i].instrument->on_prepared_sysex_accepted(data, size, composite->children[i].get());
  }
}

int LayeredInstrument::latency_samples() const noexcept {
  int latency = 0;
  for (const Layer& layer : layers_) {
    latency = std::max(latency, layer.instrument->latency_samples());
  }
  return latency;
}

int LayeredInstrument::tail_samples() const noexcept {
  int tail = 0;
  for (const Layer& layer : layers_) {
    tail = std::max(tail, layer.instrument->tail_samples());
  }
  return tail;
}

int LayeredInstrument::parameter_id_for_key(const std::string& key) const noexcept {
  // "<layer index>.<child key>" addresses one layer; the id is the child's own
  // offset by the layer's stride, so it stays stable without a lookup table.
  const size_t dot = key.find('.');
  if (dot == std::string::npos || dot == 0) return -1;
  size_t index = 0;
  for (size_t i = 0; i < dot; ++i) {
    if (key[i] < '0' || key[i] > '9') return -1;
    index = index * 10u + static_cast<size_t>(key[i] - '0');
    if (index >= layers_.size()) return -1;
  }
  const int child = layers_[index].instrument->parameter_id_for_key(key.substr(dot + 1));
  if (child < 0 || child >= kLayerParamStride) return -1;
  return static_cast<int>(index) * kLayerParamStride + child;
}

bool LayeredInstrument::apply_parameter(unsigned int param_id, float value) noexcept {
  const size_t index = param_id / static_cast<unsigned int>(kLayerParamStride);
  if (index >= layers_.size()) return false;
  const unsigned int child = param_id % static_cast<unsigned int>(kLayerParamStride);
  return layers_[index].instrument->apply_parameter(child, value);
}

bool LayeredInstrument::describe_parameter(unsigned int param_id,
                                           automation::ParameterDescription* out) const {
  if (out == nullptr) return false;
  const size_t index = param_id / static_cast<unsigned int>(kLayerParamStride);
  if (index >= layers_.size()) return false;
  const unsigned int child = param_id % static_cast<unsigned int>(kLayerParamStride);
  if (!layers_[index].instrument->describe_parameter(child, out)) return false;
  // Same "<layer index>.<child key>" address parameter_id_for_key resolves.
  out->name = std::to_string(index) + "." + out->name;
  return true;
}

}  // namespace sonare::midi
