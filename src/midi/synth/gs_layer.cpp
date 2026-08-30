#include "midi/synth/gs_layer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

#include "midi/synth/gs_address_table.h"
#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

int8_t clamp_offset(int8_t v) noexcept { return static_cast<int8_t>(std::clamp<int>(v, -64, 63)); }

/// The EFX block is 40 03 00-1F, so a run reaching past its 0x20th byte carries
/// only addresses outside the block.
constexpr size_t kGsEfxBlockSize = 0x20;

/// The size the EFX PARAMETER row claims, so the table and GsEfx::params cannot
/// fall out of step.
constexpr uint8_t gs_efx_parameter_row_size() noexcept {
  for (const GsAddressEntry& entry : kGsAddressTable) {
    if (entry.param == GsParam::kEfxParameter) return entry.size;
  }
  return 0;
}

static_assert(gs_efx_parameter_row_size() == std::tuple_size<decltype(GsEfx::params)>::value,
              "EFX PARAMETER row and GsEfx::params disagree on the parameter count");

/// The size the TONE MODIFY row claims, for the same reason.
constexpr uint8_t gs_tone_modify_row_size() noexcept {
  for (const GsAddressEntry& entry : kGsAddressTable) {
    if (entry.param == GsParam::kPartToneModify) return entry.size;
  }
  return 0;
}

static_assert(gs_tone_modify_row_size() == kGsToneModifyCount,
              "TONE MODIFY row and GsPartParams disagree on the parameter count");

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

float gs_master_tune_cents(const GsMasterParams& master) noexcept {
  uint32_t word = 0;
  for (const uint8_t nibble : master.tune) word = (word << 4) | (nibble & 0x0Fu);
  return (static_cast<float>(word) - 0x0400) * 0.1f;
}

float gs_master_volume_gain(uint8_t value) noexcept {
  const float v = static_cast<float>(value & 0x7Fu) / 127.0f;
  return v * v;
}

float gs_key_shift_cents(uint8_t value) noexcept {
  using ::sonare::constants::kCentsPerSemitone;
  // Clamped to the row's own 28-58: the apply layer already drops anything
  // outside it, and the corpus does reach 6F at 40 00 05.
  return static_cast<float>(std::clamp(static_cast<int>(value & 0x7Fu), 0x28, 0x58) - 0x40) *
         kCentsPerSemitone;
}

void gs_master_pan_gains(uint8_t value, float* left, float* right) noexcept {
  // 01-7F reads as -63..+63 around 40. Attenuating only the far leg keeps the
  // centre at exactly 1 on both, which is what makes an untouched render
  // bit-identical; a constant-power law would move it by 3 dB.
  // Clamped because 00 is below the row's range and would otherwise come out as
  // a small NEGATIVE right gain, which is a phase flip rather than a pan. The
  // apply layer drops the value before it arrives, so this is a second line.
  const float balance =
      std::clamp((static_cast<float>(value & 0x7Fu) - 64.0f) / 63.0f, -1.0f, 1.0f);
  if (left != nullptr) *left = balance > 0.0f ? 1.0f - balance : 1.0f;
  if (right != nullptr) *right = balance < 0.0f ? 1.0f + balance : 1.0f;
}

void gs_apply_tone_modify(GsPartParams& gs, uint8_t index, uint8_t value) noexcept {
  const int8_t offset = static_cast<int8_t>(static_cast<int>(value & 0x7Fu) - 64);
  // The manual names each of the eight and the NRPN it shares, so the order is
  // the address order and not a choice made here.
  switch (index) {
    case 0:
      gs.vibrato_rate = offset;  // NRPN 01 08
      break;
    case 1:
      gs.vibrato_depth = offset;  // NRPN 01 09
      break;
    case 2:
      gs.tvf_cutoff = offset;  // NRPN 01 20
      break;
    case 3:
      gs.tvf_resonance = offset;  // NRPN 01 21
      break;
    case 4:
      gs.eg_attack = offset;  // NRPN 01 63
      break;
    case 5:
      gs.eg_decay = offset;  // NRPN 01 64
      break;
    case 6:
      gs.eg_release = offset;  // NRPN 01 66
      break;
    case 7:
      gs.vibrato_delay = offset;  // NRPN 01 0A
      break;
    default:
      break;
  }
}

GsPartMod gs_part_mod(const GsPartParams& gs) noexcept {
  GsPartMod mod;
  if (gs.tvf_cutoff != 0) {
    mod.cutoff_cents = gs_cutoff_offset_cents(gs.tvf_cutoff);
    mod.filter_edited = true;
  }
  if (gs.tvf_resonance != 0) {
    mod.resonance_gain = gs_resonance_gain(gs.tvf_resonance);
    mod.filter_edited = true;
  }
  if (gs.eg_attack != 0) mod.attack_scale = gs_time_scale(gs.eg_attack);
  if (gs.eg_decay != 0) mod.decay_scale = gs_time_scale(gs.eg_decay);
  if (gs.eg_release != 0) mod.release_scale = gs_time_scale(gs.eg_release);
  if (gs.vibrato_rate != 0) mod.vib_rate_scale = gs_vib_rate_scale(gs.vibrato_rate);
  if (gs.vibrato_depth != 0) mod.vib_depth_cents = gs_vib_depth_cents(gs.vibrato_depth);
  // Positive offset lengthens the onset delay (same 75 tc/step scale).
  if (gs.vibrato_delay != 0) mod.vib_delay_scale = gs_time_scale(gs.vibrato_delay);
  return mod;
}

float gs_vib_delay_seconds(float base_s, float scale) noexcept {
  const float delay_s = base_s * scale;
  // A zero base delay still gains an audible onset when pushed up.
  if (delay_s < 1.0e-3f && scale > 1.0f) return 0.05f * (scale - 1.0f);
  return delay_s;
}

void apply_gs_part_params(Sf2VoiceParams& params, const GsPartParams& gs) noexcept {
  if (!gs.any()) return;
  const GsPartMod mod = gs_part_mod(gs);
  if (mod.cutoff_cents != 0.0f) params.filter_fc_cents += mod.cutoff_cents;
  if (mod.resonance_gain != 1.0f) {
    params.filter_q = std::max(0.5f, params.filter_q * mod.resonance_gain);
  }
  if (mod.filter_edited) params.filter_bypass = false;  // an edited filter is always engaged
  params.volume_env.attack_ms *= mod.attack_scale;
  params.volume_env.decay_ms *= mod.decay_scale;
  params.volume_env.release_ms *= mod.release_scale;
  params.vib_lfo_freq_hz *= mod.vib_rate_scale;
  if (mod.vib_depth_cents != 0.0f) {
    params.vib_lfo_to_pitch = std::max(0.0f, params.vib_lfo_to_pitch + mod.vib_depth_cents);
  }
  if (mod.vib_delay_scale != 1.0f) {
    params.vib_lfo_delay_s = gs_vib_delay_seconds(params.vib_lfo_delay_s, mod.vib_delay_scale);
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
  // A drum note's sends MULTIPLY what the note sends into that unit, they do not
  // add to it (docs/gs.md; the manual calls the field a multiplicand over
  // 0.0-1.0). The scale is carried to the render, which applies it to the zone's
  // send and the part's together.
  if ((drum.flags & GsDrumNoteParams::kReverb) != 0) {
    params.reverb_send_scale *= static_cast<float>(drum.reverb & 0x7Fu) / 127.0f;
  }
  if ((drum.flags & GsDrumNoteParams::kChorus) != 0) {
    params.chorus_send_scale *= static_cast<float>(drum.chorus & 0x7Fu) / 127.0f;
  }
  if ((drum.flags & GsDrumNoteParams::kDelay) != 0) {
    params.delay_send_scale *= static_cast<float>(drum.delay & 0x7Fu) / 127.0f;
  }
}

GsSysEx parse_gs_sysex(const uint8_t* data, size_t size) noexcept {
  GsSysEx out;
  if (data == nullptr || size < 4) return out;

  // GM System On / GM2 System On is Universal SysEx rather than a Roland frame,
  // so it is matched ahead of the address table: 7E dd 09 01 / 03.
  const uint8_t* body = data;
  size_t body_size = size;
  if (body[0] == 0xF0) {
    ++body;
    --body_size;
  }
  if (body_size > 0 && body[body_size - 1] == 0xF7) --body_size;
  if (body_size >= 4 && body[0] == 0x7E && body[2] == 0x09 &&
      (body[3] == 0x01 || body[3] == 0x03)) {
    out.kind = GsSysExKind::kGmReset;
    return out;
  }

  // Everything else is a Roland frame the address table names. The kind comes
  // from the FIRST data byte only: the message's start address is what a caller
  // selects on, so a run that reaches one of these addresses partway through is
  // not one of these messages.
  GsWrite write;
  if (gs_decode_sysex(data, size, &write, 1, nullptr) == 0) return out;

  switch (write.param) {
    case GsParam::kModeSet:
    case GsParam::kSystemModeSet: {
      // Both reset on value 00 and on nothing else. SYSTEM MODE SET reaches the
      // same place because the target has no Mode-2 (docs/gs.md): its row is
      // lo = hi = 00, so an SC-88Pro Mode-2 request falls outside and is
      // ignored rather than resetting or clamping.
      const GsAddressEntry* entry = gs_lookup_address(write.addr);
      if (entry != nullptr && gs_value_in_range(*entry, write.value)) {
        out.kind = GsSysExKind::kGsReset;
      }
      break;
    }
    case GsParam::kUseForRhythmPart:
      out.kind = GsSysExKind::kUseForRhythm;
      out.channel = write.part;
      // 0 off / 1 map1 / 2 map2; an unmapped value reads as map 1 rather than
      // being ignored, which is wider than the row's range.
      out.value = static_cast<uint8_t>(write.value <= 2 ? write.value : 1);
      break;
    case GsParam::kPartEfxAssign:
      out.kind = GsSysExKind::kEfxPartSwitch;
      out.channel = write.part;
      // Any non-zero assignment routes the part through insertion unit 0. The
      // row accepts 02-10 for units 1-15 (docs/gs.md); nothing realises them,
      // so they read as unit 0 rather than as a part with no effect.
      out.value = static_cast<uint8_t>(write.value != 0 ? 1 : 0);
      break;
    default:
      break;
  }
  return out;
}

bool apply_gs_efx_sysex(GsEfx& efx, const uint8_t* data, size_t size,
                        bool* out_type_changed) noexcept {
  if (out_type_changed != nullptr) *out_type_changed = false;
  if (data == nullptr || size < 4) return false;

  const GsFrame frame = gs_sysex_frame(data, size);
  if (!frame.valid || frame.model != kGsModelId || frame.command != kGsCommandDt1) return false;
  // Insertion unit 0, address 40 03 xx. A run starting anywhere else belongs to
  // another parameter group.
  if ((frame.addr & 0xFFFF00u) != 0x400300u) return false;

  std::array<GsWrite, kGsEfxBlockSize> writes{};
  const size_t decoded =
      std::min(gs_decode_writes(frame, writes.data(), writes.size(), nullptr), writes.size());

  const uint16_t old_type = efx.type;
  uint8_t type_msb = static_cast<uint8_t>(efx.type >> 8);
  uint8_t type_lsb = static_cast<uint8_t>(efx.type & 0x7Fu);
  bool touched = false;
  // A byte landing on a reserved offset, or on a block address GsEfx holds no
  // field for, is ignored (preserved) rather than dropping the whole message.
  for (size_t i = 0; i < decoded; ++i) {
    const GsWrite& write = writes[i];
    switch (write.param) {
      case GsParam::kEfxType:
        if (write.index == 0) {
          type_msb = write.value;
        } else {
          type_lsb = write.value;
        }
        touched = true;
        break;
      case GsParam::kEfxParameter:
        efx.params[write.index] = write.value;
        touched = true;
        break;
      case GsParam::kEfxSendToReverb:
        efx.send_reverb = write.value;
        touched = true;
        break;
      case GsParam::kEfxSendToChorus:
        efx.send_chorus = write.value;
        touched = true;
        break;
      case GsParam::kEfxSendToDelay:
        efx.send_delay = write.value;
        touched = true;
        break;
      default:
        break;
    }
  }
  if (touched) {
    efx.type = static_cast<uint16_t>((static_cast<uint16_t>(type_msb) << 8) | type_lsb);
    efx.assigned = true;
  }
  // A type change restructures the insert chain (the caller rebuilds); a write
  // that leaves the type value untouched is a parameter/send-only edit the
  // caller can apply to the live processors in place.
  if (out_type_changed != nullptr) *out_type_changed = touched && efx.type != old_type;
  return touched;
}

std::string_view gs_efx_insert_name(uint16_t type) noexcept {
  // Intentionally partial: EFX types with an existing insert adapter are
  // mapped; everything else returns empty so the caller bypasses + logs (a
  // layer-3 promotion just adds a case + its param translation, no ABI change).
  // Type numbers are the GS EFX map (MSB << 8 | LSB, SC-88Pro).
  switch (type) {
    case 0x0100:  // Stereo-EQ
      return "eq.parametric";
    case 0x0101:  // Spectrum -> the multi-band graphic EQ.
      return "eq.graphic";
    case 0x0102:  // Enhancer -> the high-overtone presence enhancer.
      return "spectral.presenceEnhancer";
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
    case 0x0125:  // Tremolo -> the ring modulator driven as amplitude modulation.
      return "effects.modulation.ringModulator";
    case 0x0126:  // Auto Pan
      return "stereo.autoPan";
    case 0x0130:  // Compressor
      return "dynamics.compressor";
    case 0x0131:  // Limiter
      return "dynamics.limiter";
    case 0x0140:  // Hexa Chorus -> the six-voice ensemble (its richer voicing).
      return "effects.modulation.ensemble";
    case 0x0141:  // Tremolo Chorus -> the chorus block; its tremolo is a chain stage.
    case 0x0142:  // Stereo Chorus
    case 0x0143:  // Space-D (an unmodulated stereo chorus)
    case 0x0144:  // 3D Chorus (the widened chorus, without the binaural stage)
      return "effects.modulation.chorus";
    case 0x0150:  // Stereo Delay
    case 0x0151:  // Modulation Delay (delay with LFO -> the stereo delay insert)
    case 0x0152:  // 3-tap Delay
    case 0x0153:  // 4-tap Delay
    case 0x0154:  // Time Control Delay (all multi-tap variants -> the stereo delay)
    case 0x0157:  // 3D Delay (without the binaural stage -> the stereo delay)
      return "effects.delay.stereo";
    case 0x0155:  // Reverb (per-part insertion reverb)
    case 0x0156:  // Gate Reverb (approximated by the plate reverb; no gate stage yet)
      return "effects.reverb.dattorro";
    case 0x0160:  // 2-voice Pitch Shifter
    case 0x0161:  // Feedback Pitch Shifter (the feedback loop is not modelled)
      return "effects.modulation.pitchShifter";
    case 0x0172:  // Lo-Fi 1
    case 0x0173:  // Lo-Fi 2 -> the bit-depth / sample-rate reducer.
      return "saturation.bitcrusher";
    default:
      // A single-effect type is refused when its identity is carried by
      // something this tree cannot supply — DSP that does not exist, or a
      // parameter whose position the transcribed manual does not give:
      //   0x0103 Humanizer: a vowel formant filter whose identity IS the vowel,
      //     and no parameter position for the vowel is transcribed. A fixed
      //     vowel would be a strong resonant filter chosen at random.
      //   0x0170 3D Auto / 0x0171 3D Manual: binaural panners, no stock insert.
      //     3D Chorus (0x0144) and 3D Delay (0x0157) map because their 3D stage
      //     sits on an effect that exists; here the 3D stage IS the effect.
      // The SC-88Pro has no standalone Ring Modulator type; that DSP is reached
      // as Tremolo (0x0125, amplitude modulation) and inside Keyboard Multi.
      return {};
  }
}

namespace {

/// Tremolo voicing, shared by the standalone type (0x0125) and the Tremolo
/// Chorus chain (0x0141) so the two cannot drift: ~5 Hz at a ~10 dB depth.
constexpr const char* kGsTremoloJson = "{\"carrierHz\":5.0,\"dryWet\":0.35}";

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
    case 0x0125:
      // Tremolo. The ring modulator computes dry*x + wet*x*sin = x*(dry + wet*
      // sin), which IS sinusoidal amplitude modulation, so the type is realised
      // exactly rather than approximated; wet 0.35 is a ~10 dB depth. Rate and
      // depth read no EFX parameter: the Effect list that gives the Tremolo's
      // parameter positions is not transcribed (see gs_efx_insert_chain).
      return kGsTremoloJson;
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
  // Series-2 / multi building blocks. Sub-block parameters stay at their insert
  // defaults for now (the mapping is the coverage deliverable; per-block param
  // translation is follow-up work); only the shared guitar-multi EQ, above,
  // reads a confirmed layout.
  const auto ds = [] { return GsEfxStage{"saturation.ampSim", "{\"ampModel\":2,\"drive\":0.7}"}; };
  const auto eh = [] { return GsEfxStage{"spectral.presenceEnhancer", "{}"}; };
  const auto fl = [] { return GsEfxStage{"effects.modulation.flanger", "{}"}; };
  const auto rot = [] { return GsEfxStage{"effects.modulation.rotary", "{}"}; };
  const auto ph = [] { return GsEfxStage{"effects.modulation.phaser", "{}"}; };
  const auto pan = [] { return GsEfxStage{"stereo.autoPan", "{}"}; };
  const auto rm = [] { return GsEfxStage{"effects.modulation.ringModulator", "{}"}; };
  const auto ps = [] { return GsEfxStage{"effects.modulation.pitchShifter", "{}"}; };
  const auto eq3 = [] { return GsEfxStage{"eq.parametric", "{}"}; };
  const auto trem = [] { return GsEfxStage{"effects.modulation.ringModulator", kGsTremoloJson}; };
  switch (efx.type) {
    case 0x0141:  // Tremolo Chorus: the chorus with its output amplitude-modulated.
      return {cf(), trem()};
    // Series-2 composites (SC-88Pro MSB 02): two stock effects in signal order.
    case 0x0200:  // OD -> Chorus
      return {od(false), cf()};
    case 0x0201:  // OD -> Flanger
      return {od(false), fl()};
    case 0x0202:  // OD -> Delay
      return {od(false), delay()};
    case 0x0203:  // DS -> Chorus
      return {ds(), cf()};
    case 0x0204:  // DS -> Flanger
      return {ds(), fl()};
    case 0x0205:  // DS -> Delay
      return {ds(), delay()};
    case 0x0206:  // EH -> Chorus
      return {eh(), cf()};
    case 0x0207:  // EH -> Flanger
      return {eh(), fl()};
    case 0x0208:  // EH -> Delay
      return {eh(), delay()};
    case 0x0209:  // Cho -> Delay
      return {cf(), delay()};
    case 0x020A:  // FL -> Delay
      return {fl(), delay()};
    case 0x020B:  // Cho -> Flanger
      return {cf(), fl()};
    // Rotary Multi: OD -> 3-band EQ -> Rotary. The manual prints two type numbers
    // for it (chapter-4 body 03 00 vs appendix table 02 0C); accept both.
    case 0x020C:
    case 0x0300:
      return {od(false), eq3(), rot()};
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
    case 0x0406:  // Rhodes Multi: Enhancer -> Phaser -> Chorus -> Tremolo/Pan
      return {eh(), ph(), cf(), pan()};
    case 0x0500:  // Keyboard Multi: Ring Mod -> EQ -> Pitch Shifter -> Phaser -> Delay.
                  // The only GS type that binds the ring-modulator insert.
      return {rm(), eq3(), ps(), ph(), delay()};
    default: {
      // Single-effect types: a one-stage chain from the name/param mapping.
      // The parallel-2 types (MSB 11) fall through here to the empty chain and
      // stay there. They split the signal into two effects and sum them, which
      // a series the realiser runs in order cannot express; folding one into a
      // series would deliver a different effect under the right type name, and
      // unlike a bypass — which the caller logs — that is invisible.
      const std::string_view name = gs_efx_insert_name(efx.type);
      if (name.empty()) return {};
      return {{std::string(name), gs_efx_insert_params(efx)}};
    }
  }
}

std::string_view gs_drum_kit_name(uint8_t program, GsToneMap map) noexcept {
  const GsDrumKit* kit = gs_drum_kit_entry(program, map);
  return kit != nullptr ? kit->name : std::string_view{};
}

}  // namespace sonare::midi::synth
