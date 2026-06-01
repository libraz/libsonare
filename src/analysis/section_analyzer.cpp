#include "analysis/section_analyzer.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/spectral.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

using constants::kEpsilon;

namespace {

/// @brief Cosine similarity above which two sections are considered repetitions.
constexpr float kRepetitionSimilarity = 0.80f;
/// @brief Energy below which a section is treated as low-energy (Intro/Outro/Instrumental).
constexpr float kLowEnergyThreshold = 0.35f;
/// @brief Vocal-likelihood below which a non-boundary low/mid section is Instrumental.
constexpr float kInstrumentalVocalThreshold = 0.40f;
/// @brief Lower edge of the vocal energy band in Hz.
constexpr float kVocalBandLowHz = 300.0f;
/// @brief Upper edge of the vocal energy band in Hz.
constexpr float kVocalBandHighHz = 3400.0f;

void add_fallback_section(std::vector<Section>& sections, float audio_duration) {
  if (!sections.empty() || audio_duration <= 0.0f) {
    return;
  }

  Section section;
  section.start = 0.0f;
  section.end = audio_duration;
  section.energy_level = 0.0f;
  section.confidence = 0.5f;
  section.type = SectionType::Verse;
  sections.push_back(section);
}

}  // namespace

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
    : audio_(audio), config_(config), sr_(audio.sample_rate()), hop_length_(config.hop_length) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  analyze();
}

SectionAnalyzer::SectionAnalyzer(const Audio& audio, const std::vector<float>& boundaries,
                                 const SectionConfig& config)
    : boundaries_(boundaries),
      audio_(audio),
      config_(config),
      sr_(audio.sample_rate()),
      hop_length_(config.hop_length) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute RMS energy curve
  energy_curve_ = rms_energy(audio_, config_.n_fft, config_.hop_length);

  // Create sections from pre-computed boundaries
  float audio_duration = audio_.duration();

  // Add start and end as implicit boundaries
  std::vector<float> all_boundaries;
  all_boundaries.push_back(0.0f);
  for (float b : boundaries_) {
    all_boundaries.push_back(b);
  }
  all_boundaries.push_back(audio_duration);

  // Create sections
  sections_.clear();
  for (size_t i = 0; i + 1 < all_boundaries.size(); ++i) {
    Section section;
    section.start = all_boundaries[i];
    section.end = all_boundaries[i + 1];
    section.energy_level = compute_section_energy(section.start, section.end);
    section.confidence = 0.5f;          // Will be updated by classification
    section.type = SectionType::Verse;  // Default, will be classified later

    // Only add if section is long enough
    if (section.duration() >= config_.min_section_sec * 0.5f) {
      sections_.push_back(section);
    }
  }

  add_fallback_section(sections_, audio_duration);

  // Classify sections
  classify_sections();
}

void SectionAnalyzer::analyze() {
  // Compute RMS energy curve
  energy_curve_ = rms_energy(audio_, config_.n_fft, config_.hop_length);

  // Detect boundaries using BoundaryDetector
  BoundaryConfig boundary_config;
  boundary_config.n_fft = config_.n_fft;
  boundary_config.hop_length = config_.hop_length;
  boundary_config.threshold = config_.boundary_threshold;
  boundary_config.kernel_size = config_.kernel_size;
  boundary_config.peak_distance = config_.min_section_sec;

  BoundaryDetector detector(audio_, boundary_config);
  boundaries_ = detector.boundary_times();

  // Create sections from boundaries
  float audio_duration = audio_.duration();

  // Add start and end as implicit boundaries
  std::vector<float> all_boundaries;
  all_boundaries.push_back(0.0f);
  for (float b : boundaries_) {
    all_boundaries.push_back(b);
  }
  all_boundaries.push_back(audio_duration);

  // Create sections
  sections_.clear();
  for (size_t i = 0; i + 1 < all_boundaries.size(); ++i) {
    Section section;
    section.start = all_boundaries[i];
    section.end = all_boundaries[i + 1];
    section.energy_level = compute_section_energy(section.start, section.end);
    section.confidence = 0.5f;          // Will be updated by classification
    section.type = SectionType::Verse;  // Default, will be classified later

    // Only add if section is long enough
    if (section.duration() >= config_.min_section_sec * 0.5f) {
      sections_.push_back(section);
    }
  }

  add_fallback_section(sections_, audio_duration);

  // Classify sections
  classify_sections();
}

float SectionAnalyzer::compute_section_energy(float start, float end) const {
  if (energy_curve_.empty() || start >= end) {
    return 0.0f;
  }

  float hop_duration = static_cast<float>(hop_length_) / sr_;
  int start_frame = static_cast<int>(start / hop_duration);
  int end_frame = static_cast<int>(end / hop_duration);

  start_frame = std::max(0, start_frame);
  end_frame = std::min(static_cast<int>(energy_curve_.size()), end_frame);

  if (start_frame >= end_frame) {
    return 0.0f;
  }

  float sum = 0.0f;
  for (int i = start_frame; i < end_frame; ++i) {
    sum += energy_curve_[i];
  }

  return sum / (end_frame - start_frame);
}

std::vector<SectionAnalyzer::SectionDescriptor> SectionAnalyzer::build_descriptors() const {
  std::vector<SectionDescriptor> descriptors(sections_.size());
  if (sections_.empty()) {
    return descriptors;
  }

  // Compute a chromagram and a magnitude spectrogram once for the whole signal.
  ChromaConfig chroma_config;
  chroma_config.n_fft = config_.n_fft;
  chroma_config.hop_length = config_.hop_length;
  Chroma chroma = Chroma::compute(audio_, chroma_config);

  Spectrogram spec =
      Spectrogram::compute(audio_, make_stft_config(config_.n_fft, config_.hop_length));
  const std::vector<float>& mag = spec.magnitude();
  const int n_bins = spec.n_bins();
  const int n_spec_frames = spec.n_frames();

  const float hop_duration = static_cast<float>(hop_length_) / static_cast<float>(sr_);
  const float bin_hz = static_cast<float>(sr_) / static_cast<float>(config_.n_fft);
  const int vocal_low_bin =
      std::clamp(static_cast<int>(std::floor(kVocalBandLowHz / bin_hz)), 0, n_bins - 1);
  const int vocal_high_bin =
      std::clamp(static_cast<int>(std::ceil(kVocalBandHighHz / bin_hz)), 0, n_bins - 1);

  // Per-frame spectral flatness to gauge tonal (harmonic/vocal) vs noise-like content.
  const std::vector<float> flatness = (n_spec_frames > 0)
                                          ? spectral_flatness(mag.data(), n_bins, n_spec_frames)
                                          : std::vector<float>{};

  for (size_t s = 0; s < sections_.size(); ++s) {
    const Section& section = sections_[s];
    SectionDescriptor& desc = descriptors[s];

    // Chroma frame range for this section.
    const int c_start = std::clamp(static_cast<int>(section.start / hop_duration), 0,
                                   std::max(0, chroma.n_frames()));
    const int c_end =
        std::clamp(static_cast<int>(section.end / hop_duration), c_start, chroma.n_frames());
    int c_count = 0;
    for (int f = c_start; f < c_end; ++f) {
      for (int c = 0; c < chroma.n_chroma() && c < 12; ++c) {
        desc.chroma[static_cast<size_t>(c)] += chroma.at(c, f);
      }
      ++c_count;
    }
    if (c_count > 0) {
      for (float& value : desc.chroma) value /= static_cast<float>(c_count);
    }
    // L2-normalize the mean chroma vector so cosine similarity is just a dot product.
    float norm = 0.0f;
    for (float value : desc.chroma) norm += value * value;
    norm = std::sqrt(norm);
    if (norm > kEpsilon) {
      for (float& value : desc.chroma) value /= norm;
    }

    // Vocal likelihood: fraction of energy in the vocal band weighted by tonality.
    const int sp_start =
        std::clamp(static_cast<int>(section.start / hop_duration), 0, std::max(0, n_spec_frames));
    const int sp_end =
        std::clamp(static_cast<int>(section.end / hop_duration), sp_start, n_spec_frames);
    double band_energy = 0.0;
    double total_energy = 0.0;
    double tonality_sum = 0.0;
    int sp_count = 0;
    for (int f = sp_start; f < sp_end; ++f) {
      for (int k = 0; k < n_bins; ++k) {
        const float m = mag[static_cast<size_t>(k) * n_spec_frames + f];
        const double e = static_cast<double>(m) * m;
        total_energy += e;
        if (k >= vocal_low_bin && k <= vocal_high_bin) band_energy += e;
      }
      if (f < static_cast<int>(flatness.size())) {
        // Tonality = 1 - flatness (flatness near 0 => tonal => harmonic/vocal).
        tonality_sum += 1.0 - std::clamp(flatness[static_cast<size_t>(f)], 0.0f, 1.0f);
      }
      ++sp_count;
    }
    const float band_ratio =
        total_energy > kEpsilon ? static_cast<float>(band_energy / total_energy) : 0.0f;
    const float mean_tonality = sp_count > 0 ? static_cast<float>(tonality_sum / sp_count) : 0.0f;
    desc.vocal_likelihood = std::clamp(band_ratio * mean_tonality * 2.0f, 0.0f, 1.0f);
    desc.energy = section.energy_level;
  }

  return descriptors;
}

std::vector<float> SectionAnalyzer::self_similarity(
    const std::vector<SectionDescriptor>& descriptors) const {
  const size_t n = descriptors.size();
  std::vector<float> sim(n * n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j) {
      float dot = 0.0f;
      for (size_t c = 0; c < 12; ++c) {
        dot += descriptors[i].chroma[c] * descriptors[j].chroma[c];
      }
      sim[i * n + j] = std::clamp(dot, 0.0f, 1.0f);
    }
  }
  return sim;
}

void SectionAnalyzer::classify_sections() {
  if (sections_.empty()) {
    return;
  }

  const int n_sections = static_cast<int>(sections_.size());

  // Normalize energy levels to [0, 1].
  float max_energy = 0.0f;
  for (const auto& section : sections_) {
    max_energy = std::max(max_energy, section.energy_level);
  }
  if (max_energy > kEpsilon) {
    for (auto& section : sections_) {
      section.energy_level /= max_energy;
    }
  }

  // Build chroma / energy / vocal descriptors and the self-similarity matrix.
  const std::vector<SectionDescriptor> descriptors = build_descriptors();
  const std::vector<float> sim = self_similarity(descriptors);

  // For each section, count how many *other* sections it repeats (cosine >= threshold)
  // and track its mean similarity to its repetitions (used for confidence).
  std::vector<int> repeat_count(n_sections, 0);
  std::vector<float> repeat_mean_sim(n_sections, 0.0f);
  for (int i = 0; i < n_sections; ++i) {
    float sim_sum = 0.0f;
    for (int j = 0; j < n_sections; ++j) {
      if (i == j) continue;
      const float s = sim[static_cast<size_t>(i) * n_sections + j];
      if (s >= kRepetitionSimilarity) {
        ++repeat_count[i];
        sim_sum += s;
      }
    }
    repeat_mean_sim[i] = repeat_count[i] > 0 ? sim_sum / static_cast<float>(repeat_count[i]) : 0.0f;
  }

  // The repeating group with the highest mean energy is the Chorus; other
  // repeating groups are Verses. Determine the energy of the most-repeated group.
  float best_repeat_energy = 0.0f;
  int max_repeats = 0;
  for (int i = 0; i < n_sections; ++i) {
    max_repeats = std::max(max_repeats, repeat_count[i]);
  }
  if (max_repeats > 0) {
    for (int i = 0; i < n_sections; ++i) {
      if (repeat_count[i] >= 1) {
        best_repeat_energy = std::max(best_repeat_energy, descriptors[i].energy);
      }
    }
  }

  for (int i = 0; i < n_sections; ++i) {
    const SectionDescriptor& desc = descriptors[i];
    const bool is_first = (i == 0);
    const bool is_last = (i == n_sections - 1);
    const bool is_repeated = repeat_count[i] >= 1;
    const bool is_low_energy = desc.energy < kLowEnergyThreshold;
    const bool low_vocal = desc.vocal_likelihood < kInstrumentalVocalThreshold;

    SectionType type;
    float confidence;

    if (is_first && is_low_energy) {
      type = SectionType::Intro;
      confidence = 0.6f + 0.3f * (1.0f - desc.energy);
    } else if (is_last && is_low_energy) {
      type = SectionType::Outro;
      confidence = 0.6f + 0.3f * (1.0f - desc.energy);
    } else if (is_repeated && desc.energy >= best_repeat_energy - kLowEnergyThreshold &&
               !low_vocal) {
      // Repeated, high-energy, vocal section => Chorus. Confidence reflects how
      // strongly it matches its repetitions.
      type = SectionType::Chorus;
      confidence = std::clamp(0.5f + 0.5f * repeat_mean_sim[i], 0.0f, 1.0f);
    } else if (is_repeated) {
      // Repeated but lower-energy / less vocal => Verse.
      type = SectionType::Verse;
      confidence = std::clamp(0.5f + 0.5f * repeat_mean_sim[i], 0.0f, 1.0f);
    } else if (!is_first && !is_last && low_vocal) {
      // Non-repeating interior segment with little vocal content => Instrumental.
      type = SectionType::Instrumental;
      confidence = std::clamp(0.5f + 0.4f * (1.0f - desc.vocal_likelihood), 0.0f, 1.0f);
    } else if (!is_first && !is_last) {
      // Distinctive, vocal, non-repeating interior segment => Bridge.
      type = SectionType::Bridge;
      confidence = 0.5f;
    } else {
      type = SectionType::Verse;
      confidence = 0.5f;
    }

    sections_[i].type = type;
    sections_[i].confidence = confidence;
  }
}

std::string SectionAnalyzer::form() const {
  std::ostringstream oss;
  for (const auto& section : sections_) {
    oss << section_type_to_char(section.type);
  }
  return oss.str();
}

Section SectionAnalyzer::section_at(float time) const {
  for (const auto& section : sections_) {
    if (time >= section.start && time < section.end) {
      return section;
    }
  }

  // Return empty section if not found
  Section empty;
  empty.type = SectionType::Verse;
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

std::vector<float> SectionAnalyzer::boundary_times() const { return boundaries_; }

}  // namespace sonare
