#include "engine/boundary_splitter.h"

#include <catch2/catch_test_macros.hpp>

namespace {

bool has_source(const sonare::engine::BoundaryPoint& point, sonare::engine::BoundarySource source) {
  return (point.sources & sonare::engine::boundary_source_mask(source)) != 0;
}

}  // namespace

TEST_CASE("BoundarySplitter merges and sorts realtime block boundaries", "[engine][boundary]") {
  sonare::engine::BoundarySplitter splitter;
  splitter.begin({1000, 48000, 512});

  REQUIRE(splitter.add_command(128));
  REQUIRE(splitter.add_automation(64));
  REQUIRE(splitter.add_clip(256));
  REQUIRE(splitter.add_marker(128));

  const auto& boundaries = splitter.finish();
  REQUIRE_FALSE(boundaries.overflowed());
  REQUIRE(boundaries.size() == 5);

  REQUIRE(boundaries[0].offset == 0);
  REQUIRE(boundaries[0].render_frame == 1000);
  REQUIRE(boundaries[0].timeline_sample == 48000);
  REQUIRE(has_source(boundaries[0], sonare::engine::BoundarySource::kBlockStart));

  REQUIRE(boundaries[1].offset == 64);
  REQUIRE(boundaries[1].render_frame == 1064);
  REQUIRE(boundaries[1].timeline_sample == 48064);
  REQUIRE(has_source(boundaries[1], sonare::engine::BoundarySource::kAutomation));

  REQUIRE(boundaries[2].offset == 128);
  REQUIRE(has_source(boundaries[2], sonare::engine::BoundarySource::kCommand));
  REQUIRE(has_source(boundaries[2], sonare::engine::BoundarySource::kMarker));

  REQUIRE(boundaries[3].offset == 256);
  REQUIRE(has_source(boundaries[3], sonare::engine::BoundarySource::kClip));

  REQUIRE(boundaries[4].offset == 512);
  REQUIRE(boundaries[4].render_frame == 1512);
  REQUIRE(boundaries[4].timeline_sample == 48512);
  REQUIRE(has_source(boundaries[4], sonare::engine::BoundarySource::kBlockEnd));
}

TEST_CASE("BoundarySplitter maps timeline discontinuity after loop wrap", "[engine][boundary]") {
  sonare::engine::BoundarySplitter splitter;
  splitter.begin({2000, 95800, 512, false, 0, 48000});

  REQUIRE(splitter.add_command(100));
  REQUIRE(splitter.add_loop(200));
  REQUIRE(splitter.add_automation(240));

  const auto& boundaries = splitter.finish();
  REQUIRE(boundaries.size() == 5);

  REQUIRE(boundaries[1].offset == 100);
  REQUIRE(boundaries[1].timeline_sample == 95900);
  REQUIRE(has_source(boundaries[1], sonare::engine::BoundarySource::kCommand));

  REQUIRE(boundaries[2].offset == 200);
  REQUIRE(boundaries[2].render_frame == 2200);
  REQUIRE(boundaries[2].timeline_sample == 48000);
  REQUIRE(has_source(boundaries[2], sonare::engine::BoundarySource::kLoop));

  REQUIRE(boundaries[3].offset == 240);
  REQUIRE(boundaries[3].timeline_sample == 48040);
  REQUIRE(has_source(boundaries[3], sonare::engine::BoundarySource::kAutomation));

  REQUIRE(boundaries[4].offset == 512);
  REQUIRE(boundaries[4].timeline_sample == 48312);
  REQUIRE(has_source(boundaries[4], sonare::engine::BoundarySource::kBlockEnd));
}

TEST_CASE("BoundarySplitter clamps out of range offsets", "[engine][boundary]") {
  sonare::engine::BoundarySplitter splitter;
  splitter.begin({0, 100, 128});

  REQUIRE(splitter.add_command(-32));
  REQUIRE(splitter.add_marker(300));

  const auto& boundaries = splitter.finish();
  REQUIRE(boundaries.size() == 2);
  REQUIRE(boundaries[0].offset == 0);
  REQUIRE(has_source(boundaries[0], sonare::engine::BoundarySource::kBlockStart));
  REQUIRE(has_source(boundaries[0], sonare::engine::BoundarySource::kCommand));
  REQUIRE(boundaries[1].offset == 128);
  REQUIRE(has_source(boundaries[1], sonare::engine::BoundarySource::kBlockEnd));
  REQUIRE(has_source(boundaries[1], sonare::engine::BoundarySource::kMarker));
}

TEST_CASE("BoundarySplitter records overflow while preserving block end", "[engine][boundary]") {
  sonare::engine::BoundarySplitter splitter;
  splitter.begin({500, 1000, 4096});

  for (int i = 1; i < 80; ++i) {
    splitter.add_command(i * 16);
  }

  const auto& boundaries = splitter.finish();
  REQUIRE(boundaries.overflowed());
  REQUIRE(boundaries.dropped_count() > 0);
  REQUIRE(boundaries.size() == sonare::engine::BoundaryList::kCapacity);
  REQUIRE(boundaries[0].offset == 0);
  REQUIRE(has_source(boundaries[0], sonare::engine::BoundarySource::kBlockStart));
  REQUIRE(boundaries[boundaries.size() - 1].offset == 4096);
  REQUIRE(has_source(boundaries[boundaries.size() - 1], sonare::engine::BoundarySource::kBlockEnd));
}

TEST_CASE("BoundarySplitter uses prepared block capacity and merges duplicates before overflow",
          "[engine][boundary]") {
  sonare::engine::BoundarySplitter splitter;
  splitter.prepare(97);
  splitter.begin({0, 0, 96});

  for (int offset = 1; offset < 96; ++offset) {
    REQUIRE(splitter.add_command(offset));
    REQUIRE(splitter.add_marker(offset));
  }

  const auto& boundaries = splitter.finish();
  REQUIRE_FALSE(boundaries.overflowed());
  REQUIRE(boundaries.size() == 97);
  REQUIRE(has_source(boundaries[95], sonare::engine::BoundarySource::kCommand));
  REQUIRE(has_source(boundaries[95], sonare::engine::BoundarySource::kMarker));
  REQUIRE(boundaries[96].offset == 96);
  REQUIRE(has_source(boundaries[96], sonare::engine::BoundarySource::kBlockEnd));
}

TEST_CASE("BoundarySplitter reuses a prepared list across shifted blocks", "[engine][boundary]") {
  using sonare::engine::BoundarySource;
  sonare::engine::BoundarySplitter splitter;
  splitter.prepare(97);

  // The first block leaves offset 70 indexed at point 1 after finishing.
  splitter.begin({0, 0, 96});
  REQUIRE(splitter.add_marker(70));
  const auto& first = splitter.finish();
  REQUIRE(first.size() == 3);
  REQUIRE(first[1].offset == 70);

  // A stale index would resolve 70 to the second point of this block (offset 30)
  // and merge into it instead of appending.
  splitter.begin({96, 96, 96});
  REQUIRE(splitter.add_command(5));
  REQUIRE(splitter.add_command(30));
  REQUIRE(splitter.add_command(70));
  REQUIRE(splitter.add_marker(70));

  const auto& second = splitter.finish();
  REQUIRE_FALSE(second.overflowed());
  REQUIRE(second.size() == 5);
  REQUIRE(second[1].offset == 5);
  REQUIRE(second[2].offset == 30);
  REQUIRE(has_source(second[2], BoundarySource::kCommand));
  REQUIRE_FALSE(has_source(second[2], BoundarySource::kMarker));
  REQUIRE(second[3].offset == 70);
  REQUIRE(second[3].render_frame == 166);
  REQUIRE(has_source(second[3], BoundarySource::kCommand));
  REQUIRE(has_source(second[3], BoundarySource::kMarker));
  REQUIRE(second[4].offset == 96);
}
