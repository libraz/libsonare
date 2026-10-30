#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "midi/builtin_synth.h"
#include "midi/control_value.h"
#include "midi/controller_numbers.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "util/dsp_primitives.h"

namespace sonare::midi::synth {

namespace {

constexpr size_t kDirectFxBase = 0;
constexpr size_t kDirectEqBase = kGsSystemEffectFieldCount;
constexpr size_t kDirectPartEqBase = kDirectEqBase + 4;

constexpr std::array<uint8_t GsSystemEffects::*, kGsSystemEffectFieldCount> kDirectFxMembers{{
#define SONARE_GS_DIRECT_MEMBER(name, value) &GsSystemEffects::name,
    SONARE_GS_SYSTEM_EFFECT_FIELDS(SONARE_GS_DIRECT_MEMBER)
#undef SONARE_GS_DIRECT_MEMBER
}};

// A system-effect parameter indexes kDirectFxMembers by its distance from kReverbMacro.
static_assert(static_cast<size_t>(GsParam::kDelaySendToReverb) -
                      static_cast<size_t>(GsParam::kReverbMacro) + 1 ==
                  kGsSystemEffectFieldCount,
              "the system-effect GsParam run must match SONARE_GS_SYSTEM_EFFECT_FIELDS");

// Each field is one byte, so a field's offset is its kDirectFxMembers index.
constexpr size_t kDirectChorusFirst = offsetof(GsSystemEffects, chorus_macro);
constexpr size_t kDirectDelayFirst = offsetof(GsSystemEffects, delay_macro);

}  // namespace

bool Sf2Player::append_direct_gs_node(const uint8_t* data, size_t size,
                                      std::shared_ptr<const PreparedMidiSysEx> prepared,
                                      bool legacy_full_snapshot) noexcept {
  if (data == nullptr || size == 0) return false;
  try {
    sweep_direct_gs_nodes();
    auto node = std::make_unique<DirectGsNode>();
    node->seq = direct_queue_->next_seq++;
    node->size = size;
    node->bytes = std::make_shared<const std::vector<uint8_t>>(data, data + size);
    node->prepared_owner = std::move(prepared);
    node->prepared_raw = node->prepared_owner != nullptr
                             ? dynamic_cast<const PreparedSysEx*>(node->prepared_owner.get())
                             : nullptr;
    node->legacy_full_snapshot = legacy_full_snapshot;
    DirectGsNode* raw = node.get();
    direct_queue_->owned.push_back(std::move(node));
    direct_queue_->control_tail->next.store(raw, std::memory_order_release);
    direct_queue_->control_tail = raw;
    direct_queue_->published_tail.store(raw, std::memory_order_release);
    return true;
  } catch (...) {
    return false;
  }
}

void Sf2Player::sweep_direct_gs_nodes() noexcept {
  const uint64_t consumed = direct_queue_->consumed_seq.load(std::memory_order_acquire);
  const uint64_t retired = direct_queue_->retired_seq.load(std::memory_order_acquire);
  for (auto it = direct_queue_->owned.begin(); it != direct_queue_->owned.end();) {
    DirectGsNode* node = it->get();
    if (node->seq <= consumed) {
      node->prepared_owner.reset();
      node->prepared_raw = nullptr;
    }
    if (node->seq <= retired) {
      it = direct_queue_->owned.erase(it);
    } else {
      ++it;
    }
  }
}

void Sf2Player::clear_direct_gs_queue() noexcept {
  // Quiescent boundary only: the audio consumer has stopped.
  direct_queue_->owned.clear();
  direct_queue_->stub.next.store(nullptr, std::memory_order_relaxed);
  direct_queue_->control_tail = &direct_queue_->stub;
  direct_queue_->audio_head = &direct_queue_->stub;
  direct_queue_->published_tail.store(&direct_queue_->stub, std::memory_order_release);
  direct_queue_->consumed_seq.store(0, std::memory_order_release);
  direct_queue_->retired_seq.store(0, std::memory_order_release);
  direct_queue_->next_seq = 1;
}

void Sf2Player::adopt_legacy_direct_snapshot() noexcept {
  // Custom DSP without a prepared plan uses the full-snapshot publication.
  part_fx_.acquire();
  const PartFxSnapshot* snapshot = part_fx_.current();
  if (snapshot == nullptr) return;
  if (prepared_runtime_active_) {
    // Drop the EFX overlay but keep the system/EQ state already applied.
    const GsSystemEffects saved_sys_fx = prepared_sys_fx_;
    const GsMasterEq saved_master_eq = prepared_master_eq_;
    const std::array<bool, 16> saved_eq_part_bypassed = prepared_eq_part_bypassed_;
    clear_prepared_audio_state();
    prepared_sys_fx_ = saved_sys_fx;
    prepared_master_eq_ = saved_master_eq;
    prepared_eq_part_bypassed_ = saved_eq_part_bypassed;
    apply_gs_system_state(prepared_sys_fx_, prepared_master_eq_, prepared_eq_part_bypassed_);
  }
  prepared_efx_ = snapshot->gs_efx_state;
  prepared_assign_ = snapshot->gs_part_assign;
}

void Sf2Player::drain_direct_gs_nodes() noexcept {
  DirectGsNode* stop = direct_queue_->published_tail.load(std::memory_order_acquire);
  while (direct_queue_->audio_head != stop) {
    DirectGsNode* old = direct_queue_->audio_head;
    DirectGsNode* next = old->next.load(std::memory_order_acquire);
    if (next == nullptr) break;
    if (next->restart_domain != 0) {
      restart_prepared_audio_runtime(next->restart_domain);
    } else if (next->legacy_full_snapshot) {
      adopt_legacy_direct_snapshot();
    } else if (next->prepared_raw != nullptr && next->bytes != nullptr) {
      // Direct deltas update and activate the audio-owned overlay.
      apply_prepared_gs_delta(*next->prepared_raw, next->bytes->data(), next->size, false);
    }
    direct_queue_->audio_head = next;
    direct_queue_->consumed_seq.store(next->seq, std::memory_order_release);
    direct_queue_->retired_seq.store(old->seq, std::memory_order_release);
  }
}

void Sf2Player::publish_direct_system_patch(bool reset, const uint8_t* data, size_t size) noexcept {
  const uint64_t seq = ++direct_system_patch_control_.publish_seq;
  if (reset) direct_system_patch_control_.last_reset_seq = seq;
  auto stamp = [&](size_t index, uint8_t value) {
    if (index >= direct_system_patch_control_.fields.size()) return;
    direct_system_patch_control_.fields[index] = {seq, value};
  };
  auto stamp_fx = [&](size_t index) {
    stamp(kDirectFxBase + index, sys_fx_.*kDirectFxMembers[index]);
  };
  auto stamp_fx_range = [&](size_t first, size_t count) {
    for (size_t i = 0; i < count && first + i < kDirectFxMembers.size(); ++i) stamp_fx(first + i);
  };
  auto stamp_eq = [&](size_t index, uint8_t value) { stamp(kDirectEqBase + index, value); };
  auto stamp_part_eq = [&](size_t part) {
    stamp(kDirectPartEqBase + (part & 0x0Fu), eq_part_bypassed_[part & 0x0Fu] ? 1 : 0);
  };

  if (!reset) {
    gs_for_each_sysex_write(data, size, [&](const GsWrite& w) noexcept {
      const GsAddressEntry* entry = gs_lookup_address(w.addr);
      if (entry == nullptr || !gs_value_in_range(*entry, w.value)) return false;
      const size_t param = static_cast<size_t>(w.param);
      const size_t first_param = static_cast<size_t>(GsParam::kReverbMacro);
      const size_t last_param = static_cast<size_t>(GsParam::kDelaySendToReverb);
      if (param >= first_param && param <= last_param) {
        const size_t fx_index = param - first_param;
        if (w.param == GsParam::kReverbMacro)
          stamp_fx_range(0, kDirectChorusFirst);
        else if (w.param == GsParam::kChorusMacro)
          stamp_fx_range(kDirectChorusFirst, kDirectDelayFirst - kDirectChorusFirst);
        else if (w.param == GsParam::kDelayMacro)
          stamp_fx_range(kDirectDelayFirst, kGsSystemEffectFieldCount - kDirectDelayFirst);
        else
          stamp_fx(fx_index);
      } else {
        switch (w.param) {
          case GsParam::kEqLowFreq:
            stamp_eq(0, master_eq_.low_freq);
            break;
          case GsParam::kEqLowGain:
            stamp_eq(1, master_eq_.low_gain);
            break;
          case GsParam::kEqHighFreq:
            stamp_eq(2, master_eq_.high_freq);
            break;
          case GsParam::kEqHighGain:
            stamp_eq(3, master_eq_.high_gain);
            break;
          case GsParam::kPartEqSwitch:
            stamp_part_eq(w.part);
            break;
          default:
            return false;
        }
      }
      return true;
    });
  }
  direct_system_patch_->store(direct_system_patch_control_);
}

void Sf2Player::drain_direct_system_patch() noexcept {
  if (direct_system_patch_reader_ == nullptr) return;
  DirectSystemPatch patch;
  if (!direct_system_patch_reader_->try_load_into(&patch)) return;
  if (patch.publish_seq <= last_system_direct_seq_) return;
  bool changed = false;
  bool prepared_changed = false;
  const bool new_reset = patch.last_reset_seq > last_system_direct_seq_;
  const auto should_apply = [&](size_t index) {
    const DirectSystemField& field = patch.fields[index];
    // Written after the watermark, or older than a newly stamped reset.
    return field.seq > last_system_direct_seq_ || (new_reset && patch.last_reset_seq >= field.seq);
  };
  const auto value = [&](size_t index, uint8_t default_value) {
    const DirectSystemField& field = patch.fields[index];
    return patch.last_reset_seq >= field.seq ? default_value : field.value;
  };
  for (size_t i = 0; i < kDirectFxMembers.size(); ++i) {
    if (!should_apply(kDirectFxBase + i)) continue;
    const uint8_t next = value(kDirectFxBase + i, GsSystemEffects{}.*kDirectFxMembers[i]);
    if (direct_system_audio_fx_.*kDirectFxMembers[i] != next) changed = true;
    if (prepared_sys_fx_.*kDirectFxMembers[i] != next) prepared_changed = true;
    direct_system_audio_fx_.*kDirectFxMembers[i] = next;
    prepared_sys_fx_.*kDirectFxMembers[i] = next;
  }
  const GsMasterEq eq_default{};
  const uint8_t eq_values[4] = {eq_default.low_freq, eq_default.low_gain, eq_default.high_freq,
                                eq_default.high_gain};
  uint8_t* eq_fields[4] = {&direct_system_audio_eq_.low_freq, &direct_system_audio_eq_.low_gain,
                           &direct_system_audio_eq_.high_freq, &direct_system_audio_eq_.high_gain};
  uint8_t* prepared_eq_fields[4] = {&prepared_master_eq_.low_freq, &prepared_master_eq_.low_gain,
                                    &prepared_master_eq_.high_freq, &prepared_master_eq_.high_gain};
  for (size_t i = 0; i < 4; ++i) {
    if (!should_apply(kDirectEqBase + i)) continue;
    const uint8_t next = value(kDirectEqBase + i, eq_values[i]);
    if (*eq_fields[i] != next) changed = true;
    if (*prepared_eq_fields[i] != next) prepared_changed = true;
    *eq_fields[i] = next;
    *prepared_eq_fields[i] = next;
  }
  for (size_t part = 0; part < 16; ++part) {
    if (!should_apply(kDirectPartEqBase + part)) continue;
    const uint8_t next = value(kDirectPartEqBase + part, 0);
    const bool bypassed = next != 0;
    if (direct_system_audio_bypassed_[part] != bypassed) changed = true;
    if (prepared_eq_part_bypassed_[part] != bypassed) prepared_changed = true;
    direct_system_audio_bypassed_[part] = bypassed;
    prepared_eq_part_bypassed_[part] = bypassed;
  }
  last_system_direct_seq_ = patch.publish_seq;
  // The active overlay carries scheduled fields the direct mirror lacks, so it alone is applied.
  if (prepared_runtime_active_) {
    if (prepared_changed) {
      apply_gs_system_state(prepared_sys_fx_, prepared_master_eq_, prepared_eq_part_bypassed_);
    }
  } else if (changed) {
    apply_gs_system_state(direct_system_audio_fx_, direct_system_audio_eq_,
                          direct_system_audio_bypassed_);
  }
}

Sf2Player::DirectGsNode* Sf2Player::reserve_restart_node() {
  if (!prepared_) return nullptr;
  sweep_direct_gs_nodes();
  direct_queue_->owned.push_back(std::make_unique<DirectGsNode>());
  DirectGsNode* node = direct_queue_->owned.back().get();
  node->seq = direct_queue_->next_seq++;
  return node;
}

void Sf2Player::restart_prepared_runtime(DirectGsNode* reserved) noexcept {
  ++prepared_domain_;
  if (reserved != nullptr) {
    reserved->restart_domain = prepared_domain_;
    direct_queue_->control_tail->next.store(reserved, std::memory_order_release);
    direct_queue_->control_tail = reserved;
    direct_queue_->published_tail.store(reserved, std::memory_order_release);
  }
  release_prepared_nodes();
}

void Sf2Player::restart_prepared_audio_runtime(uint64_t domain) noexcept {
  prepared_audio_domain_ = domain;
  // The rebuilt snapshot was published before this node.
  part_fx_.acquire();
  const PartFxSnapshot* snapshot = part_fx_.current();
  for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
    activate_prepared_node(unit, nullptr);
    prepared_unit_overridden_[unit] = false;
    if (!prepared_runtime_active_) continue;
    const bool matching = snapshot != nullptr && snapshot->unit_fed[unit] &&
                          snapshot->gs_efx_state[unit].type == prepared_efx_[unit].type;
    if (matching) {
      part_fx_.apply_legacy_plan(unit, prepared_efx_[unit], prepared_control_part(unit), *this);
    } else {
      prepared_unit_overridden_[unit] = true;
    }
  }
  raise_prepared_fx_tail();
}

void Sf2Player::set_gs_efx_realization(GsEfxRealization realization) {
  if (part_fx_.realization() == realization) return;
  DirectGsNode* restart = reserve_restart_node();
  const GsEfxRealization previous = part_fx_.realization();
  const PartFxStage::Checkpoint saved = part_fx_.checkpoint();
  part_fx_.set_realization(realization);
  try {
    realize_gs_efx();
  } catch (...) {
    part_fx_.set_realization(previous);
    part_fx_.restore(saved);
    if (restart != nullptr) direct_queue_->owned.pop_back();
    throw;
  }
  restart_prepared_runtime(restart);
}

void Sf2Player::set_gs_efx_rows(const GsEfxRowView* rows) {
  if (part_fx_.rows() == rows) return;
  DirectGsNode* restart = reserve_restart_node();
  const GsEfxRowView* previous_rows = part_fx_.rows();
  const PartFxStage::Checkpoint saved = part_fx_.checkpoint();
  part_fx_.set_rows(rows);
  try {
    // A throwing factory leaves the published graph and its tokens usable.
    realize_gs_efx();
  } catch (...) {
    part_fx_.set_rows(previous_rows);
    part_fx_.restore(saved);
    if (restart != nullptr) direct_queue_->owned.pop_back();
    throw;
  }
  restart_prepared_runtime(restart);
}

void Sf2Player::on_control_sysex(const uint8_t* data, size_t size) noexcept {
  if (!prepared_ || data == nullptr || size == 0) return;
  const GsSysEx msg = parse_gs_sysex(data, size);
  const bool efx_assign_message = apply_gs_efx_assign_sysex(nullptr, data, size);
  const uint32_t efx_units = gs_efx_units_in_sysex(data, size);
  const bool efx_message = gs_sysex_resets(msg.kind) || efx_assign_message || efx_units != 0;
  uint32_t assignment_units = 0;
  if (efx_assign_message) {
    const std::array<uint8_t, 16>& current_assign = part_fx_.part_assign();
    gs_for_each_sysex_write(data, size, [&](const GsWrite& write) noexcept {
      if (!gs_efx_assign_accepted(write)) return false;
      const size_t part = write.part & 0x0Fu;
      const int old_unit = gs_efx_assign_unit(current_assign[part]);
      const int new_unit = gs_efx_assign_unit(write.value);
      if (old_unit >= 0) assignment_units |= uint32_t{1} << old_unit;
      if (new_unit >= 0) assignment_units |= uint32_t{1} << new_unit;
      return true;
    });
  }
  const uint32_t affected_units = efx_units | assignment_units;
  // A checkpoint, so a throwing rebuild under this noexcept hook leaves the mirror whole.
  const PartFxStage::Checkpoint mirror_before = part_fx_.checkpoint();
  const std::array<bool, kGsEfxUnitCount> fallback_before = direct_legacy_efx_fallback_;
  bool direct_rebuild_failed = false;
  const auto restore_direct_mirror = [&]() noexcept {
    part_fx_.restore(mirror_before);
    direct_legacy_efx_fallback_ = fallback_before;
  };
  std::shared_ptr<const PreparedMidiSysEx> prepared;
  bool prepared_ok = true;
  if (efx_message) {
    // The same plan as a scheduled event; a non-RT custom processor uses the full snapshot.
    try {
      prepared_ok = prepare_sysex(data, size, prepared);
    } catch (...) {
      prepared_ok = false;
      prepared.reset();
    }
  }
  // Only this hook rejects a plan, and only while a custom unit renders from the full snapshot.
  const PreparedSysEx* prepared_token =
      prepared != nullptr ? dynamic_cast<const PreparedSysEx*>(prepared.get()) : nullptr;
  uint32_t legacy_fallback_units = 0;
  for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
    if (direct_legacy_efx_fallback_[unit]) legacy_fallback_units |= uint32_t{1} << unit;
  }
  bool forced_legacy_direct = false;
  uint32_t legacy_candidate_replaces_units = 0;
  if (prepared_ok && prepared_token != nullptr && (affected_units & legacy_fallback_units) != 0) {
    // The overlay would replace an unrelated custom unit with a null node; stay
    // on the full snapshot only when this message reaches that unit.
    forced_legacy_direct = true;
    for (const PreparedEfxCandidate& candidate : prepared_token->candidates) {
      if (candidate.unit < kGsEfxUnitCount) {
        legacy_candidate_replaces_units |= uint32_t{1} << candidate.unit;
      }
    }
    prepared_ok = false;
    prepared.reset();
    prepared_token = nullptr;
  }
  if (efx_message) {
    if (prepared_ok) {
      // Update the mirror only; a rebuild would overwrite scheduled raw state and cut tails.
      part_fx_.mirror_sysex(data, size);
      switch (msg.kind) {
        case GsSysExKind::kGm1Reset:
        case GsSysExKind::kGm2Reset:
        case GsSysExKind::kGsReset:
          direct_legacy_efx_fallback_.fill(false);
          break;
        case GsSysExKind::kEfxPartSwitch:
          break;
        case GsSysExKind::kUseForRhythm:
        case GsSysExKind::kNone: {
          // A selected prepared node ends this unit's full-snapshot fallback.
          if (prepared_token != nullptr) {
            uint32_t candidate_units = 0;
            for (const PreparedEfxCandidate& candidate : prepared_token->candidates) {
              if (candidate.unit < kGsEfxUnitCount)
                candidate_units |= uint32_t{1} << candidate.unit;
            }
            for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
              if ((candidate_units & (uint32_t{1} << unit)) != 0)
                direct_legacy_efx_fallback_[unit] = false;
            }
          }
          break;
        }
      }
      part_fx_.clear_dirty();
    } else {
      if (!forced_legacy_direct) {
        for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
          if ((affected_units & (uint32_t{1} << unit)) != 0)
            direct_legacy_efx_fallback_[unit] = true;
        }
      }
      bool rebuilt = false;
      try {
        rebuilt = part_fx_.apply_control_sysex(data, size);
      } catch (...) {
        // Translation allocates; keep the previous generation and the mirror.
        restore_direct_mirror();
        direct_rebuild_failed = true;
      }
      if (rebuilt) {
        // A non-RT custom processor uses the direct full-snapshot publication.
        // Adopting it drops the overlay, so no unit may come back holding the
        // state it froze with while overridden.
        try {
          part_fx_.discard_unit_runtimes();
          realize_gs_efx();
        } catch (...) {
          restore_direct_mirror();
          direct_rebuild_failed = true;
        }
      }
      if (rebuilt && !direct_rebuild_failed) {
        for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
          if ((legacy_candidate_replaces_units & (uint32_t{1} << unit)) != 0)
            direct_legacy_efx_fallback_[unit] = false;
        }
      }
    }
  }
  if (efx_message && !direct_rebuild_failed) {
    const bool queued = append_direct_gs_node(data, size, std::move(prepared), !prepared_ok);
    if (!queued && prepared_ok) {
      // A node that could not be queued falls back to the full-snapshot publication.
      try {
        part_fx_.discard_unit_runtimes();
        realize_gs_efx();
        for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
          if ((affected_units & (uint32_t{1} << unit)) != 0)
            direct_legacy_efx_fallback_[unit] = true;
        }
      } catch (...) {
        restore_direct_mirror();
      }
    }
  }

  bool changed = gs_sysex_resets(msg.kind);
  if (changed) {
    sys_fx_ = {};
    master_eq_ = {};
    eq_part_bypassed_ = {};
  }
  changed = apply_gs_system_sysex(data, size) || changed;
  if (changed) publish_direct_system_patch(gs_sysex_resets(msg.kind), data, size);
}

void Sf2Player::on_prepared_sysex_accepted(const uint8_t* data, size_t size,
                                           const PreparedMidiSysEx* prepared) noexcept {
  // The audio event applies it at its frame; never mirror it here, whatever the token.
  (void)data;
  (void)size;
  (void)prepared;
}

}  // namespace sonare::midi::synth
