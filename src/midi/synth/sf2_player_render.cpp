#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "midi/builtin_synth.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"

namespace sonare::midi::synth {

namespace {

constexpr float kCcSendDepth = 0.35f;

}  // namespace

void Sf2Player::render_chunk(int n) noexcept {
  std::memset(mix_l_.data(), 0, sizeof(float) * static_cast<size_t>(n));
  std::memset(mix_r_.data(), 0, sizeof(float) * static_cast<size_t>(n));
  // Read the realised-EFX routing for this block from the snapshot the control
  // thread published (adopted in process() via acquire()). A null snapshot (none
  // published yet) or an all-dry one routes everything straight to the dry mix.
  const Sf2RealizedEfx* efx = efx_pub_->current();
  static constexpr std::array<bool, 16> kNoBus{};
  const std::array<bool, 16>& part_bussed = efx != nullptr ? efx->part_bussed : kNoBus;
  const bool any_bussed = efx != nullptr && efx->any_bussed;
  if (any_bussed && !part_bus_.empty()) {
    std::memset(part_bus_.data(), 0, sizeof(float) * part_bus_.size());
  }

#if defined(SONARE_MIDI_WITH_FX)
  // GS EFX -> system FX send routing. A part bussed through the single GS
  // insertion effect (config insert kNone, so it is bussed only because a GS EFX
  // chain was realised) feeds the system reverb/chorus/delay from its
  // POST-effect bus, scaled by the EFX unit's own send amounts (GS 40 03
  // 17/18/19). Its pre-effect CC91/93/94 send is suppressed below so the wet
  // tail follows the processed signal without double-sending. Parts with a
  // static config insert keep the CC-driven pre-insert send unchanged.
  std::array<bool, 16> efx_routed{};
  for (int part = 0; part < 16; ++part) {
    efx_routed[static_cast<size_t>(part)] =
        part_bussed[static_cast<size_t>(part)] &&
        config_.part_inserts[static_cast<size_t>(part)].type == Sf2InsertType::kNone;
  }
  // Normalised EFX send amounts (raw 0..127 -> the CC send-depth scale). Read
  // from the EFX mirror: offline it is render-thread-owned; in the live engine
  // it is updated on the control thread, but a single-byte read cannot tear and
  // a superseded value simply settles on the next block, so no lock is needed.
  const float efx_send_reverb = kCcSendDepth * static_cast<float>(efx_.send_reverb) / 127.0f;
  const float efx_send_chorus = kCcSendDepth * static_cast<float>(efx_.send_chorus) / 127.0f;
  const float efx_send_delay = kCcSendDepth * static_cast<float>(efx_.send_delay) / 127.0f;
  float* rev_l = nullptr;
  float* rev_r = nullptr;
  float* cho_l = nullptr;
  float* cho_r = nullptr;
  float* dly_l = nullptr;
  float* dly_r = nullptr;
  if (effects_ != nullptr) {
    effects_->begin_chunk();
    rev_l = effects_->reverb_in(0);
    rev_r = effects_->reverb_in(1);
    cho_l = effects_->chorus_in(0);
    cho_r = effects_->chorus_in(1);
    dly_l = effects_->delay_in(0);
    dly_r = effects_->delay_in(1);
  }
#endif

  // Organ wind demand per part (sounding pipe-organ fallback voices): the
  // shared wind chest advances once per sample per part, not per voice.
  int organ_demand[16] = {0};
  bool any_wind = false;
  bool body_has_voice[16] = {false};
  for (const NativeSynthVoice& v : fallback_pool_) {
    if (!v.active || v.patch == nullptr) continue;
    const uint8_t part = v.channel & 0x0Fu;
    if (v.patch->mode == SynthEngineMode::kPipeOrgan) {
      ++organ_demand[part];
      any_wind = any_wind || fallback_wind_[part].active();
    }
    if (fallback_body_[part].kind != FallbackBodyKind::kNone) body_has_voice[part] = true;
  }
  // Body resonators keep ringing for a bounded tail after the last voice dies
  // (the bank's own decay), then stop costing anything.
  bool body_active[16] = {false};
  bool any_body = false;
  for (int part = 0; part < 16; ++part) {
    FallbackBodyState& body = fallback_body_[static_cast<size_t>(part)];
    if (body.kind == FallbackBodyKind::kNone) continue;
    if (body_has_voice[part]) {
      body.ringout = static_cast<int64_t>(2.0 * sample_rate_);
    } else if (body.ringout > 0) {
      body.ringout = std::max<int64_t>(0, body.ringout - n);
    }
    body_active[part] = body_has_voice[part] || body.ringout > 0;
    any_body = any_body || body_active[part];
  }

  for (int i = 0; i < n; ++i) {
    OrganWindSupply::State wind_state[16];
    if (any_wind) {
      for (int part = 0; part < 16; ++part) {
        if (organ_demand[part] > 0 && fallback_wind_[static_cast<size_t>(part)].active()) {
          wind_state[part] = fallback_wind_[static_cast<size_t>(part)].process(organ_demand[part]);
        }
      }
    }
    for (Sf2Voice& v : pool_) {
      if (!v.active) continue;
      const uint8_t part = v.channel & 0x0Fu;
      const Sf2ChannelMod& mod = channel_mods_[part];
      const float s = v.render(mod);
      const float l = s * v.gain_left;
      const float r = s * v.gain_right;
      if (part_bussed[part]) {
        float* bus = part_bus_.data() + static_cast<size_t>(part) * 2 * kChunkFrames;
        bus[i] += l;
        bus[kChunkFrames + i] += r;
      } else {
        mix_l_[static_cast<size_t>(i)] += l;
        mix_r_[static_cast<size_t>(i)] += r;
      }
#if defined(SONARE_MIDI_WITH_FX)
      // Suppressed for GS-EFX-routed parts: their send is taken post-effect.
      if (rev_l != nullptr && !efx_routed[part]) {
        const float rs = std::min(1.0f, v.params.reverb_send + mod.reverb_send);
        if (rs > 0.0f) {
          rev_l[i] += l * rs;
          rev_r[i] += r * rs;
        }
        const float cs = std::min(1.0f, v.params.chorus_send + mod.chorus_send);
        if (cs > 0.0f) {
          cho_l[i] += l * cs;
          cho_r[i] += r * cs;
        }
        if (mod.delay_send > 0.0f) {
          dly_l[i] += l * mod.delay_send;
          dly_r[i] += r * mod.delay_send;
        }
      }
#endif
    }
    // Synth-fallback voices: same bus routing, channel-level (CC) sends only
    // (no zone send generators).
    float body_dry[16] = {0.0f};
    for (NativeSynthVoice& v : fallback_pool_) {
      if (!v.active) continue;
      const uint8_t part = v.channel & 0x0Fu;
      const Sf2ChannelMod& mod = channel_mods_[part];
      const OrganWindSupply::State& wind = wind_state[part];
      const float s = v.render(mod, wind.pitch_ratio, wind.gain);
      float l = s * v.gain_left;
      float r = s * v.gain_right;
      if (body_active[part]) body_dry[part] += 0.5f * (l + r);
      // Piano radiates mostly through the board (the body block below); only
      // the direct share of the raw string waveform stays in the voice path.
      if (fallback_body_[part].kind == FallbackBodyKind::kPiano) {
        l *= kPianoDirectGain;
        r *= kPianoDirectGain;
      }
      if (part_bussed[part]) {
        float* bus = part_bus_.data() + static_cast<size_t>(part) * 2 * kChunkFrames;
        bus[i] += l;
        bus[kChunkFrames + i] += r;
      } else {
        mix_l_[static_cast<size_t>(i)] += l;
        mix_r_[static_cast<size_t>(i)] += r;
      }
#if defined(SONARE_MIDI_WITH_FX)
      // Suppressed for GS-EFX-routed parts: their send is taken post-effect.
      if (rev_l != nullptr && !efx_routed[part]) {
        if (mod.fallback_reverb_send > 0.0f) {
          rev_l[i] += l * mod.fallback_reverb_send;
          rev_r[i] += r * mod.fallback_reverb_send;
        }
        if (mod.fallback_chorus_send > 0.0f) {
          cho_l[i] += l * mod.fallback_chorus_send;
          cho_r[i] += r * mod.fallback_chorus_send;
        }
        if (mod.delay_send > 0.0f) {
          dly_l[i] += l * mod.delay_send;
          dly_r[i] += r * mod.delay_send;
        }
      }
#endif
    }
    // Shared body resonators, fed by the part's summed fallback dry signal and
    // folded back centre-panned (the same bus-level coupling the NativeSynth
    // host applies): the piano's soundboard + pedal-gated sympathetic bank,
    // the plucked halo held open (no dampers).
    if (any_body) {
      for (int part = 0; part < 16; ++part) {
        if (!body_active[part]) continue;
        const FallbackBodyState& body = fallback_body_[static_cast<size_t>(part)];
        const float dry = body_dry[part];
        float add = 0.0f;
        if (body.kind == FallbackBodyKind::kPiano) {
          PianoSoundboard& board = fallback_board_[static_cast<size_t>(part)];
          add = board.process(dry) +
                fallback_reso_[static_cast<size_t>(part)].process(
                    board.last_diffused(), channels_[static_cast<size_t>(part)].sustain);
        } else {
          add = fallback_reso_[static_cast<size_t>(part)].process(dry, /*damper_open=*/true);
        }
        if (add == 0.0f) continue;
        if (part_bussed[part]) {
          float* bus = part_bus_.data() + static_cast<size_t>(part) * 2 * kChunkFrames;
          bus[i] += add;
          bus[kChunkFrames + i] += add;
        } else {
          mix_l_[static_cast<size_t>(i)] += add;
          mix_r_[static_cast<size_t>(i)] += add;
        }
#if defined(SONARE_MIDI_WITH_FX)
        // Suppressed for GS-EFX-routed parts: their send is taken post-effect.
        if (rev_l != nullptr && !efx_routed[part]) {
          const Sf2ChannelMod& mod = channel_mods_[part];
          if (mod.fallback_reverb_send > 0.0f) {
            rev_l[i] += add * mod.fallback_reverb_send;
            rev_r[i] += add * mod.fallback_reverb_send;
          }
          if (mod.fallback_chorus_send > 0.0f) {
            cho_l[i] += add * mod.fallback_chorus_send;
            cho_r[i] += add * mod.fallback_chorus_send;
          }
        }
#endif
      }
    }
  }

  // Per-part insert processing, then sum the parts into the dry mix.
  if (any_bussed) {
    for (int part = 0; part < 16; ++part) {
      if (!part_bussed[static_cast<size_t>(part)]) continue;
      float* bus_l = part_bus_.data() + static_cast<size_t>(part) * 2 * kChunkFrames;
      float* bus_r = bus_l + kChunkFrames;
      const Sf2PartInsert& insert = config_.part_inserts[static_cast<size_t>(part)];
      if (insert.type == Sf2InsertType::kDrive && insert.amount > 0.0f) {
        // Gain-compensated tanh drive: normalise so a full-scale input keeps
        // roughly unit level regardless of the drive amount.
        const float drive = 1.0f + 9.0f * insert.amount;
        const float makeup = 1.0f / std::tanh(drive);
        for (int i = 0; i < n; ++i) {
          bus_l[i] = std::tanh(drive * bus_l[i]) * makeup;
          bus_r[i] = std::tanh(drive * bus_r[i]) * makeup;
        }
      } else {
        // A built insert chain (config kProcessor slot, or a GS-EFX-installed
        // one — single-stage or a composite's multi-stage rig) runs in series
        // in place on the part's stereo bus. An empty chain is an inert no-op.
        float* chans[2] = {bus_l, bus_r};
        for (auto& proc : efx->chains[static_cast<size_t>(part)]) {
          proc->process(chans, 2, n);
        }
      }
#if defined(SONARE_MIDI_WITH_FX)
      // GS EFX -> system FX: feed the POST-effect bus into reverb/chorus/delay by
      // the EFX unit's send amounts (the pre-effect CC send was suppressed for
      // these parts), so an insertion-effect part's wet tail is generated from
      // the processed signal rather than the clean input.
      if (rev_l != nullptr && efx_routed[static_cast<size_t>(part)]) {
        for (int i = 0; i < n; ++i) {
          if (efx_send_reverb > 0.0f) {
            rev_l[i] += bus_l[i] * efx_send_reverb;
            rev_r[i] += bus_r[i] * efx_send_reverb;
          }
          if (efx_send_chorus > 0.0f) {
            cho_l[i] += bus_l[i] * efx_send_chorus;
            cho_r[i] += bus_r[i] * efx_send_chorus;
          }
          if (efx_send_delay > 0.0f) {
            dly_l[i] += bus_l[i] * efx_send_delay;
            dly_r[i] += bus_r[i] * efx_send_delay;
          }
        }
      }
#endif
      for (int i = 0; i < n; ++i) {
        mix_l_[static_cast<size_t>(i)] += bus_l[i];
        mix_r_[static_cast<size_t>(i)] += bus_r[i];
      }
    }
  }

#if defined(SONARE_MIDI_WITH_FX)
  if (effects_ != nullptr) effects_->render_returns(mix_l_.data(), mix_r_.data(), n);
#endif
}

void Sf2Player::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_ || channels == nullptr || num_channels <= 0 || num_samples <= 0) return;
  // Offline hosts realise a pending EFX change inline (an EFX SysEx dispatched
  // for this block installs its inserts before the block renders). This
  // allocates, so it is gated to single-threaded/offline use; the live engine
  // realises on the control thread via on_control_sysex instead.
  if (config_.realize_efx_inline && gs_efx_dirty_) realize_gs_efx();
  // Adopt the newest realised-EFX snapshot for this block (wait-free, no alloc).
  // Offline this picks up the inline publish just above; live it picks up the
  // control thread's on_control_sysex publish.
  efx_pub_->acquire();
  // Apply any pending GS EFX parameter automation to the adopted chain, on this
  // (audio) thread, before rendering — the sanctioned resolve-on-control /
  // apply-on-audio path. A no-op cost (two atomic loads) when the queue is empty.
  drain_efx_param_updates();
  if (mix_l_.size() < static_cast<size_t>(kChunkFrames)) return;
  float* left = channels[0];
  float* right = num_channels > 1 ? channels[1] : nullptr;
  const bool mono = right == nullptr;

  int offset = 0;
  while (offset < num_samples) {
    const int n = std::min(kChunkFrames, num_samples - offset);
    render_chunk(n);
    for (int i = 0; i < n; ++i) {
      const float mix_l = mix_l_[static_cast<size_t>(i)] * config_.gain;
      const float mix_r = mix_r_[static_cast<size_t>(i)] * config_.gain;
      if (left != nullptr) {
        // Mono host: fold both pan legs so centre-panned voices keep level.
        left[offset + i] += mono ? 0.70710678f * (mix_l + mix_r) : mix_l;
      }
      if (right != nullptr) right[offset + i] += mix_r;
      // Fan a mono fold-down to any additional channels.
      for (int ch = 2; ch < num_channels; ++ch) {
        if (channels[ch] != nullptr) channels[ch][offset + i] += 0.70710678f * (mix_l + mix_r);
      }
    }
    offset += n;
  }
}

}  // namespace sonare::midi::synth
