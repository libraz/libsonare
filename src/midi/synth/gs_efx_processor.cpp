#include "midi/synth/gs_efx_processor.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "midi/synth/gs_efx_bindings.h"
#if defined(SONARE_GS_CLASSIC)
#include "midi/synth/gs_classic/classic_unit.h"
#include "midi/synth/gs_classic/model_registry.h"
#endif
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::midi::synth {

using sonare::constants::kDefaultDawSampleRate;

namespace {

#if defined(SONARE_GS_CLASSIC)
constexpr std::string_view kClassicStageName = "gs.classic";
#endif
constexpr double kConstructionSampleRate = kDefaultDawSampleRate;
constexpr int kConstructionBlock = 512;

uint16_t binding_type(const GsEfx& state) noexcept {
  constexpr GsEfxRowView rows{kGsEfxBindingRows.data(), kGsEfxBindingRows.size(),
                              kGsEfxEnables.data(), kGsEfxEnables.size()};
  return gs_efx_binding_type(rows, state.type);
}

}  // namespace

int sf2_find_efx_stage(const Sf2EfxUnitRt& unit, std::string_view name, uint8_t ordinal) noexcept {
  for (size_t i = 0; i < unit.stages.size(); ++i) {
    if (unit.stages[i].name == name && unit.stages[i].ordinal == ordinal) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

Sf2EfxRowTarget sf2_resolve_efx_row(const Sf2EfxUnitRt& unit, const GsEfxBindingRow& row) {
  Sf2EfxRowTarget target;
  if (row.stage >= kGsEfxRowStages.size() || row.key >= kGsEfxRowKeys.size()) return target;
  target.stage_index = sf2_find_efx_stage(unit, kGsEfxRowStages[row.stage], row.ordinal);
  if (target.stage_index < 0) {
    target.status = Sf2EfxRowResolution::kNoStage;
    return target;
  }
  const rt::ProcessorBase* proc = unit.stages[static_cast<size_t>(target.stage_index)].proc.get();
  if (proc == nullptr) {
    target.status = Sf2EfxRowResolution::kNoProcessor;
    return target;
  }
  target.status = Sf2EfxRowResolution::kNoRealtimeControl;
  const std::string_view key = kGsEfxRowKeys[row.key];
  for (const rt::ParamDescriptor& descriptor : proc->parameter_descriptors()) {
    if (descriptor.key != key) continue;
    if (proc->parameter_is_realtime_safe(descriptor.id)) {
      target.status = Sf2EfxRowResolution::kResolved;
      target.param_id = descriptor.id;
    }
    break;
  }
  return target;
}

Sf2EfxUnitRt sf2_build_efx_unit(const GsEfx& efx, const std::vector<GsEfxStage>& stages,
                                GsEfxRealization realization, const GsEfxStageFactory& factory,
                                double sample_rate, int max_block) {
  Sf2EfxUnitRt out;
  out.realization = realization;
  const double fade_samples = static_cast<double>(kSf2EfxFadeMs) * 1e-3 * sample_rate;
  out.fade_step = fade_samples > 1.0 ? static_cast<float>(1.0 / fade_samples) : 1.0f;
  if (realization == GsEfxRealization::kClassic) {
#if defined(SONARE_GS_CLASSIC)
    const gs_classic::GsClassicModelRegistry& registry = gs_classic::gs_classic_default_registry();
    const gs_classic::GsClassicType* model =
        efx.type != 0 && registry.valid() ? registry.find(efx.type) : nullptr;
    if (model != nullptr) {
      Sf2EfxStageRt stage;
      stage.name = std::string(kClassicStageName);
      auto unit = std::make_unique<gs_classic::GsClassicUnit>(registry.models(), *model);
      // Write every construction byte, not only the classic setter's realtime-safe subset.
      for (size_t slot = 0; slot < efx.params.size(); ++slot) {
        unit->set_parameter(static_cast<unsigned int>(slot), static_cast<float>(efx.params[slot]));
      }
      stage.proc = std::move(unit);
      out.stages.push_back(std::move(stage));
    }
#else
    (void)efx;
#endif
  } else {
    out.stages.reserve(stages.size());
    for (const GsEfxStage& stage : stages) {
      Sf2EfxStageRt rt;
      rt.name = stage.name;
      rt.branch = stage.branch;
      rt.ordinal = stage.ordinal;
      rt.enabled_target = stage.enabled;
      rt.enabled_now = stage.enabled;
      rt.fade = stage.enabled ? 1.0f : 0.0f;
      if (factory) {
        rt.proc = factory(stage.name, stage.params_json);
      }
      out.stages.push_back(std::move(rt));
    }
  }
  sf2_prepare_efx_unit(out, sample_rate, max_block);
  return out;
}

namespace {

constexpr int64_t kLatencyOverflow = static_cast<int64_t>(std::numeric_limits<int>::max()) + 1;

void add_latency_q8(int64_t& total, int latency_q8) noexcept {
  const int64_t value = std::max(0, latency_q8);
  if (total >= kLatencyOverflow - value) {
    total = kLatencyOverflow;
  } else {
    total += value;
  }
}

int64_t add_support_samples(int64_t lhs, int64_t rhs) noexcept {
  lhs = std::max<int64_t>(0, lhs);
  rhs = std::max<int64_t>(0, rhs);
  if (lhs > std::numeric_limits<int64_t>::max() - rhs) {
    return std::numeric_limits<int64_t>::max();
  }
  return lhs + rhs;
}

int64_t delay_support_samples(int delay_samples_q8) noexcept {
  const int value = std::max(0, delay_samples_q8);
  const int base = value >> 8;
  if ((value & 0xFF) == 0) return base;
  return static_cast<int64_t>(base) + (base == 0 ? 3 : 2);
}

}  // namespace

void Sf2EfxDelayRt::prepare(int delay_samples_q8) {
  // Never clamp a nonnegative value: a shorter delay would disagree with the reported latency.
  delay_samples_q8_ = std::max(0, delay_samples_q8);
  const int integer_samples = delay_samples_q8_ >> 8;
  const bool fractional = (delay_samples_q8_ & 0xFF) != 0;
  const size_t fractional_size =
      fractional ? static_cast<size_t>(std::max(8, integer_samples + 8)) : 0u;
  for (size_t lane = 0; lane < integer_.size(); ++lane) {
    // With a fractional remainder the Lagrange buffer holds the whole history; no integer ring.
    integer_[lane].prepare(fractional ? 0u : static_cast<size_t>(integer_samples));
    if (fractional) {
      fractional_[lane].assign(fractional_size, 0.0f);
    } else {
      fractional_[lane].clear();
    }
    fractional_write_[lane] = 0;
  }
}

void Sf2EfxDelayRt::reset() noexcept {
  for (rt::DelayLine& delay : integer_) delay.reset();
  for (std::vector<float>& buffer : fractional_) std::fill(buffer.begin(), buffer.end(), 0.0f);
  fractional_write_.fill(0);
}

void Sf2EfxDelayRt::process_lane(float* samples, int num_samples, size_t lane) const noexcept {
  if (samples == nullptr || num_samples <= 0 || delay_samples_q8_ == 0) return;
  if ((delay_samples_q8_ & 0xFF) == 0) {
    rt::DelayLine& delay = integer_[lane];
    for (int i = 0; i < num_samples; ++i) samples[i] = delay.process(samples[i]);
    return;
  }
  std::vector<float>& buffer = fractional_[lane];
  size_t& write = fractional_write_[lane];
  for (int i = 0; i < num_samples; ++i) {
    samples[i] = rt::lagrange3_fractional_delay(buffer, write, delay_samples_q8_, samples[i]);
  }
}

void Sf2EfxDelayRt::prime_lane(const float* samples, int num_samples, size_t lane) const noexcept {
  if (samples == nullptr || num_samples <= 0 || delay_samples_q8_ == 0) return;
  if ((delay_samples_q8_ & 0xFF) == 0) {
    rt::DelayLine& delay = integer_[lane];
    for (int i = 0; i < num_samples; ++i) static_cast<void>(delay.process(samples[i]));
    return;
  }
  std::vector<float>& buffer = fractional_[lane];
  size_t& write = fractional_write_[lane];
  for (int i = 0; i < num_samples; ++i) {
    static_cast<void>(rt::lagrange3_fractional_delay(buffer, write, delay_samples_q8_, samples[i]));
  }
}

void Sf2EfxDelayRt::process(float* left, float* right, int num_samples) const noexcept {
  process_lane(left, num_samples, 0);
  process_lane(right, num_samples, 1);
}

void Sf2EfxDelayRt::prime(const float* left, const float* right, int num_samples) const noexcept {
  prime_lane(left, num_samples, 0);
  prime_lane(right, num_samples, 1);
}

void sf2_prepare_efx_unit(Sf2EfxUnitRt& unit, double sample_rate, int max_block) {
  if (max_block <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "GS EFX max block must be positive");
  }
  int64_t front = 0;
  int64_t half_a = 0;
  int64_t half_b = 0;
  int64_t back = 0;
  int64_t front_support = 0;
  int64_t half_a_support = 0;
  int64_t half_b_support = 0;
  int64_t back_support = 0;
  unit.latency_compensation_tail_samples = 0;
  for (Sf2EfxStageRt& stage : unit.stages) {
    stage.latency_samples_q8 = 0;
    if (stage.proc == nullptr) {
      continue;
    }
    stage.proc->prepare(sample_rate, max_block, 2);
    stage.latency_samples_q8 = unit.realization == GsEfxRealization::kModern
                                   ? std::max(0, stage.proc->latency_samples_q8())
                                   : 0;
    int64_t* branch_latency = &front;
    int64_t* branch_support = &front_support;
    if (stage.branch == kGsEfxBranchHalfA) {
      branch_latency = &half_a;
      branch_support = &half_a_support;
    } else if (stage.branch == kGsEfxBranchHalfB) {
      branch_latency = &half_b;
      branch_support = &half_b_support;
    } else if (stage.branch == kGsEfxBranchBack) {
      branch_latency = &back;
      branch_support = &back_support;
    }
    add_latency_q8(*branch_latency, stage.latency_samples_q8);
    *branch_support =
        add_support_samples(*branch_support, delay_support_samples(stage.latency_samples_q8));
  }
  // Reject an aggregate latency beyond int before allocating any stage or branch delay.
  if (unit.realization == GsEfxRealization::kModern) {
    const int64_t graph_latency_q8 = front + std::max(half_a, half_b) + back;
    if (graph_latency_q8 > std::numeric_limits<int>::max()) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "GS EFX graph latency exceeds the realtime delay range");
    }
    unit.latency_samples_q8 = static_cast<int>(graph_latency_q8);
    const int parallel_latency = static_cast<int>(std::max(half_a, half_b));
    const int half_a_latency = static_cast<int>(half_a);
    const int half_b_latency = static_cast<int>(half_b);
    unit.half_a_alignment.prepare(
        parallel_latency > half_a_latency ? parallel_latency - half_a_latency : 0);
    unit.half_b_alignment.prepare(
        parallel_latency > half_b_latency ? parallel_latency - half_b_latency : 0);
  } else {
    unit.latency_samples_q8 = 0;
    unit.half_a_alignment.prepare(0);
    unit.half_b_alignment.prepare(0);
  }

  for (Sf2EfxStageRt& stage : unit.stages) {
    stage.dry_delay.prepare(stage.latency_samples_q8);
  }
  if (unit.realization == GsEfxRealization::kModern) {
    const int64_t half_a_with_alignment = add_support_samples(
        half_a_support, delay_support_samples(unit.half_a_alignment.delay_samples_q8()));
    const int64_t half_b_with_alignment = add_support_samples(
        half_b_support, delay_support_samples(unit.half_b_alignment.delay_samples_q8()));
    const int64_t graph_support = add_support_samples(
        add_support_samples(front_support, std::max(half_a_with_alignment, half_b_with_alignment)),
        back_support);
    const int64_t floor_latency = unit.latency_samples_q8 >> 8;
    const int64_t compensation = graph_support > floor_latency ? graph_support - floor_latency : 0;
    unit.latency_compensation_tail_samples = compensation > std::numeric_limits<int>::max()
                                                 ? std::numeric_limits<int>::max()
                                                 : static_cast<int>(compensation);
  }
  if (!unit.stages.empty()) {
    unit.scratch.assign(3 * 2 * static_cast<size_t>(max_block), 0.0f);
  } else {
    unit.scratch.clear();
  }
}

void sf2_reset_efx_unit(Sf2EfxUnitRt& unit) noexcept {
  for (Sf2EfxStageRt& stage : unit.stages) {
    if (stage.proc != nullptr) stage.proc->reset();
    stage.dry_delay.reset();
    stage.enabled_now = stage.enabled_target;
    stage.fade = stage.enabled_now ? 1.0f : 0.0f;
  }
  unit.half_a_alignment.reset();
  unit.half_b_alignment.reset();
  std::fill(unit.scratch.begin(), unit.scratch.end(), 0.0f);
}

int sf2_efx_unit_latency_samples_q8(const Sf2EfxUnitRt& unit) noexcept {
  return unit.realization == GsEfxRealization::kModern ? std::max(0, unit.latency_samples_q8) : 0;
}

namespace {

/// Runs one stage in place. A stage fully off passes its input through the
/// stage's dry delay with its state frozen; an active stage primes that delay;
/// a fading stage crossfades its delayed input (kept in @p dry_l / @p dry_r)
/// into its output, one fade_step per sample.
void run_efx_stage(const Sf2EfxStageRt& stage, float* left, float* right, int n, float* dry_l,
                   float* dry_r, float fade_step) noexcept {
  if (stage.proc == nullptr) return;
  const float target = stage.enabled_now ? 1.0f : 0.0f;
  float* chans[2] = {left, right};
  if (stage.fade == target) {
    if (target == 0.0f) {
      stage.dry_delay.process(left, right, n);
      return;
    }
    stage.dry_delay.prime(left, right, n);
    stage.proc->process(chans, 2, n);
    return;
  }
  std::copy(left, left + n, dry_l);
  std::copy(right, right + n, dry_r);
  stage.dry_delay.process(dry_l, dry_r, n);
  stage.proc->process(chans, 2, n);
  float fade = stage.fade;
  for (int i = 0; i < n; ++i) {
    fade = target > fade ? std::min(target, fade + fade_step) : std::max(target, fade - fade_step);
    left[i] = dry_l[i] + fade * (left[i] - dry_l[i]);
    right[i] = dry_r[i] + fade * (right[i] - dry_r[i]);
  }
  stage.fade = fade;
}

/// Runs, in chain order, every stage of @p unit on @p branch.
void run_efx_branch(const Sf2EfxUnitRt& unit, uint8_t branch, float* left, float* right, int n,
                    float* dry_l, float* dry_r) noexcept {
  for (const Sf2EfxStageRt& stage : unit.stages) {
    if (stage.branch == branch) run_efx_stage(stage, left, right, n, dry_l, dry_r, unit.fade_step);
  }
}

}  // namespace

void sf2_run_efx_unit(const Sf2EfxUnitRt& unit, float* left, float* right, int n) noexcept {
  if (unit.stages.empty() || n <= 0) return;
  const size_t span = unit.scratch.size() / 6;
  assert(static_cast<size_t>(n) <= span);
  float* dry_l = unit.scratch.data();
  float* dry_r = dry_l + span;
  float* a_l = dry_r + span;
  float* a_r = a_l + span;
  float* b_l = a_r + span;
  float* b_r = b_l + span;
  run_efx_branch(unit, kGsEfxBranchFront, left, right, n, dry_l, dry_r);
  const bool halves =
      std::any_of(unit.stages.begin(), unit.stages.end(), [](const Sf2EfxStageRt& s) {
        return s.branch == kGsEfxBranchHalfA || s.branch == kGsEfxBranchHalfB;
      });
  if (halves) {
    // Each half runs on its own copy of the front's output; the back takes the sum.
    std::copy(left, left + n, a_l);
    std::copy(right, right + n, a_r);
    std::copy(left, left + n, b_l);
    std::copy(right, right + n, b_r);
    run_efx_branch(unit, kGsEfxBranchHalfA, a_l, a_r, n, dry_l, dry_r);
    run_efx_branch(unit, kGsEfxBranchHalfB, b_l, b_r, n, dry_l, dry_r);
    unit.half_a_alignment.process(a_l, a_r, n);
    unit.half_b_alignment.process(b_l, b_r, n);
    for (int i = 0; i < n; ++i) {
      left[i] = a_l[i] + b_l[i];
      right[i] = a_r[i] + b_r[i];
    }
  }
  run_efx_branch(unit, kGsEfxBranchBack, left, right, n, dry_l, dry_r);
}

GsEfxProcessor::GsEfxProcessor(const GsEfx& state, GsEfxRealization realization,
                               GsEfxStageFactory factory, GsEfxBuildPolicy policy)
    : state_(state), realization_(realization), policy_(policy), factory_(std::move(factory)) {
  if (realization_ != GsEfxRealization::kModern && realization_ != GsEfxRealization::kClassic) {
    throw SonareException(ErrorCode::InvalidParameter, "GsEfxProcessor: unknown realization");
  }
  if (state.type > 0x7F7Fu || state.type_msb != static_cast<uint8_t>(state.type >> 8)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "GsEfxProcessor: type bytes are not a canonical 7-bit pair");
  }
  state_.type_msb = static_cast<uint8_t>(state_.type >> 8);
  if (state_.type != 0 && gs_efx_type_defaults(state_.type) == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "GsEfxProcessor: unsupported EFX type " + std::to_string(state_.type));
  }
  for (size_t slot = 0; slot < state_.params.size(); ++slot) {
    if (state_.params[slot] > 0x7Fu ||
        !gs_efx_parameter_takes(state_.type, static_cast<uint8_t>(slot), state_.params[slot])) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "GsEfxProcessor: parameter byte is outside the selected type");
    }
  }
  const std::vector<GsEfxStage> stages = realization_ == GsEfxRealization::kModern
                                             ? gs_efx_insert_chain(state_)
                                             : std::vector<GsEfxStage>{};
  unit_ = sf2_build_efx_unit(state_, stages, realization_, factory_, kConstructionSampleRate,
                             kConstructionBlock);
  if (policy_ == GsEfxBuildPolicy::kRequireCompleteGraph && !stages.empty()) {
    for (const Sf2EfxStageRt& stage : unit_.stages) {
      if (stage.proc == nullptr) {
        throw SonareException(ErrorCode::InvalidParameter,
                              "GsEfxProcessor: unavailable stage " + stage.name);
      }
    }
  }
  if (state_.type != 0 && unit_.stages.empty() &&
      policy_ == GsEfxBuildPolicy::kRequireCompleteGraph) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "GsEfxProcessor: type has no drawable graph");
  }
  build_parameter_plan();
}

GsEfxProcessor::GsEfxProcessor(uint16_t type, GsEfxRealization realization,
                               GsEfxStageFactory factory, GsEfxBuildPolicy policy)
    : GsEfxProcessor(
          [&] {
            GsEfx state;
            state.type = type;
            state.type_msb = static_cast<uint8_t>(type >> 8);
            state.assigned = type != 0;
            if (const GsEfxTypeDefaults* defaults = gs_efx_type_defaults(type);
                defaults != nullptr) {
              state.params = defaults->params;
            }
            return state;
          }(),
          realization, std::move(factory), policy) {}

void GsEfxProcessor::build_parameter_plan() {
  destination_count_.fill(0);
  enables_ = {};
  enable_count_ = 0;
  enable_slots_.fill(false);
  if (state_.type == 0 || unit_.stages.empty()) return;

  if (realization_ == GsEfxRealization::kClassic) return;

  const uint16_t row_type = binding_type(state_);
  std::array<bool, 20> unsupported{};
  for (const GsEfxBindingRow& row : kGsEfxBindingRows) {
    if (row.type != row_type || row.slot >= destination_count_.size()) continue;
    const Sf2EfxRowTarget target = sf2_resolve_efx_row(unit_, row);
    if (target.status == Sf2EfxRowResolution::kInvalidRow) continue;
    if (target.status != Sf2EfxRowResolution::kResolved) {
      unsupported[row.slot] = true;
      continue;
    }
    const uint8_t count = destination_count_[row.slot];
    if (count >= destinations_[row.slot].size()) {
      unsupported[row.slot] = true;
      continue;
    }
    destinations_[row.slot][count] =
        ParamDestination{static_cast<uint8_t>(target.stage_index), target.param_id, row};
    destination_count_[row.slot] = static_cast<uint8_t>(count + 1);
  }

  for (size_t slot = 0; slot < unsupported.size(); ++slot) {
    if (unsupported[slot]) destination_count_[slot] = 0;
  }

  for (const GsEfxEnable& rule : kGsEfxEnables) {
    if (rule.type != row_type || enable_count_ >= enables_.size()) continue;
    EnableDestination plan;
    plan.rule = rule;
    bool mapped = rule.n_stages <= plan.stage_indices.size();
    for (uint8_t i = 0; mapped && i < rule.n_stages; ++i) {
      if (rule.stages[i] >= kGsEfxRowStages.size()) {
        mapped = false;
        break;
      }
      const int stage_index =
          sf2_find_efx_stage(unit_, kGsEfxRowStages[rule.stages[i]], rule.ordinals[i]);
      if (stage_index < 0 || stage_index > 255) {
        mapped = false;
        break;
      }
      plan.stage_indices[i] = static_cast<uint8_t>(stage_index);
    }
    if (mapped) {
      enables_[enable_count_++] = plan;
      if (rule.slot < enable_slots_.size() && !unsupported[rule.slot]) {
        enable_slots_[rule.slot] = true;
      }
    }
  }
}

void GsEfxProcessor::apply_enable_state(bool reset_fade) noexcept {
  if (realization_ != GsEfxRealization::kModern) return;
  std::array<bool, 64> enabled{};
  const size_t count = std::min(unit_.stages.size(), enabled.size());
  // Start from default-on so a selector can re-enable a stage an earlier byte turned off.
  for (size_t i = 0; i < count; ++i) enabled[i] = true;
  for (size_t i = 0; i < enable_count_; ++i) {
    const EnableDestination& plan = enables_[i];
    const uint8_t byte = plan.rule.slot < state_.params.size() ? state_.params[plan.rule.slot] : 0;
    for (uint8_t s = 0; s < plan.rule.n_stages && s < plan.stage_indices.size(); ++s) {
      const uint8_t stage_index = plan.stage_indices[s];
      if (stage_index < count)
        enabled[stage_index] = enabled[stage_index] && gs_efx_enable_on(plan.rule, byte, s);
    }
  }
  for (size_t i = 0; i < count; ++i) {
    Sf2EfxStageRt& stage = unit_.stages[i];
    if (enabled[i] && !stage.enabled_now && stage.fade <= 0.0f && stage.proc != nullptr) {
      stage.proc->reset();
    }
    stage.enabled_target = enabled[i];
    stage.enabled_now = enabled[i];
    if (reset_fade) stage.fade = enabled[i] ? 1.0f : 0.0f;
  }
}

void GsEfxProcessor::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, 2);
}

void GsEfxProcessor::prepare(double sample_rate, int max_block_size, int max_channels) {
  if (!(sample_rate > 0.0) || !std::isfinite(sample_rate) || max_block_size <= 0 ||
      max_channels < 1 || max_channels > 2) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "GsEfxProcessor needs a positive rate, block, and 1-2 channels");
  }
  // A throwing stage prepare must not leave the old buffers behind the new block size.
  prepared_ = false;
  sf2_prepare_efx_unit(unit_, sample_rate, max_block_size);
  mono_right_.assign(static_cast<size_t>(max_block_size), 0.0f);
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  max_channels_ = max_channels;
  const double fade_samples = static_cast<double>(kSf2EfxFadeMs) * 1e-3 * sample_rate;
  unit_.fade_step = fade_samples > 1.0 ? static_cast<float>(1.0 / fade_samples) : 1.0f;
  prepared_ = true;
  reset();
  apply_enable_state(true);
}

bool GsEfxProcessor::validate_channels(int num_channels) const noexcept {
  return num_channels >= 1 && num_channels <= max_channels_ && num_channels <= 2;
}

void GsEfxProcessor::process(float* const* channels, int num_channels, int num_samples) {
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "GsEfxProcessor block size is negative");
  }
  if (num_channels == 0 || num_samples == 0) return;
  if (!validate_channels(num_channels)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "GsEfxProcessor accepts mono or stereo only");
  }
  ensure_prepared(prepared_, "GsEfxProcessor");
  validate_channel_buffers(channels, num_channels);
  for (int offset = 0; offset < num_samples;) {
    const int n = std::min(max_block_size_, num_samples - offset);
    float* left = channels[0] + offset;
    float* right = num_channels > 1 ? channels[1] + offset : nullptr;
    if (right != nullptr) {
      sf2_run_efx_unit(unit_, left, right, n);
    } else {
      std::copy(left, left + n, mono_right_.data());
      sf2_run_efx_unit(unit_, left, mono_right_.data(), n);
      for (int i = 0; i < n; ++i) left[i] = 0.5f * (left[i] + mono_right_[i]);
    }
    offset += n;
  }
}

void GsEfxProcessor::reset() { sf2_reset_efx_unit(unit_); }

int GsEfxProcessor::latency_samples() const noexcept {
  if (realization_ != GsEfxRealization::kModern || state_.type == 0) return 0;
  return sf2_efx_unit_latency_samples_q8(unit_) >> 8;
}

int GsEfxProcessor::latency_samples_q8() const noexcept {
  if (realization_ != GsEfxRealization::kModern || state_.type == 0) return 0;
  return sf2_efx_unit_latency_samples_q8(unit_);
}

int GsEfxProcessor::tail_samples() const noexcept {
  int64_t front = 0;
  int64_t half_a = 0;
  int64_t half_b = 0;
  int64_t back = 0;
  for (const Sf2EfxStageRt& stage : unit_.stages) {
    if (stage.proc == nullptr) continue;
    const int64_t value = std::max(0, stage.proc->tail_samples());
    switch (stage.branch) {
      case kGsEfxBranchHalfA:
        half_a = add_support_samples(half_a, value);
        break;
      case kGsEfxBranchHalfB:
        half_b = add_support_samples(half_b, value);
        break;
      case kGsEfxBranchBack:
        back = add_support_samples(back, value);
        break;
      case kGsEfxBranchFront:
      default:
        front = add_support_samples(front, value);
        break;
    }
  }
  int64_t total = add_support_samples(add_support_samples(front, std::max(half_a, half_b)), back);
  total = add_support_samples(total, unit_.latency_compensation_tail_samples);
  return total > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                 : static_cast<int>(total);
}

bool GsEfxProcessor::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  if (param_id >= state_.params.size()) return false;
  // Thru has no stage and its type cannot change, so no byte can be heard.
  if (state_.type == 0) return false;
  if (realization_ == GsEfxRealization::kClassic) {
    return !unit_.stages.empty() && unit_.stages.front().proc != nullptr &&
           unit_.stages.front().proc->parameter_is_realtime_safe(param_id);
  }
  return destination_count_[param_id] != 0 || enable_slots_[param_id];
}

std::vector<rt::ParamDescriptor> GsEfxProcessor::parameter_descriptors() const {
  std::vector<rt::ParamDescriptor> descriptors;
  descriptors.reserve(state_.params.size());
  for (unsigned int slot = 0; slot < state_.params.size(); ++slot) {
    descriptors.push_back({"byte" + std::to_string(slot), slot});
  }
  return descriptors;
}

bool GsEfxProcessor::set_parameter_impl(unsigned int param_id, float value) {
  if (param_id >= state_.params.size() || !std::isfinite(value) || value < 0.0f || value > 127.0f ||
      std::floor(value) != value || !parameter_is_realtime_safe(param_id)) {
    return false;
  }
  const uint8_t byte = static_cast<uint8_t>(std::lround(value));
  if (!gs_efx_parameter_takes(state_.type, static_cast<uint8_t>(param_id), byte)) return false;
  if (realization_ == GsEfxRealization::kClassic) {
    if (unit_.stages.empty() || unit_.stages.front().proc == nullptr ||
        !unit_.stages.front().proc->set_parameter(param_id, static_cast<float>(byte))) {
      return false;
    }
  } else if (destination_count_[param_id] != 0) {
    // Resolve every destination before the first write so a missing stage changes nothing.
    for (uint8_t i = 0; i < destination_count_[param_id]; ++i) {
      const ParamDestination& destination = destinations_[param_id][i];
      if (destination.stage_index >= unit_.stages.size() ||
          unit_.stages[destination.stage_index].proc == nullptr) {
        return false;
      }
    }
    for (uint8_t i = 0; i < destination_count_[param_id]; ++i) {
      const ParamDestination& destination = destinations_[param_id][i];
      rt::ProcessorBase* proc = unit_.stages[destination.stage_index].proc.get();
      if (!proc->set_parameter(destination.param_id, gs_efx_binding_value(destination.row, byte))) {
        return false;
      }
    }
  }
  state_.params[param_id] = byte;
  apply_enable_state();
  return true;
}

}  // namespace sonare::midi::synth
