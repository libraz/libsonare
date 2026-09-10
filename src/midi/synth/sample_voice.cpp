#include "midi/synth/sample_voice.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::midi::synth {

using sonare::constants::kCentsPerOctave;
using sonare::constants::kCentsPerSemitone;

bool SampleVoiceCore::start_layer(Layer& layer, const SampleZone& zone, const SamplePatchParams& p,
                                  double sample_rate, uint8_t note, float weight) noexcept {
  SampleRegion region = zone.region;
  if (p.loop_override >= 0) {
    region.loop_mode =
        (p.loop_override == 1 || p.loop_override == 3) && region.loop_end > region.loop_start
            ? p.loop_override
            : 0;
  }

  const double span = static_cast<double>(region.end) - static_cast<double>(region.start);
  const double offset = std::clamp(static_cast<double>(p.start_offset01), 0.0, 0.999) * span;
  layer.reader.start(bank_->pool(), region, offset);
  if (!layer.reader.valid()) return false;

  const float key_cents =
      p.key_track
          ? kCentsPerSemitone * (static_cast<float>(note) - static_cast<float>(zone.root_key))
          : 0.0f;
  const float cents = key_cents + zone.tune_cents;
  const double rate_ratio = zone.source_rate > 0.0 ? zone.source_rate / sample_rate : 1.0;
  layer.increment = rate_ratio * std::exp2(static_cast<double>(cents) / kCentsPerOctave);

  layer.gain = zone.gain * p.level * weight;
  layer.finished = false;
  return true;
}

bool SampleVoiceCore::start(const SamplePatchParams& p, double sample_rate, uint8_t note,
                            uint8_t velocity) noexcept {
  // Cleared before the lookup so a failed start cannot leave a reused slot
  // placed and levelled by the note that last played through it.
  finished_ = true;
  layer_count_ = 0;
  pan_units_ = 0.0f;
  for (Layer& layer : layers_) layer.finished = true;
  if (bank_ == nullptr || sample_rate <= 0.0) return false;

  const SampleZoneMix mix = bank_->find_mix(p.set_index, note, velocity);
  if (mix.low == nullptr) return false;

  // A weight that reached an end of its travel wants one layer, not two at a
  // gain of zero: the silent one would still step its region and end the voice
  // when it ran out.
  const float high_weight = mix.high != nullptr ? mix.high_weight : 0.0f;
  if (mix.high == nullptr || high_weight <= 0.0f) {
    if (!start_layer(layers_[0], *mix.low, p, sample_rate, note, 1.0f)) return false;
    pan_units_ = mix.low->pan_units;
    layer_count_ = 1;
  } else if (high_weight >= 1.0f) {
    if (!start_layer(layers_[0], *mix.high, p, sample_rate, note, 1.0f)) return false;
    pan_units_ = mix.high->pan_units;
    layer_count_ = 1;
  } else {
    const bool low_ok = start_layer(layers_[0], *mix.low, p, sample_rate, note, 1.0f - high_weight);
    const bool high_ok = start_layer(layers_[1], *mix.high, p, sample_rate, note, high_weight);
    if (low_ok && high_ok) {
      pan_units_ = mix.low->pan_units * (1.0f - high_weight) + mix.high->pan_units * high_weight;
      layer_count_ = 2;
    } else if (low_ok || high_ok) {
      // A partner that cannot be read must not take its share of the level with
      // it, or the note lands quieter than the velocities on either side of the
      // overlap. The survivor stands in for the whole note. The bank validates
      // a zone's region as it is added, so this is a guard rather than a path.
      const SampleZone& survivor = low_ok ? *mix.low : *mix.high;
      if (!start_layer(layers_[0], survivor, p, sample_rate, note, 1.0f)) return false;
      pan_units_ = survivor.pan_units;
      layer_count_ = 1;
    } else {
      return false;
    }
  }

  finished_ = false;
  return true;
}

float SampleVoiceCore::render(float pitch_ratio, bool key_down) noexcept {
  if (finished_) return 0.0f;

  float out = 0.0f;
  bool sounding = false;
  for (int i = 0; i < layer_count_; ++i) {
    Layer& layer = layers_[i];
    if (layer.finished || !layer.reader.valid()) continue;
    const bool looping = layer.reader.looping(key_down);
    if (!layer.reader.wrap(looping)) {
      layer.finished = true;
      continue;
    }
    out += layer.reader.read(looping) * layer.gain;
    layer.reader.advance(layer.increment * static_cast<double>(pitch_ratio));
    sounding = true;
  }
  // The voice ends with its LAST layer: a crossfade pair whose two regions are
  // different lengths would otherwise be cut to the shorter one.
  if (!sounding) {
    finished_ = true;
    return 0.0f;
  }
  return out;
}

}  // namespace sonare::midi::synth
