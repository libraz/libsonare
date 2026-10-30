/// @file native_synth_render.cpp
/// @brief NativeSynth block rendering: process, process_source_tracks, process_impl.

#include <algorithm>
#include <array>
#include <cmath>

#include "midi/synth/native_synth.h"
#include "util/constants.h"
#include "util/non_finite_sample.h"

namespace sonare::midi::synth {

void NativeSynth::process(float* const* channels, int num_channels, int num_samples) {
  process_impl(channels, nullptr, 0, num_channels, num_samples);
}

bool NativeSynth::process_source_tracks(const MidiInstrumentSourceOutput* outputs,
                                        size_t output_count, int num_channels,
                                        int num_samples) noexcept {
  if (outputs == nullptr || output_count == 0 || outputs[0].source_track_id != 0 ||
      outputs[0].channels == nullptr) {
    return false;
  }
  process_impl(nullptr, outputs, output_count, num_channels, num_samples);
  return true;
}

void NativeSynth::process_impl(float* const* channels,
                               const MidiInstrumentSourceOutput* source_outputs,
                               size_t source_output_count, int num_channels,
                               int num_samples) noexcept {
  const bool source_render = source_outputs != nullptr;
  if (!prepared_ || num_channels <= 0 || num_samples <= 0 ||
      (!source_render && channels == nullptr)) {
    return;
  }
  // Offline a pending rig or EFX change is realised here, before the block
  // renders (this allocates); live the control thread has already published it.
  if (part_fx_.enabled()) {
    if (config_.realize_efx_inline && part_fx_.dirty()) realize_part_fx();
    part_fx_.acquire();
    part_fx_.settle_block(*this);
  }
  // Automation written since the last block: re-derive the latched state of
  // the voices playing config_.patch (GM voices point elsewhere).
  if (live_dirty_ != 0) {
    for (NativeSynthVoice& v : pool_) {
      if (v.active && v.patch == &config_.patch && !v.choked) {
        v.refresh_live(config_.patch, sample_rate_, live_dirty_);
      }
    }
    live_dirty_ = 0;
  }
  // Diagnostic, offline only: the path this block renders through.
  if (path_recorder_ != nullptr) record_render_path();
  const PartFxSnapshot* fx = part_fx_.enabled() ? part_fx_.current() : nullptr;
  // With no part bussed every sample takes exactly the arithmetic it took
  // before the stage existed.
  const bool any_bussed = fx != nullptr && fx->any_bussed && part_fx_.has_buses();
  const bool any_unit = any_bussed && fx->any_unit && part_fx_.has_unit_buses();
  const uint64_t fx_discards_before = any_bussed ? part_fx_.discard_sum(nullptr) : 0;
  float* left = source_render ? nullptr : channels[0];
  float* right = !source_render && num_channels > 1 ? channels[1] : nullptr;
  const bool mono = right == nullptr;

  const auto target_for = [&](uint32_t source_track_id) noexcept -> float* const* {
    if (source_render) {
      for (size_t index = 1; index < source_output_count; ++index) {
        if (source_outputs[index].source_track_id == source_track_id &&
            source_outputs[index].channels != nullptr) {
          return source_outputs[index].channels;
        }
      }
      return source_outputs[0].channels;
    }
    return nullptr;
  };
  const auto add_output = [&](float* const* target, int sample, float l, float r) noexcept {
    if (target == nullptr) return;
    if (target[0] != nullptr) {
      target[0][sample] += num_channels == 1 ? constants::kInvSqrt2 * (l + r) : l;
    }
    if (num_channels > 1 && target[1] != nullptr) target[1][sample] += r;
    for (int ch = 2; ch < num_channels; ++ch) {
      if (target[ch] != nullptr) target[ch][sample] += constants::kInvSqrt2 * (l + r);
    }
  };
  // A bus output reaches the lanes of the voices that fed it, at the master gain.
  const auto flush_bus = [&](SourceResidualSplitter& splitter, const float* bus_l,
                             const float* bus_r, int n, int offset) noexcept {
    float scaled_l[kPartFxChunkFrames];
    float scaled_r[kPartFxChunkFrames];
    for (int c = 0; c < n; ++c) {
      scaled_l[c] = bus_l[c] * config_.gain;
      scaled_r[c] = bus_r[c] * config_.gain;
    }
    splitter.flush(source_outputs, source_output_count, n, scaled_l, scaled_r, offset, add_output);
  };

  // Sympathetic resonance is gated by the dampers being lifted on any channel
  // (sustain pedal down). Sustain state is fixed for the block (events are
  // applied before process()). Shared by the piano board and the guitar
  // halo below -- two bus-level banks, one gate rule.
  bool damper_open = false;
  if (piano_body_active_ || guitar_halo_active_) {
    for (const ChannelState& ch : channels_) {
      if (ch.sustain) {
        damper_open = true;
        break;
      }
    }
  }
  // A held halo key keeps the hand off its part's open strings, and any part's off the shared bank.
  std::array<bool, 16> halo_held{};
  bool any_halo_held = false;
  for (const NativeSynthVoice& v : pool_) {
    if (v.active && !v.releasing && v.patch != nullptr &&
        v.patch->mode == SynthEngineMode::kKarplusStrong && v.patch->ks.sympathetic) {
      halo_held[static_cast<size_t>(v.channel & 0x0Fu)] = true;
      any_halo_held = true;
    }
  }

  // Swell boxes are per organ part. A GM organ may share the host with another
  // engine, and each part's CC11 must affect only the pipe leg it owns.
  std::array<bool, 16> part_swell_active{};
  for (size_t part = 0; part < part_swell_active.size(); ++part) {
    const float depth = part_swell_depth_[part];
    if (!organ_part_params_[part].armed || depth <= 0.0f) {
      part_swell_coeff_[part] = 1.0f;
      continue;
    }
    const float expression = channels_[part].expression.f7();
    const float shut = (1.0f - expression / 127.0f) * depth;
    const float fc = std::exp(std::log(20000.0f) + shut * (std::log(300.0f) - std::log(20000.0f)));
    // Above ~19 kHz the shutter is effectively open.
    if (fc < 19000.0f) {
      part_swell_active[part] = true;
      part_swell_coeff_[part] = std::clamp(
          1.0f - std::exp(-constants::kTwoPi * fc / static_cast<float>(sample_rate_)), 0.0f, 1.0f);
    }
  }

  // The per-part board is independent of routing, so a queued strike can be
  // committed once the audio block begins. The board return itself is routed
  // from the same current snapshot as the dry part below.
  if (part_fx_.enabled()) {
    for (NativeSynthVoice& v : pool_) {
      if (!v.active || v.patch == nullptr || v.patch->mode != SynthEngineMode::kPiano ||
          (v.piano_case_strike_pending == 0.0f && v.piano_board_strike_pending == 0.0f)) {
        continue;
      }
      const size_t part = static_cast<size_t>(v.channel & 0x0Fu);
      part_soundboards_[part].strike(v.piano_case_strike_pending);
      part_soundboards_[part].strike_board(v.piano_board_strike_pending);
      v.piano_case_strike_pending = 0.0f;
      v.piano_board_strike_pending = 0.0f;
    }
  }

  // Set when any sample in this call's per-sample loop below actually
  // discarded; bumped once after the loop rather than per sample, since the
  // unit is one process() call, not one sample.
  bool discarded = false;
  for (int offset = 0; offset < num_samples; offset += kPartFxChunkFrames) {
    const int n = std::min(kPartFxChunkFrames, num_samples - offset);
    if (any_bussed) part_fx_.clear_part_buses();
    // First pass: the voices. A bussed part's go into its bus; every other
    // voice is summed as it always was.
    for (int c = 0; c < n; ++c) {
      const int i = offset + c;
      float mix_l = 0.0f;
      float mix_r = 0.0f;
      // The default state is unity, so non-organ voices never inherit an organ's wind.
      std::array<OrganWindSupply::State, 16> wind_state{};
      const OrganWindSupply::State unity_wind{};
      bool any_wind = false;
      for (size_t part = 0; part < wind_state.size(); ++part) {
        any_wind = any_wind || (organ_part_params_[part].armed && part_wind_[part].active());
      }
      if (any_wind) {
        // Per-sample, per-part pipe count: a release ending mid-block must stop loading its chest.
        std::array<int, 16> organ_demand{};
        for (const NativeSynthVoice& active_voice : pool_) {
          if (!active_voice.active || active_voice.patch == nullptr ||
              active_voice.patch->mode != SynthEngineMode::kPipeOrgan) {
            continue;
          }
          const size_t organ_part = static_cast<size_t>(active_voice.channel & 0x0Fu);
          // Counting pipes keeps the sag order-independent; a zero-level rank adds none.
          organ_demand[organ_part] += active_voice.pipe_organ.sounding_pipe_count();
        }
        for (size_t part = 0; part < wind_state.size(); ++part) {
          if (organ_part_params_[part].armed && part_wind_[part].active()) {
            // Zero demand lets pressure recover while this part is silent.
            wind_state[part] = part_wind_[part].process(organ_demand[part]);
          }
        }
      }
      // Piano voices are summed apart from the rest: the body below attenuates
      // and re-radiates only them, so in GM mode — where one bus carries many
      // programs — the flute sharing the render must not be pulled through the
      // soundboard. With a single-patch configuration one of the two legs stays
      // at zero, so the sum is unchanged.
      float piano_l = 0.0f;
      float piano_r = 0.0f;
      // Guitar/harp/banjo halo drive: the same isolation as the piano leg above,
      // so drums and brass sharing a GM bus never reach a bank tuned to open
      // strings. Unlike the piano leg this one stays IN mix_l/mix_r too — the
      // halo is additive to the dry KS voice rather than replacing it.
      float guitar_l = 0.0f;
      float guitar_r = 0.0f;
      for (size_t part = 0; part < 16; ++part) {
        chunk_part_piano_l_[part][static_cast<size_t>(c)] = 0.0f;
        chunk_part_piano_r_[part][static_cast<size_t>(c)] = 0.0f;
        chunk_part_organ_l_[part][static_cast<size_t>(c)] = 0.0f;
        chunk_part_organ_r_[part][static_cast<size_t>(c)] = 0.0f;
        chunk_part_guitar_l_[part][static_cast<size_t>(c)] = 0.0f;
        chunk_part_guitar_r_[part][static_cast<size_t>(c)] = 0.0f;
      }
      for (NativeSynthVoice& v : pool_) {
        if (!v.active) continue;
        // The channel's, except on a member channel sounding more than one note,
        // where the voice carries the part of the channel's the note was
        // attributed (M1-100-UM v1.1 section 2.2.4.1).
        const Sf2ChannelMod& channel_mod =
            v.mpe_mod_active ? v.mpe_mod : channel_mods_[v.channel & 0x0Fu];
        const size_t part_index = static_cast<size_t>(v.channel & 0x0Fu);
        const bool organ_voice = v.patch != nullptr && v.patch->mode == SynthEngineMode::kPipeOrgan;
        const OrganWindSupply::State& voice_wind =
            organ_voice ? wind_state[part_index] : unity_wind;
        // A key carrying per-note pitch renders through a copy of that mod with its offset
        // added; every other voice reads the mod itself.
        const float per_note_offset = per_note_[static_cast<size_t>(&v - pool_.data())].cents;
        float s = 0.0f;
        if (per_note_offset == 0.0f) {
          s = v.render(channel_mod, voice_wind.pitch_ratio, voice_wind.gain);
        } else {
          Sf2ChannelMod tuned = channel_mod;
          tuned.pitch_cents += per_note_offset;
          s = v.render(tuned, voice_wind.pitch_ratio, voice_wind.gain);
        }
        const bool piano_voice =
            piano_body_active_ && v.patch != nullptr && v.patch->mode == SynthEngineMode::kPiano;
        const bool halo_voice = v.patch != nullptr &&
                                v.patch->mode == SynthEngineMode::kKarplusStrong &&
                                v.patch->ks.sympathetic;
        const int part = v.channel & 0x0F;
        if (any_bussed && fx->part_bussed[static_cast<size_t>(part)]) {
          // The rig's processors are recursive state a non-finite sample would poison.
          discarded |= resolve_non_finite(SampleDestination::kRecursiveState, s);
          float bus_l = 0.0f;
          float bus_r = 0.0f;
          v.stereo_legs(s, &bus_l, &bus_r);
          const bool mono_rig = fx->mono_prefix[static_cast<size_t>(part)] != 0;
          if (organ_voice) {
            // Organ swell is a part stage before the rig, so retain its stereo
            // legs until the per-part filter pass below.
            if (mono_rig) {
              const float raw = constants::kInvSqrt2 * s;
              chunk_part_organ_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += raw;
              chunk_part_organ_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += raw;
            } else {
              chunk_part_organ_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += bus_l;
              chunk_part_organ_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += bus_r;
            }
          } else if (part_fx_.enabled() && piano_voice) {
            // Factory-backed piano bodies are also part stages. A mono pickup
            // feeds the board with the same common leg the rig receives.
            const float raw = constants::kInvSqrt2 * s;
            if (mono_rig) {
              chunk_part_piano_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += raw;
              chunk_part_piano_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += raw;
            } else {
              chunk_part_piano_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += bus_l;
              chunk_part_piano_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += bus_r;
            }
          } else if (part_fx_.enabled() && halo_voice &&
                     part_halo_armed_[static_cast<size_t>(part)]) {
            if (mono_rig) {
              const float raw = constants::kInvSqrt2 * s;
              chunk_part_guitar_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += raw;
              chunk_part_guitar_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += raw;
            } else {
              chunk_part_guitar_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += bus_l;
              chunk_part_guitar_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += bus_r;
            }
          } else {
            // Remaining voices feed the part bus directly (pianos at their direct gain).
            const float direct = piano_voice ? kPianoDirectGain : 1.0f;
            if (mono_rig) {
              // A mono pickup feeds the rig's amp, so CC10 cannot move its drive.
              part_fx_.add_mono(part, c, direct * constants::kInvSqrt2 * s);
            } else {
              part_fx_.add_stereo(part, c, direct * bus_l, direct * bus_r);
            }
            if (halo_voice && part_halo_armed_[static_cast<size_t>(part)]) {
              // Without per-part FX the halo leg is driven from the bussed dry signal.
              if (any_bussed) {
                const float dry = mono_rig ? constants::kInvSqrt2 * s : 0.5f * (bus_l + bus_r);
                chunk_part_guitar_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += dry;
                chunk_part_guitar_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += dry;
              }
            }
          }
          if (source_render) {
            const float src_l = bus_l * config_.gain;
            const float src_r = bus_r * config_.gain;
            residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
            if (piano_voice && !part_fx_.enabled()) {
              piano_residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
            }
            // Keep a body owner's source weights independent of the current
            // EFX assignment. If a held note moves from a bussed part to a
            // dry part, its ringing return must follow its original source
            // lane instead of falling back to lane 0.
            if (piano_voice) {
              part_piano_splitters_[static_cast<size_t>(part)].accumulate(v.source_track_id, src_l,
                                                                          src_r);
            }
            if (organ_voice) {
              part_organ_splitters_[static_cast<size_t>(part)].accumulate(v.source_track_id, src_l,
                                                                          src_r);
            }
            if (halo_voice && part_halo_armed_[static_cast<size_t>(part)]) {
              part_guitar_splitters_[static_cast<size_t>(part)].accumulate(v.source_track_id, src_l,
                                                                           src_r);
            }
            const uint8_t unit = fx->part_unit[static_cast<size_t>(part)];
            SourceResidualSplitter& bus_splitter =
                unit != PartFxSnapshot::kNoUnit && any_unit
                    ? unit_splitters_[unit]
                    : part_bus_splitters_[static_cast<size_t>(part)];
            bus_splitter.accumulate(v.source_track_id, src_l, src_r);
          }
          continue;
        }
        float voice_l = 0.0f;
        float voice_r = 0.0f;
        v.stereo_legs(s, &voice_l, &voice_r);
        if (organ_voice) {
          // Keep the unbussed dry leg in the mix and on its source lane; the
          // pre-rig swell residual is added and attributed per part below.
          mix_l += voice_l;
          mix_r += voice_r;
          chunk_part_organ_l_[static_cast<size_t>(part_index)][static_cast<size_t>(c)] += voice_l;
          chunk_part_organ_r_[static_cast<size_t>(part_index)][static_cast<size_t>(c)] += voice_r;
        } else if (part_fx_.enabled() && piano_voice) {
          // Keep the raw string contribution in the per-part stage. The full
          // body return is added below in one operation, matching the legacy
          // unbussed piano arithmetic while still giving a bussed part a
          // complete pre-rig return.
          chunk_part_piano_l_[static_cast<size_t>(part_index)][static_cast<size_t>(c)] += voice_l;
          chunk_part_piano_r_[static_cast<size_t>(part_index)][static_cast<size_t>(c)] += voice_r;
        } else if (part_fx_.enabled() && halo_voice && part_halo_armed_[part_index]) {
          mix_l += voice_l;
          mix_r += voice_r;
          chunk_part_guitar_l_[part_index][static_cast<size_t>(c)] += voice_l;
          chunk_part_guitar_r_[part_index][static_cast<size_t>(c)] += voice_r;
        } else if (piano_voice) {
          piano_l += voice_l;
          piano_r += voice_r;
        } else {
          mix_l += voice_l;
          mix_r += voice_r;
          if (guitar_halo_active_ && halo_voice) {
            guitar_l += voice_l;
            guitar_r += voice_r;
          }
        }
        if (source_render) {
          const float src_l = voice_l * config_.gain;
          const float src_r = voice_r * config_.gain;
          add_output(target_for(v.source_track_id), i, src_l, src_r);
          // The remainder follows every source; the piano body and guitar halo
          // only the voices that drive them.
          residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
          if (piano_voice && !part_fx_.enabled()) {
            piano_residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
          }
          if (guitar_halo_active_ && halo_voice && !part_fx_.enabled()) {
            guitar_residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
          }
          if (part_fx_.enabled() && piano_voice) {
            part_piano_splitters_[part_index].accumulate(v.source_track_id, src_l, src_r);
          }
          if (organ_voice) {
            part_organ_splitters_[part_index].accumulate(v.source_track_id, src_l, src_r);
          }
          if (part_fx_.enabled() && halo_voice && part_halo_armed_[part_index]) {
            part_guitar_splitters_[part_index].accumulate(v.source_track_id, src_l, src_r);
          }
        }
      }
      // Part-owned body returns form before the part chain; unbussed parts add only the residual.
      for (size_t part = 0; part < 16; ++part) {
        const bool bussed_part = any_bussed && fx->part_bussed[part];

        // A swell box is a part pre-rig stage even when the factory is absent.
        // This keeps a configured pipe-organ render on its historical path,
        // while its source residual remains isolated from other parts.
        const float organ_raw_l = chunk_part_organ_l_[part][static_cast<size_t>(c)];
        const float organ_raw_r = chunk_part_organ_r_[part][static_cast<size_t>(c)];
        float organ_out_l = organ_raw_l;
        float organ_out_r = organ_raw_r;
        if (organ_part_params_[part].armed && part_swell_depth_[part] > 0.0f) {
          if (part_swell_active[part]) {
            part_swell_lp_l_[part] +=
                part_swell_coeff_[part] * (organ_raw_l - part_swell_lp_l_[part]);
            part_swell_lp_r_[part] +=
                part_swell_coeff_[part] * (organ_raw_r - part_swell_lp_r_[part]);
            organ_out_l = part_swell_lp_l_[part];
            organ_out_r = part_swell_lp_r_[part];
          } else {
            // Track the live input while the shutter is effectively open. If
            // CC11 closes on a held note, the lowpass therefore starts at the
            // current sample instead of a stale zero and cannot click down.
            part_swell_lp_l_[part] = organ_raw_l;
            part_swell_lp_r_[part] = organ_raw_r;
          }
        }
        const float organ_res_l = organ_out_l - organ_raw_l;
        const float organ_res_r = organ_out_r - organ_raw_r;
        chunk_part_organ_l_[part][static_cast<size_t>(c)] = organ_res_l;
        chunk_part_organ_r_[part][static_cast<size_t>(c)] = organ_res_r;
        if (bussed_part) {
          if (fx->mono_prefix[part] != 0) {
            part_fx_.add_mono(static_cast<int>(part), c, 0.5f * (organ_out_l + organ_out_r));
          } else {
            part_fx_.add_stereo(static_cast<int>(part), c, organ_out_l, organ_out_r);
          }
        } else {
          mix_l += organ_res_l;
          mix_r += organ_res_r;
        }

        if (part_fx_.enabled() && part_piano_state_[part].prepared) {
          const float piano_raw_l = chunk_part_piano_l_[part][static_cast<size_t>(c)];
          const float piano_raw_r = chunk_part_piano_r_[part][static_cast<size_t>(c)];
          PianoSoundboard& board = part_soundboards_[part];
          PianoResonanceBank& resonance = part_resonance_[part];
          const float body = board.process(0.5f * (piano_raw_l + piano_raw_r));
          const float side = board.last_side();
          const float symp = resonance.process(board.last_diffused(), channels_[part].sustain);
          const float piano_out_l = kPianoDirectGain * piano_raw_l + body + side + symp;
          const float piano_out_r = kPianoDirectGain * piano_raw_r + body - side + symp;
          const float piano_res_l = piano_out_l - piano_raw_l;
          const float piano_res_r = piano_out_r - piano_raw_r;
          chunk_part_piano_l_[part][static_cast<size_t>(c)] = piano_res_l;
          chunk_part_piano_r_[part][static_cast<size_t>(c)] = piano_res_r;
          if (bussed_part) {
            if (fx->mono_prefix[part] != 0) {
              part_fx_.add_mono(static_cast<int>(part), c, 0.5f * (piano_out_l + piano_out_r));
            } else {
              part_fx_.add_stereo(static_cast<int>(part), c, piano_out_l, piano_out_r);
            }
          } else {
            mix_l += piano_out_l;
            mix_r += piano_out_r;
          }
        }

        if (part_fx_.enabled() && part_halo_armed_[part]) {
          const float guitar_raw_l = chunk_part_guitar_l_[part][static_cast<size_t>(c)];
          const float guitar_raw_r = chunk_part_guitar_r_[part][static_cast<size_t>(c)];
          const float halo = part_halo_[part].process(0.5f * (guitar_raw_l + guitar_raw_r),
                                                      channels_[part].sustain,
                                                      halo_held[part] || channels_[part].sustain);
          chunk_part_guitar_l_[part][static_cast<size_t>(c)] = halo;
          chunk_part_guitar_r_[part][static_cast<size_t>(c)] = halo;
          if (bussed_part) {
            if (fx->mono_prefix[part] != 0) {
              part_fx_.add_mono(static_cast<int>(part), c,
                                0.5f * (guitar_raw_l + guitar_raw_r) + halo);
            } else {
              part_fx_.add_stereo(static_cast<int>(part), c, guitar_raw_l + halo,
                                  guitar_raw_r + halo);
            }
          } else {
            mix_l += halo;
            mix_r += halo;
          }
        }
      }

      chunk_mix_l_[static_cast<size_t>(c)] = mix_l;
      chunk_mix_r_[static_cast<size_t>(c)] = mix_r;
      chunk_piano_l_[static_cast<size_t>(c)] = piano_l;
      chunk_piano_r_[static_cast<size_t>(c)] = piano_r;
      chunk_guitar_l_[static_cast<size_t>(c)] = guitar_l;
      chunk_guitar_r_[static_cast<size_t>(c)] = guitar_r;
    }

    if (source_render && any_bussed) {
      for (size_t part = 0; part < 16; ++part) {
        if (!fx->part_bussed[part]) continue;
        const uint8_t unit = fx->part_unit[part];
        SourceResidualSplitter& destination = unit != PartFxSnapshot::kNoUnit && any_unit
                                                  ? unit_splitters_[unit]
                                                  : part_bus_splitters_[part];
        const auto bridge = [&](SourceResidualSplitter& owners, const float* l, const float* r) {
          float energy = 0.0f;
          for (int c = 0; c < n; ++c) {
            const float scaled_l = l[c] * config_.gain;
            const float scaled_r = r[c] * config_.gain;
            energy += scaled_l * scaled_l + scaled_r * scaled_r;
          }
          // Learn/decay once even while bussed. Only the downstream bus writes
          // audio; this pass transfers the body's source ownership.
          owners.flush(source_outputs, source_output_count, n, l, r, offset,
                       [](float* const*, int, float, float) {});
          destination.accumulate_residual_energy(owners, energy);
        };
        bridge(part_piano_splitters_[part], chunk_part_piano_l_[part].data(),
               chunk_part_piano_r_[part].data());
        bridge(part_organ_splitters_[part], chunk_part_organ_l_[part].data(),
               chunk_part_organ_r_[part].data());
        bridge(part_guitar_splitters_[part], chunk_part_guitar_l_[part].data(),
               chunk_part_guitar_r_[part].data());
      }
    }

    // The rig chains, then each bussed part into its insertion unit or straight
    // to the mix, ahead of the master gain.
    if (any_bussed) {
      if (any_unit) part_fx_.clear_unit_buses();
      part_fx_.run_part_chains(n, fx->part_bussed, fx->mono_prefix, *this);
      for (int part = 0; part < 16; ++part) {
        if (!fx->part_bussed[static_cast<size_t>(part)]) continue;
        const float* bus_l = part_fx_.bus_l(part);
        const float* bus_r = part_fx_.bus_r(part);
        const uint8_t unit = fx->part_unit[static_cast<size_t>(part)];
        if (unit != PartFxSnapshot::kNoUnit && any_unit) {
          float* unit_l = part_fx_.unit_bus_l(unit);
          float* unit_r = part_fx_.unit_bus_r(unit);
          for (int c = 0; c < n; ++c) {
            unit_l[c] += bus_l[c];
            unit_r[c] += bus_r[c];
          }
          continue;
        }
        for (int c = 0; c < n; ++c) {
          chunk_mix_l_[static_cast<size_t>(c)] += bus_l[c];
          chunk_mix_r_[static_cast<size_t>(c)] += bus_r[c];
        }
        if (source_render) {
          flush_bus(part_bus_splitters_[static_cast<size_t>(part)], bus_l, bus_r, n, offset);
        }
      }
      if (any_unit) {
        // One pass per unit over the sum of the parts feeding it.
        part_fx_.run_units(n, fx->unit_fed, nullptr);
        for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
          if (!fx->unit_fed[unit]) continue;
          const float* unit_l = part_fx_.unit_bus_l(unit);
          const float* unit_r = part_fx_.unit_bus_r(unit);
          for (int c = 0; c < n; ++c) {
            chunk_mix_l_[static_cast<size_t>(c)] += unit_l[c];
            chunk_mix_r_[static_cast<size_t>(c)] += unit_r[c];
          }
          if (source_render) flush_bus(unit_splitters_[unit], unit_l, unit_r, n, offset);
        }
      }
    }

    // Routing can leave a bus unused for many chunks. Keep its ownership
    // clock running so a later source does not inherit frozen old weights.
    if (source_render) {
      static constexpr std::array<float, kPartFxChunkFrames> silence{};
      const auto age = [&](SourceResidualSplitter& splitter) {
        splitter.flush(source_outputs, source_output_count, n, silence.data(), silence.data(),
                       offset, [](float* const*, int, float, float) {});
      };
      for (size_t part = 0; part < 16; ++part) {
        const bool flushed = any_bussed && fx->part_bussed[part] &&
                             (!any_unit || fx->part_unit[part] == PartFxSnapshot::kNoUnit);
        if (!flushed) age(part_bus_splitters_[part]);
      }
      for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
        if (!any_unit || !fx->unit_fed[unit]) age(unit_splitters_[unit]);
      }
    }

    // Second pass: the bus-level post stages, in the order they always ran.
    for (int c = 0; c < n; ++c) {
      const int i = offset + c;
      const size_t at = static_cast<size_t>(c);
      float mix_l = chunk_mix_l_[at] * config_.gain;
      float mix_r = chunk_mix_r_[at] * config_.gain;
      const float piano_raw_l = chunk_piano_l_[at];
      const float piano_raw_r = chunk_piano_r_[at];
      const float piano_l = piano_raw_l * config_.gain;
      const float piano_r = piano_raw_r * config_.gain;
      const float guitar_raw_l = chunk_guitar_l_[at];
      const float guitar_raw_r = chunk_guitar_r_[at];
      // Per-part residuals are already in the mix; flush each through its own source splitter.
      float part_piano_res_l = 0.0f;
      float part_piano_res_r = 0.0f;
      float part_organ_res_l = 0.0f;
      float part_organ_res_r = 0.0f;
      float part_guitar_res_l = 0.0f;
      float part_guitar_res_r = 0.0f;
      for (size_t part = 0; part < 16; ++part) {
        if (any_bussed && fx->part_bussed[part]) {
          chunk_part_piano_l_[part][at] = 0.0f;
          chunk_part_piano_r_[part][at] = 0.0f;
          chunk_part_organ_l_[part][at] = 0.0f;
          chunk_part_organ_r_[part][at] = 0.0f;
          chunk_part_guitar_l_[part][at] = 0.0f;
          chunk_part_guitar_r_[part][at] = 0.0f;
          continue;
        }
        const float piano_component_l = chunk_part_piano_l_[part][at] * config_.gain;
        const float piano_component_r = chunk_part_piano_r_[part][at] * config_.gain;
        const float organ_component_l = chunk_part_organ_l_[part][at] * config_.gain;
        const float organ_component_r = chunk_part_organ_r_[part][at] * config_.gain;
        const float guitar_component_l = chunk_part_guitar_l_[part][at] * config_.gain;
        const float guitar_component_r = chunk_part_guitar_r_[part][at] * config_.gain;
        part_piano_res_l += piano_component_l;
        part_piano_res_r += piano_component_r;
        part_organ_res_l += organ_component_l;
        part_organ_res_r += organ_component_r;
        part_guitar_res_l += guitar_component_l;
        part_guitar_res_r += guitar_component_r;
        if (source_render) {
          chunk_part_piano_l_[part][at] = piano_component_l;
          chunk_part_piano_r_[part][at] = piano_component_r;
          chunk_part_organ_l_[part][at] = organ_component_l;
          chunk_part_organ_r_[part][at] = organ_component_r;
          chunk_part_guitar_l_[part][at] = guitar_component_l;
          chunk_part_guitar_r_[part][at] = guitar_component_r;
        }
      }
      const float dry_l = mix_l + piano_l - part_piano_res_l - part_organ_res_l - part_guitar_res_l;
      const float dry_r = mix_r + piano_r - part_piano_res_r - part_organ_res_r - part_guitar_res_r;
      // Shared modal soundboard plus pedal-gated sympathetic resonance, both
      // driven by the summed dry piano mix. The sympathetic bank returns to the
      // centre; the board does not, because its two radiation paths differ. Runs
      // independently of the halo below them, since GM can voice a piano and a
      // guitar together.
      // What the piano body and the guitar halo add beyond their dry voices.
      float piano_res_l = 0.0f;
      float piano_res_r = 0.0f;
      float guitar_res_l = 0.0f;
      float guitar_res_r = 0.0f;
      if (piano_body_active_ && !part_fx_.enabled()) {
        const float drive_l = piano_raw_l;
        const float drive_r = piano_raw_r;
        // Radiation split: the board returns the phase-diffused complement of
        // the direct share (plus the modal colour), so most of the note reaches
        // the mix through the board rather than as the raw string waveform.
        const float dry_mono = 0.5f * (drive_l + drive_r);
        const float body = soundboard_.process(dry_mono);
        // The board's two radiation paths differ in phase, so its return is not
        // the same signal on both legs. Zero at a zero board width.
        const float side = soundboard_.last_side();
        const float symp = resonance_.process(soundboard_.last_diffused(), damper_open);
        const float piano_out_l =
            config_.gain * (kPianoDirectGain * piano_raw_l + body + side + symp);
        const float piano_out_r =
            config_.gain * (kPianoDirectGain * piano_raw_r + body - side + symp);
        piano_res_l = piano_out_l - piano_l;
        piano_res_r = piano_out_r - piano_r;
        mix_l += piano_out_l;
        mix_r += piano_out_r;
      }
      if (guitar_halo_active_ && !part_fx_.enabled()) {
        // Plucked-string sound halo: the open strings ring behind the note,
        // gated by damper_open exactly as the piano board is above -- a guitar's
        // open strings are damped unless the sustain pedal is holding them open.
        // Driven by guitar_l/guitar_r alone (the halo-eligible voices' own dry
        // mix), never by the rest of the GM bus. Skipped entirely when no
        // eligible voice has ever sounded, so every existing KS voicing with no
        // halo renders bit-identically.
        const float dry_mono = 0.5f * (guitar_raw_l + guitar_raw_r);
        const float symp = config_.gain * guitar_halo_.process(dry_mono, damper_open,
                                                               any_halo_held || damper_open);
        guitar_res_l = symp;
        guitar_res_r = symp;
        mix_l += symp;
        mix_r += symp;
      }
      // Gentle gain-neutral bus saturation (glue), then the DC blocker — the
      // physical-model voices can carry a small DC component.
      if (bus_drive_gain_ > 0.0f) {
        mix_l = std::tanh(bus_drive_gain_ * mix_l) / bus_drive_gain_;
        mix_r = std::tanh(bus_drive_gain_ * mix_r) / bus_drive_gain_;
      }
      // Scrub any non-finite bus sample before it reaches the DC blocker: a single
      // NaN/Inf reaching dc_x1_/dc_y1_ would persist in the IIR state and poison
      // every subsequent sample for the whole render. Bit-identical for finite
      // input, which is not the same as safe for it -- the blocker is a feedback
      // cell whose worst-case gain is well over unity, so a large enough finite
      // sample still leaves float range. Mirrors the host-side scrub in
      // au_instrument_provider.
      discarded |= resolve_non_finite(SampleDestination::kRecursiveState, mix_l);
      discarded |= resolve_non_finite(SampleDestination::kRecursiveState, mix_r);
      if (config_.dc_block) {
        const float l = mix_l - dc_x1_[0] + dc_r_ * dc_y1_[0];
        dc_x1_[0] = mix_l;
        dc_y1_[0] = l;
        mix_l = l;
        const float r = mix_r - dc_x1_[1] + dc_r_ * dc_y1_[1];
        dc_x1_[1] = mix_r;
        dc_y1_[1] = r;
        mix_r = r;
      }
      if (source_render) {
        // Shared bodies, bus drive and DC filtering are destination-scoped; their
        // residual (mix minus dry, where a bus output counts as dry) is split in
        // three components, each across the sources that produced it. With one
        // live source each split adds its share unmultiplied, so that source's
        // target is bit-identical to a slot-0-only render; with several it is
        // exact only up to floating-point rounding. Staged per chunk and flushed
        // at its end.
        piano_residual_l_[at] = piano_res_l;
        piano_residual_r_[at] = piano_res_r;
        guitar_residual_l_[at] = guitar_res_l;
        guitar_residual_r_[at] = guitar_res_r;
        residual_l_[at] = (mix_l - dry_l) - piano_res_l - guitar_res_l - part_piano_res_l -
                          part_organ_res_l - part_guitar_res_l;
        residual_r_[at] = (mix_r - dry_r) - piano_res_r - guitar_res_r - part_piano_res_r -
                          part_organ_res_r - part_guitar_res_r;
      } else {
        if (left != nullptr) {
          // Mono host: fold both pan legs so centre-panned voices keep level.
          left[i] += mono ? constants::kInvSqrt2 * (mix_l + mix_r) : mix_l;
        }
        if (right != nullptr) right[i] += mix_r;
        // Fan a mono fold-down to any additional channels.
        for (int ch = 2; ch < num_channels; ++ch) {
          if (channels[ch] != nullptr) {
            channels[ch][i] += constants::kInvSqrt2 * (mix_l + mix_r);
          }
        }
      }
    }
    if (source_render) {
      residual_splitter_.flush(source_outputs, source_output_count, n, residual_l_.data(),
                               residual_r_.data(), offset, add_output);
      piano_residual_splitter_.flush(source_outputs, source_output_count, n,
                                     piano_residual_l_.data(), piano_residual_r_.data(), offset,
                                     add_output);
      guitar_residual_splitter_.flush(source_outputs, source_output_count, n,
                                      guitar_residual_l_.data(), guitar_residual_r_.data(), offset,
                                      add_output);
      // A factory-backed body's unbussed return was added to the common mix
      // after its dry source write. Attribute that return to the same part's
      // sources, while bussed returns have already been flushed through the
      // part or unit splitter above.
      for (size_t part = 0; part < 16; ++part) {
        if (any_bussed && fx->part_bussed[part]) continue;
        part_piano_splitters_[part].flush(source_outputs, source_output_count, n,
                                          chunk_part_piano_l_[part].data(),
                                          chunk_part_piano_r_[part].data(), offset, add_output);
        part_organ_splitters_[part].flush(source_outputs, source_output_count, n,
                                          chunk_part_organ_l_[part].data(),
                                          chunk_part_organ_r_[part].data(), offset, add_output);
        part_guitar_splitters_[part].flush(source_outputs, source_output_count, n,
                                           chunk_part_guitar_l_[part].data(),
                                           chunk_part_guitar_r_[part].data(), offset, add_output);
      }
    }
  }
  if (any_bussed && part_fx_.discard_sum(nullptr) != fx_discards_before) discarded = true;
  if (discarded) note_non_finite_discard();
}

}  // namespace sonare::midi::synth
