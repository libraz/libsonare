#include "transport/marker.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "transport/tempo_map.h"
#include "transport/transport.h"

TEST_CASE("MarkerMap sorts markers and navigates by id and position", "[transport][marker]") {
  sonare::transport::MarkerMap markers;
  markers.set_markers({{4.0, 4, "chorus"}, {1.0, 1, "intro"}, {2.0, 2, "verse"}});

  REQUIRE(markers.marker_count() == 3);
  sonare::transport::Marker marker{};
  REQUIRE(markers.marker_by_index(0, &marker));
  REQUIRE(marker.id == 1);
  REQUIRE(std::strcmp(marker.name, "intro") == 0);
  REQUIRE(markers.marker_by_id(4, &marker));
  REQUIRE(marker.ppq == 4.0);
  REQUIRE(markers.next_marker(1.5, &marker));
  REQUIRE(marker.id == 2);
  REQUIRE(markers.previous_marker(3.0, &marker));
  REQUIRE(marker.id == 2);
}

TEST_CASE("MarkerMap preserves marker kind and key signature fields", "[transport][marker]") {
  sonare::transport::MarkerMap markers;
  sonare::transport::Marker key{};
  key.ppq = 0.0;
  key.id = 7;
  key.name = "E minor";
  key.kind = 4;  // Key signature.
  key.key_fifths = 1;
  key.key_minor = true;
  markers.set_markers({key});

  sonare::transport::Marker out{};
  REQUIRE(markers.marker_by_id(7, &out));
  REQUIRE(out.kind == 4);
  REQUIRE(out.key_fifths == 1);
  REQUIRE(out.key_minor == true);
}

TEST_CASE("Transport seeks to marker with sample accuracy", "[transport][marker]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);
  sonare::transport::MarkerMap markers;
  markers.set_markers({{2.0, 10, "cue"}});

  sonare::transport::Transport transport;
  transport.prepare(48000.0, &tempo);
  REQUIRE(transport.seek_marker(10, markers));
  REQUIRE(transport.sample_position() == 48000);
}

TEST_CASE("Transport creates loop from marker region", "[transport][marker]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);
  sonare::transport::MarkerMap markers;
  markers.set_markers({{1.0, 1, "a"}, {3.0, 2, "b"}});

  sonare::transport::Transport transport;
  transport.prepare(48000.0, &tempo);
  REQUIRE(transport.set_loop_from_markers(1, 2, markers));
  transport.seek_ppq(2.99);
  transport.play();
  transport.advance(1024);
  REQUIRE(transport.sample_position() < tempo.ppq_to_sample(3.0));
}
