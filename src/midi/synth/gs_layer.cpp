#include "midi/synth/gs_layer.h"

#include <algorithm>
#include <cmath>

namespace sonare::midi::synth {

namespace {

/// CC send default-modulator depth (matches the player's CC91/93 scaling).
constexpr float kSendDepth = 0.2f;

int8_t clamp_offset(int8_t v) noexcept { return static_cast<int8_t>(std::clamp<int>(v, -64, 63)); }

bool valid_roland_dt1_checksum(const uint8_t* data, size_t size) noexcept {
  // Framing has already been stripped. Roland DT1 payload:
  // 41 dd 42 12 aa bb cc <data...> sum
  if (data == nullptr || size < 9) return false;
  uint32_t sum = 0;
  for (size_t i = 4; i + 1 < size; ++i) {
    sum += data[i];
  }
  const uint8_t expected = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return data[size - 1] == expected;
}

}  // namespace

float gs_cutoff_offset_cents(int8_t offset) noexcept {
  return 150.0f * static_cast<float>(clamp_offset(offset));
}

float gs_resonance_gain(int8_t offset) noexcept {
  // 3 cB per step: gain = 10^(3*offset/200).
  return std::pow(10.0f, 3.0f * static_cast<float>(clamp_offset(offset)) / 200.0f);
}

float gs_time_scale(int8_t offset) noexcept {
  // 75 timecents per step: scale = 2^(75*offset/1200).
  return std::exp2(75.0f * static_cast<float>(clamp_offset(offset)) / 1200.0f);
}

float gs_vib_rate_scale(int8_t offset) noexcept {
  return std::exp2(25.0f * static_cast<float>(clamp_offset(offset)) / 1200.0f);
}

float gs_vib_depth_cents(int8_t offset) noexcept {
  return 3.0f * static_cast<float>(clamp_offset(offset));
}

void apply_gs_part_params(Sf2VoiceParams& params, const GsPartParams& gs) noexcept {
  if (!gs.any()) return;
  if (gs.tvf_cutoff != 0) {
    params.filter_fc_cents += gs_cutoff_offset_cents(gs.tvf_cutoff);
    params.filter_bypass = false;  // an edited filter is always engaged
  }
  if (gs.tvf_resonance != 0) {
    params.filter_q = std::max(0.5f, params.filter_q * gs_resonance_gain(gs.tvf_resonance));
    params.filter_bypass = false;
  }
  if (gs.eg_attack != 0) params.volume_env.attack_ms *= gs_time_scale(gs.eg_attack);
  if (gs.eg_decay != 0) params.volume_env.decay_ms *= gs_time_scale(gs.eg_decay);
  if (gs.eg_release != 0) params.volume_env.release_ms *= gs_time_scale(gs.eg_release);
  if (gs.vibrato_rate != 0) params.vib_lfo_freq_hz *= gs_vib_rate_scale(gs.vibrato_rate);
  if (gs.vibrato_depth != 0) {
    params.vib_lfo_to_pitch =
        std::max(0.0f, params.vib_lfo_to_pitch + gs_vib_depth_cents(gs.vibrato_depth));
  }
  if (gs.vibrato_delay != 0) {
    // Positive offset lengthens the onset delay (same 75 tc/step scale).
    params.vib_lfo_delay_s *= gs_time_scale(gs.vibrato_delay);
    if (params.vib_lfo_delay_s < 1.0e-3f && gs.vibrato_delay > 0) {
      // A zero base delay still gains an audible onset when pushed up.
      params.vib_lfo_delay_s = 0.05f * (gs_time_scale(gs.vibrato_delay) - 1.0f);
    }
  }
}

void apply_gs_drum_params(Sf2VoiceParams& params, const GsDrumNoteParams& drum) noexcept {
  if (!drum.any()) return;
  if ((drum.flags & GsDrumNoteParams::kPitch) != 0 && drum.pitch_coarse != 0) {
    params.pitch_increment *= std::exp2(static_cast<double>(drum.pitch_coarse) / 12.0);
  }
  if ((drum.flags & GsDrumNoteParams::kLevel) != 0) {
    const float v = static_cast<float>(drum.level & 0x7Fu) / 127.0f;
    params.attenuation_gain *= v * v;  // same square law as CC7/velocity
  }
  if ((drum.flags & GsDrumNoteParams::kPan) != 0) {
    params.pan_units = (static_cast<float>(drum.pan & 0x7Fu) - 64.0f) / 63.0f * 500.0f;
  }
  if ((drum.flags & GsDrumNoteParams::kReverb) != 0) {
    params.reverb_send = std::min(
        1.0f, params.reverb_send + kSendDepth * static_cast<float>(drum.reverb & 0x7Fu) / 127.0f);
  }
  if ((drum.flags & GsDrumNoteParams::kChorus) != 0) {
    params.chorus_send = std::min(
        1.0f, params.chorus_send + kSendDepth * static_cast<float>(drum.chorus & 0x7Fu) / 127.0f);
  }
}

GsSysEx parse_gs_sysex(const uint8_t* data, size_t size) noexcept {
  GsSysEx out;
  if (data == nullptr || size < 4) return out;
  // Strip optional F0 ... F7 framing.
  if (data[0] == 0xF0) {
    ++data;
    --size;
  }
  if (size > 0 && data[size - 1] == 0xF7) --size;
  if (size < 3) return out;

  // GM System On / GM2 System On: 7E 7F 09 01 / 03.
  if (size >= 4 && data[0] == 0x7E && data[2] == 0x09 && (data[3] == 0x01 || data[3] == 0x03)) {
    out.kind = GsSysExKind::kGmReset;
    return out;
  }

  // Roland GS DT1: 41 dd 42 12 aa bb cc <data...> sum.
  if (size >= 9 && data[0] == 0x41 && data[2] == 0x42 && data[3] == 0x12) {
    if (!valid_roland_dt1_checksum(data, size)) return out;
    const uint8_t addr_hi = data[4];
    const uint8_t addr_mid = data[5];
    const uint8_t addr_lo = data[6];
    // GS Reset: 40 00 7F, data 00.
    if (addr_hi == 0x40 && addr_mid == 0x00 && addr_lo == 0x7F && data[7] == 0x00) {
      out.kind = GsSysExKind::kGsReset;
      return out;
    }
    // Use for rhythm part: 40 1x 15, data mm (0 off / 1 map1 / 2 map2).
    if (addr_hi == 0x40 && (addr_mid & 0xF0u) == 0x10 && addr_lo == 0x15 && size >= 8) {
      const uint8_t block = addr_mid & 0x0Fu;
      // GS block -> channel: block 0 = part 10 (channel index 9), blocks
      // 1..9 = parts 1..9 (indices 0..8), blocks A..F = parts 11..16.
      uint8_t channel = 0;
      if (block == 0) {
        channel = 9;
      } else if (block <= 9) {
        channel = static_cast<uint8_t>(block - 1);
      } else {
        channel = block;
      }
      out.kind = GsSysExKind::kUseForRhythm;
      out.channel = channel;
      out.value = static_cast<uint8_t>(data[7] <= 2 ? data[7] : 1);
      return out;
    }
  }
  return out;
}

std::string_view gs_drum_kit_name(uint8_t program) noexcept {
  switch (program) {
    case 0:
      return "Standard";
    case 8:
      return "Room";
    case 16:
      return "Power";
    case 24:
      return "Electronic";
    case 25:
      return "TR-808";
    case 32:
      return "Jazz";
    case 40:
      return "Brush";
    case 48:
      return "Orchestra";
    case 56:
      return "SFX";
    default:
      return {};
  }
}

}  // namespace sonare::midi::synth
