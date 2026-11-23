#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "c_api/ppq_domains.h"
#include "c_api/project_input_domains.h"
#include "c_api/project_internal.h"

using namespace sonare_c_detail;

namespace {

#if defined(SONARE_WITH_ARRANGEMENT)
constexpr double kInt64ExclusiveUpperBound = 9223372036854775808.0;
constexpr double kBarEpsilon = 1.0e-9;

struct BarGridPosition {
  double index = 0.0;
  double start_ppq = 0.0;
};

BarGridPosition locate_bar_grid(double origin, double length, double ppq) noexcept {
  double index = std::floor(std::max(0.0, ppq - origin) / length);
  double start = origin + index * length;
  if (start > ppq && index > 0.0 && index - 1.0 < index) {
    index -= 1.0;
    start = origin + index * length;
  } else {
    const double next_index = index + 1.0;
    const double next_start = origin + next_index * length;
    if (next_start <= ppq && next_start > start) {
      index = next_index;
      start = next_start;
    }
  }
  return {index, start};
}

std::vector<sonare::transport::TimeSignatureSegment> normalized_time_signatures(
    const arr::Project& project) {
  using sonare::transport::TimeSignatureSegment;
  std::vector<TimeSignatureSegment> signatures = project.time_signatures();
  signatures.erase(std::remove_if(signatures.begin(), signatures.end(),
                                  [](const auto& segment) {
                                    return !std::isfinite(segment.start_ppq) ||
                                           segment.time_sig.numerator <= 0 ||
                                           segment.time_sig.denominator <= 0;
                                  }),
                   signatures.end());
  sonare::transport::sort_by_start(&signatures);
  std::vector<TimeSignatureSegment> normalized;
  normalized.reserve(signatures.size() + 1);
  for (const TimeSignatureSegment& signature : signatures) {
    if (!normalized.empty() && normalized.back().start_ppq == signature.start_ppq) {
      normalized.back() = signature;
    } else {
      normalized.push_back(signature);
    }
  }
  if (normalized.empty()) {
    normalized.push_back({0.0, sonare::transport::kDefaultTimeSignature});
  } else if (normalized.front().start_ppq > 0.0) {
    TimeSignatureSegment first = normalized.front();
    first.start_ppq = 0.0;
    normalized.insert(normalized.begin(), first);
  }
  return normalized;
}

bool nonnegative_int64_from_double(double value, int64_t* out) noexcept {
  if (out == nullptr || !std::isfinite(value) || value < 0.0 ||
      value >= kInt64ExclusiveUpperBound) {
    return false;
  }
  *out = static_cast<int64_t>(value);
  return true;
}

bool checked_add_bars(int64_t count, int64_t* total) noexcept {
  if (count < 0 || total == nullptr || *total > std::numeric_limits<int64_t>::max() - count) {
    return false;
  }
  *total += count;
  return true;
}

// TempoMap::ppq_to_bar_beat saturates counts, so mirror its grid arithmetic before summing.
bool project_bar_index(const arr::Project& project, double ppq, int64_t* out) {
  using sonare::transport::bar_length_ppq;
  const auto signatures = normalized_time_signatures(project);
  size_t signature_index = 0;
  while (signature_index + 1 < signatures.size() &&
         signatures[signature_index + 1].start_ppq <= ppq) {
    ++signature_index;
  }

  int64_t bar_count = 0;
  for (size_t i = 0; i < signature_index; ++i) {
    const double span = std::max(0.0, signatures[i + 1].start_ppq - signatures[i].start_ppq);
    const double bars = std::ceil(span / bar_length_ppq(signatures[i].time_sig) - kBarEpsilon);
    int64_t segment_bars = 0;
    if (!nonnegative_int64_from_double(bars, &segment_bars) ||
        !checked_add_bars(segment_bars, &bar_count)) {
      return false;
    }
  }

  const auto& signature = signatures[signature_index];
  const BarGridPosition local =
      locate_bar_grid(signature.start_ppq, bar_length_ppq(signature.time_sig), ppq);
  int64_t local_index = 0;
  if (!nonnegative_int64_from_double(local.index, &local_index) ||
      !checked_add_bars(local_index, &bar_count)) {
    return false;
  }
  *out = bar_count;
  return true;
}

[[noreturn]] void refuse_unrepresentable_position(const char* subject) {
  sonare::util::check::refuse_relation(
      {subject},
      "{0} must produce a bar_index in the representable range [0, 9223372036854775807]");
}

[[noreturn]] void refuse_unrepresentable_bar_start(const char* subject) {
  sonare::util::check::refuse_relation(
      {subject}, "{0} must identify a bar with a representable PPQ start in [0, 1000000000000]");
}

#endif

}  // namespace

SonareError sonare_project_ppq_to_sec(const SonareProject* project, double ppq, double* out_sec) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_sec);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_sec);
  SONARE_C_TRY
  sonare::util::check::within(ppq, kPublicPpqDomain, "ppq");
  sonare::transport::TempoMap map;
  fill_project_tempo_map(project->history.project(), &map);
  *out_sec = map.ppq_to_seconds(ppq);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kArrangement, project, ppq, out_sec);
#endif
}

SonareError sonare_project_sec_to_ppq(const SonareProject* project, double sec, double* out_ppq) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_ppq);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_ppq);
  SONARE_C_TRY
  sonare::util::check::within(sec, kProjectPositionSecDomain, "sec");
  sonare::transport::TempoMap map;
  fill_project_tempo_map(project->history.project(), &map);
  const double max_seconds = map.ppq_to_seconds(sonare::transport::kMaxPublicPpq);
  auto sec_domain = kProjectPositionSecDomain;
  sec_domain.hi = max_seconds;
  sonare::util::check::within(sec, sec_domain, "sec");
  *out_ppq = sec == max_seconds ? sonare::transport::kMaxPublicPpq : map.seconds_to_ppq(sec);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kArrangement, project, sec, out_ppq);
#endif
}

SonareError sonare_project_position_at_ppq(const SonareProject* project, double ppq,
                                           SonareProjectPosition* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  sonare::util::check::within(ppq, kPublicPpqDomain, "ppq");
  int64_t checked_bar_index = 0;
  if (!project_bar_index(project->history.project(), ppq, &checked_bar_index)) {
    refuse_unrepresentable_position("ppq");
  }

  sonare::transport::TempoMap map;
  fill_project_tempo_map(project->history.project(), &map);
  const sonare::transport::BarBeat bar_beat = map.ppq_to_bar_beat(ppq);
  const double bar_start = map.bar_start_ppq(ppq);
  const sonare::transport::BarBeat at_bar_start = map.ppq_to_bar_beat(bar_start);
  if (bar_beat.bar != checked_bar_index ||
      map.bar_index_start_ppq(checked_bar_index) != bar_start ||
      at_bar_start.bar != checked_bar_index || at_bar_start.beat != 1 ||
      at_bar_start.beat_fraction != 0.0) {
    sonare::util::check::refuse("ppq",
                                "must identify a bar with an exactly representable PPQ start");
  }
  const auto signature = map.time_signature_at_ppq(ppq);
  *out = {map.bpm_at_ppq(ppq),
          {map.time_signature_start_ppq(ppq), signature.numerator, signature.denominator},
          bar_start,
          checked_bar_index,
          bar_beat.beat,
          bar_beat.beat_fraction};
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kArrangement, project, ppq, out);
#endif
}

SonareError sonare_project_bar_start_ppq(const SonareProject* project, int64_t bar_index,
                                         double* out_ppq) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_ppq);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_ppq);
  SONARE_C_TRY
  sonare::util::check::integer_within(bar_index, kProjectBarIndexDomain, "bar_index");
  sonare::transport::TempoMap map;
  fill_project_tempo_map(project->history.project(), &map);
  const double bar_start = map.bar_index_start_ppq(bar_index);
  if (!sonare::util::check::in_domain(bar_start, kPublicPpqDomain)) {
    refuse_unrepresentable_bar_start("bar_index");
  }
  int64_t round_trip_index = 0;
  if (!project_bar_index(project->history.project(), bar_start, &round_trip_index)) {
    refuse_unrepresentable_bar_start("bar_index");
  }
  const sonare::transport::BarBeat round_trip = map.ppq_to_bar_beat(bar_start);
  if (round_trip_index != bar_index || round_trip.bar != bar_index || round_trip.beat != 1 ||
      round_trip.beat_fraction != 0.0) {
    refuse_unrepresentable_bar_start("bar_index");
  }
  *out_ppq = bar_start;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kArrangement, project, bar_index, out_ppq);
#endif
}

SonareError sonare_project_end_ppq(const SonareProject* project, double* out_end_ppq) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_end_ppq);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_end_ppq);
  SONARE_C_TRY
  *out_end_ppq = arr::arrangement_end_ppq(project->history.project());
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kArrangement, project, out_end_ppq);
#endif
}
