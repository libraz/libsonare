#include "analysis/section_analyzer.h"

#include <string>
#include <utility>

#include "analysis/analysis_rate.h"
#include "analysis/beat_analyzer.h"
#include "analysis/section_structure.h"
#include "core/spectrum.h"
#include "util/exception.h"

namespace sonare {

std::string Section::type_string() const { return section_type_to_string(type); }

char section_type_to_char(SectionType type) {
  switch (type) {
    case SectionType::Intro:
      return 'I';
    case SectionType::Verse:
      return 'A';
    case SectionType::PreChorus:
      return 'P';
    case SectionType::Chorus:
      return 'B';
    case SectionType::Bridge:
      return 'C';
    case SectionType::Instrumental:
      return 'S';
    case SectionType::Outro:
      return 'O';
    case SectionType::Unknown:
      return '?';
  }
  return '?';
}

std::string section_type_to_string(SectionType type) {
  switch (type) {
    case SectionType::Intro:
      return "Intro";
    case SectionType::Verse:
      return "Verse";
    case SectionType::PreChorus:
      return "Pre-Chorus";
    case SectionType::Chorus:
      return "Chorus";
    case SectionType::Bridge:
      return "Bridge";
    case SectionType::Instrumental:
      return "Instrumental";
    case SectionType::Outro:
      return "Outro";
    case SectionType::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

SectionAnalyzer::SectionAnalyzer(const Audio& audio, const SectionConfig& config)
    : audio_(analysis_rate_audio(audio)), config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Beats on the same analysis-rate signal, framed like every other analyzer here.
  BeatConfig beat_config;
  beat_config.n_fft = config_.n_fft;
  beat_config.hop_length = config_.hop_length;
  const BeatAnalyzer beats(audio_, beat_config);
  const Spectrogram spec = Spectrogram::compute(
      audio_,
      section_structure::section_chroma_config(config_, audio_.sample_rate()).to_stft_config());
  analyze(section_structure::make_section_grid(beats), spec);
}

SectionAnalyzer::SectionAnalyzer(const Audio& audio, const SectionGrid& grid,
                                 const Spectrogram& spec, const SectionConfig& config)
    : audio_(analysis_rate_audio(audio)), config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  validate_section_grid(grid);
  // Checked against the geometry this analyzer would have computed for itself. audio_ is the
  // analysis-rate signal, which is what the caller's STFT has to have been taken over -- a
  // spectrogram of the native-rate input fails here rather than mapping section spans onto
  // frames of a different hop duration.
  validate_reused_geometry(
      spec,
      section_structure::section_chroma_config(config_, audio_.sample_rate()).to_stft_config(),
      audio_.sample_rate(), audio_.size());
  analyze(grid, spec);
}

void SectionAnalyzer::analyze(const SectionGrid& grid, const Spectrogram& spec) {
  section_structure::StructureResult result = section_structure::analyze_structure(
      section_structure::frame_features(audio_, spec, config_), grid, config_);
  sections_ = std::move(result.sections);
  boundaries_ = std::move(result.boundary_times);
  pair_scores_ = std::move(result.pair_scores);
}

std::string SectionAnalyzer::form() const {
  std::string out;
  out.reserve(sections_.size());
  for (const auto& section : sections_) {
    out += section_type_to_char(section.type);
  }
  return out;
}

Section SectionAnalyzer::section_at(float time) const {
  for (const auto& section : sections_) {
    if (time >= section.start && time < section.end) {
      return section;
    }
  }

  // Return empty section if not found
  Section empty;
  empty.type = SectionType::Unknown;
  empty.start = 0.0f;
  empty.end = 0.0f;
  empty.energy_level = 0.0f;
  empty.confidence = 0.0f;
  return empty;
}

float SectionAnalyzer::duration() const {
  if (sections_.empty()) {
    return 0.0f;
  }
  return sections_.back().end;
}

}  // namespace sonare
