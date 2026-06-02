#include "effects/acoustic/room_morph.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "analysis/acoustic_analyzer.h"
#include "core/audio.h"

using namespace sonare;
using namespace sonare::effects::acoustic;
using sonare::acoustic::Material;
using sonare::acoustic::ShoeboxRoom;
using sonare::acoustic::SourceListener;
using sonare::acoustic::synthesize_rir;
using sonare::acoustic::uniform_material;

namespace {

ShoeboxRoom uniform_room(float length, float width, float height, float absorption) {
  ShoeboxRoom room;
  room.dims = {length, width, height};
  for (Material& w : room.walls) w = uniform_material(absorption, 0.0f);
  return room;
}

double energy(const Audio& a) {
  double e = 0.0;
  for (size_t i = 0; i < a.size(); ++i) e += static_cast<double>(a[i]) * a[i];
  return e;
}

}  // namespace

TEST_CASE("room_morph moves the reverberation toward the target room",
          "[effects][acoustic][room_morph]") {
  const int sr = 48000;
  // A small, fairly dead source room (short RT60) recorded as its own RIR, and
  // a large, live target room (long RT60).
  const ShoeboxRoom source_room = uniform_room(4.0f, 3.0f, 2.5f, 0.4f);
  const ShoeboxRoom target_room = uniform_room(12.0f, 9.0f, 5.0f, 0.08f);
  const SourceListener src_pl{{1.0f, 1.0f, 1.2f}, {2.5f, 2.0f, 1.5f}};
  const SourceListener tgt_pl{{2.0f, 2.0f, 1.5f}, {8.0f, 6.0f, 1.7f}};

  const Audio recording = synthesize_rir(source_room, src_pl, sr).rir;
  const Audio target_rir = synthesize_rir(target_room, tgt_pl, sr).rir;
  REQUIRE(recording.size() > 0);
  REQUIRE(target_rir.size() > 0);

  RoomMorphConfig cfg;
  cfg.target = target_room;
  cfg.placement = tgt_pl;
  cfg.wet = 0.6f;
  cfg.source_tail_suppression = 0.5f;

  const Audio morphed = room_morph(recording, cfg);
  REQUIRE(morphed.size() > recording.size());  // target reverb tail was appended

  const AcousticParameters src = detect_acoustic(recording);
  const AcousticParameters tgt = detect_acoustic(target_rir);
  const AcousticParameters morph = detect_acoustic(morphed);
  REQUIRE(src.rt60 > 0.0f);
  REQUIRE(tgt.rt60 > src.rt60);  // the target room really is more reverberant

  // The morph must lengthen the source decay and land closer to the target than
  // the untouched recording does (directional, not exact -- no dereverb).
  REQUIRE(morph.rt60 > src.rt60);
  REQUIRE(std::abs(morph.rt60 - tgt.rt60) < std::abs(src.rt60 - tgt.rt60));

  // Clarity moves the same way: adding the larger room's reverberation reduces
  // C50, so the morph's clarity must drop below the (dead) recording's, toward
  // the more reverberant target. (The target RIR's own broadband C50 is not
  // asserted against -- the analyzer reports it blind for a pure RIR -- but the
  // direction of travel from the source is the meaningful check.)
  REQUIRE(std::isfinite(src.c50));
  REQUIRE(std::isfinite(morph.c50));
  REQUIRE(morph.c50 < src.c50);
}

TEST_CASE("room_morph is a passthrough at zero wet and zero suppression",
          "[effects][acoustic][room_morph]") {
  const int sr = 48000;
  const ShoeboxRoom target_room = uniform_room(8.0f, 6.0f, 3.5f, 0.15f);

  std::vector<float> samples(2000, 0.0f);
  samples[0] = 1.0f;
  for (int i = 1; i < 400; ++i) samples[i] = 0.6f * std::exp(-static_cast<float>(i) / 120.0f);
  const Audio rec = Audio::from_vector(std::vector<float>(samples), sr);

  RoomMorphConfig cfg;
  cfg.target = target_room;
  cfg.placement = {{1.0f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};
  cfg.wet = 0.0f;                      // no target room added
  cfg.source_tail_suppression = 0.0f;  // suppressor bypassed

  const Audio out = room_morph(rec, cfg);
  // Dry-only, latency-compensated: the leading recording is reproduced exactly.
  for (size_t i = 0; i < rec.size(); ++i) {
    REQUIRE(std::abs(out[i] - rec[i]) < 1e-5f);
  }
}

TEST_CASE("room_morph suppression reduces the source tail energy",
          "[effects][acoustic][room_morph]") {
  const int sr = 48000;
  // A decaying tail after a transient -- the reverberant content the suppressor
  // should pull down. Compare wet=0 (suppression only) at full vs zero amount.
  std::vector<float> samples(8000, 0.0f);
  samples[0] = 1.0f;
  for (int i = 1; i < 6000; ++i) samples[i] = 0.5f * std::exp(-static_cast<float>(i) / 1500.0f);
  const Audio rec = Audio::from_vector(std::vector<float>(samples), sr);

  RoomMorphConfig base;
  base.target = uniform_room(8.0f, 6.0f, 3.5f, 0.15f);
  base.placement = {{1.0f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};
  base.wet = 0.0f;  // isolate the suppressor

  RoomMorphConfig none = base;
  none.source_tail_suppression = 0.0f;
  RoomMorphConfig full = base;
  full.source_tail_suppression = 1.0f;

  const Audio out_full = room_morph(rec, full);
  const double e_none = energy(room_morph(rec, none));
  const double e_full = energy(out_full);
  REQUIRE(e_full < e_none);  // the tail was attenuated
  REQUIRE(e_full > 0.0);     // but not removed (the transient survives)

  // The expander is light, not a gate: even at full suppression the direct
  // onset is preserved near unity (smoothing holds the transient gain high).
  float out_peak = 0.0f;
  for (size_t i = 0; i < out_full.size(); ++i) out_peak = std::max(out_peak, std::abs(out_full[i]));
  REQUIRE(out_peak >= 0.8f);  // input onset was 1.0
}

TEST_CASE("room_morph degrades cleanly for an invalid target room",
          "[effects][acoustic][room_morph]") {
  const int sr = 48000;
  ShoeboxRoom target = uniform_room(8.0f, 6.0f, 3.5f, 0.15f);
  std::vector<float> samples(1000, 0.0f);
  samples[0] = 1.0f;
  const Audio rec = Audio::from_vector(std::vector<float>(samples), sr);

  RoomMorphConfig cfg;
  cfg.target = target;
  // Source placed outside the room => validate_shoebox errors => empty RIR.
  cfg.placement = {{99.0f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};

  const Audio out = room_morph(rec, cfg);
  REQUIRE(out.size() == rec.size());  // no reverb tail appended, no crash
}

TEST_CASE("room_morph is deterministic", "[effects][acoustic][room_morph]") {
  const int sr = 48000;
  std::vector<float> samples(1500, 0.0f);
  samples[0] = 1.0f;
  samples[200] = 0.3f;
  const Audio rec = Audio::from_vector(std::vector<float>(samples), sr);

  RoomMorphConfig cfg;
  cfg.target = uniform_room(7.0f, 5.0f, 3.0f, 0.15f);
  cfg.placement = {{1.5f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};

  const Audio a = room_morph(rec, cfg);
  const Audio b = room_morph(rec, cfg);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
}
