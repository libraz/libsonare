#include "midi/synth/gs_layer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <string_view>
#include <tuple>
#include <utility>

#include "effects/common/mix_law.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_efx_convert.h"
#include "midi/synth/pitch.h"
#include "midi/sysex_framing.h"
#include "rt/biquad_design.h"
#include "util/constants.h"

namespace sonare::midi::synth {

using ::sonare::constants::kCentsPerOctave;
using ::sonare::constants::kCentsPerSemitone;

namespace {

int8_t clamp_offset(int8_t v) noexcept {
  return static_cast<int8_t>(std::clamp<int>(v, kGsPartOffsetMin, kGsPartOffsetMax));
}

/// Vibrato rate, in cents of LFO frequency per step. Named because the static
/// edit and the wheel-scaled one both spend it and must not part company.
constexpr float kGsVibRateCentsPerStep = 25.0f;

/// The signed offset a part byte centred on 40 carries.
int8_t centred_offset(uint8_t value) noexcept {
  return clamp_offset(static_cast<int8_t>(static_cast<int>(value & 0x7Fu) - 64));
}

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

/// Whether GsEfx powers on holding the control-assignment rows' own defaults.
constexpr bool gs_efx_controls_start_at_power_on() noexcept {
  const GsEfx efx{};
  for (const GsAddressEntry& entry : kGsAddressTable) {
    uint8_t held = entry.def;
    if (entry.param == GsParam::kEfxControlSource1) held = efx.control_source[0];
    if (entry.param == GsParam::kEfxControlDepth1) held = efx.control_depth[0];
    if (entry.param == GsParam::kEfxControlSource2) held = efx.control_source[1];
    if (entry.param == GsParam::kEfxControlDepth2) held = efx.control_depth[1];
    if (held != entry.def) return false;
  }
  return true;
}

static_assert(gs_efx_controls_start_at_power_on(),
              "GsEfx does not start in the state the EFX CONTROL rows call their default");

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

bool gs_apply_tone_modify_cc(GsPartParams& gs, uint8_t controller, uint8_t value) noexcept {
  if (controller < 71 || controller > 78) return false;
  // The eight controllers are contiguous but not in address order.
  static constexpr uint8_t kToneModifyIndex[8] = {3, 6, 4, 2, 5, 0, 1, 7};
  gs_apply_tone_modify(gs, kToneModifyIndex[controller - 71u], value);
  return true;
}

bool gs_apply_part_nrpn(GsPartParams& gs, uint8_t msb, uint8_t lsb, uint8_t value) noexcept {
  if ((msb & 0x7Fu) != 0x01u) return false;
  switch (lsb & 0x7Fu) {
    case 0x08:
      gs_apply_tone_modify(gs, 0, value);
      return true;
    case 0x09:
      gs_apply_tone_modify(gs, 1, value);
      return true;
    case 0x0A:
      gs_apply_tone_modify(gs, 7, value);
      return true;
    case 0x20:
      gs_apply_tone_modify(gs, 2, value);
      return true;
    case 0x21:
      gs_apply_tone_modify(gs, 3, value);
      return true;
    case 0x63:
      gs_apply_tone_modify(gs, 4, value);
      return true;
    case 0x64:
      gs_apply_tone_modify(gs, 5, value);
      return true;
    case 0x66:
      gs_apply_tone_modify(gs, 6, value);
      return true;
    default:
      return false;
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
  const SysExBody framed = sysex_body(data, size);
  const uint8_t* body = framed.data;
  const size_t body_size = framed.size;
  const bool body_is_7bit = body_size == 4 && (body[0] & 0x80u) == 0 && (body[1] & 0x80u) == 0 &&
                            (body[2] & 0x80u) == 0 && (body[3] & 0x80u) == 0;
  if (body_is_7bit && body[0] == 0x7E && body[2] == 0x09 && (body[3] == 0x01 || body[3] == 0x03)) {
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

bool gs_efx_assign_accepted(const GsWrite& write) noexcept {
  const GsAddressEntry* entry = gs_lookup_address(write.addr);
  return entry != nullptr && entry->param == GsParam::kPartEfxAssign &&
         gs_value_in_range(*entry, write.value);
}

bool apply_gs_efx_assign_sysex(std::array<uint8_t, 16>* assignments, const uint8_t* data,
                               size_t size) noexcept {
  // Not classified through parse_gs_sysex: the assignment row may be several
  // bytes after the run's start, and the walk reaches every byte of a dump.
  return gs_for_each_sysex_write(data, size, [&](const GsWrite& write) noexcept {
    if (!gs_efx_assign_accepted(write)) return false;
    if (assignments != nullptr) (*assignments)[write.part & 0x0Fu] = write.value;
    return true;
  });
}

namespace {

int efx_unit_for_block_address(uint32_t addr) noexcept {
  const uint32_t block = addr & 0xFFFF00u;
  if (block == 0x400300u) return 0;
  // 40 30 is the uniform alias for unit 0; 40 31-3F are units 1-15.
  if ((block & 0xFFF000u) == 0x403000u) {
    return static_cast<int>((block >> 8) & 0x0Fu);
  }
  return -1;
}

}  // namespace

bool gs_next_efx_block_slice(const GsFrame& frame, size_t* cursor, GsEfxBlockSlice* out) noexcept {
  if (cursor == nullptr || out == nullptr || !frame.valid || frame.data == nullptr ||
      frame.command != kGsCommandDt1 || frame.model != kGsModelId) {
    return false;
  }
  size_t offset = *cursor;
  if (offset >= frame.len) return false;
  while (offset < frame.len) {
    const uint32_t addr = gs_address_offset(frame.addr, static_cast<uint32_t>(offset));
    const int unit = efx_unit_for_block_address(addr);
    const size_t block_offset = static_cast<size_t>(addr & 0x7Fu);
    if (unit >= 0 && block_offset < kGsEfxBlockSize) {
      const size_t count = std::min(frame.len - offset, kGsEfxBlockSize - block_offset);
      GsFrame slice = frame;
      slice.addr = addr;
      slice.data = frame.data + offset;
      slice.len = count;
      out->unit = static_cast<uint8_t>(unit);
      out->frame = slice;
      *cursor = offset + count;
      return true;
    }
    ++offset;
  }
  *cursor = frame.len;
  return false;
}

namespace {

bool apply_gs_efx_frame(GsEfx& efx, const GsFrame& frame, bool* out_type_changed) noexcept {
  if (out_type_changed != nullptr) *out_type_changed = false;
  if (!frame.valid || frame.data == nullptr || frame.model != kGsModelId ||
      frame.command != kGsCommandDt1) {
    return false;
  }
  const size_t block_length = gs_efx_block_write_count(frame.addr, frame.len);
  if (block_length == 0) return false;
  GsFrame bounded_frame = frame;
  bounded_frame.len = block_length;
  std::array<GsWrite, kGsEfxBlockSize> writes{};
  const size_t decoded = std::min(
      gs_decode_writes(bounded_frame, writes.data(), writes.size(), nullptr), writes.size());

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
          // The MSB alone resolves nothing; it waits for the LSB.
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
        if (gs_efx_parameter_takes(efx.type, static_cast<uint8_t>(write.index), write.value)) {
          efx.params[write.index] = write.value;
        }
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
      case GsParam::kEfxControlSource1:
        efx.control_source[0] = write.value;
        touched = true;
        break;
      case GsParam::kEfxControlDepth1:
        efx.control_depth[0] = write.value;
        touched = true;
        break;
      case GsParam::kEfxControlSource2:
        efx.control_source[1] = write.value;
        touched = true;
        break;
      case GsParam::kEfxControlDepth2:
        efx.control_depth[1] = write.value;
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

}  // namespace

uint32_t gs_efx_units_in_sysex(const uint8_t* data, size_t size) noexcept {
  const GsFrame frame = gs_sysex_frame(data, size);
  if (!frame.valid || frame.model != kGsModelId || frame.command != kGsCommandDt1) return 0;
  uint32_t mask = 0;
  size_t cursor = 0;
  GsEfxBlockSlice slice;
  while (gs_next_efx_block_slice(frame, &cursor, &slice)) {
    GsEfx probe{};
    if (apply_gs_efx_frame(probe, slice.frame, nullptr)) {
      mask |= uint32_t{1} << slice.unit;
    }
  }
  return mask;
}

bool apply_gs_efx_units_sysex(std::array<GsEfx, kGsEfxUnitCount>& efx, const uint8_t* data,
                              size_t size, uint32_t* out_type_changed) noexcept {
  if (out_type_changed != nullptr) *out_type_changed = 0;
  const GsFrame frame = gs_sysex_frame(data, size);
  if (!frame.valid || frame.model != kGsModelId || frame.command != kGsCommandDt1) return false;

  bool touched = false;
  size_t cursor = 0;
  GsEfxBlockSlice slice;
  while (gs_next_efx_block_slice(frame, &cursor, &slice)) {
    bool type_changed = false;
    const bool slice_touched = apply_gs_efx_frame(efx[slice.unit], slice.frame, &type_changed);
    touched |= slice_touched;
    if (type_changed && out_type_changed != nullptr) {
      *out_type_changed |= uint32_t{1} << slice.unit;
    }
  }
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
    case 0x0103:  // Humanizer -> the vowel formant filter.
      return "effects.filter.vowel";
    case 0x0110:  // Overdrive -> the overdrive pedal; its amp stages follow it.
      return "saturation.overdrive";
    case 0x0111:  // Distortion -> the distortion pedal; its amp stages follow it.
      return "saturation.distortion";
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
    case 0x0144:  // 3D Chorus (the chorus; its binaural stage is a chain stage)
      return "effects.modulation.chorus";
    case 0x0150:  // Stereo Delay
    case 0x0151:  // Modulation Delay (delay with LFO -> the stereo delay insert)
    case 0x0152:  // 3-tap Delay
    case 0x0153:  // 4-tap Delay
    case 0x0154:  // Time Control Delay (all multi-tap variants -> the stereo delay)
    case 0x0157:  // 3D Delay (the stereo delay; its binaural stage is a chain stage)
      return "effects.delay.stereo";
    case 0x0155:  // Reverb (per-part insertion reverb)
    case 0x0156:  // Gate Reverb (approximated by the plate reverb; no gate stage yet)
      return "effects.reverb.dattorro";
    case 0x0160:  // 2-voice Pitch Shifter
    case 0x0161:  // Feedback Pitch Shifter (the feedback loop is not modelled)
      return "effects.modulation.pitchShifter";
    case 0x0170:  // 3D Auto
    case 0x0171:  // 3D Manual -> the binaural panner, which is the whole effect.
      return "stereo.binaural";
    case 0x0172:  // Lo-Fi 1
    case 0x0173:  // Lo-Fi 2 -> the bit-depth / sample-rate reducer.
      return "saturation.bitcrusher";
    default:
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
  void text(const char* key, const char* value) { append(key, '"' + std::string(value) + '"'); }
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

/// The wet depth Tremolo Chorus's tremolo runs at, a type that prints no depth
/// byte. The ring modulator's dry and wet terms multiply the same input, so the
/// pair collapses to one envelope whose minimum is 1 - 2*wet: 0.35 is a ~10 dB
/// depth with a unity peak.
constexpr float kGsTremoloDryWet = 0.35f;

/// Tremolo as sinusoidal amplitude modulation: dry*x + wet*x*sin = x*(dry +
/// wet*sin), so the ring modulator realises the type exactly. The carrier is
/// the modulator's rate byte, which is bound.
std::string gs_tremolo_json() {
  ParamsJson out;
  out.number("dryWet", kGsTremoloDryWet);
  return out.str();
}

}  // namespace

std::string gs_efx_insert_params(const GsEfx& efx) {
  // Every printed byte is written by the binding table; what is left here is
  // the skeleton's own: shapes and voicings no byte reaches.
  switch (efx.type) {
    case 0x0100:  // Stereo-EQ -> four bands of the parametric EQ.
      return gs_stereo_eq_json();
    default:
      return "{}";
  }
}

uint16_t gs_efx_binding_type(const GsEfxRowView& view, uint16_t type) noexcept {
  for (size_t i = 0; i < view.n_rows; ++i) {
    if (view.rows[i].type == type) return type;
  }
  for (size_t i = 0; i < view.n_enables; ++i) {
    if (view.enables[i].type == type) return type;
  }
  return gs_efx_alias_type(type);
}

namespace {

/// Writes one bound byte as the value the row's own law gives it.
void write_bound(ParamsJson& out, const char* key, const GsEfxBindingRow& row, uint8_t byte) {
  out.number(key, gs_efx_binding_value(row, byte));
}

/// The stage named @p name at @p ordinal, or end.
std::vector<GsEfxStage>::iterator find_stage(std::vector<GsEfxStage>& chain, std::string_view name,
                                             uint8_t ordinal) {
  return std::find_if(chain.begin(), chain.end(), [name, ordinal](const GsEfxStage& s) {
    return s.ordinal == ordinal && s.name == name;
  });
}

/// How many stages of the chain carry @p name.
uint8_t count_named(const std::vector<GsEfxStage>& chain, std::string_view name) {
  return static_cast<uint8_t>(std::count_if(
      chain.begin(), chain.end(), [name](const GsEfxStage& s) { return s.name == name; }));
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

/// Removes @p key and its value from a params object; the skeleton writes only
/// flat numeric values, so a value ends at the next comma or closing brace.
void drop_key(std::string& params, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":";
  const std::size_t at = params.find(needle);
  if (at == std::string::npos) return;
  std::size_t end = params.find_first_of(",}", at + needle.size());
  if (params[end] == ',') {
    ++end;
  } else if (at > 1 && params[at - 1] == ',') {
    params.erase(at - 1, end - (at - 1));
    return;
  }
  params.erase(at, end - at);
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

}  // namespace

void gs_efx_fit_to_rate(std::vector<GsEfxStage>& chain, double sample_rate) {
  if (!(sample_rate > 0.0)) return;
  const float ceiling = rt::max_design_frequency_hz(sample_rate);
  static constexpr std::string_view kSuffix = ".frequencyHz\":";
  for (GsEfxStage& stage : chain) {
    if (stage.name != "eq.parametric") continue;
    std::string& params = stage.params_json;
    for (std::size_t at = params.find(kSuffix); at != std::string::npos;
         at = params.find(kSuffix, at + kSuffix.size())) {
      const std::size_t value_start = at + kSuffix.size();
      const std::size_t value_end = params.find_first_of(",}", value_start);
      if (std::strtod(params.c_str() + value_start, nullptr) <= ceiling) continue;
      params.replace(value_start, value_end - value_start, std::to_string(ceiling));
    }
  }
}

namespace {

/// Selects the modern method of each stage whose insert offers one: cubic
/// Lagrange delay reads, the pitch shifter's pre-write anti-aliasing, the
/// geometric rotary, octave-linear wah sweeps and ADAA on the enhancer's
/// shaper. The inserts' own defaults stay today's behaviour; only this chain
/// asks for the newer one, and it writes integer method keys and no constants.
void write_method_keys(std::vector<GsEfxStage>& chain) {
  constexpr int kLagrange3 = 1;        // DelayInterpolation::kLagrange3
  constexpr int kGeometricRotary = 1;  // RotaryModel, the two-microphone model
  constexpr int kOctaveSweep = 1;      // WahSweepLaw::kLinearOctave
  constexpr int kAdaa1 = 1;            // rt::AliasingControl::Adaa1
  for (GsEfxStage& stage : chain) {
    ParamsJson keys;
    const std::string& name = stage.name;
    if (name == "effects.modulation.chorus" || name == "effects.modulation.flanger" ||
        name == "effects.modulation.ensemble" || name == "effects.delay.stereo") {
      keys.integer("interpolation", kLagrange3);
    } else if (name == "effects.modulation.rotary") {
      keys.integer("interpolation", kLagrange3);
      keys.integer("model", kGeometricRotary);
    } else if (name == "effects.modulation.pitchShifter") {
      keys.integer("interpolation", kLagrange3);
      keys.integer("antiAlias", 1);
    } else if (name == "effects.modulation.wah" || name == "effects.modulation.autoWah") {
      keys.integer("sweepLaw", kOctaveSweep);
    } else if (name == "spectral.presenceEnhancer") {
      keys.integer("aliasing", kAdaa1);
    } else {
      continue;
    }
    const std::string rendered = keys.str();
    merge_key(stage.params_json, rendered.substr(1, rendered.size() - 2));
  }
}

/// Writes every control the binding rows give this type, into the stage each
/// row names by (name, ordinal) -- appending the stage where the effect chain
/// has none.
///
/// The skeleton writes shapes, modes and the bytes it reads under a law of its
/// own, never a bound control, so no row finds its key already written; the
/// check keeps a collision from rendering one key twice.
///
/// This is also what realises the unit's output stage. The tone pair, the pan
/// and the output level sit at the same slots for every type rather than inside
/// any one effect, which is why nineteen inserts do not each carry a copy of
/// them: the module puts one stage after the effect and the table says so.
void apply_bindings(std::vector<GsEfxStage>& chain, const GsEfx& efx, const GsEfxRowView& view) {
  const uint16_t type = gs_efx_binding_type(view, efx.type);
  // Rows arrive in slot order, which is the unit's own: an appended stage lands
  // where the unit puts it -- tone pair, pan, level.
  for (size_t i = 0; i < view.n_rows; ++i) {
    const GsEfxBindingRow& row = view.rows[i];
    if (row.type != type) continue;
    assert(row.stage < kGsEfxRowStages.size() && row.key < kGsEfxRowKeys.size());
    const std::string_view stage = kGsEfxRowStages[row.stage];
    const std::string_view key = kGsEfxRowKeys[row.key];

    auto found = find_stage(chain, stage, row.ordinal);
    if (found == chain.end()) {
      // A stage the rows bring into being takes the next ordinal of its name,
      // so a row may only name the one that comes next.
      if (row.ordinal != count_named(chain, stage)) continue;
      ParamsJson constants;
      write_output_stage_constants(constants, stage);
      chain.push_back({std::string(stage), constants.str(), kGsEfxBranchBack, row.ordinal});
      found = std::prev(chain.end());
    }
    // A skeleton constant is only the default; the bound byte replaces it.
    drop_key(found->params_json, key);

    ParamsJson one;
    write_bound(one, std::string(key).c_str(), row, efx_byte(efx, row.slot));
    // A balance byte is a two-ramp position, which only that law reads back
    // as the measured direct and effect gains.
    if (row.conv_class == kGsEfxClassBalance && row.law.form == kGsEfxFormNone) {
      drop_key(found->params_json, "mixLaw");
      one.integer("mixLaw", static_cast<int>(sonare::effects::common::MixLaw::kTwoRamps));
    }
    const std::string rendered = one.str();
    // ParamsJson closes itself, so unwrap the pairs it just wrote.
    merge_key(found->params_json, rendered.substr(1, rendered.size() - 2));
  }
}

/// Sets each stage's `enabled` from the switch and selector rows. A stage two
/// rules name is on only where both turn it on (a switch over a selector).
void apply_enables(std::vector<GsEfxStage>& chain, const GsEfx& efx, const GsEfxRowView& view) {
  const uint16_t type = gs_efx_binding_type(view, efx.type);
  for (size_t i = 0; i < view.n_enables; ++i) {
    const GsEfxEnable& enable = view.enables[i];
    if (enable.type != type) continue;
    const uint8_t byte = efx_byte(efx, enable.slot);
    for (uint8_t s = 0; s < enable.n_stages; ++s) {
      assert(enable.stages[s] < kGsEfxRowStages.size());
      const auto found = find_stage(chain, kGsEfxRowStages[enable.stages[s]], enable.ordinals[s]);
      if (found == chain.end()) continue;
      found->enabled = found->enabled && gs_efx_enable_on(enable, byte, s);
    }
  }
}

/// Numbers every stage among the stages sharing its name, in chain order.
void number_ordinals(std::vector<GsEfxStage>& chain) {
  for (size_t i = 0; i < chain.size(); ++i) {
    uint8_t ordinal = 0;
    for (size_t j = 0; j < i; ++j) {
      if (chain[j].name == chain[i].name) ++ordinal;
    }
    chain[i].ordinal = ordinal;
  }
}

std::vector<GsEfxStage> gs_efx_effect_chain(const GsEfx& efx);

/// One single type's effect stages as its own power-on bytes realise them.
/// The bytes a parallel type prints for the half reach it through its rows.
std::vector<GsEfxStage> gs_efx_single_chain(uint16_t type) {
  GsEfx single;
  single.type = type;
  single.type_msb = static_cast<uint8_t>(type >> 8);
  const GsEfxTypeDefaults* defaults = gs_efx_type_defaults(type);
  if (defaults != nullptr) single.params = defaults->params;
  return gs_efx_effect_chain(single);
}

constexpr std::string_view kGsOverdrive = "saturation.overdrive";
constexpr std::string_view kGsDistortion = "saturation.distortion";

/// saturation::CabModel values the amp stages sit on.
constexpr int kGsCabGuitar4x12 = 0;
constexpr int kGsCabGuitar1x12Combo = 2;
constexpr int kGsCabGuitar2x12Open = 3;

/// One amp stage of an OD/DS block: an amp preset and the cabinet under it.
struct GsAmpVoicing {
  const char* preset;
  int cab_model;
};

/// The printed Amp Type states in order (Small, BltIn, 2-Stk, 3-Stk), one amp
/// stage each. The assignment is the library's; nothing measured it.
constexpr std::array<GsAmpVoicing, 4> kGsAmpTypes = {{
    {"cleanCombo", kGsCabGuitar1x12Combo},
    {"chimeEdge", kGsCabGuitar2x12Open},
    {"britStack", kGsCabGuitar4x12},
    {"rectifierChug", kGsCabGuitar4x12},
}};

/// Rotary Multi prints no Amp Type, and the manual names no amp for it.
constexpr std::size_t kGsRotaryMultiAmp = 2;
/// Bass Multi's OD Amp prints the first three states only.
constexpr std::size_t kGsBassMultiAmpTypes = 3;

GsEfxStage gs_amp_stage(const GsAmpVoicing& voicing) {
  ParamsJson out;
  out.text("preset", voicing.preset);
  out.integer("cabModel", voicing.cab_model);
  return {"saturation.ampSim", out.str()};
}

/// An OD/DS block: its pedal, or the pair an OD Sel picks between, then one amp
/// stage per printed Amp Type state. The Drive byte reaches the pedal; Amp Type
/// turns one amp on and Amp Sw takes the cabinet off every one of them.
std::vector<GsEfxStage> gs_drive_block(std::initializer_list<std::string_view> pedals,
                                       std::size_t amp_types) {
  std::vector<GsEfxStage> block;
  for (const std::string_view pedal : pedals) block.push_back({std::string(pedal), "{}"});
  for (std::size_t i = 0; i < amp_types; ++i) block.push_back(gs_amp_stage(kGsAmpTypes[i]));
  return block;
}

/// A parallel-2 type: which single types realise each half, in the order the
/// half runs them. Two entries place an overdrive/distortion selector's pair.
struct GsEfxParallelHalves {
  uint16_t type;
  std::array<uint16_t, 2> a;
  std::array<uint16_t, 2> b;
};

/// The halves the printed type names select (OD = the Overdrive/Distortion
/// pair its OD Sel byte picks between). A zero entry is no second type.
constexpr std::array<GsEfxParallelHalves, 9> kGsEfxParallelHalves = {{
    {0x1100, {0x0142, 0}, {0x0150, 0}},            // Cho / Delay
    {0x1101, {0x0123, 0}, {0x0150, 0}},            // FL / Delay
    {0x1102, {0x0142, 0}, {0x0123, 0}},            // Cho / Flanger
    {0x1103, {0x0110, 0x0111}, {0x0110, 0x0111}},  // OD1 / OD2
    {0x1104, {0x0110, 0x0111}, {0x0122, 0}},       // OD / Rotary
    {0x1105, {0x0110, 0x0111}, {0x0120, 0}},       // OD / Phaser
    {0x1106, {0x0110, 0x0111}, {0x0121, 0}},       // OD / AutoWah
    {0x1107, {0x0120, 0}, {0x0122, 0}},            // PH / Rotary
    {0x1108, {0x0120, 0}, {0x0121, 0}},            // PH / AutoWah
}};

/// Appends one half: its single types' stages, then its own level and pan.
void append_half(std::vector<GsEfxStage>& chain, const std::array<uint16_t, 2>& types,
                 uint8_t branch) {
  // An OD Sel pair is one block: both pedals share the amp stages behind them.
  std::vector<GsEfxStage> stages;
  if (types[0] == 0x0110 && types[1] == 0x0111) {
    stages = gs_drive_block({kGsOverdrive, kGsDistortion}, kGsAmpTypes.size());
  } else {
    for (const uint16_t type : types) {
      if (type == 0) continue;
      for (GsEfxStage& stage : gs_efx_single_chain(type)) stages.push_back(std::move(stage));
    }
  }
  for (GsEfxStage& stage : stages) {
    stage.branch = branch;
    chain.push_back(std::move(stage));
  }
  chain.push_back({"utility.gain", "{}", branch});
  // The raw constant-power law, so the half's pan byte moves its pair.
  ParamsJson pan;
  pan.integer("law", 1);
  chain.push_back({"stereo.stereoBalance", pan.str(), branch});
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
  // here carry only what the skeleton owns. A block the type's selector picks
  // between (OD Sel, CF Sel, TP Sel) places both candidates side by side in the
  // series; the selector's enable row turns one of them off.
  const auto comp = [] { return GsEfxStage{"dynamics.compressor", "{}"}; };
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
  const auto binaural = [] { return GsEfxStage{"stereo.binaural", "{}"}; };
  const auto od = [] { return gs_drive_block({kGsOverdrive}, kGsAmpTypes.size()); };
  const auto ds = [] { return gs_drive_block({kGsDistortion}, kGsAmpTypes.size()); };
  const auto od_sel = [](std::size_t amp_types) {
    return gs_drive_block({kGsOverdrive, kGsDistortion}, amp_types);
  };
  // A drive block between the stages before and after it.
  const auto around = [](std::initializer_list<GsEfxStage> head, std::vector<GsEfxStage> block,
                         std::initializer_list<GsEfxStage> tail) {
    std::vector<GsEfxStage> chain(head);
    chain.insert(chain.end(), std::make_move_iterator(block.begin()),
                 std::make_move_iterator(block.end()));
    chain.insert(chain.end(), tail.begin(), tail.end());
    return chain;
  };
  switch (efx.type) {
    case 0x0110:  // Overdrive
      return od();
    case 0x0111:  // Distortion
      return ds();
    case 0x0141:  // Tremolo Chorus: the chorus with its output amplitude-modulated.
      return {cf(), trem()};
    case 0x0144:  // 3D Chorus: the chorus placed by the binaural stage.
    case 0x0157:  // 3D Delay: the stereo delay placed by the binaural stage.
      return {{std::string(gs_efx_insert_name(efx.type)), gs_efx_insert_params(efx)}, binaural()};
    // Series-2 composites (SC-88Pro MSB 02): two stock effects in signal order.
    case 0x0200:  // OD -> Chorus
      return around({}, od(), {cf()});
    case 0x0201:  // OD -> Flanger
      return around({}, od(), {fl()});
    case 0x0202:  // OD -> Delay
      return around({}, od(), {delay()});
    case 0x0203:  // DS -> Chorus
      return around({}, ds(), {cf()});
    case 0x0204:  // DS -> Flanger
      return around({}, ds(), {fl()});
    case 0x0205:  // DS -> Delay
      return around({}, ds(), {delay()});
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
      return {{std::string(kGsOverdrive), "{}"},
              gs_amp_stage(kGsAmpTypes[kGsRotaryMultiAmp]),
              eq(),
              rot()};
    case 0x0400:  // GTR Multi 1: Cmp-OD-CF-Dly
      return around({comp()}, od_sel(kGsAmpTypes.size()), {cf(), fl(), delay()});
    case 0x0401:  // GTR Multi 2: Cmp-OD-EQ-CF
      return around({comp()}, od_sel(kGsAmpTypes.size()), {eq(), cf(), fl()});
    case 0x0402:  // GTR Multi 3: Wah-OD-CF-Dly
      return around({wah()}, od_sel(kGsAmpTypes.size()), {cf(), fl(), delay()});
    case 0x0403:  // Clean GTR Multi 1: Cmp-EQ-CF-Dly (no OD block)
      return {comp(), eq(), cf(), fl(), delay()};
    case 0x0404:  // Clean GTR Multi 2: AW-EQ-CF-Dly (Auto-Wah at the front)
      return {autowah(), eq(), cf(), fl(), delay()};
    case 0x0405:  // Bass Multi: Cmp-OD-EQ-CF
      return around({comp()}, od_sel(kGsBassMultiAmpTypes), {eq(), cf(), fl()});
    case 0x0406:  // Rhodes Multi: Enhancer -> Phaser -> Chorus -> Tremolo/Pan
      return {eh(), ph(), cf(), fl(), rm(), pan()};
    case 0x0500:  // Keyboard Multi: Ring Mod -> EQ -> Pitch Shifter -> Phaser -> Delay.
                  // The only GS type that binds the ring-modulator insert.
      return {rm(), eq(), ps(), ph(), delay()};
    default: {
      // Parallel-2 types (MSB 11): two halves side by side, each the skeleton of
      // the single type its printed name selects.
      for (const GsEfxParallelHalves& halves : kGsEfxParallelHalves) {
        if (halves.type != efx.type) continue;
        std::vector<GsEfxStage> chain;
        append_half(chain, halves.a, kGsEfxBranchHalfA);
        append_half(chain, halves.b, kGsEfxBranchHalfB);
        return chain;
      }
      // Single-effect types: a one-stage chain from the name/param mapping.
      const std::string_view name = gs_efx_insert_name(efx.type);
      if (name.empty()) return {};
      return {{std::string(name), gs_efx_insert_params(efx)}};
    }
  }
}

/// The generated binding rows and enables.
constexpr GsEfxRowView kGsEfxGeneratedRows{kGsEfxBindingRows.data(), kGsEfxBindingRows.size(),
                                           kGsEfxEnables.data(), kGsEfxEnables.size()};

}  // namespace

std::vector<GsEfxStage> gs_efx_insert_chain(const GsEfx& efx) {
  return gs_efx_insert_chain(efx, kGsEfxGeneratedRows);
}

std::vector<GsEfxStage> gs_efx_insert_chain(const GsEfx& efx, const GsEfxRowView& rows) {
  std::vector<GsEfxStage> chain = gs_efx_effect_chain(efx);
  // An unmapped type bypasses whole: appending an output stage to nothing would
  // put a gain where the caller logged that it plays the part dry.
  if (chain.empty()) return chain;
  number_ordinals(chain);
  write_method_keys(chain);
  apply_bindings(chain, efx, rows);
  apply_enables(chain, efx, rows);
  return chain;
}

std::string_view gs_drum_kit_name(uint8_t program, GsToneMap map) noexcept {
  const GsDrumKit* kit = gs_drum_kit_entry(program, map);
  return kit != nullptr ? kit->name : std::string_view{};
}

}  // namespace sonare::midi::synth
