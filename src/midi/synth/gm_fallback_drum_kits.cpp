#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_layer.h"

namespace sonare::midi::synth {

uint8_t gm_fallback_drum_kit(uint8_t program, GsToneMap map) noexcept {
  const GsDrumKit* kit = gs_drum_kit_entry(program & 0x7Fu, map);
  return kit != nullptr ? kit->index : 0;  // no kit in this map -> Standard
}

float apply_gs_drum_kit(PercussionPatchParams& perc, DahdsrConfig& amp, uint8_t kit,
                        uint8_t note) noexcept {
  const bool kick = note == 35 || note == 36;
  const bool snare = note == 38 || note == 40;
  const bool tom = note == 41 || note == 43 || note == 45 || note == 47 || note == 48 || note == 50;
  const bool cymbal = note == 49 || note == 51 || note == 52 || note == 53 || note == 55 ||
                      note == 57 || note == 59;
  const bool hat = note == 42 || note == 44 || note == 46;
  const bool membrane = kick || snare || tom;
  float gain = 1.0f;
  switch (kit) {
    case 1:  // Room: more shell body and a longer, ambient tail.
      perc.shell_mix = std::min(perc.shell_mix + 0.12f, 1.0f);
      for (float& t60 : perc.shell_t60_s) t60 *= 1.6f;
      perc.mode_decay_s *= 1.15f;
      perc.noise_decay_ms *= 1.25f;
      amp.decay_ms *= 1.25f;
      break;
    case 2:  // Power (Rock): bigger, lower, longer shells.
      if (membrane) {
        perc.base_freq_hz *= 0.86f;
        perc.mode_decay_s *= 1.4f;
        amp.decay_ms *= 1.4f;
        gain = 1.2f;
      }
      break;
    case 3:  // Electronic: sine-ify the membranes and dry them out.
      if (membrane) {
        if (perc.num_modes > 1) perc.num_modes = 1;
        perc.base_freq_hz *= 0.92f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.5f);
        perc.noise_gain *= 0.5f;
      }
      break;
    case 4:  // TR-808: the iconic decaying-sine recipes.
      if (kick) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.82f;
        perc.pitch_drop = 1.0f;
        perc.pitch_drop_ms = 60.0f;
        perc.mode_decay_s *= 2.5f;
        amp.decay_ms *= 2.5f;
        perc.noise_gain *= 0.3f;
        gain = 1.2f;
      } else if (snare) {
        perc.num_modes = 1;  // single body tone under the noise
        perc.noise_gain *= 1.2f;
        perc.mode_decay_s *= 0.8f;
      } else if (tom) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.9f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.6f);
        perc.noise_gain *= 0.4f;
      } else if (cymbal) {
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.2f, 18000.0f);
      }
      break;
    case 5:  // Jazz: tighter, higher, softer.
      if (membrane) {
        perc.base_freq_hz *= 1.06f;
        perc.mode_decay_s *= 0.8f;
        amp.decay_ms *= 0.8f;
        gain = 0.9f;
      }
      break;
    case 6:  // Brush: the snare becomes a sustained swish, not a crack.
      if (snare) {
        perc.noise_decay_ms *= 2.5f;
        amp.decay_ms *= 2.0f;
        perc.noise_cutoff_hz *= 0.8f;
        perc.wire_buzz *= 0.4f;
        gain = 0.85f;
      } else if (membrane) {
        gain = 0.9f;
      }
      break;
    case 7:  // Orchestra: concert bass drum / timpani rings, longer cymbals.
      if (membrane) {
        perc.mode_decay_s *= 2.0f;
        amp.decay_ms *= 2.0f;
      } else if (cymbal) {
        perc.mode_decay_s *= 1.5f;
        amp.decay_ms *= 1.5f;
      }
      break;
    case 9:  // CM-64/32L: the LA-synth era kit — short, thin, bright samples.
      if (membrane) {
        if (perc.num_modes > 2) perc.num_modes = 2;
        perc.base_freq_hz *= 1.04f;
        perc.mode_decay_s *= 0.55f;
        amp.decay_ms *= 0.55f;
        perc.noise_gain *= 0.8f;
      } else if (cymbal || hat) {
        perc.noise_decay_ms *= 0.7f;
      }
      gain = 0.95f;
      break;
    case 10:  // Standard 2: the alternate standard — a drier, tighter room.
      if (membrane) {
        perc.base_freq_hz *= 0.97f;
        for (float& t60 : perc.shell_t60_s) t60 *= 0.9f;
        perc.mode_decay_s *= 0.92f;
      }
      if (snare) perc.wire_buzz *= 1.1f;
      break;
    case 11:  // Dance: the drum-machine kit of a club record — sine kick, clap-lit
              // snare, tight hats.
      if (kick) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.85f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.8f);
        perc.mode_decay_s *= 1.8f;
        amp.decay_ms *= 1.8f;
        perc.noise_gain *= 0.35f;
        gain = 1.15f;
      } else if (snare) {
        perc.noise_gain *= 1.35f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.15f, 18000.0f);
        perc.mode_decay_s *= 0.7f;
      } else if (tom) {
        perc.num_modes = 1;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.5f);
      } else if (hat) {
        perc.noise_decay_ms *= 0.7f;
      }
      break;
    case 12:  // Ethnic: hand drums. Struck near the rim rather than the centre, so
              // the higher modes speak and the drum is pitched, over a thin shell
              // that rings far less than a kit drum's.
      if (membrane) {
        perc.strike_r = std::max(perc.strike_r, 0.6f);
        perc.base_freq_hz *= 1.1f;
        perc.mode_decay_s *= 0.7f;
        amp.decay_ms *= 0.7f;
        perc.noise_gain *= 0.5f;
        perc.shell_mix *= 0.5f;
      }
      gain = 0.95f;
      break;
    case 13:  // Kick & Snare: a bank of alternate kicks and snares over an
              // otherwise Standard kit, so only those two move.
      if (kick) {
        perc.base_freq_hz *= 0.9f;
        perc.mode_decay_s *= 1.25f;
        amp.decay_ms *= 1.25f;
        gain = 1.1f;
      } else if (snare) {
        perc.wire_buzz *= 1.25f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.1f, 18000.0f);
      }
      break;
    case 15:  // Standard 3: the set whose pieces vary strike to strike. The
              // variation itself needs a per-strike seed this hook is not given
              // (it runs once on the resolved patch, before the voice is seeded),
              // so what is voiced here is the rest of the set's character: struck
              // off-centre and left more open than Standard.
      if (membrane) {
        perc.strike_r = std::max(perc.strike_r, 0.45f);
        perc.shell_mix = std::min(perc.shell_mix + 0.08f, 1.0f);
      }
      break;
    case 16:  // Hip Hop: low, short and squashed — the sampled, heavily processed
              // kit rather than a room.
      if (kick) {
        perc.base_freq_hz *= 0.8f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.6f);
        perc.mode_decay_s *= 1.5f;
        amp.decay_ms *= 1.5f;
        perc.noise_gain *= 0.5f;
        gain = 1.25f;
      } else if (snare) {
        perc.noise_cutoff_hz *= 0.8f;
        perc.noise_decay_ms *= 0.8f;
        perc.wire_buzz *= 0.7f;
        gain = 1.1f;
      } else if (tom) {
        perc.base_freq_hz *= 0.85f;
      }
      break;
    case 17:  // Jungle: a breakbeat chopped short — everything cut off early and
              // pushed bright.
      if (membrane) {
        perc.base_freq_hz *= 1.05f;
        perc.mode_decay_s *= 0.5f;
        amp.decay_ms *= 0.5f;
      }
      if (snare) {
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.25f, 18000.0f);
        perc.noise_decay_ms *= 0.6f;
      } else if (cymbal || hat) {
        perc.noise_decay_ms *= 0.5f;
      }
      break;
    case 18:  // Techno: Electronic taken further — pure synthetic membranes and a
              // hard, bright top end.
      if (membrane) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.9f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.6f);
        perc.noise_gain *= 0.35f;
      } else if (cymbal || hat) {
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.3f, 18000.0f);
      }
      gain = 1.1f;
      break;
    case 19:  // CR-78: the 1978 preset-rhythm box. Barely any tone at all — short
              // filtered noise ticks over tiny sines, and a snare that is a noise
              // burst with no wire under it.
      if (kick) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.95f;
        perc.mode_decay_s *= 0.5f;
        amp.decay_ms *= 0.5f;
        perc.noise_gain *= 0.2f;
      } else if (snare) {
        perc.num_modes = 1;
        perc.noise_decay_ms *= 0.35f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.4f, 18000.0f);
        perc.wire_buzz = 0.0f;
      } else if (tom) {
        perc.num_modes = 1;
        perc.mode_decay_s *= 0.5f;
        amp.decay_ms *= 0.5f;
      } else if (cymbal || hat) {
        perc.noise_decay_ms *= 0.3f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.5f, 18000.0f);
      }
      gain = 0.85f;
      break;
    case 20:  // TR-606: thin and tinny — the smallest of the analog boxes.
      if (kick) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.9f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.7f);
        perc.mode_decay_s *= 1.1f;
        perc.noise_gain *= 0.25f;
      } else if (snare) {
        perc.num_modes = 1;
        perc.noise_gain *= 1.3f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.3f, 18000.0f);
        perc.noise_decay_ms *= 0.55f;
        perc.wire_buzz *= 0.4f;
      } else if (tom) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.95f;
        perc.mode_decay_s *= 0.7f;
        amp.decay_ms *= 0.7f;
      } else if (cymbal || hat) {
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.45f, 18000.0f);
        perc.noise_decay_ms *= 0.6f;
      }
      gain = 0.9f;
      break;
    case 21:  // TR-707: sampled, not analog — crisp, dry and short, with none of
              // the analog boxes' decay tails and little room around it.
      if (membrane) {
        perc.mode_decay_s *= 0.65f;
        amp.decay_ms *= 0.65f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.15f, 18000.0f);
        perc.shell_mix *= 0.6f;
      }
      gain = 1.05f;
      break;
    case 22:  // TR-909: the long decaying-sine kick with a click on top, a snare
              // that is mostly noise, and bright metallic cymbals.
      if (kick) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.86f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.9f);
        perc.pitch_drop_ms = 45.0f;
        perc.mode_decay_s *= 2.2f;
        amp.decay_ms *= 2.2f;
        perc.noise_gain *= 0.5f;  // the attack click, not a skin
        gain = 1.2f;
      } else if (snare) {
        perc.noise_gain *= 1.45f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.2f, 18000.0f);
        perc.mode_decay_s *= 0.75f;
      } else if (tom) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.92f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.55f);
      } else if (cymbal || hat) {
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.35f, 18000.0f);
        perc.mode_decay_s *= 1.2f;
      }
      break;
    case 23:  // Asia: gongs, taiko and temple blocks — big, low and long-ringing.
      if (membrane) {
        perc.base_freq_hz *= 0.82f;
        perc.mode_decay_s *= 2.2f;
        amp.decay_ms *= 2.2f;
        for (float& t60 : perc.shell_t60_s) t60 *= 1.5f;
      } else if (cymbal) {
        perc.mode_decay_s *= 2.5f;
        amp.decay_ms *= 2.5f;
        perc.noise_decay_ms *= 1.6f;
      }
      gain = 1.05f;
      break;
    case 8:   // SFX: a set of one-shots in real GS — leave the Standard voicing.
    case 14:  // Rhythm FX: likewise one-shots.
    case 24:  // Cymbal & Claps: a bank of metal and hands over an unchanged kit.
    case 25:  // Rhythm FX 2: likewise one-shots.
    default:  // Standard.
      break;
  }
  return gain;
}

bool gs_drum_kit_is_voiced_apart(uint8_t kit) noexcept {
  // memcpy rather than assignment, and memcmp rather than a field list: the two
  // sides then share their padding byte for byte, so the comparison stays exact
  // while covering a field added to either struct without being told about it.
  static_assert(std::is_trivially_copyable_v<PercussionPatchParams>);
  static_assert(std::is_trivially_copyable_v<DahdsrConfig>);
  if (kit == 0) return false;
  for (int note = 0; note < 128; ++note) {
    const NativeSynthPatch& standard = gm_fallback_drum_patch(static_cast<uint8_t>(note));
    PercussionPatchParams perc;
    DahdsrConfig amp;
    std::memcpy(&perc, &standard.percussion, sizeof(perc));
    std::memcpy(&amp, &standard.amp_env, sizeof(amp));
    const float gain = apply_gs_drum_kit(perc, amp, kit, static_cast<uint8_t>(note));
    if (gain != 1.0f) return true;
    if (std::memcmp(&perc, &standard.percussion, sizeof(perc)) != 0) return true;
    if (std::memcmp(&amp, &standard.amp_env, sizeof(amp)) != 0) return true;
  }
  return false;
}

namespace {

/// A run of keys a kit's own listing under one map prints as no sound ("---")
/// or does not print at all, read from the owner's manual drum set tables.
struct UnlistedKeys {
  uint8_t kit;
  GsToneMap map;
  uint8_t first;
  uint8_t last;
};

// clang-format off
constexpr UnlistedKeys kUnlistedKeys[] = {
    {0, GsToneMap::kSc55, 0, 26},  // Standard, p.211
    {0, GsToneMap::kSc55, 88, 127},  // Standard, p.211
    {0, GsToneMap::kSc88, 0, 24},  // Standard, p.208
    {0, GsToneMap::kSc88, 88, 127},  // Standard, p.208
    {0, GsToneMap::kSc88Pro, 20, 21},  // Standard, p.201
    {0, GsToneMap::kSc88Pro, 89, 96},  // Standard, p.201
    {0, GsToneMap::kSc8850, 89, 94},  // Standard, p.188
    {1, GsToneMap::kSc55, 0, 26},  // Room, p.211
    {1, GsToneMap::kSc55, 88, 127},  // Room, p.211
    {1, GsToneMap::kSc88, 0, 24},  // Room, p.208
    {1, GsToneMap::kSc88, 88, 127},  // Room, p.208
    {1, GsToneMap::kSc88Pro, 20, 21},  // Room, p.201
    {1, GsToneMap::kSc88Pro, 89, 96},  // Room, p.201
    {1, GsToneMap::kSc8850, 89, 94},  // Room, p.188
    {2, GsToneMap::kSc55, 0, 26},  // Power, p.211
    {2, GsToneMap::kSc55, 88, 127},  // Power, p.211
    {2, GsToneMap::kSc88, 0, 24},  // Power, p.208
    {2, GsToneMap::kSc88, 88, 127},  // Power, p.208
    {2, GsToneMap::kSc88Pro, 20, 21},  // Power, p.202
    {2, GsToneMap::kSc88Pro, 89, 96},  // Power, p.202
    {2, GsToneMap::kSc8850, 20, 21},  // Power, p.189
    {2, GsToneMap::kSc8850, 89, 96},  // Power, p.189
    {3, GsToneMap::kSc55, 0, 26},  // Electronic, p.211
    {3, GsToneMap::kSc55, 88, 127},  // Electronic, p.211
    {3, GsToneMap::kSc88, 0, 24},  // Electronic, p.208
    {3, GsToneMap::kSc88, 88, 127},  // Electronic, p.208
    {3, GsToneMap::kSc88Pro, 20, 21},  // Electronic, p.202
    {3, GsToneMap::kSc88Pro, 89, 96},  // Electronic, p.202
    {3, GsToneMap::kSc8850, 20, 21},  // Electronic, p.190
    {3, GsToneMap::kSc8850, 89, 96},  // Electronic, p.190
    {4, GsToneMap::kSc55, 0, 26},  // TR-808, p.211
    {4, GsToneMap::kSc55, 88, 127},  // TR-808, p.211
    {4, GsToneMap::kSc88, 0, 24},  // TR-808, p.209
    {4, GsToneMap::kSc88, 88, 127},  // TR-808, p.209
    {4, GsToneMap::kSc88Pro, 20, 21},  // TR-808, p.202
    {4, GsToneMap::kSc88Pro, 89, 96},  // TR-808, p.202
    {4, GsToneMap::kSc8850, 20, 21},  // TR-808, p.190
    {4, GsToneMap::kSc8850, 89, 96},  // TR-808, p.190
    {5, GsToneMap::kSc55, 0, 26},  // Jazz, p.211
    {5, GsToneMap::kSc55, 88, 127},  // Jazz, p.211
    {5, GsToneMap::kSc88, 0, 24},  // Jazz, p.209
    {5, GsToneMap::kSc88, 88, 127},  // Jazz, p.209
    {5, GsToneMap::kSc88Pro, 20, 21},  // Jazz, p.204
    {5, GsToneMap::kSc88Pro, 89, 99},  // Jazz, p.204
    {5, GsToneMap::kSc8850, 89, 97},  // Jazz, p.191
    {6, GsToneMap::kSc55, 0, 26},  // Brush, p.211
    {6, GsToneMap::kSc55, 88, 127},  // Brush, p.211
    {6, GsToneMap::kSc88, 0, 24},  // Brush, p.209
    {6, GsToneMap::kSc88, 88, 127},  // Brush, p.209
    {6, GsToneMap::kSc88Pro, 20, 21},  // Brush, p.204
    {6, GsToneMap::kSc88Pro, 89, 99},  // Brush, p.204
    {6, GsToneMap::kSc8850, 20, 21},  // Brush, p.191
    {6, GsToneMap::kSc8850, 89, 99},  // Brush, p.191
    {7, GsToneMap::kSc55, 0, 26},  // Orchestra, p.211
    {7, GsToneMap::kSc55, 89, 127},  // Orchestra, p.211
    {7, GsToneMap::kSc88, 0, 24},  // Orchestra, p.209
    {7, GsToneMap::kSc88, 89, 127},  // Orchestra, p.209
    {7, GsToneMap::kSc88Pro, 20, 21},  // Orchestra, p.204
    {7, GsToneMap::kSc88Pro, 89, 96},  // Orchestra, p.204
    {7, GsToneMap::kSc88Pro, 114, 127},  // Orchestra, p.204
    {7, GsToneMap::kSc8850, 20, 21},  // Orchestra, p.192
    {7, GsToneMap::kSc8850, 89, 96},  // Orchestra, p.192
    {7, GsToneMap::kSc8850, 114, 127},  // Orchestra, p.192
    {9, GsToneMap::kSc55, 0, 34},  // CM-64/32L, p.212
    {9, GsToneMap::kSc55, 52, 53},  // CM-64/32L, p.212
    {9, GsToneMap::kSc55, 55, 55},  // CM-64/32L, p.212
    {9, GsToneMap::kSc55, 57, 59},  // CM-64/32L, p.212
    {9, GsToneMap::kSc55, 74, 74},  // CM-64/32L, p.212
    {9, GsToneMap::kSc55, 109, 127},  // CM-64/32L, p.212
    {10, GsToneMap::kSc88, 0, 24},  // Standard 2, p.208
    {10, GsToneMap::kSc88, 88, 127},  // Standard 2, p.208
    {10, GsToneMap::kSc88Pro, 20, 21},  // Standard 2, p.201
    {10, GsToneMap::kSc88Pro, 89, 96},  // Standard 2, p.201
    {10, GsToneMap::kSc8850, 20, 21},  // Standard 2, p.188
    {10, GsToneMap::kSc8850, 89, 96},  // Standard 2, p.188
    {11, GsToneMap::kSc88, 0, 24},  // Dance, p.209
    {11, GsToneMap::kSc88, 88, 127},  // Dance, p.209
    {11, GsToneMap::kSc88Pro, 20, 21},  // Dance, p.203
    {11, GsToneMap::kSc88Pro, 89, 96},  // Dance, p.203
    {12, GsToneMap::kSc88, 0, 24},  // Ethnic, p.210
    {12, GsToneMap::kSc88, 100, 127},  // Ethnic, p.210
    {12, GsToneMap::kSc88Pro, 0, 24},  // Ethnic, p.205
    {12, GsToneMap::kSc88Pro, 100, 127},  // Ethnic, p.205
    {12, GsToneMap::kSc8850, 0, 24},  // Ethnic, p.192
    {12, GsToneMap::kSc8850, 100, 127},  // Ethnic, p.192
    {13, GsToneMap::kSc88, 0, 39},  // Kick & Snare, p.210
    {13, GsToneMap::kSc88, 92, 127},  // Kick & Snare, p.210
    {13, GsToneMap::kSc88Pro, 0, 24},  // Kick & Snare, p.205
    {13, GsToneMap::kSc88Pro, 112, 127},  // Kick & Snare, p.205
    {13, GsToneMap::kSc8850, 0, 24},  // Kick & Snare, p.192
    {13, GsToneMap::kSc8850, 112, 127},  // Kick & Snare, p.192
    {15, GsToneMap::kSc88Pro, 20, 21},  // Standard 3, p.201
    {15, GsToneMap::kSc88Pro, 89, 96},  // Standard 3, p.201
    {15, GsToneMap::kSc8850, 0, 16},  // Standard 3, p.188
    {15, GsToneMap::kSc8850, 20, 21},  // Standard 3, p.188
    {15, GsToneMap::kSc8850, 89, 94},  // Standard 3, p.188
    {15, GsToneMap::kSc8850, 119, 127},  // Standard 3, p.188
    {16, GsToneMap::kSc88Pro, 20, 21},  // Hip Hop, p.201
    {16, GsToneMap::kSc88Pro, 89, 96},  // Hip Hop, p.201
    {17, GsToneMap::kSc88Pro, 20, 21},  // Jungle, p.202
    {17, GsToneMap::kSc88Pro, 89, 96},  // Jungle, p.202
    {18, GsToneMap::kSc88Pro, 20, 21},  // Techno, p.202
    {18, GsToneMap::kSc88Pro, 89, 96},  // Techno, p.202
    {19, GsToneMap::kSc88Pro, 20, 21},  // CR-78, p.203
    {19, GsToneMap::kSc88Pro, 89, 96},  // CR-78, p.203
    {19, GsToneMap::kSc8850, 20, 21},  // CR-78, p.190
    {19, GsToneMap::kSc8850, 89, 96},  // CR-78, p.190
    {20, GsToneMap::kSc88Pro, 20, 21},  // TR-606, p.203
    {20, GsToneMap::kSc88Pro, 89, 96},  // TR-606, p.203
    {20, GsToneMap::kSc8850, 20, 21},  // TR-606, p.190
    {20, GsToneMap::kSc8850, 89, 96},  // TR-606, p.190
    {21, GsToneMap::kSc88Pro, 20, 21},  // TR-707, p.203
    {21, GsToneMap::kSc88Pro, 89, 96},  // TR-707, p.203
    {21, GsToneMap::kSc8850, 20, 21},  // TR-707, p.191
    {21, GsToneMap::kSc8850, 89, 96},  // TR-707, p.191
    {22, GsToneMap::kSc88Pro, 20, 21},  // TR-909, p.204
    {22, GsToneMap::kSc88Pro, 89, 96},  // TR-909, p.204
    {22, GsToneMap::kSc8850, 20, 21},  // TR-909, p.191
    {22, GsToneMap::kSc8850, 89, 96},  // TR-909, p.191
    {23, GsToneMap::kSc88Pro, 0, 24},  // Asia, p.205
    {23, GsToneMap::kSc88Pro, 89, 127},  // Asia, p.205
    {23, GsToneMap::kSc8850, 0, 24},  // Asia, p.193
    {23, GsToneMap::kSc8850, 91, 127},  // Asia, p.193
};
// clang-format on

constexpr uint8_t kit_index_of(uint8_t program) noexcept {
  for (const GsDrumKit& kit : kGsDrumKits) {
    if (kit.program == program) return kit.index;
  }
  return 0;
}

/// CM-64/32L has a column on the SC-55 map's pages only (p.187 prints none for it on the others).
constexpr uint8_t kCm64Kit = kit_index_of(127);

bool kit_is_realised(uint8_t kit) noexcept {
  static const std::array<bool, kGsDrumKits.size()> realised = [] {
    std::array<bool, kGsDrumKits.size()> r{};
    for (size_t k = 0; k < r.size(); ++k) {
      r[k] = k == 0 || gs_drum_kit_is_voiced_apart(static_cast<uint8_t>(k));
    }
    return r;
  }();
  return kit < realised.size() && realised[kit];
}

}  // namespace

bool gm_fallback_drum_key_is_silent(uint8_t kit, GsToneMap map, uint8_t note) noexcept {
  const uint8_t listed = kit_is_realised(kit) ? kit : 0;
  GsToneMap from = map == GsToneMap::kModuleDefault ? GsToneMap::kSc8850 : map;
  if (listed == kCm64Kit) from = GsToneMap::kSc55;
  const uint8_t key = note & 0x7Fu;
  for (const UnlistedKeys& r : kUnlistedKeys) {
    if (r.kit == listed && r.map == from && key >= r.first && key <= r.last) return true;
  }
  return false;
}

}  // namespace sonare::midi::synth
