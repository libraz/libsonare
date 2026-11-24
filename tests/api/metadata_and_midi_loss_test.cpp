#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#if defined(SONARE_WITH_ARRANGEMENT)
#include "midi/routing.h"
#endif

#if defined(SONARE_WITH_MASTERING)
TEST_CASE("C insert metadata refuses an undeclared insert", "[c_api][metadata-and-midi-loss]") {
  constexpr char kUnknownInsert[] = "not.a.real.processor";
  constexpr char kRefusal[] = "unknown insert processor: not.a.real.processor";
  CHECK(sonare_mastering_insert_param_names(kUnknownInsert) == nullptr);
  CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string_view(sonare_last_error_message()) == kRefusal);

  for (const int rate : {0, 48000}) {
    char sentinel = 0;
    char* json = &sentinel;
    CHECK(sonare_mastering_insert_param_info(kUnknownInsert, rate, &json) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(json == nullptr);
    CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string_view(sonare_last_error_message()) == kRefusal);
  }

  // The insert is the first parameter, so its refusal wins over a bad rate or output.
  char* json = nullptr;
  CHECK(sonare_mastering_insert_param_info(kUnknownInsert, -1, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string_view(sonare_last_error_message()) == kRefusal);
  CHECK(sonare_mastering_insert_param_info(kUnknownInsert, -1, &json) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string_view(sonare_last_error_message()) == kRefusal);
  CHECK(json == nullptr);

  CHECK(sonare_mastering_insert_param_names(nullptr) == nullptr);
  CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string_view(sonare_last_error_message()) == "insert must not be NULL");
}

TEST_CASE("C insert metadata serves declared processors", "[c_api][metadata-and-midi-loss]") {
  const char* names = sonare_mastering_insert_param_names("dynamics.compressor");
  REQUIRE(names != nullptr);
  CHECK(std::string_view(names).find("thresholdDb") != std::string_view::npos);

  for (const char* insert : {"dynamics.compressor", "repair.declick"}) {
    char* json = nullptr;
    REQUIRE(sonare_mastering_insert_param_info(insert, 0, &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    CHECK(std::string_view(json).find("\"name\"") != std::string_view::npos);
    sonare_free_string(json);
  }
}
#else
TEST_CASE("C insert metadata refuses when mastering is compiled out",
          "[c_api][metadata-and-midi-loss]") {
  char sentinel = 0;
  char* json = &sentinel;
  REQUIRE(sonare_mastering_insert_param_info("dynamics.compressor", 0, &json) ==
          SONARE_ERROR_NOT_SUPPORTED);
  CHECK(json == nullptr);
  const std::string message = sonare_last_error_message();
  CHECK(message == "mastering support is not compiled in");

  CHECK(sonare_mastering_insert_param_names("dynamics.compressor") == nullptr);
  CHECK(sonare_last_error_code() == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(std::string_view(sonare_last_error_message()) == message);
}
#endif

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("C MIDI routing reports losses from both output limits",
          "[c_api][metadata-and-midi-loss]") {
  using RouteFn = SonareError (*)(const SonareMidiEventPod*, size_t, const SonareMidiRouteConfig*,
                                  SonareMidiEventPod*, size_t, size_t*, int*, size_t*);
  static_assert(std::is_same_v<decltype(&sonare_midi_route_events), RouteFn>);

  constexpr uint32_t kNoteOn = 0x20903C64u;
  constexpr size_t kExtraEvents = 3;
  constexpr size_t kRouteCapacity = sonare::midi::MidiRouteOutput::kCapacity;
  const std::vector<SonareMidiEventPod> events(kRouteCapacity + kExtraEvents,
                                               SonareMidiEventPod{0.0, kNoteOn, 0});
  std::vector<SonareMidiEventPod> out(kRouteCapacity - 1);
  size_t count = 0;
  int overflowed = 0;
  size_t dropped = 0;
  REQUIRE(sonare_midi_route_events(events.data(), events.size(), nullptr, out.data(), out.size(),
                                   &count, &overflowed, &dropped) == SONARE_OK);
  CHECK(count == out.size());
  CHECK(overflowed == 1);
  CHECK(dropped == kExtraEvents + 1);

  CHECK(sonare_midi_route_events(nullptr, 1, nullptr, out.data(), out.size(), &count, &overflowed,
                                 &dropped) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(count == 0);
  CHECK(overflowed == 0);
  CHECK(dropped == 0);
}
#endif
