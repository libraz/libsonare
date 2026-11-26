#pragma once

/// @file sweep_parts.h
/// @brief Splits one exhaustive sweep into parts that register as separate Catch2 cases.
///
/// Catch2 shards by test case, so a sweep held in one case sets the floor of whichever shard runs
/// it. `TEMPLATE_LIST_TEST_CASE(name, tags, sonare::test::SweepParts<N>)` registers N cases, and
/// `TestType::contains(index)` keeps the units whose running index falls in that case's part.
/// Interleaving by index spreads neighbouring units, which tend to cost alike, across the parts.

#include <cstddef>
#include <tuple>
#include <utility>

namespace sonare::test {

/// Part @p Index of a sweep split into @p Count parts.
template <std::size_t Index, std::size_t Count>
struct SweepPart {
  static_assert(Index < Count);
  static constexpr bool contains(std::size_t unit) { return unit % Count == Index; }
};

template <std::size_t Count, std::size_t... Index>
std::tuple<SweepPart<Index, Count>...> sweep_part_list(std::index_sequence<Index...>);

/// The @p Count parts of one sweep, as the type list a TEMPLATE_LIST_TEST_CASE takes.
template <std::size_t Count>
using SweepParts = decltype(sweep_part_list<Count>(std::make_index_sequence<Count>{}));

}  // namespace sonare::test
