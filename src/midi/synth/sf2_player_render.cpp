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
#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kInvSqrt2;

}  // namespace

void Sf2Player::render_chunk(int n, const MidiInstrumentSourceOutput* source_outputs,
                             size_t source_output_count, int output_offset,
                             int num_channels) noexcept {
  std::memset(mix_l_.data(), 0, sizeof(float) * static_cast<size_t>(n));
  std::memset(mix_r_.data(), 0, sizeof(float) * static_cast<size_t>(n));
  const bool source_render = source_outputs != nullptr;
  float attributed_l[kChunkFrames] = {};
  float attributed_r[kChunkFrames] = {};
  const auto target_for = [&](uint32_t source_track_id) noexcept -> float* const* {
    if (!source_render) return nullptr;
    for (size_t index = 1; index < source_output_count; ++index) {
      if (source_outputs[index].source_track_id == source_track_id &&
          source_outputs[index].channels != nullptr) {
        return source_outputs[index].channels;
      }
    }
    return source_outputs[0].channels;
  };
  const auto add_output = [&](float* const* target, int sample, float l, float r) noexcept {
    if (target == nullptr) return;
    if (target[0] != nullptr) {
      target[0][sample] += num_channels == 1 ? kInvSqrt2 * (l + r) : l;
    }
    if (num_channels > 1 && target[1] != nullptr) target[1][sample] += r;
    for (int ch = 2; ch < num_channels; ++ch) {
      if (target[ch] != nullptr) target[ch][sample] += kInvSqrt2 * (l + r);
    }
  };
  // Master tuning (RPN 00 01 fine / 00 02 coarse) is a static per-part pitch
  // offset, and both voice hosts take pitch offsets through Sf2ChannelMod; fold
  // it in once per chunk rather than per voice. Zero at the power-on defaults,
  // which leaves a render that was never tuned bit-identical.
  std::array<Sf2ChannelMod, 16> mods = channel_mods_;
  for (size_t part = 0; part < mods.size(); ++part) {
    mods[part].pitch_cents += channels_[part].tune_cents();
  }
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
  // Master EQ (GS 40 02 xx) is one stage on the output; a part switched out of
  // it (40 4x 20) accumulates into the bypass bus as well as into the mix, so
  // the EQ runs on the difference and that part passes through. Both are skipped
  // while the EQ is flat — the power-on state, and therefore bit-exact.
  float* eq_byp_l = nullptr;
  float* eq_byp_r = nullptr;
  if (eq_.active() && !eq_bypass_bus_.empty()) {
    for (const bool bypassed : eq_bypassed_) {
      if (!bypassed) continue;
      eq_byp_l = eq_bypass_bus_.data();
      eq_byp_r = eq_byp_l + kChunkFrames;
      std::memset(eq_byp_l, 0, sizeof(float) * eq_bypass_bus_.size());
      break;
    }
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
      body.ringout = static_cast<int64_t>(kPianoBodyRingS * sample_rate_);
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
      const Sf2ChannelMod& mod = mods[part];
      // Same non-finite scrub as the fallback loop below: this leg feeds the
      // insert bus and the system effect sends, which are persistent IIR state.
      const float rendered = v.render(mod);
      const float s = std::isfinite(rendered) ? rendered : 0.0f;
      const float l = s * v.gain_left;
      const float r = s * v.gain_right;
      if (part_bussed[part]) {
        float* bus = part_bus_.data() + static_cast<size_t>(part) * 2 * kChunkFrames;
        bus[i] += l;
        bus[kChunkFrames + i] += r;
      } else {
        mix_l_[static_cast<size_t>(i)] += l;
        mix_r_[static_cast<size_t>(i)] += r;
        if (eq_byp_l != nullptr && eq_bypassed_[part]) {
          eq_byp_l[i] += l;
          eq_byp_r[i] += r;
        }
        if (source_render) {
          add_output(target_for(v.source_track_id), output_offset + i, l * config_.gain,
                     r * config_.gain);
          attributed_l[i] += l;
          attributed_r[i] += r;
        }
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
      const Sf2ChannelMod& mod = mods[part];
      const OrganWindSupply::State& wind = wind_state[part];
      // Scrub any non-finite voice sample before it reaches a shared IIR state:
      // a single NaN/Inf would persist in the part's body resonators, the
      // insert bus and the reverb/chorus tanks and poison every later sample
      // for the whole render. Bit-identical for finite input. The same guard
      // the NativeSynth host applies to its own physical-model mix bus.
      const float rendered = v.render(mod, wind.pitch_ratio, wind.gain);
      const float s = std::isfinite(rendered) ? rendered : 0.0f;
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
        if (eq_byp_l != nullptr && eq_bypassed_[part]) {
          eq_byp_l[i] += l;
          eq_byp_r[i] += r;
        }
        if (source_render) {
          add_output(target_for(v.source_track_id), output_offset + i, l * config_.gain,
                     r * config_.gain);
          attributed_l[i] += l;
          attributed_r[i] += r;
        }
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
        // The board's two radiation paths differ in phase, so its return is
        // added to one leg and subtracted from the other. Zero at a zero board
        // width, and never non-zero for a non-piano body.
        float side = 0.0f;
        if (body.kind == FallbackBodyKind::kPiano) {
          PianoSoundboard& board = fallback_board_[static_cast<size_t>(part)];
          add = board.process(dry) +
                fallback_reso_[static_cast<size_t>(part)].process(
                    board.last_diffused(), channels_[static_cast<size_t>(part)].sustain);
          side = board.last_side();
        } else {
          add = fallback_reso_[static_cast<size_t>(part)].process(dry, /*damper_open=*/true);
        }
        if (add == 0.0f && side == 0.0f) continue;
        if (part_bussed[part]) {
          float* bus = part_bus_.data() + static_cast<size_t>(part) * 2 * kChunkFrames;
          bus[i] += add + side;
          bus[kChunkFrames + i] += add - side;
        } else {
          mix_l_[static_cast<size_t>(i)] += add + side;
          mix_r_[static_cast<size_t>(i)] += add - side;
          if (eq_byp_l != nullptr && eq_bypassed_[static_cast<size_t>(part)]) {
            eq_byp_l[i] += add + side;
            eq_byp_r[i] += add - side;
          }
        }
#if defined(SONARE_MIDI_WITH_FX)
        // Suppressed for GS-EFX-routed parts: their send is taken post-effect.
        if (rev_l != nullptr && !efx_routed[part]) {
          const Sf2ChannelMod& mod = mods[part];
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
      // A bussed part reaches the EQ bypass bus post-insert: what the mix gets
      // is what has to pass through unfiltered.
      const bool eq_bypass_part = eq_byp_l != nullptr && eq_bypassed_[static_cast<size_t>(part)];
      for (int i = 0; i < n; ++i) {
        mix_l_[static_cast<size_t>(i)] += bus_l[i];
        mix_r_[static_cast<size_t>(i)] += bus_r[i];
        if (eq_bypass_part) {
          eq_byp_l[i] += bus_l[i];
          eq_byp_r[i] += bus_r[i];
        }
      }
    }
  }

#if defined(SONARE_MIDI_WITH_FX)
  if (effects_ != nullptr) effects_->render_returns(mix_l_.data(), mix_r_.data(), n);
#endif
  // Scrub the summed bus before the DC blocker: the part inserts are
  // host-injected processors and the effect returns are their own IIR state, so
  // whatever they produced has already been added by this point. A single
  // NaN/Inf reaching dc_x1_/dc_y1_ would persist there and poison every
  // remaining sample of the render. Bit-identical for finite input; the same
  // guard the NativeSynth host applies ahead of its blocker.
  for (int i = 0; i < n; ++i) {
    if (!std::isfinite(mix_l_[static_cast<size_t>(i)])) mix_l_[static_cast<size_t>(i)] = 0.0f;
    if (!std::isfinite(mix_r_[static_cast<size_t>(i)])) mix_r_[static_cast<size_t>(i)] = 0.0f;
  }
  // Master EQ, ahead of the DC blocker so a low-shelf boost cannot leave the bus
  // with an offset the blocker was there to remove.
  if (eq_.active()) {
    if (eq_byp_l != nullptr) {
      for (int i = 0; i < n; ++i) {
        mix_l_[static_cast<size_t>(i)] -= eq_byp_l[i];
        mix_r_[static_cast<size_t>(i)] -= eq_byp_r[i];
      }
      eq_.process(mix_l_.data(), mix_r_.data(), n);
      for (int i = 0; i < n; ++i) {
        mix_l_[static_cast<size_t>(i)] += eq_byp_l[i];
        mix_r_[static_cast<size_t>(i)] += eq_byp_r[i];
      }
    } else {
      eq_.process(mix_l_.data(), mix_r_.data(), n);
    }
  }
  if (config_.dc_block) {
    // The fallback floor renders physical-model voices, which can carry a small
    // DC component; block it here rather than in process_impl so a source-track
    // render sees the same bus (the filtered delta lands in the target-zero
    // residual below, like the part inserts and the effect returns).
    for (int i = 0; i < n; ++i) {
      const float in_l = mix_l_[static_cast<size_t>(i)];
      const float l = in_l - dc_x1_[0] + dc_r_ * dc_y1_[0];
      dc_x1_[0] = in_l;
      dc_y1_[0] = l;
      mix_l_[static_cast<size_t>(i)] = l;
      const float in_r = mix_r_[static_cast<size_t>(i)];
      const float r = in_r - dc_x1_[1] + dc_r_ * dc_y1_[1];
      dc_x1_[1] = in_r;
      dc_y1_[1] = r;
      mix_r_[static_cast<size_t>(i)] = r;
    }
  }
  if (source_render) {
    // Part inserts, body resonators and system effect returns are
    // destination-scoped. Attribute their residual to target zero while keeping
    // the sum of all targets equal to the ordinary player render.
    for (int i = 0; i < n; ++i) {
      add_output(source_outputs[0].channels, output_offset + i,
                 (mix_l_[static_cast<size_t>(i)] - attributed_l[i]) * config_.gain,
                 (mix_r_[static_cast<size_t>(i)] - attributed_r[i]) * config_.gain);
    }
  }
}

void Sf2Player::process(float* const* channels, int num_channels, int num_samples) {
  process_impl(channels, nullptr, 0, num_channels, num_samples);
}

bool Sf2Player::process_source_tracks(const MidiInstrumentSourceOutput* outputs,
                                      size_t output_count, int num_channels,
                                      int num_samples) noexcept {
  if (outputs == nullptr || output_count == 0 || outputs[0].source_track_id != 0 ||
      outputs[0].channels == nullptr) {
    return false;
  }
  process_impl(nullptr, outputs, output_count, num_channels, num_samples);
  return true;
}

void Sf2Player::process_impl(float* const* channels,
                             const MidiInstrumentSourceOutput* source_outputs,
                             size_t source_output_count, int num_channels,
                             int num_samples) noexcept {
  const bool source_render = source_outputs != nullptr;
  if (!prepared_ || num_channels <= 0 || num_samples <= 0 ||
      (!source_render && channels == nullptr)) {
    return;
  }
  // Offline hosts realise a pending EFX change inline (an EFX SysEx dispatched
  // for this block installs its inserts before the block renders). This
  // allocates, so it is gated to single-threaded/offline use; the live engine
  // realises on the control thread via on_control_sysex instead.
  if (config_.realize_efx_inline && gs_efx_dirty_) realize_gs_efx();
  // GS system-effect / master-EQ state. Offline the render thread owns the
  // mirror and re-aims the units here; live the control thread hands the newest
  // state over through the queue. Both are coefficient-only, so an edit mid-note
  // keeps the reverb and delay tails it was already ringing.
  if (config_.realize_efx_inline && gs_system_dirty_) {
    gs_system_dirty_ = false;
    apply_gs_system_state(sys_fx_, master_eq_, eq_part_bypassed_);
  }
  drain_gs_system_updates();
  // Adopt the newest realised-EFX snapshot for this block (wait-free, no alloc).
  // Offline this picks up the inline publish just above; live it picks up the
  // control thread's on_control_sysex publish.
  efx_pub_->acquire();
  // Apply any pending GS EFX parameter automation to the adopted chain, on this
  // (audio) thread, before rendering — the sanctioned resolve-on-control /
  // apply-on-audio path. A no-op cost (two atomic loads) when the queue is empty.
  drain_efx_param_updates();
  if (mix_l_.size() < static_cast<size_t>(kChunkFrames)) return;
  float* left = source_render ? nullptr : channels[0];
  float* right = !source_render && num_channels > 1 ? channels[1] : nullptr;
  const bool mono = right == nullptr;

  int offset = 0;
  while (offset < num_samples) {
    const int n = std::min(kChunkFrames, num_samples - offset);
    render_chunk(n, source_outputs, source_output_count, offset, num_channels);
    if (!source_render) {
      for (int i = 0; i < n; ++i) {
        const float mix_l = mix_l_[static_cast<size_t>(i)] * config_.gain;
        const float mix_r = mix_r_[static_cast<size_t>(i)] * config_.gain;
        if (left != nullptr) {
          // Mono host: fold both pan legs so centre-panned voices keep level.
          left[offset + i] += mono ? kInvSqrt2 * (mix_l + mix_r) : mix_l;
        }
        if (right != nullptr) right[offset + i] += mix_r;
        // Fan a mono fold-down to any additional channels.
        for (int ch = 2; ch < num_channels; ++ch) {
          if (channels[ch] != nullptr) channels[ch][offset + i] += kInvSqrt2 * (mix_l + mix_r);
        }
      }
    }
    offset += n;
  }
}

}  // namespace sonare::midi::synth
