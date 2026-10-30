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
#include "midi/synth/channel_mapping.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "util/dsp_primitives.h"

namespace sonare::midi::synth {

namespace {

/// The control as efx_control_byte reads it: @p control's range with @p efx's raw bytes.
template <typename Control>
Sf2EfxControlRt effective_efx_control(const Control& control, const GsEfx& efx, size_t k,
                                      uint8_t source) noexcept {
  Sf2EfxControlRt effective;
  effective.source = source;
  effective.depth = efx.control_depth[k];
  effective.slot = control.slot;
  effective.lo = control.lo;
  effective.hi = control.hi;
  effective.states = control.states;
  effective.base_byte = efx.params[control.slot];
  return effective;
}

}  // namespace

float Sf2Player::part_controller_position(int part, uint8_t source) const noexcept {
  // Read at full width, as refresh_channel_mod reads the same controllers.
  const ChannelState& st = channels_[static_cast<size_t>(part & 0x0F)];
  if (source == kEfxSourceBend) return bend_bipolar(st.pitch_bend);
  if (source == kEfxSourceAftertouch) return st.channel_pressure.f7() / 127.0f;
  return st.cc_position[source & 0x7Fu].f7() / 127.0f;
}

PartFxUnitOverrides Sf2Player::prepared_unit_overrides() const noexcept {
  PartFxUnitOverrides out{};
  for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
    if (!prepared_unit_overridden_[unit]) continue;
    out[unit] = prepared_active_nodes_[unit] != nullptr ? &prepared_active_nodes_[unit]->unit_rt
                                                        : &prepared_empty_unit_;
  }
  return out;
}

int Sf2Player::prepared_control_part(size_t unit) const noexcept {
  for (size_t part = 0; part < prepared_part_unit_.size(); ++part) {
    if (prepared_part_unit_[part] == unit) return static_cast<int>(part);
  }
  return -1;
}

std::shared_ptr<Sf2Player::PreparedEfxNode> Sf2Player::find_or_build_prepared_node(size_t unit,
                                                                                   uint16_t type) {
  release_prepared_nodes();
  for (const std::shared_ptr<PreparedEfxNode>& existing : prepared_nodes_) {
    if (existing->domain == prepared_domain_ && existing->unit == unit && existing->type == type) {
      return existing;
    }
  }
  if (unit >= kGsEfxUnitCount || type == 0 || gs_efx_type_defaults(type) == nullptr) {
    return nullptr;
  }

  GsEfx defaults;
  defaults.type = type;
  defaults.type_msb = static_cast<uint8_t>(type >> 8);
  defaults.assigned = true;
  defaults.params = gs_efx_type_defaults(type)->params;
  const GsEfxRealization realization = part_fx_.realization();
  auto node = std::make_shared<PreparedEfxNode>();
  node->domain = prepared_domain_;
  node->unit = static_cast<uint8_t>(unit);
  node->type = type;
  node->unit_rt = part_fx_.build_unit(defaults);
  node->stage_count = static_cast<uint8_t>(std::min<size_t>(node->unit_rt.stages.size(), 255));
  for (size_t s = 0; s < std::min(node->unit_rt.stages.size(), node->default_enabled.size()); ++s) {
    // All on: the enable rules alone carry selector/switch state.
    node->default_enabled[s] = true;
  }

  const GsEfxRowView& rows = part_fx_.row_view();
  const uint16_t row_type = gs_efx_binding_type(rows, type);

  // A null stage is a no-DSP hole; a built one must take every row realtime-safely.
  if (realization == GsEfxRealization::kModern) {
    for (size_t i = 0; i < rows.n_rows; ++i) {
      const GsEfxBindingRow& row = rows.rows[i];
      if (row.type != row_type) continue;
      const Sf2EfxRowTarget target = sf2_resolve_efx_row(node->unit_rt, row);
      if (target.status == Sf2EfxRowResolution::kNoStage ||
          target.status == Sf2EfxRowResolution::kNoProcessor) {
        continue;
      }
      if (target.status != Sf2EfxRowResolution::kResolved) return nullptr;
      if (node->param_dest_count >= node->param_dests.size()) return nullptr;
      PreparedEfxParamDest& dest = node->param_dests[node->param_dest_count++];
      dest.row = row;
      dest.stage_index = static_cast<uint8_t>(target.stage_index);
      dest.param_id = target.param_id;
      dest.transform = Sf2EfxValueTransform::kBinding;
      if (target.companion_stage_index >= 0) {
        if (node->param_dest_count >= node->param_dests.size()) return nullptr;
        PreparedEfxParamDest& companion = node->param_dests[node->param_dest_count++];
        companion.row = row;
        companion.stage_index = static_cast<uint8_t>(target.companion_stage_index);
        companion.param_id = target.companion_param_id;
        companion.transform = target.companion_transform;
      }
    }
    for (size_t i = 0; i < rows.n_enables; ++i) {
      const GsEfxEnable& enable = rows.enables[i];
      if (enable.type != row_type) continue;
      if (node->enable_plan_count >= node->enable_plans.size()) return nullptr;
      PreparedEfxEnablePlan& plan = node->enable_plans[node->enable_plan_count++];
      plan.rule = enable;
      plan.stage_indices.fill(0xFF);
      for (uint8_t s = 0; s < enable.n_stages && s < plan.stage_indices.size(); ++s) {
        if (enable.stages[s] >= kGsEfxRowStages.size()) return nullptr;
        const int stage_index = sf2_find_efx_stage(node->unit_rt, kGsEfxRowStages[enable.stages[s]],
                                                   enable.ordinals[s]);
        if (stage_index >= 0) plan.stage_indices[s] = static_cast<uint8_t>(stage_index);
      }
    }
  } else if (!node->unit_rt.stages.empty() && node->unit_rt.stages.front().proc != nullptr) {
    const rt::ProcessorBase* proc = node->unit_rt.stages.front().proc.get();
    for (size_t slot = 0; slot < 20; ++slot) {
      if (!proc->parameter_is_realtime_safe(static_cast<unsigned int>(slot))) return nullptr;
    }
  }

  // Freeze the CONTROL fan-out, every marked row for the slot, with the node.
  for (size_t k = 0; k < node->controls.size(); ++k) {
    const Sf2EfxLegacyControlPlan plan = sf2_resolve_efx_control(node->unit_rt, rows, type, k);
    if (plan.n_dest == 0) continue;
    Sf2EfxControlRt& control = node->controls[k];
    control.slot = plan.slot;
    control.lo = plan.lo;
    control.hi = plan.hi;
    control.states = plan.states;
    control.n_dest = plan.n_dest;
    control.dest = plan.dest;
    control.base_byte = defaults.params[control.slot];
    control.applied_byte = control.base_byte;
  }

  prepared_nodes_.push_back(node);
  return node;
}

bool Sf2Player::prepare_sysex(const uint8_t* data, size_t size,
                              std::shared_ptr<const PreparedMidiSysEx>& out) {
  out.reset();
  if (data == nullptr || size == 0) return false;
  // Unprepared: a null token; the engine re-prepares every clip SysEx at prepare().
  if (!prepared_) return true;
  // A moved-from player lost its identity; only this CONTROL path recreates it.
  if (prepared_owner_identity_ == nullptr) {
    prepared_owner_identity_ = std::make_shared<PreparedOwnerIdentity>();
  }

  // bad_alloc propagates: the engine reports it apart from a missing binding.
  try {
    auto token = std::make_shared<PreparedSysEx>();
    token->owner_identity = prepared_owner_identity_;
    token->domain = prepared_domain_;

    const uint32_t efx_units = gs_efx_units_in_sysex(data, size);
    token->efx_units = efx_units;
    if (efx_units != 0) {
      // Type state is discovered in wire order. An MSB-only write updates the
      // current pairing for its unit, while the MSB that resolved the final LSB
      // is retained separately so a later alias cannot rewrite that resolution.
      token->candidates.reserve(kGsEfxUnitCount);
      const auto add_candidate = [&](uint8_t unit, uint16_t type) {
        // Type 0000 is the measured Thru/default state. Unknown types and Thru
        // both intentionally carry no prepared node; the runtime selects an
        // empty override for them rather than rejecting the token.
        if (type == 0 || gs_efx_type_defaults(type) == nullptr) return true;
        for (const PreparedEfxCandidate& existing : token->candidates) {
          if (existing.unit == unit && existing.type == type) return true;
        }
        const std::shared_ptr<PreparedEfxNode> node = find_or_build_prepared_node(unit, type);
        if (node == nullptr) return false;

        PreparedEfxCandidate candidate;
        candidate.unit = unit;
        candidate.type = type;
        candidate.node = node.get();
        candidate.lease = node;
        token->candidates.push_back(std::move(candidate));
        return true;
      };

      struct EfxTypeScan final {
        bool have_msb = false;
        uint8_t current_msb = 0;
        bool have_lsb = false;
        uint8_t last_lsb = 0;
        bool last_lsb_had_msb = false;
        uint8_t last_lsb_msb = 0;
      };
      std::array<EfxTypeScan, kGsEfxUnitCount> type_scan{};
      const GsFrame frame = gs_sysex_frame(data, size);
      size_t cursor = 0;
      GsEfxBlockSlice slice;
      while (gs_next_efx_block_slice(frame, &cursor, &slice)) {
        std::array<GsWrite, kGsEfxBlockSize> writes{};
        const size_t decoded = std::min(
            gs_decode_writes(slice.frame, writes.data(), writes.size(), nullptr), writes.size());
        for (size_t i = 0; i < decoded; ++i) {
          const GsWrite& write = writes[i];
          if (write.param != GsParam::kEfxType) continue;
          const uint32_t bit = uint32_t{1} << slice.unit;
          if (write.index == 0) {
            type_scan[slice.unit].current_msb = write.value;
            type_scan[slice.unit].have_msb = true;
          } else if (write.index == 1) {
            token->full_reapply_units |= bit;
            type_scan[slice.unit].have_lsb = true;
            type_scan[slice.unit].last_lsb = write.value;
            type_scan[slice.unit].last_lsb_had_msb = type_scan[slice.unit].have_msb;
            type_scan[slice.unit].last_lsb_msb = type_scan[slice.unit].current_msb;
          }
        }
      }

      // Build only the final LSB resolution for each unit. A later MSB-only
      // alias changes current_msb but cannot retroactively change this type.
      for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
        const EfxTypeScan& scan = type_scan[unit];
        if ((efx_units & (uint32_t{1} << unit)) == 0 || !scan.have_lsb) continue;
        if (scan.last_lsb_had_msb) {
          const uint16_t type = static_cast<uint16_t>(
              (static_cast<uint16_t>(scan.last_lsb_msb) << 8) | scan.last_lsb);
          if (!add_candidate(static_cast<uint8_t>(unit), type)) {
            out.reset();
            return false;
          }
        } else {
          // With no MSB before the final LSB, every measured type carrying
          // that low byte is possible at event time. A known non-Thru type
          // must have a prepared node; otherwise this token is unsafe.
          for (const GsEfxTypeDefaults& defaults : kGsEfxTypeDefaults) {
            if (static_cast<uint8_t>(defaults.type & 0x7Fu) != scan.last_lsb) continue;
            if (!add_candidate(static_cast<uint8_t>(unit), defaults.type)) {
              out.reset();
              return false;
            }
          }
        }
      }
    }

    out = std::shared_ptr<const PreparedMidiSysEx>(std::move(token));
    return true;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    out.reset();
    return false;
  }
}

void Sf2Player::activate_prepared_node(size_t unit, PreparedEfxNode* node) noexcept {
  if (unit >= kGsEfxUnitCount || prepared_active_nodes_[unit] == node) return;
  if (node != nullptr) node->audio_pins.fetch_add(1, std::memory_order_acq_rel);
  if (prepared_active_nodes_[unit] != nullptr) {
    prepared_active_nodes_[unit]->audio_pins.fetch_sub(1, std::memory_order_acq_rel);
  }
  prepared_active_nodes_[unit] = node;
}

void Sf2Player::apply_prepared_node_plan(PreparedEfxNode& node, const GsEfx& target,
                                         bool preserve_enable_fade) noexcept {
  if (node.unit_rt.realization == GsEfxRealization::kClassic) {
    if (!node.unit_rt.stages.empty() && node.unit_rt.stages.front().proc != nullptr) {
      rt::ProcessorBase* proc = node.unit_rt.stages.front().proc.get();
      for (size_t slot = 0; slot < target.params.size(); ++slot) {
        if (proc->parameter_is_realtime_safe(static_cast<unsigned int>(slot))) {
          proc->set_parameter(static_cast<unsigned int>(slot),
                              static_cast<float>(target.params[slot]));
        }
      }
    }
    return;
  }

  for (size_t i = 0; i < node.param_dest_count; ++i) {
    const PreparedEfxParamDest& dest = node.param_dests[i];
    if (dest.stage_index >= node.unit_rt.stages.size()) continue;
    Sf2EfxStageRt& stage = node.unit_rt.stages[dest.stage_index];
    if (stage.proc == nullptr || !stage.proc->parameter_is_realtime_safe(dest.param_id)) continue;
    stage.proc->set_parameter(
        dest.param_id,
        sf2_efx_destination_value(dest.row, target.params[dest.row.slot], dest.transform));
  }

  // AND every rule in table order over the raw bytes, as gs_efx_insert_chain does.
  std::array<bool, 32> enabled{};
  const size_t stage_count = std::min(node.unit_rt.stages.size(), enabled.size());
  for (size_t s = 0; s < stage_count; ++s) enabled[s] = node.default_enabled[s];
  for (size_t i = 0; i < node.enable_plan_count; ++i) {
    const PreparedEfxEnablePlan& plan = node.enable_plans[i];
    const uint8_t byte = plan.rule.slot < target.params.size() ? target.params[plan.rule.slot] : 0;
    for (uint8_t s = 0; s < plan.rule.n_stages && s < plan.stage_indices.size(); ++s) {
      const uint8_t stage_index = plan.stage_indices[s];
      if (stage_index < stage_count) {
        enabled[stage_index] = enabled[stage_index] && gs_efx_enable_on(plan.rule, byte, s);
      }
    }
  }
  for (size_t s = 0; s < stage_count; ++s) {
    Sf2EfxStageRt& stage = node.unit_rt.stages[s];
    const bool on = enabled[s];
    if (preserve_enable_fade) {
      if (on && !stage.enabled_now && stage.fade <= 0.0f && stage.proc != nullptr) {
        stage.proc->reset();
      }
      stage.enabled_target = on;
      stage.enabled_now = on;
    } else {
      stage.enabled_target = on;
      stage.enabled_now = on;
      stage.fade = on ? 1.0f : 0.0f;
    }
  }
}

void Sf2Player::apply_prepared_efx_controls() noexcept {
  if (!prepared_runtime_active_) return;
  const GsEfx& efx = prepared_efx_[0];
  // The controllers are the lowest part the overlay routes to unit 0 at this block.
  const int routed_part = prepared_control_part(0);
  if (routed_part < 0) return;
  const auto apply_controls = [&](const auto& controls, const auto& stages) noexcept {
    for (size_t k = 0; k < controls.size() && k < efx.control_source.size(); ++k) {
      const auto& control = controls[k];
      const uint8_t source = efx.control_source[k];
      if (source == 0 || source > kEfxSourceBend || control.n_dest == 0 ||
          control.slot >= efx.params.size()) {
        continue;
      }
      const Sf2EfxControlRt effective = effective_efx_control(control, efx, k, source);
      const uint8_t byte =
          efx_control_byte(effective, part_controller_position(routed_part, effective.source));
      for (uint8_t d = 0; d < control.n_dest; ++d) {
        const Sf2EfxControlDest& dest = control.dest[d];
        if (dest.stage_index >= stages.size()) continue;
        rt::ProcessorBase* proc = stages[dest.stage_index].proc.get();
        if (proc == nullptr || !proc->parameter_is_realtime_safe(dest.param_id)) continue;
        const float value = dest.binding != nullptr
                                ? sf2_efx_destination_value(*dest.binding, byte, dest.transform)
                                : byte;
        proc->set_parameter(dest.param_id, value);
      }
    }
  };

  // The tail is raised after the destinations move, so the bound a block
  // publishes covers the configuration it renders.
  if (prepared_active_nodes_[0] != nullptr && prepared_unit_overridden_[0]) {
    PreparedEfxNode& node = *prepared_active_nodes_[0];
    apply_controls(node.controls, node.unit_rt.stages);
    raise_prepared_fx_tail();
    return;
  }
  if (prepared_unit_overridden_[0]) return;
  const PartFxSnapshot* snapshot = part_fx_.current();
  if (!snapshot_unit_matches(snapshot, 0, efx.type)) return;
  apply_controls(snapshot->unit(0).legacy_controls, snapshot->unit(0).stages);
  raise_prepared_fx_tail();
}

void Sf2Player::apply_prepared_candidate(const PreparedSysEx& token,
                                         const PreparedEfxCandidate& candidate) noexcept {
  if (candidate.node == nullptr || candidate.unit >= kGsEfxUnitCount) return;
  const PreparedEfxNode* previous = prepared_active_nodes_[candidate.unit];
  const bool switching = previous != candidate.node;
  // A newly selected node is reset after its parameters are written; a kept one keeps its tail.
  activate_prepared_node(candidate.unit, candidate.node);
  prepared_unit_overridden_[candidate.unit] = true;
  // The candidate selects the node; the audio-owned raw state supplies every byte.
  apply_prepared_node_plan(*candidate.node, prepared_efx_[candidate.unit], !switching);
  if (switching) {
    sf2_reset_efx_unit(candidate.node->unit_rt);
  }
  (void)token;
}

void Sf2Player::apply_prepared_gs_delta(const PreparedSysEx& token, const uint8_t* data,
                                        size_t size, bool apply_performance) noexcept {
  if (prepared_owner_identity_ == nullptr || token.owner_identity == nullptr ||
      token.owner_identity.get() != prepared_owner_identity_.get() ||
      token.domain != prepared_audio_domain_ || data == nullptr || size == 0) {
    return;
  }
  if (!prepared_runtime_active_) {
    // The overlay was seeded at the last quiescent boundary, never from control mirrors.
    prepared_runtime_active_ = true;
    prepared_base_synced_ = false;
  }
  sync_prepared_base();

  const GsSysEx msg = parse_gs_sysex(data, size);
  if (gs_sysex_resets(msg.kind)) {
    // Host/static part chains stay in the snapshot and keep their tails.
    if (apply_performance) handle_sysex(data, size);
    for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
      activate_prepared_node(unit, nullptr);
      prepared_unit_overridden_[unit] = true;
    }
    prepared_efx_ = {};
    prepared_assign_ = {};
    prepared_part_unit_.fill(PartFxSnapshot::kNoUnit);
    prepared_unit_fed_.fill(false);
    if (prepared_base_synced_) rebuild_prepared_routing();
    if (apply_performance) {
      prepared_sys_fx_ = {};
      prepared_master_eq_ = {};
      prepared_eq_part_bypassed_ = {};
      apply_gs_system_state(prepared_sys_fx_, prepared_master_eq_, prepared_eq_part_bypassed_);
    }
    return;
  }

  // Channel bytes first; EFX and system state below use only the overlay.
  if (apply_performance) handle_sysex(data, size);
  // PART EFX ASSIGN may be any byte in a validated DT1 run. Apply it directly
  // to the audio-owned overlay so prepared events do not depend on the parser's
  // first-byte kind.
  if (apply_gs_efx_assign_sysex(&prepared_assign_, data, size)) {
    if (prepared_base_synced_) rebuild_prepared_routing();
  }

  if (token.efx_units != 0 && apply_gs_efx_units_sysex(prepared_efx_, data, size, nullptr)) {
    // All units are first brought to their final raw state. Only then are the
    // selected nodes reconciled, so a multi-unit frame rebuilds routing once and
    // cannot expose a half-applied graph between its slices.
    for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
      const uint32_t bit = uint32_t{1} << unit;
      if ((token.efx_units & bit) == 0) continue;
      const bool was_overridden = prepared_unit_overridden_[unit];
      PreparedEfxNode* selected = nullptr;
      for (const PreparedEfxCandidate& candidate : token.candidates) {
        if (candidate.unit == unit && candidate.type == prepared_efx_[unit].type) {
          selected = candidate.node;
          apply_prepared_candidate(token, candidate);
          break;
        }
      }
      if (selected != nullptr) continue;

      // Keep the active node for parameter-only writes. An LSB write, or an
      // unsupported type, explicitly requires a fresh selection and therefore
      // falls back to an empty prepared override when no candidate exists.
      PreparedEfxNode* active = prepared_active_nodes_[unit];
      const bool full_reapply = (token.full_reapply_units & bit) != 0;
      if (!full_reapply && active != nullptr && active->type == prepared_efx_[unit].type) {
        prepared_unit_overridden_[unit] = true;
        apply_prepared_node_plan(*active, prepared_efx_[unit], true);
      } else if (!full_reapply && !was_overridden && active == nullptr) {
        const PartFxSnapshot* snapshot = part_fx_.current();
        if (snapshot_unit_matches(snapshot, unit, prepared_efx_[unit].type)) {
          // Not overridden: the published processors keep running with their tail.
          part_fx_.apply_legacy_plan(unit, prepared_efx_[unit], prepared_control_part(unit), *this);
        } else {
          activate_prepared_node(unit, nullptr);
          prepared_unit_overridden_[unit] = true;
        }
      } else {
        activate_prepared_node(unit, nullptr);
        prepared_unit_overridden_[unit] = true;
      }
    }
    if (prepared_base_synced_) rebuild_prepared_routing();
  }

  if (apply_performance && apply_gs_system_sysex_to(prepared_sys_fx_, prepared_master_eq_,
                                                    prepared_eq_part_bypassed_, data, size)) {
    apply_gs_system_state(prepared_sys_fx_, prepared_master_eq_, prepared_eq_part_bypassed_);
  }
  // The prepared graph is now active at this event boundary. Raise the
  // conservative bounce bound only after its selected node and routing have
  // been applied; speculative prepare/build work remains side-effect free.
  raise_prepared_fx_tail();
}

int Sf2Player::gs_efx_control_byte(size_t control) const noexcept {
  if (prepared_runtime_active_ && prepared_active_nodes_[0] != nullptr) {
    const PreparedEfxNode& node = *prepared_active_nodes_[0];
    if (control >= node.controls.size()) return -1;
    const Sf2EfxControlRt& stored = node.controls[control];
    const GsEfx& efx = prepared_efx_[0];
    const uint8_t source = efx.control_source[control];
    if (source == 0 || source > kEfxSourceBend || stored.n_dest == 0) return -1;
    const int part = prepared_control_part(0);
    if (part < 0) return -1;
    if (stored.slot >= efx.params.size()) return -1;
    const Sf2EfxControlRt effective = effective_efx_control(stored, efx, control, source);
    return efx_control_byte(effective, part_controller_position(part, effective.source));
  }
  const PartFxSnapshot* snapshot = part_fx_.current();
  if (snapshot == nullptr || control >= snapshot->controls.size()) return -1;
  const Sf2EfxControlRt& c = snapshot->controls[control];
  const bool live = c.source != 0 && c.source <= kEfxSourceBend;
  return c.n_dest == 0 || !live ? -1 : c.applied_byte;
}

}  // namespace sonare::midi::synth
