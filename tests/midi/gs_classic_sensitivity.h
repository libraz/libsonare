#pragma once

/// @file gs_classic_sensitivity.h
/// @brief The context search the GS classic type-sensitivity tests share (D32, D44).
///
/// A printed slot passes when its two printed ends draw differently: with every other byte
/// at power-on first; failing that, with exactly one other printed slot at one end of its
/// printed range, one such context at a time; failing every one of those, with two other
/// printed slots each at one end of its range. There is no exception list. Pairs are tried
/// only for a slot no single context reaches, so a slot heard earlier is judged exactly as
/// the single-slot search judged it. For a range of more than two bytes whose ends alias,
/// the midpoint is then tried against each end in the same contexts.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace sonare::midi::synth::gs_classic::test {

/// Twenty EFX parameter bytes, slot 0 first.
using GsClassicBytes = std::array<uint8_t, 20>;

/// Whether @p slot of @p type is heard at @p rest or in a one- or two-slot context.
///
/// @p slots maps (type, slot) to the slot's printed ends (`lo`, `hi`); @p heard is
/// `bool(uint16_t type, int slot, const Ends& ends, const GsClassicBytes& context)` and draws
/// the slot's two ends over `context`. A context reached past power-on is reported by WARN.
template <typename Slots, typename Heard>
bool gs_classic_heard_in_context(const Slots& slots, uint16_t type, int slot,
                                 const GsClassicBytes& rest, Heard heard) {
  const auto& ends = slots.at({type, slot});
  const auto heard_pair = [&](uint8_t lo, uint8_t hi, const GsClassicBytes& context) {
    auto pair = ends;
    pair.lo = lo;
    pair.hi = hi;
    return heard(type, slot, pair, context);
  };
  if (heard_pair(ends.lo, ends.hi, rest)) return true;

  // The other printed slots of the type, each with the ends that move it off power-on.
  std::vector<std::pair<int, std::vector<uint8_t>>> others;
  for (auto it = slots.lower_bound({type, 0}); it != slots.end() && it->first.first == type; ++it) {
    const int moved = it->first.second;
    if (moved == slot) continue;
    std::vector<uint8_t> moves;
    for (uint8_t end : {it->second.lo, it->second.hi}) {
      if (end != rest[static_cast<std::size_t>(moved)]) moves.push_back(end);
    }
    others.emplace_back(moved, moves);
  }

  for (const auto& [moved, moves] : others) {
    for (uint8_t end : moves) {
      GsClassicBytes context = rest;
      context[static_cast<std::size_t>(moved)] = end;
      if (heard_pair(ends.lo, ends.hi, context)) {
        WARN("type " << std::hex << type << std::dec << " slot " << slot << " is heard with slot "
                     << moved << " at byte " << int(end));
        return true;
      }
    }
  }

  for (std::size_t a = 0; a < others.size(); ++a) {
    for (std::size_t b = a + 1; b < others.size(); ++b) {
      for (uint8_t end_a : others[a].second) {
        for (uint8_t end_b : others[b].second) {
          GsClassicBytes context = rest;
          context[static_cast<std::size_t>(others[a].first)] = end_a;
          context[static_cast<std::size_t>(others[b].first)] = end_b;
          if (heard_pair(ends.lo, ends.hi, context)) {
            WARN("type " << std::hex << type << std::dec << " slot " << slot
                         << " is heard with slot " << others[a].first << " at byte " << int(end_a)
                         << " and slot " << others[b].first << " at byte " << int(end_b));
            return true;
          }
        }
      }
    }
  }

  // Ends that alias (pan-like) are retried with the midpoint against each end.
  if (ends.hi <= static_cast<uint8_t>(ends.lo + 1)) return false;
  const uint8_t mid = static_cast<uint8_t>(ends.lo + (ends.hi - ends.lo + 1) / 2);
  const auto heard_interior = [&](const GsClassicBytes& context) {
    return heard_pair(ends.lo, mid, context) || heard_pair(mid, ends.hi, context);
  };
  if (heard_interior(rest)) {
    WARN("type " << std::hex << type << std::dec << " slot " << slot
                 << " is heard with interior byte " << int(mid) << " at power-on");
    return true;
  }
  for (const auto& [moved, moves] : others) {
    for (uint8_t end : moves) {
      GsClassicBytes context = rest;
      context[static_cast<std::size_t>(moved)] = end;
      if (heard_interior(context)) {
        WARN("type " << std::hex << type << std::dec << " slot " << slot
                     << " is heard with interior byte " << int(mid) << " with slot " << moved
                     << " at byte " << int(end));
        return true;
      }
    }
  }
  for (std::size_t a = 0; a < others.size(); ++a) {
    for (std::size_t b = a + 1; b < others.size(); ++b) {
      for (uint8_t end_a : others[a].second) {
        for (uint8_t end_b : others[b].second) {
          GsClassicBytes context = rest;
          context[static_cast<std::size_t>(others[a].first)] = end_a;
          context[static_cast<std::size_t>(others[b].first)] = end_b;
          if (heard_interior(context)) {
            WARN("type " << std::hex << type << std::dec << " slot " << slot
                         << " is heard with interior byte " << int(mid) << " with slot "
                         << others[a].first << " at byte " << int(end_a) << " and slot "
                         << others[b].first << " at byte " << int(end_b));
            return true;
          }
        }
      }
    }
  }
  return false;
}

/// Requires every slot of @p slots that @p pick keeps to be heard in some context.
///
/// @p pick is `bool(bool first_of_type)`; @p power_on is `GsClassicBytes(uint16_t type)`;
/// @p heard is as for gs_classic_heard_in_context.
template <typename Slots, typename Pick, typename PowerOn, typename Heard>
void gs_classic_require_heard(const Slots& slots, Pick pick, PowerOn power_on, Heard heard) {
  REQUIRE(!slots.empty());
  std::size_t compared = 0;
  uint16_t previous_type = 0;
  for (const auto& [key, ends] : slots) {
    const auto [type, slot] = key;
    const bool first_of_type = type != previous_type;
    previous_type = type;
    if (!pick(first_of_type)) continue;
    INFO("type " << std::hex << type << std::dec << " slot " << slot << " bytes " << int(ends.lo)
                 << "/" << int(ends.hi));
    REQUIRE(ends.lo != ends.hi);
    CHECK(gs_classic_heard_in_context(slots, type, slot, power_on(type), heard));
    ++compared;
  }
  CHECK(compared > 0);
}

}  // namespace sonare::midi::synth::gs_classic::test
