/// @file ism_golden_test.cpp
/// @brief Optional cross-engine ISM oracle: compares shoebox image-source
///        arrival times against a pyroomacoustics-generated golden.
///
/// The golden is produced by tests/fixtures/acoustic/room_sim/gen_ism_golden.py
/// (pyroomacoustics, an optional dev dependency). When the fixture is absent the
/// test SKIPs — the analytic image-source tests still cover this in normal CI.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "acoustic/image_source.h"
#include "acoustic/material.h"
#include "acoustic/room_model.h"

using namespace sonare;
using namespace sonare::acoustic;

namespace {

std::filesystem::path fixture_root() {
  if (const char* root = std::getenv("SONARE_ACOUSTIC_FIXTURE_ROOT")) {
    if (root[0] != '\0') return std::filesystem::path(root);
  }
  return "tests/fixtures/acoustic";
}

struct Golden {
  RoomDimensions dims;
  Vec3 source, listener;
  int sample_rate = 48000;
  int max_order = 2;
  float absorption = 0.0f;
  // (distance_m, reflection_product) pairs, sorted by distance.
  std::vector<std::pair<float, float>> images;
};

bool read_golden(const std::filesystem::path& path, Golden& g) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string tag;
    ss >> tag;
    if (tag == "room") {
      ss >> g.dims.length >> g.dims.width >> g.dims.height;
    } else if (tag == "source") {
      ss >> g.source.x >> g.source.y >> g.source.z;
    } else if (tag == "listener") {
      ss >> g.listener.x >> g.listener.y >> g.listener.z;
    } else if (tag == "sample_rate") {
      ss >> g.sample_rate;
    } else if (tag == "max_order") {
      ss >> g.max_order;
    } else if (tag == "absorption") {
      ss >> g.absorption;
    } else if (tag == "image") {
      float dist, refl;
      ss >> dist >> refl;
      g.images.emplace_back(dist, refl);
    }
  }
  std::sort(g.images.begin(), g.images.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return !g.images.empty();
}

}  // namespace

TEST_CASE("shoebox ISM arrival times match the pyroomacoustics golden",
          "[acoustic][image_source][dataset]") {
  const std::filesystem::path path = fixture_root() / "room_sim" / "ism_golden.tsv";
  Golden g;
  if (!read_golden(path, g)) {
    SKIP("pyroomacoustics ISM golden not present (run gen_ism_golden.py)");
  }

  ShoeboxRoom room;
  room.dims = g.dims;
  for (auto& w : room.walls) w = uniform_material(g.absorption, 0.1f);
  const SourceListener pl{g.source, g.listener};

  const auto images = shoebox_image_sources(room, pl, g.max_order);
  std::vector<std::pair<float, float>> got;  // (distance, broadband reflection)
  got.reserve(images.size());
  for (const auto& im : images) {
    // Uniform walls => every band carries the same beta^order, so the arithmetic
    // mean over bands is the broadband pressure reflection product pyroomacoustics
    // reports in its `damping`.
    float sum = 0.0f;
    for (float r : im.reflection) sum += r;
    const float refl =
        im.reflection.empty() ? 1.0f : sum / static_cast<float>(im.reflection.size());
    got.emplace_back(im.distance, refl);
  }
  // Sort both by (distance, reflection) so equal-distance images (symmetric
  // paths) pair up deterministically before the element-wise comparison.
  const auto by_dist_refl = [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first < b.first : a.second < b.second;
  };
  std::sort(got.begin(), got.end(), by_dist_refl);
  std::sort(g.images.begin(), g.images.end(), by_dist_refl);

  REQUIRE(got.size() == g.images.size());
  // Arrival time within ±1 sample == distance within c/sr metres.
  const float tol_m = kSoundSpeed / static_cast<float>(g.sample_rate);
  for (size_t i = 0; i < got.size(); ++i) {
    REQUIRE(std::fabs(got[i].first - g.images[i].first) <= tol_m);
    // Reflection product (and hence polarity): relative error < 1%.
    REQUIRE(std::fabs(got[i].second - g.images[i].second) <=
            0.01f * std::fabs(g.images[i].second) + 1e-6f);
  }
}
