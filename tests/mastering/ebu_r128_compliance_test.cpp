#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "dr_wav.h"
#include "metering/lufs.h"
#include "metering/true_peak.h"

using Catch::Matchers::WithinAbs;

namespace sonare::mastering {
namespace {

std::filesystem::path fixture_root() {
  if (const char* root = std::getenv("SONARE_EBU_R128_FIXTURE_ROOT")) {
    if (root[0] != '\0') {
      return std::filesystem::path(root);
    }
  }
  return "tests/fixtures/ebu_r128";
}

struct EbuVector {
  std::string filename;
  float integrated_lufs = 0.0f;
  bool has_lra = false;
  float lra = 0.0f;
  bool has_true_peak = false;
  float max_true_peak_db = 0.0f;
  float tolerance_lu = 0.1f;
  float tolerance_db = 0.1f;
};

struct WavFixture {
  std::vector<float> samples;
  size_t frames = 0;
  int channels = 0;
  int sample_rate = 0;
};

WavFixture load_wav_interleaved(const std::filesystem::path& path) {
  drwav wav;
  REQUIRE(drwav_init_file(&wav, path.string().c_str(), nullptr));

  WavFixture fixture;
  fixture.frames = static_cast<size_t>(wav.totalPCMFrameCount);
  fixture.channels = static_cast<int>(wav.channels);
  fixture.sample_rate = static_cast<int>(wav.sampleRate);
  fixture.samples.resize(fixture.frames * static_cast<size_t>(fixture.channels));

  const drwav_uint64 frames_read =
      drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, fixture.samples.data());
  drwav_uninit(&wav);

  REQUIRE(static_cast<size_t>(frames_read) == fixture.frames);
  REQUIRE(fixture.frames > 0);
  REQUIRE(fixture.channels > 0);
  REQUIRE(fixture.sample_rate > 0);
  return fixture;
}

float max_true_peak_db(const WavFixture& fixture) {
  float max_peak = 0.0f;
  std::vector<float> channel(fixture.frames);
  for (int ch = 0; ch < fixture.channels; ++ch) {
    for (size_t frame = 0; frame < fixture.frames; ++frame) {
      channel[frame] =
          fixture.samples[frame * static_cast<size_t>(fixture.channels) + static_cast<size_t>(ch)];
    }
    max_peak = std::max(max_peak, metering::true_peak(channel.data(), channel.size(), 4));
  }
  return max_peak > 0.0f ? 20.0f * std::log10(max_peak) : -std::numeric_limits<float>::infinity();
}

bool parse_optional_float(const std::string& token, float& out) {
  if (token == "-" || token.empty()) return false;
  out = std::stof(token);
  return true;
}

std::vector<EbuVector> read_manifest(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::vector<EbuVector> rows;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;

    std::istringstream stream(line);
    std::vector<std::string> fields;
    std::string field;
    while (std::getline(stream, field, '\t')) {
      fields.push_back(field);
    }
    if (fields.size() < 6) continue;

    EbuVector row;
    row.filename = fields[0];
    row.integrated_lufs = std::stof(fields[1]);
    row.has_lra = parse_optional_float(fields[2], row.lra);
    row.has_true_peak = parse_optional_float(fields[3], row.max_true_peak_db);
    row.tolerance_lu = std::stof(fields[4]);
    row.tolerance_db = std::stof(fields[5]);
    rows.push_back(row);
  }
  return rows;
}

}  // namespace

TEST_CASE("EBU R128 optional loudness test vectors", "[mastering][ebu_r128]") {
  const std::filesystem::path root = fixture_root();
  const std::filesystem::path manifest = root / "manifest.tsv";
  if (!std::filesystem::exists(manifest)) {
    SKIP("EBU R128 manifest is not present");
  }

  const auto vectors = read_manifest(manifest);
  REQUIRE_FALSE(vectors.empty());

  size_t checked = 0;
  for (const auto& vector : vectors) {
    const std::filesystem::path audio_path = root / vector.filename;
    if (!std::filesystem::exists(audio_path)) continue;

    INFO("EBU vector: " << vector.filename);
    const WavFixture audio = load_wav_interleaved(audio_path);
    const auto loudness = metering::lufs_interleaved(audio.samples.data(), audio.frames,
                                                     audio.channels, audio.sample_rate);

    REQUIRE(std::isfinite(loudness.integrated_lufs));
    WARN("Report EBU R128 fixture " << vector.filename << " measured integrated_lufs "
                                    << loudness.integrated_lufs << "; expected "
                                    << vector.integrated_lufs << " +/- " << vector.tolerance_lu);
    REQUIRE_THAT(loudness.integrated_lufs, WithinAbs(vector.integrated_lufs, vector.tolerance_lu));

    if (vector.has_lra) {
      WARN("Report EBU R128 fixture " << vector.filename << " measured lra "
                                      << loudness.loudness_range << "; expected " << vector.lra
                                      << " +/- " << vector.tolerance_lu);
      REQUIRE_THAT(loudness.loudness_range, WithinAbs(vector.lra, vector.tolerance_lu));
    }
    if (vector.has_true_peak) {
      const float true_peak_db = max_true_peak_db(audio);
      WARN("Report EBU R128 fixture " << vector.filename << " measured true_peak_db "
                                      << true_peak_db << "; expected " << vector.max_true_peak_db
                                      << " +/- " << vector.tolerance_db);
      REQUIRE_THAT(true_peak_db, WithinAbs(vector.max_true_peak_db, vector.tolerance_db));
    }
    ++checked;
  }

  if (checked == 0) {
    SKIP("No EBU R128 audio fixtures are present");
  }
}

}  // namespace sonare::mastering
