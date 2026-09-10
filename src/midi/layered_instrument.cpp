#include "midi/layered_instrument.h"

#include <algorithm>
#include <cstdlib>

#include "midi/ump.h"
#include "rt/pan_law.h"
#include "util/exception.h"

namespace sonare::midi {

namespace {

/// 7-bit velocity of a note message in either protocol.
uint8_t velocity7(const Ump& u) noexcept {
  if (u.message_type() == UmpMessageType::kMidi1ChannelVoice) return u.data2_7bit();
  return scale_note_on_velocity_16_to_7(static_cast<uint16_t>(u.words[1] >> 16));
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
  note_owners_.assign(kNoteSlots, 0u);
  prepared_ = true;
}

void LayeredInstrument::reset() {
  for (Layer& layer : layers_) layer.instrument->reset();
  std::fill(note_owners_.begin(), note_owners_.end(), 0u);
}

bool LayeredInstrument::covers(const Layer& layer, uint8_t note, uint8_t velocity) const noexcept {
  const InstrumentLayerSpec& s = layer.spec;
  return note >= s.key_lo && note <= s.key_hi && velocity >= s.vel_lo && velocity <= s.vel_hi;
}

Ump LayeredInstrument::retune(const Ump& ump, uint8_t note) noexcept {
  Ump out = ump;
  out.words[0] = (out.words[0] & ~(0x7Fu << 8)) | (static_cast<uint32_t>(note & 0x7Fu) << 8);
  return out;
}

void LayeredInstrument::send_note(size_t layer_index, const MidiEvent& event, uint8_t note,
                                  uint32_t destination_id) noexcept {
  MidiEvent copy = event;
  copy.ump = retune(event.ump, note);
  layers_[layer_index].instrument->on_event(destination_id, copy);
}

void LayeredInstrument::on_event(uint32_t destination_id, const MidiEvent& event) noexcept {
  const Ump& u = event.ump;
  const bool note_on = u.is_note_on();
  const bool note_off = u.is_note_off();
  if (!note_on && !note_off) {
    for (Layer& layer : layers_) layer.instrument->on_event(destination_id, event);
    return;
  }

  const uint8_t note = u.note_number();
  const size_t slot = static_cast<size_t>(u.channel()) * 128u + note;
  if (slot >= note_owners_.size()) return;

  if (note_on) {
    const uint8_t vel = velocity7(u);
    uint32_t owners = 0;
    for (size_t i = 0; i < layers_.size(); ++i) {
      if (!covers(layers_[i], note, vel)) continue;
      const int transposed = static_cast<int>(note) + layers_[i].spec.transpose;
      if (transposed < 0 || transposed > 127) continue;
      owners |= 1u << i;
      send_note(i, event, static_cast<uint8_t>(transposed), destination_id);
    }
    // A retrigger before the note-off replaces the owner set; the layers that
    // held the previous strike are exactly the ones that just received a
    // second note-on, which each resolves in its own voice pool.
    note_owners_[slot] = owners;
    return;
  }

  const uint32_t owners = note_owners_[slot];
  note_owners_[slot] = 0;
  for (size_t i = 0; i < layers_.size(); ++i) {
    if ((owners & (1u << i)) == 0) continue;
    const int transposed = static_cast<int>(note) + layers_[i].spec.transpose;
    if (transposed < 0 || transposed > 127) continue;
    send_note(i, event, static_cast<uint8_t>(transposed), destination_id);
  }
}

void LayeredInstrument::process(float* const* channels, int num_channels, int num_samples) {
  ensure_prepared(prepared_, "LayeredInstrument");
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) return;
  if (num_samples > max_block_size_) return;

  const int legs = std::min(num_channels, 2);
  float* scratch_left = scratch_.data();
  float* scratch_right = scratch_.data() + max_block_size_;
  float* scratch_chans[2] = {scratch_left, scratch_right};

  for (Layer& layer : layers_) {
    std::fill_n(scratch_left, num_samples, 0.0f);
    std::fill_n(scratch_right, num_samples, 0.0f);
    layer.instrument->process(scratch_chans, legs, num_samples);
    for (int i = 0; i < num_samples; ++i) {
      channels[0][i] += scratch_left[i] * layer.gain_left;
    }
    if (legs > 1) {
      for (int i = 0; i < num_samples; ++i) {
        channels[1][i] += scratch_right[i] * layer.gain_right;
      }
    }
  }
}

void LayeredInstrument::set_transport(const transport::TransportState& state) noexcept {
  for (Layer& layer : layers_) layer.instrument->set_transport(state);
}

void LayeredInstrument::on_control_sysex(const uint8_t* data, size_t size) noexcept {
  for (Layer& layer : layers_) layer.instrument->on_control_sysex(data, size);
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

}  // namespace sonare::midi
