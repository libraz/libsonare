/// @file layered_instrument_test.cpp
/// @brief Multi-tone patches (midi/layered_instrument): key and velocity
///        splits, per-layer transpose / level / balance, the note-off routing
///        that follows the note-on rather than the note-off's own velocity,
///        broadcast of everything that is not a note, latency agreement, and
///        the per-layer parameter addressing.

#include "midi/layered_instrument.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"
#include "support/midi_render.h"
#include "util/db.h"

namespace {

using sonare::db_to_linear;
using sonare::midi::InstrumentLayerSpec;
using sonare::midi::LayeredInstrument;
using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrument;
using sonare::midi::MidiInstrumentSourceOutput;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;

constexpr double kRate = 48000.0;

using sonare::test::event;

/// Records what reached it and emits a constant so the mix is measurable.
/// Not final: DiscardingProbe below extends it to reach the protected
/// note_non_finite_discard() from a subclass.
class ProbeInstrument : public MidiInstrument {
 public:
  struct Note {
    uint8_t note;
    uint8_t velocity;
    bool on;
  };

  explicit ProbeInstrument(float level = 1.0f, int latency = 0)
      : level_(level), latency_(latency) {}

  void prepare(double, int) override {
    // Reserved so the probe itself cannot allocate on the audio thread and
    // be mistaken for the layer mixer doing it.
    notes.reserve(64);
    prepared_ = true;
  }
  void process(float* const* channels, int num_channels, int num_samples) override {
    ++process_calls;
    for (int c = 0; c < num_channels; ++c) {
      for (int i = 0; i < num_samples; ++i) channels[c][i] += level_;
    }
  }
  void reset() override { notes.clear(); }
  int latency_samples() const noexcept override { return latency_; }
  int tail_samples() const noexcept override { return tail_; }
  void on_event(uint32_t, const MidiEvent& e) noexcept override {
    const sonare::midi::Ump& u = e.ump;
    if (u.is_note_on()) {
      notes.push_back({u.note_number(), u.data2_7bit(), true});
    } else if (u.is_note_off()) {
      notes.push_back({u.note_number(), u.data2_7bit(), false});
    } else {
      ++other_events;
    }
  }
  int parameter_id_for_key(const std::string& key) const noexcept override {
    if (key == "cutoff") return 7;
    return -1;
  }
  bool apply_parameter(unsigned int id, float value) noexcept override {
    applied_id = static_cast<int>(id);
    applied_value = value;
    return true;
  }

  void set_tail(int tail) noexcept { tail_ = tail; }

  std::vector<Note> notes;
  int other_events = 0;
  int process_calls = 0;
  int applied_id = -1;
  float applied_value = 0.0f;

 private:
  float level_ = 1.0f;
  int latency_ = 0;
  int tail_ = 0;
  bool prepared_ = false;
};

/// A probe that discards on demand: process() bumps its own counter
/// discards_per_call times, letting a test steer a deterministic number of
/// child discards into a single parent block without hostile audio.
class DiscardingProbe final : public ProbeInstrument {
 public:
  void process(float* const* channels, int num_channels, int num_samples) override {
    ProbeInstrument::process(channels, num_channels, num_samples);
    for (int i = 0; i < discards_per_call; ++i) note_non_finite_discard();
  }

  int discards_per_call = 0;
};

/// Renders one block and returns the summed leg peaks.
struct Legs {
  float left;
  float right;
};

Legs render(LayeredInstrument& inst, int num_samples = 64) {
  std::vector<float> l(static_cast<size_t>(num_samples), 0.0f);
  std::vector<float> r(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {l.data(), r.data()};
  inst.process(chans, 2, num_samples);
  return {l[0], r[0]};
}

/// Two default-placement (level 1, pan 0 -> unity gain) NativeSynth layers,
/// so the sum across layers exercises LayeredInstrument's source-track
/// multiply-add without perturbing bit-exactness (multiplying by 1.0f is
/// exact in IEEE 754).
std::unique_ptr<LayeredInstrument> make_two_synth_layers(double rate, int block) {
  auto inst = std::make_unique<LayeredInstrument>();
  REQUIRE(
      inst->add_layer(std::make_unique<NativeSynth>(NativeSynthConfig{}), InstrumentLayerSpec{}));
  REQUIRE(
      inst->add_layer(std::make_unique<NativeSynth>(NativeSynthConfig{}), InstrumentLayerSpec{}));
  inst->prepare(rate, block);
  return inst;
}

float rms(const std::vector<float>& buf) {
  double sum = 0.0;
  for (float v : buf) sum += static_cast<double>(v) * static_cast<double>(v);
  return static_cast<float>(std::sqrt(sum / static_cast<double>(buf.size())));
}

}  // namespace

TEST_CASE("A layered patch sends a note to every layer that covers it", "[midi][layered]") {
  LayeredInstrument inst;
  auto low = std::make_unique<ProbeInstrument>();
  auto high = std::make_unique<ProbeInstrument>();
  ProbeInstrument* low_p = low.get();
  ProbeInstrument* high_p = high.get();

  InstrumentLayerSpec low_spec;
  low_spec.key_lo = 0;
  low_spec.key_hi = 59;
  InstrumentLayerSpec high_spec;
  high_spec.key_lo = 60;
  high_spec.key_hi = 127;

  REQUIRE(inst.add_layer(std::move(low), low_spec));
  REQUIRE(inst.add_layer(std::move(high), high_spec));
  REQUIRE(inst.layer_count() == 2);
  inst.prepare(kRate, 128);

  inst.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 100)));
  inst.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 72, 100)));

  REQUIRE(low_p->notes.size() == 1);
  CHECK(low_p->notes[0].note == 48);
  REQUIRE(high_p->notes.size() == 1);
  CHECK(high_p->notes[0].note == 72);
}

TEST_CASE("A velocity split routes note-off by the note-on that opened it", "[midi][layered]") {
  // A note-off carries its own velocity, which a velocity-split layer would
  // reject; recomputing the split there strands the note sounding for ever.
  LayeredInstrument inst;
  auto soft = std::make_unique<ProbeInstrument>();
  auto loud = std::make_unique<ProbeInstrument>();
  ProbeInstrument* soft_p = soft.get();
  ProbeInstrument* loud_p = loud.get();

  InstrumentLayerSpec soft_spec;
  soft_spec.vel_lo = 1;
  soft_spec.vel_hi = 63;
  InstrumentLayerSpec loud_spec;
  loud_spec.vel_lo = 64;
  loud_spec.vel_hi = 127;

  REQUIRE(inst.add_layer(std::move(soft), soft_spec));
  REQUIRE(inst.add_layer(std::move(loud), loud_spec));
  inst.prepare(kRate, 128);

  inst.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  // Note-off at velocity 0, which is below the loud layer's floor.
  inst.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  CHECK(soft_p->notes.empty());
  REQUIRE(loud_p->notes.size() == 2);
  CHECK(loud_p->notes[0].on);
  CHECK_FALSE(loud_p->notes[1].on);
  CHECK(loud_p->notes[1].note == 60);
}

TEST_CASE("A layer transposes the note it receives, note-off included", "[midi][layered]") {
  LayeredInstrument inst;
  auto octave = std::make_unique<ProbeInstrument>();
  ProbeInstrument* octave_p = octave.get();

  InstrumentLayerSpec spec;
  spec.transpose = -12;
  REQUIRE(inst.add_layer(std::move(octave), spec));
  inst.prepare(kRate, 128);

  inst.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  inst.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  REQUIRE(octave_p->notes.size() == 2);
  CHECK(octave_p->notes[0].note == 48);
  CHECK(octave_p->notes[1].note == 48);
}

TEST_CASE("A transpose off the end of the keyboard drops the layer", "[midi][layered]") {
  LayeredInstrument inst;
  auto up = std::make_unique<ProbeInstrument>();
  ProbeInstrument* up_p = up.get();

  InstrumentLayerSpec spec;
  spec.transpose = 24;
  REQUIRE(inst.add_layer(std::move(up), spec));
  inst.prepare(kRate, 128);

  inst.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 120, 100)));
  inst.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 120, 0)));
  CHECK(up_p->notes.empty());
}

TEST_CASE("Everything that is not a note reaches every layer", "[midi][layered]") {
  LayeredInstrument inst;
  auto a = std::make_unique<ProbeInstrument>();
  auto b = std::make_unique<ProbeInstrument>();
  ProbeInstrument* a_p = a.get();
  ProbeInstrument* b_p = b.get();

  InstrumentLayerSpec narrow;
  narrow.key_lo = 60;
  narrow.key_hi = 60;
  REQUIRE(inst.add_layer(std::move(a), InstrumentLayerSpec{}));
  REQUIRE(inst.add_layer(std::move(b), narrow));
  inst.prepare(kRate, 128);

  inst.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 7, 90)));
  inst.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 10000)));
  inst.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 5)));

  CHECK(a_p->other_events == 3);
  CHECK(b_p->other_events == 3);
}

TEST_CASE("Layer level and balance shape the sum", "[midi][layered]") {
  LayeredInstrument inst;
  InstrumentLayerSpec left_spec;
  left_spec.level = 0.5f;
  left_spec.pan = -1.0f;
  InstrumentLayerSpec right_spec;
  right_spec.level = 0.25f;
  right_spec.pan = 1.0f;

  REQUIRE(inst.add_layer(std::make_unique<ProbeInstrument>(1.0f), left_spec));
  REQUIRE(inst.add_layer(std::make_unique<ProbeInstrument>(1.0f), right_spec));
  inst.prepare(kRate, 128);

  const Legs legs = render(inst);
  // Hard left / hard right at the two levels: each leg carries one layer.
  CHECK(legs.left == Catch::Approx(0.5f).margin(1e-5));
  CHECK(legs.right == Catch::Approx(0.25f).margin(1e-5));
}

TEST_CASE("A centred layer keeps its own level", "[midi][layered]") {
  // The balance law is unity at centre, so layering must not quietly attenuate
  // a layer that was never placed anywhere.
  LayeredInstrument inst;
  REQUIRE(inst.add_layer(std::make_unique<ProbeInstrument>(0.4f), InstrumentLayerSpec{}));
  inst.prepare(kRate, 128);

  const Legs legs = render(inst);
  CHECK(legs.left == Catch::Approx(0.4f).margin(1e-5));
  CHECK(legs.right == Catch::Approx(0.4f).margin(1e-5));
}

TEST_CASE("Layers sum", "[midi][layered]") {
  LayeredInstrument inst;
  REQUIRE(inst.add_layer(std::make_unique<ProbeInstrument>(0.2f), InstrumentLayerSpec{}));
  REQUIRE(inst.add_layer(std::make_unique<ProbeInstrument>(0.3f), InstrumentLayerSpec{}));
  REQUIRE(inst.add_layer(std::make_unique<ProbeInstrument>(0.1f), InstrumentLayerSpec{}));
  inst.prepare(kRate, 128);

  const Legs legs = render(inst);
  CHECK(legs.left == Catch::Approx(0.6f).margin(1e-5));
}

TEST_CASE("Disagreeing layer latencies are refused", "[midi][layered]") {
  LayeredInstrument inst;
  REQUIRE(inst.add_layer(std::make_unique<ProbeInstrument>(1.0f, 0), InstrumentLayerSpec{}));
  REQUIRE(inst.add_layer(std::make_unique<ProbeInstrument>(1.0f, 64), InstrumentLayerSpec{}));
  CHECK_THROWS(inst.prepare(kRate, 128));
}

TEST_CASE("Tail and latency come from the layers", "[midi][layered]") {
  LayeredInstrument inst;
  auto a = std::make_unique<ProbeInstrument>(1.0f, 32);
  auto b = std::make_unique<ProbeInstrument>(1.0f, 32);
  a->set_tail(100);
  b->set_tail(900);
  REQUIRE(inst.add_layer(std::move(a), InstrumentLayerSpec{}));
  REQUIRE(inst.add_layer(std::move(b), InstrumentLayerSpec{}));
  inst.prepare(kRate, 128);

  CHECK(inst.latency_samples() == 32);
  CHECK(inst.tail_samples() == 900);
}

TEST_CASE("A parameter key addresses one layer", "[midi][layered]") {
  LayeredInstrument inst;
  auto a = std::make_unique<ProbeInstrument>();
  auto b = std::make_unique<ProbeInstrument>();
  ProbeInstrument* a_p = a.get();
  ProbeInstrument* b_p = b.get();
  REQUIRE(inst.add_layer(std::move(a), InstrumentLayerSpec{}));
  REQUIRE(inst.add_layer(std::move(b), InstrumentLayerSpec{}));
  inst.prepare(kRate, 128);

  const int id0 = inst.parameter_id_for_key("0.cutoff");
  const int id1 = inst.parameter_id_for_key("1.cutoff");
  REQUIRE(id0 >= 0);
  REQUIRE(id1 >= 0);
  CHECK(id0 != id1);

  CHECK(inst.parameter_id_for_key("cutoff") == -1);      // no layer named
  CHECK(inst.parameter_id_for_key("2.cutoff") == -1);    // no such layer
  CHECK(inst.parameter_id_for_key("0.nonesuch") == -1);  // child declines
  CHECK(inst.parameter_id_for_key(".cutoff") == -1);     // empty index

  CHECK(inst.apply_parameter(static_cast<unsigned int>(id1), 0.75f));
  CHECK(b_p->applied_id == 7);
  CHECK(b_p->applied_value == Catch::Approx(0.75f));
  CHECK(a_p->applied_id == -1);
}

TEST_CASE("A clean layered render leaves the discard count at zero", "[midi][layered]") {
  LayeredInstrument inst;
  REQUIRE(inst.add_layer(std::make_unique<ProbeInstrument>(0.3f), InstrumentLayerSpec{}));
  REQUIRE(inst.add_layer(std::make_unique<ProbeInstrument>(0.2f), InstrumentLayerSpec{}));
  inst.prepare(kRate, 128);

  const Legs legs = render(inst);
  CHECK(legs.left != 0.0f);  // output has energy, so a count of 0 is not vacuous
  CHECK(inst.non_finite_discard_count() == 0);
}

TEST_CASE("A layer's discard raises the parent's count", "[midi][layered]") {
  LayeredInstrument inst;
  REQUIRE(inst.add_layer(std::make_unique<DiscardingProbe>(), InstrumentLayerSpec{}));
  inst.prepare(kRate, 128);
  REQUIRE(inst.non_finite_discard_count() == 0);

  static_cast<DiscardingProbe*>(inst.layer_at(0))->discards_per_call = 1;
  render(inst);
  CHECK(inst.non_finite_discard_count() == 1);
}

TEST_CASE("Several layers discarding several times each still move the parent by exactly one",
          "[midi][layered]") {
  // The children's summed counters move by 3 + 2 + 4 = 9 in this one block;
  // the parent's own count is the delta of the sum, not the sum, and must
  // land at exactly 1.
  LayeredInstrument inst;
  REQUIRE(inst.add_layer(std::make_unique<DiscardingProbe>(), InstrumentLayerSpec{}));
  REQUIRE(inst.add_layer(std::make_unique<DiscardingProbe>(), InstrumentLayerSpec{}));
  REQUIRE(inst.add_layer(std::make_unique<DiscardingProbe>(), InstrumentLayerSpec{}));
  inst.prepare(kRate, 128);
  REQUIRE(inst.non_finite_discard_count() == 0);

  static_cast<DiscardingProbe*>(inst.layer_at(0))->discards_per_call = 3;
  static_cast<DiscardingProbe*>(inst.layer_at(1))->discards_per_call = 2;
  static_cast<DiscardingProbe*>(inst.layer_at(2))->discards_per_call = 4;
  render(inst);

  CHECK(inst.non_finite_discard_count() == 1);
}

TEST_CASE("A clean call after a discard does not raise the parent's count further",
          "[midi][layered]") {
  LayeredInstrument inst;
  REQUIRE(inst.add_layer(std::make_unique<DiscardingProbe>(), InstrumentLayerSpec{}));
  inst.prepare(kRate, 128);
  auto* probe = static_cast<DiscardingProbe*>(inst.layer_at(0));

  probe->discards_per_call = 1;
  render(inst);
  REQUIRE(inst.non_finite_discard_count() == 1);

  probe->discards_per_call = 0;
  render(inst);
  CHECK(inst.non_finite_discard_count() == 1);
}

TEST_CASE("The layered audio path is allocation-free", "[midi][layered]") {
  LayeredInstrument inst;
  REQUIRE(inst.add_layer(std::make_unique<ProbeInstrument>(0.2f), InstrumentLayerSpec{}));
  REQUIRE(inst.add_layer(std::make_unique<ProbeInstrument>(0.2f), InstrumentLayerSpec{}));
  inst.prepare(kRate, 128);
  render(inst);  // warm anything the children allocate lazily

  std::vector<float> l(128, 0.0f);
  std::vector<float> r(128, 0.0f);
  float* chans[2] = {l.data(), r.data()};
  {
    sonare::test::AllocationGuard guard;
    inst.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    inst.process(chans, 2, 128);
    inst.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    inst.process(chans, 2, 128);
    REQUIRE(guard.count() == 0);
  }
}

// ---------------------------------------------------------------------------
// Source-track rendering: process_source_tracks() / supports_source_track_rendering()
// ---------------------------------------------------------------------------

TEST_CASE("supports_source_track_rendering is false when any layer lacks it", "[midi][layered]") {
  LayeredInstrument mixed;
  REQUIRE(
      mixed.add_layer(std::make_unique<NativeSynth>(NativeSynthConfig{}), InstrumentLayerSpec{}));
  REQUIRE(mixed.add_layer(std::make_unique<ProbeInstrument>(), InstrumentLayerSpec{}));
  mixed.prepare(kRate, 128);
  CHECK_FALSE(mixed.supports_source_track_rendering());

  LayeredInstrument all_source_aware;
  REQUIRE(all_source_aware.add_layer(std::make_unique<NativeSynth>(NativeSynthConfig{}),
                                     InstrumentLayerSpec{}));
  all_source_aware.prepare(kRate, 128);
  CHECK(all_source_aware.supports_source_track_rendering());  // non-vacuity
}

TEST_CASE(
    "LayeredInstrument source-track render: one active lane is bit-identical to a lane-less "
    "render",
    "[midi][layered]") {
  constexpr uint32_t kTrack = 77;
  constexpr int kSamples = 256;

  auto solo = make_two_synth_layers(kRate, 256);
  solo->on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  std::vector<float> solo_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> solo_r(static_cast<size_t>(kSamples), 0.0f);
  float* solo_target[] = {solo_l.data(), solo_r.data()};
  const MidiInstrumentSourceOutput solo_outputs[] = {{0, solo_target}};
  REQUIRE(solo->process_source_tracks(solo_outputs, 1, 2, kSamples));

  auto laned = make_two_synth_layers(kRate, 256);
  MidiEvent on = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  on.source_track_id = kTrack;
  laned->on_event(0, on);
  std::vector<float> fallback_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> fallback_r(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> lane_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> lane_r(static_cast<size_t>(kSamples), 0.0f);
  float* fallback_target[] = {fallback_l.data(), fallback_r.data()};
  float* lane_target[] = {lane_l.data(), lane_r.data()};
  const MidiInstrumentSourceOutput laned_outputs[] = {{0, fallback_target}, {kTrack, lane_target}};
  {
    sonare::test::AllocationGuard guard;
    REQUIRE(laned->process_source_tracks(laned_outputs, 2, 2, kSamples));
    REQUIRE(guard.count() == 0);  // S1(d)
  }

  float solo_peak = 0.0f;
  for (float v : solo_l) solo_peak = std::max(solo_peak, std::fabs(v));
  REQUIRE(solo_peak > 0.0f);  // non-vacuity: there is something to match
  for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
    REQUIRE(lane_l[i] == solo_l[i]);  // S1(b): lane target == lane-less render's slot 0
    REQUIRE(lane_r[i] == solo_r[i]);
    REQUIRE(fallback_l[i] == 0.0f);  // S1(b): slot 0 carries nothing
    REQUIRE(fallback_r[i] == 0.0f);
  }
}

TEST_CASE(
    "LayeredInstrument source-track render: muting a single lane attenuates the render by "
    "90 dB",
    "[midi][layered]") {
  constexpr uint32_t kTrack = 55;
  constexpr int kSamples = 256;
  const float lane_gain_muted = db_to_linear(-96.0f);

  auto inst = make_two_synth_layers(kRate, 256);
  MidiEvent on = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  on.source_track_id = kTrack;
  inst->on_event(0, on);

  std::vector<float> fallback_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> fallback_r(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> lane_l(static_cast<size_t>(kSamples), 0.0f);
  std::vector<float> lane_r(static_cast<size_t>(kSamples), 0.0f);
  float* fallback_target[] = {fallback_l.data(), fallback_r.data()};
  float* lane_target[] = {lane_l.data(), lane_r.data()};
  const MidiInstrumentSourceOutput outputs[] = {{0, fallback_target}, {kTrack, lane_target}};
  REQUIRE(inst->process_source_tracks(outputs, 2, 2, kSamples));

  // A real engine sums slot 0 (unfaded) and the lane target (faded by the
  // lane's own fader) into the master bus, so this reproduces what a lane
  // fader at 0 dB vs -96 dB would deliver downstream.
  std::vector<float> unmuted(static_cast<size_t>(kSamples) * 2);
  std::vector<float> muted(static_cast<size_t>(kSamples) * 2);
  for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
    unmuted[2 * i] = fallback_l[i] + lane_l[i];
    unmuted[2 * i + 1] = fallback_r[i] + lane_r[i];
    muted[2 * i] = fallback_l[i] + lane_l[i] * lane_gain_muted;
    muted[2 * i + 1] = fallback_r[i] + lane_r[i] * lane_gain_muted;
  }
  const float rms_unmuted = rms(unmuted);
  const float rms_muted = rms(muted);
  REQUIRE(rms_unmuted > 1.0e-4f);  // non-vacuity
  const double attenuation_db = 20.0 * std::log10(static_cast<double>(rms_unmuted) /
                                                  std::max(static_cast<double>(rms_muted), 1e-12));
  INFO("lane-mute attenuation (dB): " << attenuation_db);
  REQUIRE(attenuation_db >= 90.0);  // S1(a)
}
