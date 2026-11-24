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

struct InactiveFieldCase {
  const char* name;
  uint8_t SonareMidiCcBinding::*member;
  uint8_t kind;
  const char* allowed_kind;
};

constexpr InactiveFieldCase kInactiveFields[] = {
    {"cc_lsb_number", &SonareMidiCcBinding::cc_lsb_number, SONARE_MIDI_CC_RPN, "control-change 14"},
    {"selector_msb", &SonareMidiCcBinding::selector_msb, SONARE_MIDI_CC_CONTROL_CHANGE_7,
     "RPN or NRPN"},
    {"selector_lsb", &SonareMidiCcBinding::selector_lsb, SONARE_MIDI_CC_CONTROL_CHANGE_7,
     "RPN or NRPN"},
};

SonareMidiCcBinding binding_with_inactive_field(const InactiveFieldCase& field) {
  auto binding = binding_for(3, 74, 77);
  binding.kind = field.kind;
  binding.*(field.member) = 1;
  return binding;
}

void check_inactive_field_refusal(const std::string& field_subject, const std::string& kind_subject,
                                  const std::string& allowed_kind) {
  REQUIRE(sonare_last_refusal_subject_count() == 2);
  CHECK(std::string(sonare_last_refusal_subject(0)) == field_subject);
  CHECK(std::string(sonare_last_refusal_subject(1)) == kind_subject);
  CHECK(std::string(sonare_last_error_message()) ==
        field_subject + " must be 0 unless " + kind_subject + " is " + allowed_kind);
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
TEST_CASE("MIDI CC engine binding refuses nonzero inactive fields", "[midi-domain]") {
  for (const auto& field : kInactiveFields) {
    CAPTURE(field.name);
    auto engine = make_engine();
    const auto binding = binding_with_inactive_field(field);
    REQUIRE(sonare_engine_bind_midi_cc(engine.get(), &binding) == SONARE_ERROR_INVALID_PARAMETER);
    const std::string subject = std::string("binding.") + field.name;
    check_inactive_field_refusal(subject, "binding.kind", field.allowed_kind);

    SonareMidiCcBindingList after{};
    REQUIRE(sonare_engine_midi_cc_bindings(engine.get(), &after) == SONARE_OK);
    CHECK(after.items == nullptr);
    CHECK(after.count == 0);
    sonare_free_midi_cc_binding_list(&after);
  }
}

TEST_CASE("MIDI CC conversion routes refuse nonzero inactive fields and clear outputs",
          "[midi-domain]") {
  SonareMidiEventPod source_event{};
  REQUIRE(sonare_midi_cc(0, 0, 3, 74, 127, &source_event) == SONARE_OK);

  for (const auto& field : kInactiveFields) {
    CAPTURE(field.name);
    const auto binding = binding_with_inactive_field(field);
    const std::string subject = std::string("bindings[0].") + field.name;

    SonareAutomationPoint point{42.0, 1.0f, 3};
    REQUIRE(sonare_midi_cc_to_breakpoint(&binding, 1, &source_event, &point) ==
            SONARE_ERROR_INVALID_PARAMETER);
    check_inactive_field_refusal(subject, "bindings[0].kind", field.allowed_kind);
    CHECK(point.ppq == 0.0);
    CHECK(point.value == 0.0f);
    CHECK(point.curve_to_next == 0);

    SonareMidiEventPod converted{};
    converted.ppq = 42.0;
    converted.word0 = 1;
    converted.word1 = 2;
    const uint8_t sysex_byte = 0;
    converted.sysex = &sysex_byte;
    converted.sysex_size = 1;
    REQUIRE(sonare_midi_param_to_cc(&binding, 1, binding.param_id, 1.0f, 0, 2.0, &converted) ==
            SONARE_ERROR_INVALID_PARAMETER);
    check_inactive_field_refusal(subject, "bindings[0].kind", field.allowed_kind);
    CHECK(converted.ppq == 0.0);
    CHECK(converted.word0 == 0);
    CHECK(converted.word1 == 0);
    CHECK(converted.sysex == nullptr);
    CHECK(converted.sysex_size == 0);
  }
}

TEST_CASE("MIDI CC binding kinds accept zero inactive fields and populated active fields",
          "[midi-domain]") {
  const std::array<SonareMidiCcBinding, 4> bindings = {
      binding_for(3, 74, 77),
      [] {
        auto binding = binding_for(3, 1, 78);
        binding.kind = SONARE_MIDI_CC_CONTROL_CHANGE_14;
        binding.cc_lsb_number = 33;
        return binding;
      }(),
      [] {
        auto binding = binding_for(3, 6, 79);
        binding.kind = SONARE_MIDI_CC_RPN;
        binding.selector_msb = 7;
        binding.selector_lsb = 42;
        return binding;
      }(),
      [] {
        auto binding = binding_for(3, 6, 80);
        binding.kind = SONARE_MIDI_CC_NRPN;
        binding.selector_msb = 8;
        binding.selector_lsb = 43;
        return binding;
      }(),
  };
  auto engine = make_engine();
  for (const auto& binding : bindings) {
    CAPTURE(binding.kind);
    REQUIRE(sonare_engine_bind_midi_cc(engine.get(), &binding) == SONARE_OK);
    SonareMidiEventPod event{};
    REQUIRE(sonare_midi_param_to_cc(&binding, 1, binding.param_id, 0.5f, 0, 2.0, &event) ==
            SONARE_OK);
    SonareAutomationPoint point{};
    REQUIRE(sonare_midi_cc_to_breakpoint(&binding, 1, &event, &point) == SONARE_OK);
  }
}
#endif

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
