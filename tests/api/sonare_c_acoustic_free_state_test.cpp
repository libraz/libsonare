// Every result-releasing free leaves its struct in the empty state a fresh
// result starts from, so a second free, or a read after the free, sees nothing.

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_engine.h>
#include <sonare/sonare_c_vocal_project.h>
#include <sonare/sonare_c_voice_changer.h>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>

TEST_CASE("sonare_free_rir_synth_result leaves the result zeroed", "[c_api][c_free]") {
  SonareRirSynthResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  sonare_free_rir_synth_result(&r);
  CHECK(r.rir == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
}

TEST_CASE("sonare_free_room_estimate leaves the result zeroed", "[c_api][c_free]") {
  SonareRoomEstimate r{};
  r.volume = static_cast<decltype(r.volume)>(1);
  r.length_m = static_cast<decltype(r.length_m)>(1);
  r.width_m = static_cast<decltype(r.width_m)>(1);
  r.height_m = static_cast<decltype(r.height_m)>(1);
  r.drr_db = static_cast<decltype(r.drr_db)>(1);
  r.confidence = static_cast<decltype(r.confidence)>(1);
  r.band_count = static_cast<decltype(r.band_count)>(1);
  sonare_free_room_estimate(&r);
  CHECK(r.volume == static_cast<decltype(r.volume)>(0));
  CHECK(r.length_m == static_cast<decltype(r.length_m)>(0));
  CHECK(r.width_m == static_cast<decltype(r.width_m)>(0));
  CHECK(r.height_m == static_cast<decltype(r.height_m)>(0));
  CHECK(r.drr_db == static_cast<decltype(r.drr_db)>(0));
  CHECK(r.confidence == static_cast<decltype(r.confidence)>(0));
  CHECK(r.absorption_bands == nullptr);
  CHECK(r.rt60_bands == nullptr);
  CHECK(r.band_count == static_cast<decltype(r.band_count)>(0));
}

TEST_CASE("sonare_free_room_morph_stereo_result leaves the result zeroed", "[c_api][c_free]") {
  SonareRoomMorphStereoResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  sonare_free_room_morph_stereo_result(&r);
  CHECK(r.left == nullptr);
  CHECK(r.right == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
}
