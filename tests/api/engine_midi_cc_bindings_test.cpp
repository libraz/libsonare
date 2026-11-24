#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <tuple>

namespace {

using Engine = std::unique_ptr<SonareEngine, decltype(&sonare_engine_destroy)>;

Engine make_engine() {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  return Engine(engine, sonare_engine_destroy);
}

SonareMidiCcBinding binding_for(uint8_t channel, uint8_t cc, uint32_t param_id) {
  SonareMidiCcBinding binding{};
  binding.channel = channel;
  binding.cc_number = cc;
  binding.param_id = param_id;
  binding.kind = SONARE_MIDI_CC_CONTROL_CHANGE_7;
  binding.min_value = -1.0f;
  binding.max_value = 1.0f;
  return binding;
}

#if defined(SONARE_WITH_ARRANGEMENT)
auto binding_identity(const SonareMidiCcBinding& binding) {
  return std::tie(binding.channel, binding.kind, binding.cc_number, binding.cc_lsb_number,
                  binding.selector_msb, binding.selector_lsb);
}

auto binding_values(const SonareMidiCcBinding& binding) {
  return std::tie(binding.consume, binding.reserved, binding.param_id, binding.min_value,
                  binding.max_value);
}
#endif

}  // namespace

TEST_CASE("C MIDI CC binding lists are owned and round-trip", "[c_api][engine][midi_cc]") {
  const auto engine = make_engine();
  SonareMidiCcBindingList snapshot{};
  const auto status = sonare_engine_midi_cc_bindings(engine.get(), &snapshot);
#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(status == SONARE_OK);
#else
  REQUIRE(status == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_last_error_reason() != nullptr);
  CHECK(std::string(sonare_last_error_reason()) == "notCompiledIn");
#endif
  CHECK(snapshot.items == nullptr);
  CHECK(snapshot.count == 0);

#if defined(SONARE_WITH_ARRANGEMENT)
  auto cc = binding_for(3, 74, 77);
  cc.consume = 1;
  auto rpn = binding_for(SONARE_MIDI_CC_ANY_CHANNEL, 6, 78);
  rpn.kind = SONARE_MIDI_CC_RPN;
  rpn.selector_msb = 7;
  rpn.selector_lsb = 42;
  auto cc14 = binding_for(0, 1, 79);
  cc14.kind = SONARE_MIDI_CC_CONTROL_CHANGE_14;
  cc14.cc_lsb_number = 33;
  std::array expected{cc, rpn, cc14};
  for (const auto& row : expected) {
    REQUIRE(sonare_engine_bind_midi_cc(engine.get(), &row) == SONARE_OK);
  }
  REQUIRE(sonare_engine_midi_cc_bindings(engine.get(), &snapshot) == SONARE_OK);
  REQUIRE(snapshot.count == expected.size());
  REQUIRE(snapshot.items != nullptr);
  REQUIRE(sonare_engine_clear_midi_cc_bindings(engine.get()) == SONARE_OK);
  const auto other = make_engine();
  for (size_t i = 0; i < snapshot.count; ++i) {
    REQUIRE(sonare_engine_bind_midi_cc(other.get(), &snapshot.items[i]) == SONARE_OK);
  }
  SonareMidiCcBindingList round_trip{};
  REQUIRE(sonare_engine_midi_cc_bindings(other.get(), &round_trip) == SONARE_OK);
  REQUIRE(round_trip.count == expected.size());
  REQUIRE(round_trip.items != nullptr);
  REQUIRE(round_trip.items != snapshot.items);

  const auto less = [](const auto& a, const auto& b) {
    return binding_identity(a) < binding_identity(b);
  };
  std::sort(expected.begin(), expected.end(), less);
  std::sort(snapshot.items, snapshot.items + snapshot.count, less);
  std::sort(round_trip.items, round_trip.items + round_trip.count, less);
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK(binding_identity(snapshot.items[i]) == binding_identity(expected[i]));
    CHECK(binding_values(snapshot.items[i]) == binding_values(expected[i]));
    CHECK(binding_identity(round_trip.items[i]) == binding_identity(expected[i]));
    CHECK(binding_values(round_trip.items[i]) == binding_values(expected[i]));
  }
  sonare_free_midi_cc_binding_list(&round_trip);
#endif
  sonare_free_midi_cc_binding_list(&snapshot);
  CHECK(snapshot.items == nullptr);
  CHECK(snapshot.count == 0);
  sonare_free_midi_cc_binding_list(&snapshot);
  sonare_free_midi_cc_binding_list(nullptr);
}

TEST_CASE("C MIDI CC bindings name invalid fields", "[c_api][engine][midi_cc]") {
  const auto engine = make_engine();
  SonareMidiCcBindingList out{nullptr, 99};
  CHECK(sonare_engine_midi_cc_bindings(nullptr, &out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("engine") != std::string::npos);
  CHECK(out.count == 0);
  CHECK(out.items == nullptr);
  CHECK(sonare_engine_midi_cc_bindings(engine.get(), nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("out") != std::string::npos);

  SonareMidiCcBinding binding = binding_for(3, 74, 77);
  const char* field = "consume";
  SECTION("consume must be 0 or 1") { binding.consume = 2; }
  SECTION("reserved must be 0") {
    field = "reserved";
    binding.reserved = 1;
  }
  SECTION("RPN selector MSB must be a MIDI data byte") {
    field = "selector_msb";
    binding.kind = SONARE_MIDI_CC_RPN;
    binding.selector_msb = 128;
  }
  SECTION("NRPN selector LSB must be a MIDI data byte") {
    field = "selector_lsb";
    binding.kind = SONARE_MIDI_CC_NRPN;
    binding.selector_lsb = 128;
  }
  REQUIRE(sonare_engine_bind_midi_cc(engine.get(), &binding) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find(std::string("binding.") + field) !=
        std::string::npos);

#if defined(SONARE_WITH_ARRANGEMENT)
  const SonareMidiCcBinding rows[] = {binding_for(2, 7, 75), binding};
  SonareMidiEventPod event{};
  REQUIRE(sonare_midi_cc(0, 0, 3, 74, 127, &event) == SONARE_OK);
  SonareAutomationPoint point{};
  REQUIRE(sonare_midi_cc_to_breakpoint(rows, 2, &event, &point) == SONARE_ERROR_INVALID_PARAMETER);
  const std::string subject = std::string("bindings[1].") + field;
  CHECK(std::string(sonare_last_error_message()).find(subject) != std::string::npos);
  SonareMidiEventPod converted{};
  REQUIRE(sonare_midi_param_to_cc(rows, 2, binding.param_id, 1, 0, 0, &converted) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find(subject) != std::string::npos);
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("C MIDI CC conversions ignore consume and learning clears it", "[c_api][midi_cc]") {
  auto binding = binding_for(3, 74, 77);
  binding.consume = 1;
  SonareMidiEventPod event{};
  REQUIRE(sonare_midi_param_to_cc(&binding, 1, binding.param_id, 1, 0, 2, &event) == SONARE_OK);
  SonareAutomationPoint point{};
  REQUIRE(sonare_midi_cc_to_breakpoint(&binding, 1, &event, &point) == SONARE_OK);
  CHECK(point.ppq == 2);
  CHECK(point.value == 1);

  SonareMidiCcBinding learned = binding;
  REQUIRE(sonare_midi_cc_learn(&event, 1, binding.param_id, -1, 1, 0, &learned) == SONARE_OK);
  CHECK(learned.consume == 0);
  CHECK(learned.reserved == 0);
}
#endif
