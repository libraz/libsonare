#include "midi/synth/gs_layer.h"

#include <algorithm>
#include <cmath>

namespace sonare::midi::synth {

namespace {

/// CC send default-modulator depth (matches the player's CC91/93 scaling).
constexpr float kSendDepth = 0.2f;

int8_t clamp_offset(int8_t v) noexcept { return static_cast<int8_t>(std::clamp<int>(v, -64, 63)); }

/// GS part-parameter block nibble -> zero-based channel: block 0 = part 10
/// (channel 9), blocks 1..9 = parts 1..9 (0..8), blocks A..F = parts 11..16.
uint8_t gs_block_to_channel(uint8_t block) noexcept {
  if (block == 0) return 9;
  if (block <= 9) return static_cast<uint8_t>(block - 1);
  return block;
}

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
      out.kind = GsSysExKind::kUseForRhythm;
      out.channel = gs_block_to_channel(addr_mid & 0x0Fu);
      out.value = static_cast<uint8_t>(data[7] <= 2 ? data[7] : 1);
      return out;
    }
    // Per-part EFX on/off: 40 4x 22, data 00/01 (route the part through the
    // single insertion effect).
    if (addr_hi == 0x40 && (addr_mid & 0xF0u) == 0x40 && addr_lo == 0x22 && size >= 8) {
      out.kind = GsSysExKind::kEfxPartSwitch;
      out.channel = gs_block_to_channel(addr_mid & 0x0Fu);
      out.value = static_cast<uint8_t>(data[7] != 0 ? 1 : 0);
      return out;
    }
  }
  return out;
}

bool apply_gs_efx_sysex(GsEfx& efx, const uint8_t* data, size_t size) noexcept {
  if (data == nullptr || size < 4) return false;
  // Strip optional F0 ... F7 framing.
  if (data[0] == 0xF0) {
    ++data;
    --size;
  }
  if (size > 0 && data[size - 1] == 0xF7) --size;
  // Roland GS DT1 addressing the EFX block: 41 dd 42 12 40 03 lo <data...> sum.
  if (size < 9 || data[0] != 0x41 || data[2] != 0x42 || data[3] != 0x12) return false;
  if (data[4] != 0x40 || data[5] != 0x03) return false;
  if (!valid_roland_dt1_checksum(data, size)) return false;

  const uint8_t start_lo = data[6];
  uint8_t type_msb = static_cast<uint8_t>(efx.type >> 8);
  uint8_t type_lsb = static_cast<uint8_t>(efx.type & 0x7Fu);
  bool touched = false;
  // Data bytes run from index 7 to size-2 (size-1 is the checksum), each landing
  // on a consecutive EFX sub-address from start_lo. Reserved/out-of-range
  // offsets are ignored (preserved), never dropping the whole message.
  for (size_t i = 7; i + 1 < size; ++i) {
    const uint8_t value = static_cast<uint8_t>(data[i] & 0x7Fu);
    const unsigned addr = static_cast<unsigned>(start_lo) + static_cast<unsigned>(i - 7);
    switch (addr) {
      case 0x00:
        type_msb = value;
        touched = true;
        break;
      case 0x01:
        type_lsb = value;
        touched = true;
        break;
      case 0x17:
        efx.send_reverb = value;
        touched = true;
        break;
      case 0x18:
        efx.send_chorus = value;
        touched = true;
        break;
      case 0x19:
        efx.send_delay = value;
        touched = true;
        break;
      default:
        if (addr >= 0x03 && addr <= 0x16) {
          efx.params[addr - 0x03] = value;
          touched = true;
        }
        break;
    }
  }
  if (touched) {
    efx.type = static_cast<uint16_t>((static_cast<uint16_t>(type_msb) << 8) | type_lsb);
    efx.assigned = true;
  }
  return true;
}

std::string_view gs_efx_insert_name(uint16_t type) noexcept {
  // Intentionally partial: EFX types with an existing insert adapter are
  // mapped; everything else returns empty so the caller bypasses + logs (a
  // layer-3 promotion just adds a case + its param translation, no ABI change).
  // Type numbers are the GS EFX map (MSB << 8 | LSB, SC-88Pro).
  switch (type) {
    case 0x0100:  // Stereo-EQ
      return "eq.parametric";
    case 0x0110:  // Overdrive -> the full guitar amp model (crunch voicing).
    case 0x0111:  // Distortion -> the amp model on its high-gain voicing.
      return "saturation.ampSim";
    case 0x0120:  // Phaser
      return "effects.modulation.phaser";
    case 0x0121:  // Auto Wah -> the envelope-following resonant bandpass.
      return "effects.modulation.autoWah";
    case 0x0122:  // Rotary -> the dual-rotor Leslie model.
      return "effects.modulation.rotary";
    case 0x0123:  // Stereo Flanger
    case 0x0124:  // Step Flanger (a flanger variant -> the same insert)
      return "effects.modulation.flanger";
    case 0x0126:  // Auto Pan
      return "stereo.autoPan";
    case 0x0130:  // Compressor
      return "dynamics.compressor";
    case 0x0131:  // Limiter
      return "dynamics.limiter";
    case 0x0140:  // Hexa Chorus
    case 0x0142:  // Stereo Chorus
      return "effects.modulation.chorus";
    case 0x0150:  // Stereo Delay
    case 0x0151:  // Modulation Delay (delay with LFO -> the stereo delay insert)
      return "effects.delay.stereo";
    case 0x0160:  // 2-voice Pitch Shifter
    case 0x0161:  // Feedback Pitch Shifter (the feedback loop is not modelled)
      return "effects.modulation.pitchShifter";
    default:
      // Note: the SC-88Pro has no Ring Modulator in its 64-effect insertion set
      // (it arrived on later Sound Canvas models), so the ringModulator insert
      // has no GS type to bind to and stays reachable only via the generic API.
      return {};
  }
}

namespace {

/// Pitch-shifter parameter translation (SC-88Pro 2-voice / feedback pitch
/// shifter). Coarse Pitch is EFX PARAMETER 1 — a 64-centred semitone offset
/// over -24..+12 st; an untouched block reads 0, treated as unset -> 0 st so it
/// never becomes a -64 -> clamped -24 st two-octave drop. Effect Balance
/// (PARAMETER 16) maps the direct/effect mix to dry/wet, with 0 = unset -> the
/// insert's wet default. Output Level (PARAMETER 20) has no matching pitch-
/// shifter parameter, so it is not translated.
std::string gs_pitch_shift_json(const GsEfx& efx) {
  const int coarse = efx.params[0] & 0x7F;
  const float semitones =
      coarse == 0 ? 0.0f : std::clamp(static_cast<float>(coarse - 64), -24.0f, 12.0f);
  std::string out = "{\"semitones\":" + std::to_string(semitones);
  const unsigned balance = efx.params[15] & 0x7Fu;
  if (balance != 0) {
    out += ",\"dryWet\":" + std::to_string(static_cast<float>(balance) / 127.0f);
  }
  out += "}";
  return out;
}

}  // namespace

std::string gs_efx_insert_params(const GsEfx& efx) {
  // SC-88Pro Overdrive/Distortion parameter map (owner's manual appendix):
  // PARAMETER 1 (40 03 03) = OD Sel (Odrv/Dist — redundant here, the EFX type
  // already selects the family), PARAMETER 2 (40 03 04) = OD Drive (0..127),
  // PARAMETER 20 (40 03 16) = output Level (0..127). The basic OD/Dist has no
  // tone/EQ parameters (those live in the combined OD->EQ / GTR-Multi types);
  // the tone comes from the amp voicing itself.
  const float drive = static_cast<float>(efx.params[1] & 0x7Fu) / 127.0f;
  // Output Level -> levelDb. The GS default is unity (127), and an untouched
  // block reads 0; treat 0 as "unset -> 0 dB" (so a type+drive-only setup is
  // not silenced) and otherwise map the fraction to dB with a -24 dB floor.
  const auto level_db = [&]() -> std::string {
    const unsigned level = efx.params[19] & 0x7Fu;
    if (level == 0) return "";  // unset -> the insert's default (0 dB)
    const float db = std::max(-24.0f, 20.0f * std::log10(static_cast<float>(level) / 127.0f));
    return ",\"levelDb\":" + std::to_string(db);
  };
  switch (efx.type) {
    case 0x0110: {
      // Overdrive -> the amp model on its classic-crunch voicing (ampModel 0).
      // A light setting already breaks up (0.25 floor), the top reaches full
      // crunch. The amp's cab EQ is left on so the tone is amp-shaped.
      const float amp_drive = std::clamp(0.25f + 0.6f * drive, 0.0f, 1.0f);
      return "{\"ampModel\":0,\"drive\":" + std::to_string(amp_drive) + level_db() + "}";
    }
    case 0x0111: {
      // Distortion -> the amp model on its high-gain voicing (ampModel 2), which
      // saturates earlier and harder; a higher drive floor than the overdrive.
      const float amp_drive = std::clamp(0.45f + 0.55f * drive, 0.0f, 1.0f);
      return "{\"ampModel\":2,\"drive\":" + std::to_string(amp_drive) + level_db() + "}";
    }
    case 0x0160:  // 2-voice Pitch Shifter -> Coarse Pitch + Balance translated.
    case 0x0161:  // Feedback Pitch Shifter (feedback approximated as a plain shift).
      return gs_pitch_shift_json(efx);
    default:
      return "{}";
  }
}

namespace {

/// GS EQ gain byte -> dB. The GS EQ Low/Hi Gain is centred at 64 (0 dB) with a
/// +-12 dB range (values 0x34..0x4C). An untouched block reads 0, which is
/// treated as unset -> flat (0 dB), never a -64 -> clamped -12 dB cut.
float gs_eq_gain_db(uint8_t value) noexcept {
  const int v = value & 0x7F;
  if (v == 0) return 0.0f;  // unset -> flat
  return std::clamp(static_cast<float>(v - 64), -12.0f, 12.0f);
}

/// eq.parametric JSON: a low shelf (100 Hz) + high shelf (8 kHz) driven by the
/// GS EQ block's Low/Hi Gain — the composite guitar effects' tone control.
std::string gs_eq_block_json(float low_db, float high_db) {
  return "{\"band0.type\":1,\"band0.frequencyHz\":100,\"band0.gainDb\":" + std::to_string(low_db) +
         ",\"band1.type\":2,\"band1.frequencyHz\":8000,\"band1.gainDb\":" +
         std::to_string(high_db) + "}";
}

}  // namespace

std::vector<GsEfxStage> gs_efx_insert_chain(const GsEfx& efx) {
  // Composite guitar/bass multi effects (SC-88Pro MSB 04): a whole rig realised
  // as an insert chain in signal order. The block STRUCTURE and the type numbers
  // are faithful to the manual (GTR Multi 2 = 04 01 and Clean Gt Multi 2 = 04 04
  // are confirmed hex anchors that bracket the ordered block). The EQ Low/Hi
  // Gain (EFX PARAMETER 17/18, shared across the multi effects) is translated to
  // a real shelving EQ — the composite's true tone control. Every block now has
  // a matching insert (Wah / Auto-Wah realised by the wah / auto-wah inserts);
  // the compressor, chorus, delay and wah run at their defaults and the OD uses
  // a musical mid drive, pending confirmed per-block parameter positions
  // (documented approximation — only the shared EQ has a confirmed layout).
  const auto comp = [] { return GsEfxStage{"dynamics.compressor", "{}"}; };
  const auto od = [](bool bass) {
    return GsEfxStage{"saturation.ampSim", bass ? "{\"ampModel\":0,\"drive\":0.6,\"cabModel\":1}"
                                                : "{\"ampModel\":0,\"drive\":0.6}"};
  };
  const auto eq = [&efx] {
    return GsEfxStage{"eq.parametric", gs_eq_block_json(gs_eq_gain_db(efx.params[16]),
                                                        gs_eq_gain_db(efx.params[17]))};
  };
  const auto cf = [] { return GsEfxStage{"effects.modulation.chorus", "{}"}; };
  const auto delay = [] { return GsEfxStage{"effects.delay.stereo", "{}"}; };
  const auto wah = [] { return GsEfxStage{"effects.modulation.wah", "{}"}; };
  const auto autowah = [] { return GsEfxStage{"effects.modulation.autoWah", "{}"}; };
  switch (efx.type) {
    case 0x0400:  // GTR Multi 1: Cmp-OD-CF-Dly
      return {comp(), od(false), cf(), delay()};
    case 0x0401:  // GTR Multi 2: Cmp-OD-EQ-CF
      return {comp(), od(false), eq(), cf()};
    case 0x0402:  // GTR Multi 3: Wah-OD-CF-Dly
      return {wah(), od(false), cf(), delay()};
    case 0x0403:  // Clean GTR Multi 1: Cmp-EQ-CF-Dly (no OD block)
      return {comp(), eq(), cf(), delay()};
    case 0x0404:  // Clean GTR Multi 2: AW-EQ-CF-Dly (Auto-Wah at the front)
      return {autowah(), eq(), cf(), delay()};
    case 0x0405:  // Bass Multi: Cmp-OD-EQ-CF (the OD block on the bass cab)
      return {comp(), od(true), eq(), cf()};
    default: {
      // Single-effect types: a one-stage chain from the name/param mapping.
      const std::string_view name = gs_efx_insert_name(efx.type);
      if (name.empty()) return {};
      return {{std::string(name), gs_efx_insert_params(efx)}};
    }
  }
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
