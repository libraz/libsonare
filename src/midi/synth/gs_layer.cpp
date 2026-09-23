#include "midi/synth/gs_layer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <tuple>

#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_efx_convert.h"
#include "midi/synth/pitch.h"
#include "util/constants.h"

namespace sonare::midi::synth {

using ::sonare::constants::kCentsPerOctave;
using ::sonare::constants::kCentsPerSemitone;

namespace {

int8_t clamp_offset(int8_t v) noexcept { return static_cast<int8_t>(std::clamp<int>(v, -64, 63)); }

/// Vibrato rate, in cents of LFO frequency per step. Named because the static
/// edit and the wheel-scaled one both spend it and must not part company.
constexpr float kGsVibRateCentsPerStep = 25.0f;

/// The signed offset a part byte centred on 40 carries.
int8_t centred_offset(uint8_t value) noexcept {
  return clamp_offset(static_cast<int8_t>(static_cast<int>(value & 0x7Fu) - 64));
}

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

/// Element-wise, because std::array::operator== is not constexpr before C++20.
constexpr bool gs_efx_starts_at_power_on() noexcept {
  const GsEfx efx{};
  const std::array<uint8_t, 20> expected = gs_efx_power_on_params();
  for (size_t slot = 0; slot < expected.size(); ++slot) {
    if (efx.params[slot] != expected[slot]) return false;
  }
  return true;
}

static_assert(gs_efx_starts_at_power_on(),
              "GsEfx does not start in the state the EFX PARAMETER row calls its default");

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
  return std::exp2(kGsVibRateCentsPerStep * static_cast<float>(clamp_offset(offset)) / 1200.0f);
}

float gs_vib_depth_cents(int8_t offset) noexcept {
  return 3.0f * static_cast<float>(clamp_offset(offset));
}

float gs_mod_pitch_cents(uint8_t value) noexcept {
  return kCentsPerSemitone * static_cast<float>(centred_offset(value));
}

float gs_mod_cutoff_cents(uint8_t value) noexcept {
  return gs_cutoff_offset_cents(centred_offset(value));
}

float gs_mod_amp_fraction(uint8_t value) noexcept {
  // 100 % over the 64 steps below the centre, which puts 7F at +98.4 % — the
  // same asymmetry every centred GS byte has, and the one clamp_offset gives.
  return static_cast<float>(centred_offset(value)) / 64.0f;
}

float gs_mod_tva_depth(uint8_t value) noexcept {
  return static_cast<float>(value & 0x7Fu) / 127.0f;
}

float gs_mod_lfo_rate_cents(uint8_t value) noexcept {
  return kGsVibRateCentsPerStep * static_cast<float>(centred_offset(value));
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
  // Clamped to the row's own 28-58: the apply layer already drops anything
  // outside it, and the corpus does reach 6F at 40 00 05.
  return static_cast<float>(std::clamp(static_cast<int>(value & 0x7Fu), 0x28, 0x58) - 0x40) *
         kCentsPerSemitone;
}

float gs_pitch_offset_fine_cents(uint8_t value, uint8_t note) noexcept {
  // Clamped to the manual's aggregate 08-F8: the row bounds a nibble, which
  // admits words the parameter's own range excludes.
  const float hz =
      static_cast<float>(std::clamp(static_cast<int>(value), 0x08, 0xF8) - 0x80) * 0.1f;
  if (hz == 0.0f) return 0.0f;
  const float f0 = note_to_hz(note);
  // Bounded at the lowest key's own pitch: a full -12 Hz reaches zero and below
  // from note 7 down, and reaches under the keyboard from note 15 down. A bound
  // to keep the ratio positive rather than a modelled behaviour.
  return kCentsPerOctave * std::log2(std::max(f0 + hz, note_to_hz(uint8_t{0})) / f0);
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

GsDrumNoteParams gs_layer_drum_note_params(const GsDrumNoteParams& stored,
                                           const GsDrumNoteParams& live) noexcept {
  // Field by field behind the flag that owns it, so a parameter written on one
  // side only survives whichever side wrote it. Both sides are one struct, and
  // the per-parameter equivalence test walks the same list.
  GsDrumNoteParams out = stored;
  const uint16_t f = live.flags;
  if ((f & GsDrumNoteParams::kPitch) != 0) out.pitch_coarse = live.pitch_coarse;
  if ((f & GsDrumNoteParams::kLevel) != 0) out.level = live.level;
  if ((f & GsDrumNoteParams::kPan) != 0) out.pan = live.pan;
  if ((f & GsDrumNoteParams::kReverb) != 0) out.reverb = live.reverb;
  if ((f & GsDrumNoteParams::kChorus) != 0) out.chorus = live.chorus;
  if ((f & GsDrumNoteParams::kDelay) != 0) out.delay = live.delay;
  if ((f & GsDrumNoteParams::kPlayNote) != 0) out.play_note = live.play_note;
  if ((f & GsDrumNoteParams::kAssignGroup) != 0) out.assign_group = live.assign_group;
  if ((f & GsDrumNoteParams::kRxNoteOn) != 0) out.rx_note_on = live.rx_note_on;
  out.flags = static_cast<uint16_t>(stored.flags | f);
  return out;
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
  // ASSIGN GROUP replaces the kit's own exclusive class rather than adding to
  // it, so 00 takes a note out of every group. The caller chokes on the value
  // this leaves behind, which is why it is set before the choke and not after.
  if ((drum.flags & GsDrumNoteParams::kAssignGroup) != 0) {
    params.exclusive_class = drum.assign_group & 0x7Fu;
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
    out.kind = body[3] == 0x01 ? GsSysExKind::kGm1Reset : GsSysExKind::kGm2Reset;
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
    case GsParam::kPartEfxAssign: {
      // The raw assignment: 00 bypass, 01 unit 0, 02-10 units 1-15 (docs/gs.md).
      // Carried verbatim because gs_efx_assign_unit is what turns it into a
      // unit, and a value collapsed to a switch here cannot be recovered. A
      // value the row does not accept is ignored rather than read as some unit,
      // which is the rule every address but USE FOR RHYTHM PART follows.
      const GsAddressEntry* entry = gs_lookup_address(write.addr);
      if (entry != nullptr && gs_value_in_range(*entry, write.value)) {
        out.kind = GsSysExKind::kEfxPartSwitch;
        out.channel = write.part;
        out.value = write.value;
      }
      break;
    }
    default:
      break;
  }
  return out;
}

int gs_efx_addressed_unit(const uint8_t* data, size_t size) noexcept {
  if (data == nullptr || size < 4) return -1;
  const GsFrame frame = gs_sysex_frame(data, size);
  if (!frame.valid || frame.model != kGsModelId || frame.command != kGsCommandDt1) return -1;
  const uint32_t block = frame.addr & 0xFFFF00u;
  if (block == 0x400300u) return 0;
  // The extension numbers a unit by its own address nibble, so 40 30 xx is
  // unit 0 and writes the storage 40 03 xx writes (docs/gs.md).
  if ((block & 0xFFF000u) == 0x403000u) return static_cast<int>((block >> 8) & 0x0Fu);
  return -1;
}

bool apply_gs_efx_sysex(GsEfx& efx, const uint8_t* data, size_t size,
                        bool* out_type_changed) noexcept {
  if (out_type_changed != nullptr) *out_type_changed = false;
  if (data == nullptr || size < 4) return false;

  const GsFrame frame = gs_sysex_frame(data, size);
  if (!frame.valid || frame.model != kGsModelId || frame.command != kGsCommandDt1) return false;
  // An EFX block: the spec one at 40 03 xx, or an extension unit at 40 3u xx.
  // Which unit @p efx is is the caller's to have resolved (gs_efx_addressed_unit);
  // a run starting anywhere else belongs to another parameter group.
  if (gs_efx_addressed_unit(data, size) < 0) return false;

  std::array<GsWrite, kGsEfxBlockSize> writes{};
  const size_t decoded =
      std::min(gs_decode_writes(frame, writes.data(), writes.size(), nullptr), writes.size());

  const uint16_t old_type = efx.type;
  bool touched = false;
  // A byte landing on a reserved offset, or on a block address GsEfx holds no
  // field for, is ignored (preserved) rather than dropping the whole message.
  // Bytes are applied in address order, which is what lets a type's defaults
  // land before the parameters that follow it in the same message.
  for (size_t i = 0; i < decoded; ++i) {
    const GsWrite& write = writes[i];
    switch (write.param) {
      case GsParam::kEfxType:
        if (write.index == 0) {
          // The MSB alone resolves nothing; it waits for its LSB.
          efx.type_msb = write.value;
        } else {
          efx.type = static_cast<uint16_t>((static_cast<uint16_t>(efx.type_msb) << 8) |
                                           (write.value & 0x7Fu));
          // The block is the type's, whole. A type the archive never measured
          // has nothing to pour and leaves it as it stood.
          const GsEfxTypeDefaults* defaults = gs_efx_type_defaults(efx.type);
          if (defaults != nullptr) efx.params = defaults->params;
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
  if (touched) efx.assigned = true;
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
      // A single-effect type is refused when its identity is carried by DSP
      // this tree does not have:
      //   0x0103 Humanizer: a vowel formant filter whose identity IS the vowel.
      //     The vowel has a parameter position of its own — slot 2, a five-value
      //     enumeration — so what is missing is the formant filter to receive
      //     it. A fixed vowel would be a strong resonant filter chosen at random.
      //   0x0170 3D Auto / 0x0171 3D Manual: binaural panners, no stock insert.
      //     3D Chorus (0x0144) and 3D Delay (0x0157) map because their 3D stage
      //     sits on an effect that exists; here the 3D stage IS the effect.
      // The SC-88Pro has no standalone Ring Modulator type; that DSP is reached
      // as Tremolo (0x0125, amplitude modulation) and inside Keyboard Multi.
      return {};
  }
}

namespace {

/// One insert's params object, built key by key. A key is written only where a
/// conversion produced it, so a slot the archive gives no conversion to leaves
/// its insert on its own default rather than on a value nothing measured.
class ParamsJson {
 public:
  void number(const char* key, float value) { append(key, std::to_string(value)); }
  void integer(const char* key, int value) { append(key, std::to_string(value)); }
  std::string str() const { return out_.empty() ? "{}" : out_ + "}"; }

 private:
  void append(const char* key, const std::string& value) {
    out_ += out_.empty() ? '{' : ',';
    out_ += '"';
    out_ += key;
    out_ += "\":";
    out_ += value;
  }

  std::string out_;
};

/// The wire byte at a slot of the EFX parameter block.
uint8_t efx_byte(const GsEfx& efx, int slot) noexcept {
  return static_cast<uint8_t>(efx.params[static_cast<size_t>(slot)] & 0x7Fu);
}

/// An output-level byte as a gain in dB over a -24 dB floor. The multiplier is
/// the unit's own measured table rather than the byte over 127, which reads a
/// median 1.61 dB high (gs_efx_level_mul).
float gs_efx_level_db(uint8_t value) noexcept {
  // std::max returns its first argument for the silent byte's -inf.
  return std::max(-24.0f, 20.0f * std::log10(gs_efx_level_mul(value)));
}

/// eq.parametric band-type selectors, in the order processor_params.h decodes.
constexpr int kEqBandPeak = 0;
constexpr int kEqBandLowShelf = 1;
constexpr int kEqBandHighShelf = 2;

/// Writes one band's type. What the band reads -- corner, centre, width, gain --
/// is a bound byte or a fixed corner the caller writes beside it.
void append_band(ParamsJson& out, int band, int type) {
  out.integer(("band" + std::to_string(band) + ".type").c_str(), type);
}

/// Writes one shelf's type and order. Its corner and gain are bound bytes.
void append_shelf(ParamsJson& out, int band, int type) {
  append_band(out, band, type);
  // The insert's slope is 6 dB/oct a pole, which is how it selects the order.
  out.integer(("band" + std::to_string(band) + ".slopeDbOct").c_str(), 6 * kGsEfxShelfOrder);
}

/// Writes one shelf on a corner no byte selects. The gain is a bound byte.
void append_fixed_shelf(ParamsJson& out, int band, int type, float corner_hz) {
  append_shelf(out, band, type);
  out.number(("band" + std::to_string(band) + ".frequencyHz").c_str(), corner_hz);
}

/// Stereo-EQ (0x0100): a low shelf, two peaking sections, a high shelf. Only
/// the band shapes are the skeleton's; every byte the four bands read is bound,
/// the two corner bytes included.
std::string gs_stereo_eq_json() {
  ParamsJson out;
  append_shelf(out, 0, kEqBandLowShelf);
  append_band(out, 1, kEqBandPeak);
  append_band(out, 2, kEqBandPeak);
  append_shelf(out, 3, kEqBandHighShelf);
  return out.str();
}

/// The wet depth the tremolo voicing runs at. The ring modulator's dry and wet
/// terms multiply the same input, so the pair collapses to one envelope whose
/// minimum is 1 - 2*wet: 0.35 is a ~10 dB depth with a unity peak.
constexpr float kGsTremoloDryWet = 0.35f;

/// Tremolo as sinusoidal amplitude modulation: dry*x + wet*x*sin = x*(dry +
/// wet*sin), so the ring modulator realises the type exactly. The carrier is
/// the modulator's rate byte, which is bound.
std::string gs_tremolo_json() {
  ParamsJson out;
  out.number("dryWet", kGsTremoloDryWet);
  return out.str();
}

/// Amp-sim JSON for the two drive types. Drive is EFX PARAMETER 1 and the byte
/// beside it picks one of four fixed response curves whose values the archive
/// does not hold, so the voicing's own curve stands and only the drive is
/// translated. The output level is not this block's: every type carries it at
/// the same slot and the output stage reads it once, for all of them.
std::string gs_drive_json(const GsEfx& efx, int amp_model, float drive_floor, float drive_span) {
  const float drive = static_cast<float>(efx_byte(efx, 0)) / 127.0f;
  ParamsJson out;
  out.integer("ampModel", amp_model);
  out.number("drive", std::clamp(drive_floor + drive_span * drive, 0.0f, 1.0f));
  return out.str();
}

/// Pitch-shifter mix (SC-88Pro 2-voice / feedback pitch shifter). Effect Balance
/// (PARAMETER 16) maps the direct/effect mix to dry/wet; no measurement reaches
/// this type's balance byte, so that mapping is the older linear reading rather
/// than the measured two-ramp law. The byte does not read 0 as "unset":
/// selecting the type loads its own defaults, so a zero here is a zero the file
/// asked for. Coarse Pitch is bound.
std::string gs_pitch_shift_json(const GsEfx& efx) {
  ParamsJson out;
  out.number("dryWet", static_cast<float>(efx_byte(efx, 15)) / 127.0f);
  return out.str();
}

}  // namespace

std::string gs_efx_insert_params(const GsEfx& efx) {
  // Every byte an archive law reaches is written by the binding table; what is
  // left here is the skeleton's own: shapes, modes, and the bytes it reads
  // under a law of its own.
  switch (efx.type) {
    case 0x0100:  // Stereo-EQ -> four bands of the parametric EQ.
      return gs_stereo_eq_json();
    case 0x0110:
      // Overdrive -> the amp model on its classic-crunch voicing (ampModel 0).
      // A light setting already breaks up (0.25 floor), the top reaches full
      // crunch. The amp's cab EQ is left on so the tone is amp-shaped.
      return gs_drive_json(efx, 0, 0.25f, 0.6f);
    case 0x0111:
      // Distortion -> the amp model on its high-gain voicing (ampModel 2), which
      // saturates earlier and harder; a higher drive floor than the overdrive.
      return gs_drive_json(efx, 2, 0.45f, 0.55f);
    case 0x0123:  // Stereo Flanger: its pre-filter was read to be the chorus's section.
    case 0x0142: {
      // Stereo Chorus. The section in front of the delay is one pole whose shape
      // the type byte picks. Three of the byte's states were measured — 0 flat,
      // 1 low pass, 2 high pass — and PreFilterMode is declared in that order. A
      // fourth state exists and was not asked, so it writes no key and the
      // section stays out of the path rather than taking whichever shape a cast
      // would land on.
      ParamsJson out;
      if (efx_byte(efx, 0) <= 2) out.integer("preFilterMode", efx_byte(efx, 0));
      return out.str();
    }
    case 0x0125:  // Tremolo -> the ring modulator at its fixed depth.
      return gs_tremolo_json();
    case 0x0160:  // 2-voice Pitch Shifter
    case 0x0161:  // Feedback Pitch Shifter (feedback approximated as a plain shift).
      return gs_pitch_shift_json(efx);
    default:
      return "{}";
  }
}

namespace {

/// The binding rows for one type. kGsEfxBindings is sorted by (type, slot, key),
/// so a type's rows are one contiguous run.
struct BindingRun {
  const GsEfxBinding* first;
  const GsEfxBinding* last;
};

BindingRun run_of(uint16_t type) noexcept {
  const auto begin = kGsEfxBindings.begin();
  const auto end = kGsEfxBindings.end();
  const auto lower = std::lower_bound(
      begin, end, type, [](const GsEfxBinding& row, uint16_t key) { return row.type < key; });
  const auto upper = std::upper_bound(
      lower, end, type, [](uint16_t key, const GsEfxBinding& row) { return key < row.type; });
  return {kGsEfxBindings.data() + (lower - begin), kGsEfxBindings.data() + (upper - begin)};
}

BindingRun bindings_for(uint16_t type) noexcept {
  const BindingRun run = run_of(type);
  // The binding files spell Rotary Multi one way and the defaults table the
  // other, so a lookup that found nothing has one more spelling to try before
  // reporting that the type binds nothing.
  if (run.first != run.last) return run;
  const uint16_t alias = gs_efx_alias_type(type);
  return alias == type ? run : run_of(alias);
}

/// Writes one bound byte as the quantity its conversion law names. Returns false
/// for a law with no reader here, so a row that binds nothing is a row this
/// refuses rather than one it silently drops on the insert's default.
bool write_bound(ParamsJson& out, const char* key, const GsEfxBinding& row, uint8_t byte) {
  switch (row.conversion_class) {
    case kGsEfxClassRate:
      out.number(key,
                 gs_efx_rate_hz(byte, row.table == 1 ? GsRateRange::kWide : GsRateRange::kNarrow));
      return true;
    case kGsEfxClassDelayTime: {
      constexpr std::array<GsTimeLadder, 5> kLadders = {
          GsTimeLadder::kLadder0, GsTimeLadder::kLadder1, GsTimeLadder::kLadder2,
          GsTimeLadder::kLadder3, GsTimeLadder::kLadder4};
      if (row.table >= kLadders.size()) return false;
      out.number(key, gs_efx_delay_ms(byte, kLadders[row.table]));
      return true;
    }
    case kGsEfxClassFreq: {
      constexpr std::array<GsFreqColumn, 3> kColumns = {
          GsFreqColumn::kColumn0, GsFreqColumn::kColumn1, GsFreqColumn::kColumn2};
      if (row.table >= kColumns.size()) return false;
      out.number(key, gs_efx_freq_hz(byte, kColumns[row.table]));
      return true;
    }
    case kGsEfxClassGain:
      out.number(key, gs_efx_gain_db(byte));
      return true;
    case kGsEfxClassLevel:
      out.number(key, gs_efx_level_db(byte));
      return true;
    case kGsEfxClassWidth:
      out.number(key, gs_efx_width_q(byte));
      return true;
    case kGsEfxClassAccel: {
      // One divisor table, two quantities, and the control's own suffix says
      // which: the undershoot is a frequency, the time constant is a time.
      const std::string_view name(key);
      const bool hertz = name.size() > 2 && name.substr(name.size() - 2) == "Hz";
      out.number(key, hertz ? gs_efx_accel_undershoot_hz(byte) : gs_efx_accel_tau_s(byte));
      return true;
    }
    case kGsEfxClassPostGain:
      out.number(key, gs_efx_post_gain_db(byte));
      return true;
    case kGsEfxClassWindow:
      out.number(key, gs_efx_window_ms(byte));
      return true;
    case kGsEfxClassCorner:
      out.number(key,
                 gs_efx_corner_hz(byte, row.table == 1 ? GsShelfSide::kHigh : GsShelfSide::kLow));
      return true;
    case kGsEfxClassRatio: {
      if (row.range >= kGsEfxBindingRanges.size()) return false;
      const GsEfxBindingRange& ends = kGsEfxBindingRanges[row.range];
      float units = 0.0f;
      if (!gs_efx_ratio(byte, ends.lo_byte, ends.hi_byte, ends.lo_unit, ends.hi_unit, &units)) {
        return false;
      }
      // Table 0 is printed in percent and the controls take the fraction;
      // table 1 is printed in semitones, which is what they take.
      out.number(key, row.table == 0 ? units / 100.0f : units);
      return true;
    }
    default:
      return false;
  }
}

/// The constants a stage the bindings alone bring into being needs beside its
/// bound controls. A stage the skeleton builds carries its own.
void write_output_stage_constants(ParamsJson& out, std::string_view stage) {
  if (stage != "eq.parametric") return;
  // The module applies one tone pair after the effect, whatever the effect is:
  // two first-order shelves on fixed corners, each taking one gain byte.
  append_fixed_shelf(out, 0, kEqBandLowShelf, kGsEfxOutputToneLowHz);
  append_fixed_shelf(out, 1, kEqBandHighShelf, kGsEfxOutputToneHighHz);
}

/// Whether a params object already carries @p key. Keys are plain identifiers
/// with no escaping, so the spelling a writer produces is the spelling to look
/// for.
bool carries_key(const std::string& params, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":";
  return params.find(needle) != std::string::npos;
}

/// Adds one key to a finished params object, which is either "{}" or a closed
/// brace to re-open.
void merge_key(std::string& params, const std::string& addition) {
  if (params == "{}") {
    params = "{" + addition + "}";
    return;
  }
  params.pop_back();  // the closing brace
  params += "," + addition + "}";
}

/// Writes every control the binding table gives this type, into the stage the
/// row names -- appending the stage where the effect chain has none.
///
/// The skeleton writes shapes, modes and the bytes it reads under a law of its
/// own, never a bound control, so no row finds its key already written; the
/// check keeps a collision from rendering one key twice.
///
/// This is also what realises the unit's output stage. The tone pair, the pan
/// and the output level sit at the same slots for every type rather than inside
/// any one effect, which is why nineteen inserts do not each carry a copy of
/// them: the module puts one stage after the effect and the table says so.
void apply_bindings(std::vector<GsEfxStage>& chain, const GsEfx& efx) {
  const BindingRun run = bindings_for(efx.type);
  // Rows arrive in slot order, which is the unit's own: an appended stage lands
  // where the unit puts it -- tone pair, pan, level.
  for (const GsEfxBinding* row = run.first; row != run.last; ++row) {
    const std::string_view stage = kGsEfxBindingStages[row->stage];
    const std::string_view key = kGsEfxBindingKeys[row->key];

    auto found = std::find_if(chain.begin(), chain.end(),
                              [stage](const GsEfxStage& s) { return s.name == stage; });
    if (found == chain.end()) {
      ParamsJson constants;
      write_output_stage_constants(constants, stage);
      chain.push_back({std::string(stage), constants.str()});
      found = std::prev(chain.end());
    }
    if (carries_key(found->params_json, key)) continue;

    ParamsJson one;
    if (!write_bound(one, std::string(key).c_str(), *row, efx_byte(efx, row->slot))) continue;
    const std::string rendered = one.str();
    // ParamsJson closes itself, so unwrap the single pair it just wrote.
    merge_key(found->params_json, rendered.substr(1, rendered.size() - 2));
  }
}

/// The stages that realise the effect itself, before the unit's output stage.
std::vector<GsEfxStage> gs_efx_effect_chain(const GsEfx& efx) {
  // Composite guitar/bass multi effects (SC-88Pro MSB 04): a whole rig realised
  // as an insert chain in signal order. The block STRUCTURE and the type numbers
  // are faithful to the manual (GTR Multi 2 = 04 01 and Clean Gt Multi 2 = 04 04
  // are confirmed hex anchors that bracket the ordered block). Every block has a
  // matching insert (Wah / Auto-Wah realised by the wah / auto-wah inserts).
  //
  // A composite's parameter block is laid out per type, not per block, so a slot
  // number carries no meaning until the type says which block owns it. That
  // layout is the binding table's: each row names its stage, so the blocks
  // here carry only what the skeleton owns.
  const auto comp = [] { return GsEfxStage{"dynamics.compressor", "{}"}; };
  const auto od = [](bool bass) {
    return GsEfxStage{"saturation.ampSim", bass ? "{\"ampModel\":0,\"drive\":0.6,\"cabModel\":1}"
                                                : "{\"ampModel\":0,\"drive\":0.6}"};
  };
  // Every combination type's equaliser: a low shelf, one peaking section, a
  // high shelf. It prints two gains and no corner, so the shelves sit on the
  // pair the archive measured on one such type; every byte the bands read is bound.
  const auto eq = [] {
    ParamsJson out;
    append_fixed_shelf(out, 0, kEqBandLowShelf, kGsEfxCombinationEqLowHz);
    append_band(out, 1, kEqBandPeak);
    append_fixed_shelf(out, 2, kEqBandHighShelf, kGsEfxCombinationEqHighHz);
    return GsEfxStage{"eq.parametric", out.str()};
  };
  const auto cf = [] { return GsEfxStage{"effects.modulation.chorus", "{}"}; };
  const auto delay = [] { return GsEfxStage{"effects.delay.stereo", "{}"}; };
  const auto wah = [] { return GsEfxStage{"effects.modulation.wah", "{}"}; };
  const auto autowah = [] { return GsEfxStage{"effects.modulation.autoWah", "{}"}; };
  const auto ds = [] { return GsEfxStage{"saturation.ampSim", "{\"ampModel\":2,\"drive\":0.7}"}; };
  const auto eh = [] { return GsEfxStage{"spectral.presenceEnhancer", "{}"}; };
  const auto fl = [] { return GsEfxStage{"effects.modulation.flanger", "{}"}; };
  const auto rot = [] { return GsEfxStage{"effects.modulation.rotary", "{}"}; };
  const auto ph = [] { return GsEfxStage{"effects.modulation.phaser", "{}"}; };
  const auto pan = [] { return GsEfxStage{"stereo.autoPan", "{}"}; };
  const auto rm = [] { return GsEfxStage{"effects.modulation.ringModulator", "{}"}; };
  const auto ps = [] { return GsEfxStage{"effects.modulation.pitchShifter", "{}"}; };
  const auto trem = [] {
    return GsEfxStage{"effects.modulation.ringModulator", gs_tremolo_json()};
  };
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
      return {od(false), eq(), rot()};
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
      return {rm(), eq(), ps(), ph(), delay()};
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

}  // namespace

std::vector<GsEfxStage> gs_efx_insert_chain(const GsEfx& efx) {
  std::vector<GsEfxStage> chain = gs_efx_effect_chain(efx);
  // An unmapped type bypasses whole: appending an output stage to nothing would
  // put a gain where the caller logged that it plays the part dry.
  if (!chain.empty()) apply_bindings(chain, efx);
  return chain;
}

std::string_view gs_drum_kit_name(uint8_t program, GsToneMap map) noexcept {
  const GsDrumKit* kit = gs_drum_kit_entry(program, map);
  return kit != nullptr ? kit->name : std::string_view{};
}

}  // namespace sonare::midi::synth
