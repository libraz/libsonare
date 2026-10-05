#include "midi/synth/part_fx_stage.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/sf2_voice.h"
#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

/// Index of the destination default in a RigTable.
constexpr size_t kDestinationRig = 16;

/// Reads the numeric value for @p key out of a flat JSON object string
/// (`{"key":number,...}`). The realised EFX stage params are always flat
/// key -> number objects, so a full JSON parser is unnecessary here. Returns
/// false (leaving @p out untouched) when the key is absent or has no number.
bool json_find_number(std::string_view json, std::string_view key, float& out) {
  std::string needle;
  needle.reserve(key.size() + 2);
  needle.push_back('"');
  needle.append(key.data(), key.size());
  needle.push_back('"');
  const size_t kpos = json.find(needle);
  if (kpos == std::string_view::npos) return false;
  size_t p = kpos + needle.size();
  while (p < json.size() && (json[p] == ' ' || json[p] == ':')) ++p;
  const size_t start = p;
  while (p < json.size()) {
    const char c = json[p];
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E') {
      ++p;
    } else {
      break;
    }
  }
  if (p == start) return false;
  const std::string token(json.substr(start, p - start));
  char* end = nullptr;
  const double v = std::strtod(token.c_str(), &end);
  if (end == token.c_str()) return false;
  out = static_cast<float>(v);
  return true;
}

/// The generated binding rows and enables, which the stage reads when no test
/// rows are set.
constexpr GsEfxRowView kGeneratedEfxRows{kGsEfxBindingRows.data(), kGsEfxBindingRows.size(),
                                         kGsEfxEnables.data(), kGsEfxEnables.size()};

/// The CONTROL DEPTH byte that modulates nothing.
constexpr uint8_t kEfxDepthCentre = 0x40;

/// The stage after which a rig's input stops being the mono pickup.
constexpr std::string_view kAmpStage = "saturation.ampSim";

/// Part-rig latency is measured against the fixed 48 kHz contract, regardless
/// of the host rate at which the accepted chain will later be realised.
constexpr double kPartRigValidationSampleRate = 48000.0;
constexpr int kPartRigValidationBlockSize = 512;

}  // namespace

uint8_t efx_control_byte(const Sf2EfxControlRt& control, float position) noexcept {
  const float span = static_cast<float>(control.hi - control.lo);
  if (span <= 0.0f) return control.base_byte;
  const float depth =
      (static_cast<float>(control.depth) - static_cast<float>(kEfxDepthCentre)) / 64.0f;
  const float base =
      (static_cast<float>(control.base_byte) - static_cast<float>(control.lo)) / span;
  float u = std::clamp(base + depth * position, 0.0f, 1.0f);
  if (control.states >= 2) {
    const float last = static_cast<float>(control.states - 1);
    u = std::floor(u * last + 0.5f) / last;
  }
  return static_cast<uint8_t>(control.lo + static_cast<int>(std::floor(u * span + 0.5f)));
}

PartFxStage::PartFxStage(PartFxStageConfig config) : config_(std::move(config)) {
  refresh_bank_parts();
}

PartFxStage::~PartFxStage() = default;
PartFxStage::PartFxStage(PartFxStage&&) noexcept = default;
PartFxStage& PartFxStage::operator=(PartFxStage&&) noexcept = default;

void PartFxStage::prepare(double sample_rate) {
  sample_rate_ = sample_rate;
  // Allocated only where a factory exists, which is where a chain or a unit can.
  part_bus_.assign(enabled() ? 16 * 2 * static_cast<size_t>(kPartFxChunkFrames) : 0, 0.0f);
  unit_bus_.assign(enabled() ? kGsEfxUnitCount * 2 * static_cast<size_t>(kPartFxChunkFrames) : 0,
                   0.0f);
}

void PartFxStage::clear_mirror() {
  efx_ = {};
  assign_ = {};
  dirty_ = false;
  queue_ = std::make_unique<EfxParamQueue>();
}

void PartFxStage::publish() {
  std::shared_ptr<PartFxSnapshot> snapshot = build_snapshot();
  snapshot->generation = ++generation_;
  // Series stages ring out one after another; parts and units run side by side.
  int64_t tail = 0;
  std::array<int64_t, 16> chain_tails{};
  for (size_t part = 0; part < snapshot->chains.size(); ++part) {
    const auto& chain = snapshot->chains[part];
    int64_t sum = 0;
    for (const auto& proc : chain) sum += proc->tail_samples();
    chain_tails[part] = sum;
    tail = std::max(tail, sum);
  }
  std::array<int64_t, kGsEfxUnitCount> unit_input_tails{};
  for (size_t part = 0; part < snapshot->part_unit.size(); ++part) {
    const uint8_t unit = snapshot->part_unit[part];
    if (unit != PartFxSnapshot::kNoUnit) {
      unit_input_tails[unit] = std::max(unit_input_tails[unit], chain_tails[part]);
    }
  }
  for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
    if (!snapshot->unit_fed[unit]) continue;
    int64_t sum = sf2_efx_unit_tail_samples(snapshot->units[unit]);
    // A routed part's own chain runs before the shared unit. Parts feeding the
    // same unit are parallel inputs, so only the longest input tail extends it.
    sum += unit_input_tails[unit];
    tail = std::max(tail, sum);
  }
  pub_->publish(std::move(snapshot));
  tail_samples_ = tail > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                         : static_cast<int>(tail);
}

bool PartFxStage::set_part_rig(uint8_t part, const PartRig& rig) {
  if (!validate_part_rig(part, rig)) return false;
  if (rig.mode == PartRigMode::kChain && enabled()) {
    for (const PartRigStage& stage : rig.stages) {
      std::unique_ptr<rt::ProcessorBase> proc =
          config_.insert_factory(stage.processor, stage.params_json);
      if (proc == nullptr) continue;
      proc->prepare(kPartRigValidationSampleRate, kPartRigValidationBlockSize);
      if (proc->latency_samples_q8() > (kMaxPartRigLatencySamples << 8)) return false;
    }
  }
  const size_t index = part == kPartRigAllParts ? kDestinationRig : part;
  PartRig copy = rig;
  rigs_.rigs[index] = std::move(copy);
  rigs_.present[index] = true;
  refresh_bank_parts();
  return true;
}

void PartFxStage::restore_rig_table(RigTable table) noexcept {
  rigs_ = std::move(table);
  refresh_bank_parts();
}

PartRigMode PartFxStage::effective_mode(int part, const PartRig** entry) const noexcept {
  *entry = nullptr;
  const size_t p = static_cast<size_t>(part & 0x0F);
  if (rigs_.present[p]) {
    *entry = &rigs_.rigs[p];
    return rigs_.rigs[p].mode;
  }
  if (rigs_.present[kDestinationRig]) {
    *entry = &rigs_.rigs[kDestinationRig];
    return rigs_.rigs[kDestinationRig].mode;
  }
  return config_.bank_rig_binding ? PartRigMode::kBank : PartRigMode::kNone;
}

void PartFxStage::refresh_bank_parts() noexcept {
  uint32_t bits = 0;
  for (int part = 0; part < 16; ++part) {
    const PartRig* entry = nullptr;
    if (effective_mode(part, &entry) == PartRigMode::kBank) bits |= 1u << part;
  }
  bank_parts_->store(bits, std::memory_order_release);
}

bool PartFxStage::publish_part_rig(int part, uint8_t id) noexcept {
  const int shift = 4 * (part & 0x0F);
  uint64_t bits = part_rig_ids_->load(std::memory_order_relaxed);
  if (static_cast<uint8_t>((bits >> shift) & 0x0Fu) == id) return false;
  bits = (bits & ~(uint64_t{0x0F} << shift)) | (static_cast<uint64_t>(id & 0x0Fu) << shift);
  part_rig_ids_->store(bits, std::memory_order_release);
  return (bank_parts_->load(std::memory_order_acquire) & (1u << (part & 0x0F))) != 0;
}

uint8_t PartFxStage::part_rig_id(int part) const noexcept {
  const uint64_t bits = part_rig_ids_->load(std::memory_order_acquire);
  return static_cast<uint8_t>((bits >> (4 * (part & 0x0F))) & 0x0Fu);
}

void PartFxStage::restore(const Checkpoint& saved) noexcept {
  efx_ = saved.efx;
  assign_ = saved.assign;
  dirty_ = saved.dirty;
}

void PartFxStage::clear_efx() noexcept {
  efx_ = {};
  assign_ = {};
  dirty_ = true;
}

void PartFxStage::assign_part(uint8_t part, uint8_t value) noexcept {
  assign_[part & 0x0Fu] = value;
  dirty_ = true;
}

bool PartFxStage::apply_unit_sysex(const uint8_t* data, size_t size) noexcept {
  const int unit = gs_efx_addressed_unit(data, size);
  if (unit < 0 || !apply_gs_efx_sysex(efx_[static_cast<size_t>(unit)], data, size)) return false;
  dirty_ = true;
  return true;
}

void PartFxStage::mirror_sysex(const uint8_t* data, size_t size) noexcept {
  const GsSysEx msg = parse_gs_sysex(data, size);
  switch (msg.kind) {
    case GsSysExKind::kGm1Reset:
    case GsSysExKind::kGm2Reset:
    case GsSysExKind::kGsReset:
      efx_ = {};
      assign_ = {};
      break;
    case GsSysExKind::kEfxPartSwitch:
      assign_[msg.channel & 0x0Fu] = msg.value;
      break;
    case GsSysExKind::kUseForRhythm:
    case GsSysExKind::kNone: {
      const int unit = gs_efx_addressed_unit(data, size);
      if (unit >= 0) apply_gs_efx_sysex(efx_[static_cast<size_t>(unit)], data, size, nullptr);
      break;
    }
  }
}

bool PartFxStage::apply_control_sysex(const uint8_t* data, size_t size) {
  const GsSysEx msg = parse_gs_sysex(data, size);
  switch (msg.kind) {
    case GsSysExKind::kGm1Reset:
    case GsSysExKind::kGm2Reset:
    case GsSysExKind::kGsReset:
      // A GS/GM reset clears every EFX unit and the part assignments (Thru): the
      // routing structure changes, so a full rebuild is required.
      efx_ = {};
      assign_ = {};
      return true;
    case GsSysExKind::kEfxPartSwitch:
      // Moving a part between units, or out of one, changes which parts feed
      // which unit: rebuild.
      assign_[msg.channel & 0x0Fu] = msg.value;
      return true;
    case GsSysExKind::kUseForRhythm:
    case GsSysExKind::kNone:
      break;
  }
  // An EFX-block write (40 03 xx, or 40 3u xx for an extension unit). A TYPE
  // change restructures that unit's insert chain and needs a rebuild; a
  // parameter-only edit is applied to the already-built processors WITHOUT
  // rebuilding, so their DSP state (reverb/delay tails) survives (no click/tail
  // dropout). A send change rebuilds, because the parameter queue carries no
  // send bytes. The parameter values are resolved to {unit, stage, param_id,
  // value} tuples on THIS (control) thread and handed to the audio thread
  // through a wait-free SPSC queue; the audio thread applies set_parameter
  // serialized with process() (never a cross-thread mutation of a live
  // processor). If nothing maps to an automatable parameter, or a parameter is
  // not realtime-safe, fall back to a full rebuild.
  const int unit = gs_efx_addressed_unit(data, size);
  if (unit < 0) return false;
  GsEfx& target = efx_[static_cast<size_t>(unit)];
  const std::array<uint8_t, 20> previous_params = target.params;
  const std::array<uint8_t, 3> previous_sends = {target.send_reverb, target.send_chorus,
                                                 target.send_delay};
  const std::array<uint8_t, 2> previous_source = target.control_source;
  const std::array<uint8_t, 2> previous_depth = target.control_depth;
  bool type_changed = false;
  if (!apply_gs_efx_sysex(target, data, size, &type_changed)) return false;
  if (type_changed) return true;
  // The full-snapshot publication is the only carrier of a send byte.
  if (target.send_reverb != previous_sends[0] || target.send_chorus != previous_sends[1] ||
      target.send_delay != previous_sends[2]) {
    return true;
  }
  // Which slot a CONTROL drives, and from which controller, is resolved when the
  // unit is built.
  if (target.control_source != previous_source || target.control_depth != previous_depth) {
    return true;
  }
  return enqueue_param_updates(static_cast<size_t>(unit), previous_params);
}

const GsEfxRowView& PartFxStage::row_view() const noexcept {
  return rows_ != nullptr ? *rows_ : kGeneratedEfxRows;
}

std::vector<GsEfxStage> PartFxStage::efx_stages(const GsEfx& efx) const {
  return rows_ != nullptr ? gs_efx_insert_chain(efx, *rows_) : gs_efx_insert_chain(efx);
}

Sf2EfxUnitRt PartFxStage::build_unit(const GsEfx& efx) const {
  const std::vector<GsEfxStage> stages = config_.realization == GsEfxRealization::kModern
                                             ? efx_stages(efx)
                                             : std::vector<GsEfxStage>{};
  return sf2_build_efx_unit(efx, stages, config_.realization, config_.insert_factory, sample_rate_,
                            kPartFxChunkFrames);
}

std::shared_ptr<PartFxSnapshot> PartFxStage::build_snapshot() const {
  auto out = std::make_shared<PartFxSnapshot>();
  out->part_unit.fill(PartFxSnapshot::kNoUnit);
  out->gs_efx_state = efx_;
  out->gs_part_assign = assign_;
  for (int part = 0; part < 16; ++part) {
    const size_t p = static_cast<size_t>(part);
    const PartRig* entry = nullptr;
    const PartRigMode mode = effective_mode(part, &entry);
    const uint8_t rig_id = part_rig_id(part);
    std::vector<std::unique_ptr<rt::ProcessorBase>>& chain = out->chains[p];
    std::vector<std::string>& names = out->stage_names[p];
    uint8_t mono_prefix = 0;
    // Builds one stage; one the factory declines is skipped, so a partial rig
    // still runs. The mono pickup runs through the last amplifier built.
    const auto add_stage = [&](std::string_view name, std::string_view params) {
      auto proc = config_.insert_factory(name, params);
      if (proc == nullptr) return;
      proc->prepare(sample_rate_, kPartFxChunkFrames);
      chain.push_back(std::move(proc));
      names.emplace_back(name);
      if (name == kAmpStage) mono_prefix = static_cast<uint8_t>(chain.size());
    };
    // An explicit chain busses the part and runs ahead of the file's EFX rather
    // than instead of it — a part may carry both and they are in series
    // (docs/gs.md), so a guitar with an amplifier still gets the file's chorus.
    const bool explicit_chain = mode == PartRigMode::kChain && enabled();
    if (explicit_chain) {
      for (const PartRigStage& stage : entry->stages) add_stage(stage.processor, stage.params_json);
    }
    // The unit the file routed this part through, if any. The part merges into
    // it after its own chain; the unit's chain is built once, below.
    const int unit = enabled() ? gs_efx_assign_unit(assign_[p]) : -1;
    const bool routed = unit >= 0 && efx_[static_cast<size_t>(unit)].assigned;
    if (routed) {
      out->part_unit[p] = static_cast<uint8_t>(unit);
      out->unit_fed[static_cast<size_t>(unit)] = true;
      out->any_unit = true;
    }
    // The bank's default rig for the program the part is playing
    // (docs/voicing.md). It gives way to a GS route, which takes the DI. The
    // presets bind the analytic cabinet rather than a generated impulse, so the
    // stage reports no latency and the part stays aligned with every other one.
    const bool default_bank_rig = mode == PartRigMode::kBank && !routed && enabled() && rig_id != 0;
    if (default_bank_rig) {
      for (const GsEfxStage& stage : gm_rig_chain(rig_id)) add_stage(stage.name, stage.params_json);
    }
    out->default_bank_rig[p] = default_bank_rig && mono_prefix != 0;
    out->mono_prefix[p] = mono_prefix;
    out->host_part_bussed[p] = explicit_chain || !chain.empty();
    // Unaffected parts keep adding straight to the dry mix.
    out->part_bussed[p] = out->host_part_bussed[p] || routed;
    out->any_bussed = out->any_bussed || out->part_bussed[p];
  }
  // One chain per unit, built only for a unit some part actually feeds: parts
  // sharing a unit sum into it and it runs once (docs/gs.md). A stage the
  // factory cannot build (an FX stage in a no-FX build) keeps its position with
  // nothing in it, so the rest of the chain still runs and updates stay aligned.
  for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
    if (!out->unit_fed[unit]) continue;
    out->units[unit] = build_unit(efx_[unit]);
    build_legacy_plan(out->units[unit], unit, efx_[unit]);
  }
  build_controls(*out);
  return out;
}

void PartFxStage::build_legacy_plan(Sf2EfxUnitRt& unit, size_t unit_index, const GsEfx& efx) const {
  unit.legacy_param_dests.clear();
  unit.legacy_enable_plans.clear();
  unit.legacy_default_enabled.clear();
  unit.legacy_controls = {};
  unit.legacy_classic_slots.fill(0);
  unit.legacy_classic_slot_count = 0;
  if (unit.stages.empty()) return;

  if (unit.realization == GsEfxRealization::kClassic) {
    const rt::ProcessorBase* proc = unit.stages.front().proc.get();
    if (proc == nullptr) return;
    for (size_t slot = 0; slot < unit.legacy_classic_slots.size(); ++slot) {
      if (!proc->parameter_is_realtime_safe(static_cast<unsigned int>(slot))) continue;
      unit.legacy_classic_slots[unit.legacy_classic_slot_count++] = static_cast<uint8_t>(slot);
    }
    return;
  }

  const GsEfxRowView& rows = row_view();
  const uint16_t row_type = gs_efx_binding_type(rows, efx.type);

  // All on, as a prepared node: the enable plans below carry every selector rule.
  unit.legacy_default_enabled.assign(unit.stages.size(), 1);

  // Unit 0 alone has EFX CONTROL; resolve it whatever the current source byte.
  if (unit_index == 0) {
    for (size_t k = 0; k < unit.legacy_controls.size(); ++k) {
      Sf2EfxLegacyControlPlan& control = unit.legacy_controls[k];
      const char mark = k == 0 ? '+' : '#';
      const GsEfxBindingRow* first = nullptr;
      for (size_t i = 0; i < rows.n_rows; ++i) {
        const GsEfxBindingRow& row = rows.rows[i];
        if (row.type != row_type || row.printed_mark != mark) continue;
        if (first == nullptr) first = &row;
        if (row.slot != first->slot) continue;
        if (control.n_dest >= control.dest.size()) break;
        const Sf2EfxRowTarget target = sf2_resolve_efx_row(unit, row);
        if (target.status != Sf2EfxRowResolution::kResolved) continue;
        control.dest[control.n_dest++] = {static_cast<uint8_t>(target.stage_index), target.param_id,
                                          &row};
      }
      if (first == nullptr || control.n_dest == 0) continue;
      control.slot = first->slot;
      const int states = gs_efx_printed_states(efx.type, control.slot);
      if (states > 0) {
        control.lo = 0;
        control.hi = static_cast<uint8_t>(states - 1);
      } else if (first->byte_lo < first->byte_hi) {
        control.lo = first->byte_lo;
        control.hi = first->byte_hi;
        if (first->kind == kGsEfxRowDesigned && first->law.form == kGsEfxFormEnum) {
          control.states = first->law.n_states;
        }
      } else {
        control.hi = 0x7F;
      }
    }
  }

  for (size_t i = 0; i < rows.n_rows; ++i) {
    const GsEfxBindingRow& row = rows.rows[i];
    if (row.type != row_type) continue;
    const Sf2EfxRowTarget target = sf2_resolve_efx_row(unit, row);
    if (target.status != Sf2EfxRowResolution::kResolved) continue;
    Sf2EfxLegacyParamDest dest;
    dest.row = row;
    dest.stage_index = static_cast<uint8_t>(target.stage_index);
    dest.param_id = target.param_id;
    unit.legacy_param_dests.push_back(dest);
  }

  for (size_t i = 0; i < rows.n_enables; ++i) {
    const GsEfxEnable& enable = rows.enables[i];
    if (enable.type != row_type) continue;
    Sf2EfxLegacyEnablePlan plan;
    plan.rule = enable;
    bool mapped = false;
    for (uint8_t s = 0; s < enable.n_stages && s < plan.stage_indices.size(); ++s) {
      if (enable.stages[s] >= kGsEfxRowStages.size()) continue;
      const int stage_index =
          sf2_find_efx_stage(unit, kGsEfxRowStages[enable.stages[s]], enable.ordinals[s]);
      if (stage_index < 0) continue;
      plan.stage_indices[s] = static_cast<uint8_t>(stage_index);
      mapped = true;
    }
    if (mapped) unit.legacy_enable_plans.push_back(plan);
  }
}

void PartFxStage::apply_legacy_plan(size_t unit, const GsEfx& target, int control_part,
                                    const PartFxHost& host) noexcept {
  const PartFxSnapshot* snapshot = pub_->current();
  if (snapshot == nullptr || unit >= kGsEfxUnitCount ||
      snapshot->gs_efx_state[unit].type != target.type || !snapshot->unit_fed[unit]) {
    return;
  }
  const Sf2EfxUnitRt& live = snapshot->units[unit];
  if (live.realization == GsEfxRealization::kClassic) {
    if (live.stages.empty() || live.stages.front().proc == nullptr) return;
    rt::ProcessorBase* proc = live.stages.front().proc.get();
    for (uint8_t i = 0; i < live.legacy_classic_slot_count; ++i) {
      const uint8_t slot = live.legacy_classic_slots[i];
      if (slot < target.params.size()) {
        proc->set_parameter(static_cast<unsigned int>(slot),
                            static_cast<float>(target.params[slot]));
      }
    }
  } else {
    for (const Sf2EfxLegacyParamDest& dest : live.legacy_param_dests) {
      if (dest.stage_index >= live.stages.size() || dest.row.slot >= target.params.size()) continue;
      const Sf2EfxStageRt& stage = live.stages[dest.stage_index];
      if (stage.proc == nullptr || !stage.proc->parameter_is_realtime_safe(dest.param_id)) continue;
      stage.proc->set_parameter(dest.param_id,
                                gs_efx_binding_value(dest.row, target.params[dest.row.slot]));
    }
  }

  if (control_part < 0 || control_part >= 16) return;

  // Reapply each fanout from the raw source/depth bytes, even when its byte is unchanged.
  for (size_t k = 0; k < live.legacy_controls.size(); ++k) {
    const Sf2EfxLegacyControlPlan& control = live.legacy_controls[k];
    if (control.n_dest == 0 || control.slot >= target.params.size() ||
        k >= target.control_source.size()) {
      continue;
    }
    const uint8_t source = target.control_source[k];
    if (source == 0 || source > kEfxSourceBend) continue;
    Sf2EfxControlRt effective;
    effective.source = target.control_source[k];
    effective.depth = target.control_depth[k];
    effective.base_byte = target.params[control.slot];
    effective.slot = control.slot;
    effective.lo = control.lo;
    effective.hi = control.hi;
    effective.states = control.states;
    const uint8_t byte =
        efx_control_byte(effective, host.part_controller_position(control_part, effective.source));
    for (uint8_t d = 0; d < control.n_dest; ++d) {
      const Sf2EfxControlDest& dest = control.dest[d];
      if (dest.stage_index >= live.stages.size()) continue;
      rt::ProcessorBase* proc = live.stages[dest.stage_index].proc.get();
      if (proc == nullptr || !proc->parameter_is_realtime_safe(dest.param_id)) continue;
      const float value =
          dest.binding != nullptr ? gs_efx_binding_value(*dest.binding, byte) : byte;
      proc->set_parameter(dest.param_id, value);
    }
  }

  std::array<bool, 64> enabled{};
  const size_t stage_count = std::min(live.stages.size(), enabled.size());
  for (size_t s = 0; s < stage_count; ++s) {
    enabled[s] = s < live.legacy_default_enabled.size() ? live.legacy_default_enabled[s] != 0
                                                        : live.stages[s].enabled_target;
  }
  for (const Sf2EfxLegacyEnablePlan& plan : live.legacy_enable_plans) {
    const uint8_t byte = plan.rule.slot < target.params.size() ? target.params[plan.rule.slot] : 0;
    for (uint8_t s = 0; s < plan.rule.n_stages && s < plan.stage_indices.size(); ++s) {
      const uint8_t stage_index = plan.stage_indices[s];
      if (stage_index < stage_count) {
        enabled[stage_index] = enabled[stage_index] && gs_efx_enable_on(plan.rule, byte, s);
      }
    }
  }
  for (size_t s = 0; s < stage_count; ++s) {
    const Sf2EfxStageRt& stage = live.stages[s];
    const bool on = enabled[s];
    if (on && !stage.enabled_now && stage.fade <= 0.0f && stage.proc != nullptr) {
      stage.proc->reset();
    }
    stage.enabled_target = on;
    stage.enabled_now = on;
  }
}

void PartFxStage::build_controls(PartFxSnapshot& out) const {
  // Only the spec unit has the CONTROL rows, and only a unit that runs has a
  // slot to move.
  const Sf2EfxUnitRt& unit = out.units[0];
  if (!out.unit_fed[0] || unit.stages.empty()) return;
  // The controllers are the lowest-numbered part's among those the unit takes.
  uint8_t part = 0;
  while (part < 16 && out.part_unit[part] != 0) ++part;
  if (part >= 16) return;
  const GsEfx& efx = efx_[0];
  const GsEfxRowView& rows = row_view();
  const uint16_t type = gs_efx_binding_type(rows, efx.type);
  const bool classic = unit.realization == GsEfxRealization::kClassic;
  for (size_t k = 0; k < out.controls.size(); ++k) {
    const uint8_t source = efx.control_source[k];
    if (source == 0 || source > kEfxSourceBend) continue;
    // CONTROL 1 drives the type's `+` slot, CONTROL 2 its `#` slot.
    const uint8_t mark = k == 0 ? '+' : '#';
    Sf2EfxControlRt control;
    control.part = part;
    control.source = source;
    control.depth = efx.control_depth[k];
    const GsEfxBindingRow* first = nullptr;
    for (size_t i = 0; i < rows.n_rows; ++i) {
      const GsEfxBindingRow& row = rows.rows[i];
      if (row.type != type || row.printed_mark != mark) continue;
      if (first == nullptr) first = &row;
      if (row.slot != first->slot) continue;
      if (classic) {
        // The classic unit reads the wire byte itself.
        control.dest[0] = {0, row.slot, nullptr};
        control.n_dest = 1;
        break;
      }
      if (control.n_dest >= control.dest.size()) break;
      const std::string_view stage_name = kGsEfxRowStages[row.stage];
      const std::string_view key = kGsEfxRowKeys[row.key];
      for (size_t s = 0; s < unit.stages.size(); ++s) {
        const Sf2EfxStageRt& stage = unit.stages[s];
        if (stage.proc == nullptr || stage.name != stage_name || stage.ordinal != row.ordinal) {
          continue;
        }
        for (const rt::ParamDescriptor& d : stage.proc->parameter_descriptors()) {
          // A control that is not realtime-safe cannot be written per block.
          if (d.key != key || !stage.proc->parameter_is_realtime_safe(d.id)) continue;
          control.dest[control.n_dest++] = {static_cast<uint8_t>(s), d.id, &row};
          break;
        }
        break;
      }
    }
    if (control.n_dest == 0) continue;
    control.slot = first->slot;
    // A printed list of states takes its first bytes, a printed range is the
    // row's own, and a slot printing neither takes the whole byte.
    const int states = gs_efx_printed_states(efx.type, control.slot);
    if (states > 0) {
      control.lo = 0;
      control.hi = static_cast<uint8_t>(states - 1);
    } else if (first->byte_lo < first->byte_hi) {
      control.lo = first->byte_lo;
      control.hi = first->byte_hi;
      if (first->kind == kGsEfxRowDesigned && first->law.form == kGsEfxFormEnum) {
        control.states = first->law.n_states;
      }
    } else {
      control.lo = 0x00;
      control.hi = 0x7F;
    }
    control.base_byte = efx.params[control.slot];
    // The unit was built at the base, so that is what its destinations hold.
    control.applied_byte = control.base_byte;
    out.controls[k] = control;
  }
}

bool PartFxStage::enqueue_param_updates(size_t unit,
                                        const std::array<uint8_t, 20>& previous_params) {
  // CONTROL thread. Reads the last-published routing (control_current) purely to
  // discover each built stage processor's JSON-key -> param-id bridge
  // (parameter_descriptors() is const and safe to read concurrently with the
  // audio thread); it never mutates a processor here. The edit is the unit's, so
  // it reaches that unit's chain and no other — a part's own rig lives on the
  // part's chain and is nobody's to automate from a GS message.
  const PartFxSnapshot* snapshot = pub_->control_current().get();
  if (snapshot == nullptr) return true;  // nothing built yet -> rebuild
  if (unit >= kGsEfxUnitCount || !snapshot->unit_fed[unit]) return true;
  const Sf2EfxUnitRt& live = snapshot->units[unit];
  if (live.realization != config_.realization) return true;
  if (live.stages.empty()) return true;  // Thru / unmapped -> no chain, rebuild
  const GsEfx& efx = efx_[unit];
  std::array<EfxParamUpdate, EfxParamQueue::kCapacity> pending{};
  size_t pending_count = 0;
  const auto append = [&](EfxUpdateKind kind, size_t stage, uint32_t param_id, float value) {
    if (pending_count >= pending.size()) return false;
    EfxParamUpdate update;
    update.kind = kind;
    update.unit = static_cast<uint8_t>(unit);
    update.stage_index = static_cast<uint8_t>(stage);
    update.param_id = param_id;
    update.value = value;
    update.generation = snapshot->generation;
    pending[pending_count++] = update;
    return true;
  };
  // An edit to a slot an EFX CONTROL drives moves the base it modulates from.
  if (unit == 0) {
    for (size_t k = 0; k < snapshot->controls.size(); ++k) {
      const Sf2EfxControlRt& control = snapshot->controls[k];
      if (control.n_dest == 0 || efx.params[control.slot] == previous_params[control.slot]) {
        continue;
      }
      if (!append(EfxUpdateKind::kControlBase, k, 0,
                  static_cast<float>(efx.params[control.slot]))) {
        return true;
      }
    }
  }

  if (live.realization == GsEfxRealization::kClassic) {
    // The classic unit reads the wire bytes themselves, so only the slots this
    // message moved are sent.
    for (size_t slot = 0; slot < efx.params.size(); ++slot) {
      if (efx.params[slot] == previous_params[slot]) continue;
      if (!append(EfxUpdateKind::kClassicByte, 0, static_cast<uint32_t>(slot),
                  static_cast<float>(efx.params[slot]))) {
        return true;
      }
    }
    if (pending_count != 0 && !queue_->push_batch(pending.data(), pending_count)) {
      return true;
    }
    return false;
  }

  const std::vector<GsEfxStage> stages = efx_stages(efx);
  GsEfx previous_efx = efx;
  previous_efx.params = previous_params;
  const std::vector<GsEfxStage> previous_stages = efx_stages(previous_efx);
  // Updates address stages by position, so a list shaped other than the
  // published one would write a different stage: rebuild instead.
  const bool same_shape =
      stages.size() == live.stages.size() &&
      std::equal(stages.begin(), stages.end(), live.stages.begin(),
                 [](const GsEfxStage& a, const Sf2EfxStageRt& b) {
                   return a.name == b.name && a.branch == b.branch && a.ordinal == b.ordinal;
                 });
  if (!same_shape || previous_stages.size() != stages.size()) return true;
  size_t enqueued = 0;
  for (size_t s = 0; s < stages.size(); ++s) {
    const rt::ProcessorBase* proc = live.stages[s].proc.get();
    if (proc == nullptr) continue;
    for (const rt::ParamDescriptor& d : proc->parameter_descriptors()) {
      float value = 0.0f;
      if (!json_find_number(stages[s].params_json, d.key, value)) continue;
      // A parameter that is not realtime-safe would allocate/rebuild in
      // set_parameter, which is illegal on the audio thread -> rebuild instead.
      if (!proc->parameter_is_realtime_safe(d.id)) return true;
      // A record that does not fit rebuilds rather than leave a stage half-edited.
      if (!append(EfxUpdateKind::kParam, s, d.id, value)) return true;
      ++enqueued;
    }
  }
  for (size_t s = 0; s < stages.size(); ++s) {
    if (stages[s].enabled == previous_stages[s].enabled) continue;
    if (!append(EfxUpdateKind::kEnable, s, 0, stages[s].enabled ? 1.0f : 0.0f)) return true;
    ++enqueued;
  }
  // Nothing matched an automatable parameter -> rebuild so the edit is not lost.
  if (enqueued == 0) return true;
  // One release publication; a full ring leaves no prefix and rebuilds instead.
  if (!queue_->push_batch(pending.data(), pending_count)) return true;
  return false;
}

void PartFxStage::drain_param_updates() noexcept {
  // AUDIO thread, at block start after acquire(): apply every pending update to
  // the current published units. set_parameter runs here, serialized with
  // process() on this same thread — the contract it honours — so there is no
  // cross-thread race and no rebuild (the chain objects, and thus their
  // reverb/delay tails, are preserved). An update resolved against another
  // generation is dropped: the rebuild that replaced it baked the mirror in.
  const PartFxSnapshot* snapshot = pub_->current();
  EfxParamUpdate update;
  while (queue_->pop(update)) {
    if (snapshot == nullptr || update.generation != snapshot->generation ||
        update.unit >= kGsEfxUnitCount) {
      continue;
    }
    if (update.kind == EfxUpdateKind::kControlBase) {
      if (update.unit != 0 || update.stage_index >= snapshot->controls.size()) continue;
      const Sf2EfxControlRt& control = snapshot->controls[update.stage_index];
      control.base_byte = static_cast<uint8_t>(update.value);
      control.dirty = true;
      continue;
    }
    const std::vector<Sf2EfxStageRt>& stages = snapshot->units[update.unit].stages;
    if (update.stage_index >= stages.size()) continue;
    const Sf2EfxStageRt& stage = stages[update.stage_index];
    rt::ProcessorBase* proc = stage.proc.get();
    if (update.kind == EfxUpdateKind::kEnable) {
      const bool on = update.value != 0.0f;
      // A stage coming back from fully off resumes from clean state rather than
      // from the delay lines and phases it froze with.
      if (on && !stage.enabled_now && stage.fade <= 0.0f && proc != nullptr) proc->reset();
      stage.enabled_now = on;
      stage.enabled_target = on;
      continue;
    }
    if (proc == nullptr) continue;
    // Only touch parameters the processor declares realtime-safe. This read is on
    // the audio thread, serialized with set_parameter below, so it is race-free
    // here; it also guarantees we never take a non-noexcept rebuild/validate path
    // (this function is noexcept).
    if (!proc->parameter_is_realtime_safe(update.param_id)) continue;
    proc->set_parameter(update.param_id, update.value);  // scalar set; bool ignored
    // That rewrote a CONTROL's destination at its base; the apply that follows
    // puts the modulated value back in the same block.
    if (update.unit != 0) continue;
    for (const Sf2EfxControlRt& control : snapshot->controls) {
      for (uint8_t d = 0; d < control.n_dest; ++d) {
        if (control.dest[d].stage_index == update.stage_index &&
            control.dest[d].param_id == update.param_id) {
          control.dirty = true;
        }
      }
    }
  }
}

void PartFxStage::apply_controls(const PartFxHost& host) noexcept {
  const PartFxSnapshot* snapshot = pub_->current();
  if (snapshot == nullptr) return;
  const std::vector<Sf2EfxStageRt>& stages = snapshot->units[0].stages;
  for (const Sf2EfxControlRt& control : snapshot->controls) {
    if (control.n_dest == 0) continue;
    const uint8_t byte =
        efx_control_byte(control, host.part_controller_position(control.part, control.source));
    if (!control.dirty && byte == control.applied_byte) continue;
    control.dirty = false;
    control.applied_byte = byte;
    for (uint8_t d = 0; d < control.n_dest; ++d) {
      const Sf2EfxControlDest& dest = control.dest[d];
      if (dest.stage_index >= stages.size()) continue;
      rt::ProcessorBase* proc = stages[dest.stage_index].proc.get();
      if (proc == nullptr || !proc->parameter_is_realtime_safe(dest.param_id)) continue;
      const float value =
          dest.binding != nullptr ? gs_efx_binding_value(*dest.binding, byte) : byte;
      proc->set_parameter(dest.param_id, value);
    }
  }
}

void PartFxStage::clear_part_buses() noexcept {
  if (!part_bus_.empty()) std::memset(part_bus_.data(), 0, sizeof(float) * part_bus_.size());
}

void PartFxStage::clear_unit_buses() noexcept {
  if (!unit_bus_.empty()) std::memset(unit_bus_.data(), 0, sizeof(float) * unit_bus_.size());
}

void PartFxStage::run_part_chains(int n, const std::array<bool, 16>& bussed,
                                  const std::array<uint8_t, 16>& mono_prefix,
                                  const std::array<bool, 16>* skip,
                                  const PartFxHost& host) noexcept {
  const PartFxSnapshot* snapshot = pub_->current();
  if (snapshot == nullptr || part_bus_.empty()) return;
  for (int part = 0; part < 16; ++part) {
    const size_t p = static_cast<size_t>(part);
    if (!bussed[p] || (skip != nullptr && (*skip)[p])) continue;
    float* left = bus_l(part);
    float* right = bus_r(part);
    const auto& chain = snapshot->chains[p];
    if (mono_prefix[p] == 0) {
      float* chans[2] = {left, right};
      for (const auto& proc : chain) proc->process(chans, 2, n);
      continue;
    }
    float* mono_chans[1] = {left};
    const size_t prefix = std::min(static_cast<size_t>(mono_prefix[p]), chain.size());
    size_t stage = 0;
    for (; stage < prefix; ++stage) chain[stage]->process(mono_chans, 1, n);
    // Restore the part's constant-power position after the nonlinear prefix.
    const rt::PanGains pan = voice_pan_gains(host.part_pan_units(part));
    const float pan_l = ::sonare::constants::kSqrt2 * pan.left;
    const float pan_r = ::sonare::constants::kSqrt2 * pan.right;
    for (int i = 0; i < n; ++i) {
      const float mono = left[i];
      left[i] = mono * pan_l;
      right[i] = mono * pan_r;
    }
    if (stage < chain.size()) {
      float* chans[2] = {left, right};
      for (; stage < chain.size(); ++stage) chain[stage]->process(chans, 2, n);
    }
  }
}

void PartFxStage::run_units(int n, const std::array<bool, kGsEfxUnitCount>& fed,
                            const PartFxUnitOverrides* overrides) noexcept {
  if (unit_bus_.empty()) return;
  const PartFxSnapshot* snapshot = pub_->current();
  for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
    if (!fed[unit]) continue;
    const Sf2EfxUnitRt* unit_rt = overrides != nullptr ? (*overrides)[unit] : nullptr;
    if (unit_rt == nullptr && snapshot != nullptr) unit_rt = &snapshot->units[unit];
    if (unit_rt != nullptr) sf2_run_efx_unit(*unit_rt, unit_bus_l(unit), unit_bus_r(unit), n);
  }
}

uint64_t PartFxStage::discard_sum(const PartFxUnitOverrides* overrides) const noexcept {
  const PartFxSnapshot* snapshot = pub_->current();
  if (snapshot == nullptr) return 0;
  uint64_t total = 0;
  for (const auto& chain : snapshot->chains) {
    for (const auto& proc : chain) {
      if (proc) total += proc->non_finite_discard_count();
    }
  }
  for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
    const Sf2EfxUnitRt* unit_rt = overrides != nullptr ? (*overrides)[unit] : nullptr;
    if (unit_rt == nullptr) unit_rt = &snapshot->units[unit];
    for (const Sf2EfxStageRt& stage : unit_rt->stages) {
      if (stage.proc) total += stage.proc->non_finite_discard_count();
    }
  }
  return total;
}

}  // namespace sonare::midi::synth
