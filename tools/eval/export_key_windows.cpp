#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/analysis_rate.h"
#include "analysis/chord_analyzer.h"
#include "analysis/key_analyzer.h"
#include "feature/pitch.h"

namespace {
using namespace sonare;

constexpr double kWindowSeconds = 8.0;
constexpr double kFineWindowSeconds = 1.0;
constexpr float kAutoProfileMargin = 0.025f;

const std::array<std::pair<KeyProfileType, const char*>, 7> kProfileTypes = {{
    {KeyProfileType::KrumhanslSchmuckler, "KrumhanslSchmuckler"},
    {KeyProfileType::Shaath, "Shaath"},
    {KeyProfileType::FaraldoEDMT, "FaraldoEDMT"},
    {KeyProfileType::FaraldoEDMA, "FaraldoEDMA"},
    {KeyProfileType::FaraldoEDMM, "FaraldoEDMM"},
    {KeyProfileType::BellmanBudge, "BellmanBudge"},
    {KeyProfileType::Temperley, "Temperley"},
}};

float profile_score(const std::vector<KeyCandidate>& candidates) {
  if (candidates.size() < 2) throw std::runtime_error("profile returned fewer than two keys");
  return candidates[0].correlation +
         0.35f * (candidates[0].correlation - candidates[1].correlation);
}

KeyProfileType selected_profile(const std::array<float, 12>& mean, const KeyConfig& config) {
  const float ks_score = profile_score(KeyAnalyzer(mean,
                                                   [&] {
                                                     KeyConfig candidate_config = config;
                                                     candidate_config.profile_type =
                                                         KeyProfileType::KrumhanslSchmuckler;
                                                     return candidate_config;
                                                   }())
                                           .all_candidates());
  float best_score = ks_score;
  KeyProfileType best_type = KeyProfileType::KrumhanslSchmuckler;
  for (const auto& [type, name] : kProfileTypes) {
    (void)name;
    KeyConfig candidate_config = config;
    candidate_config.profile_type = type;
    const float score = profile_score(KeyAnalyzer(mean, candidate_config).all_candidates());
    if (type != KeyProfileType::KrumhanslSchmuckler && score > best_score) {
      best_score = score;
      best_type = type;
    }
  }
  return best_type != KeyProfileType::KrumhanslSchmuckler &&
                 best_score >= ks_score + kAutoProfileMargin
             ? best_type
             : KeyProfileType::KrumhanslSchmuckler;
}

const char* profile_name(KeyProfileType type) {
  for (const auto& [candidate, name] : kProfileTypes) {
    if (candidate == type) return name;
  }
  throw std::runtime_error("unknown key profile");
}

bool same_key(const Key& left, const Key& right) {
  return left.root == right.root && left.mode == right.mode;
}

void assert_parity(const KeyAnalyzer& left, const KeyAnalyzer& right) {
  if (left.mean_chroma() != right.mean_chroma()) {
    throw std::runtime_error("whole-track mean differs from the default audio analyzer");
  }
  const auto& a = left.all_candidates();
  const auto& b = right.all_candidates();
  if (a.size() != 24 || b.size() != 24) throw std::runtime_error("expected all 24 keys");
  for (size_t i = 0; i < a.size(); ++i) {
    if (!same_key(a[i].key, b[i].key) || a[i].correlation != b[i].correlation ||
        a[i].key.confidence != b[i].key.confidence) {
      throw std::runtime_error("ordered key candidates differ from default audio analyzer");
    }
  }
}

void print_candidates(const std::vector<KeyCandidate>& candidates, const char* profile) {
  if (candidates.size() != 24) throw std::runtime_error("expected 24 key candidates");
  std::cout << '[';
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (i != 0) std::cout << ',';
    const auto& candidate = candidates[i];
    std::cout << "{\"root\":" << static_cast<int>(candidate.key.root) << ",\"mode\":\""
              << (candidate.key.mode == Mode::Minor ? "minor" : "major")
              << "\",\"correlation\":" << candidate.correlation
              << ",\"posterior\":" << candidate.key.confidence << ",\"profile\":\"" << profile
              << "\"}";
  }
  std::cout << ']';
}

struct WindowMean {
  double start = 0.0;
  double end = 0.0;
  double supported_seconds = 0.0;
  size_t frame_count = 0;
  std::array<double, 12> sum{};
};

std::vector<WindowMean> pool_by_frame_centers(const Chroma& chroma, double duration,
                                              int sample_rate, double window_seconds) {
  const size_t window_count = static_cast<size_t>(std::ceil(duration / window_seconds));
  std::vector<WindowMean> windows(window_count);
  for (size_t i = 0; i < window_count; ++i) {
    windows[i].start = static_cast<double>(i) * window_seconds;
    windows[i].end = std::min(duration, windows[i].start + window_seconds);
  }

  const int hop = chroma.hop_length();
  for (int frame = 0; frame < chroma.n_frames(); ++frame) {
    const double timestamp = static_cast<double>(frame) * hop / sample_rate;
    if (timestamp >= duration) continue;
    const size_t window = static_cast<size_t>(timestamp / window_seconds);
    if (window >= window_count) continue;
    WindowMean& aggregate = windows[window];
    ++aggregate.frame_count;
    for (size_t root = 0; root < aggregate.sum.size(); ++root) {
      aggregate.sum[root] += chroma.data()[root * chroma.n_frames() + frame];
    }
  }
  return windows;
}

std::vector<WindowMean> pool_by_frame_support(const Chroma& chroma, double duration,
                                              int sample_rate, double window_seconds) {
  const size_t window_count = static_cast<size_t>(std::ceil(duration / window_seconds));
  std::vector<WindowMean> windows(window_count);
  for (size_t i = 0; i < window_count; ++i) {
    windows[i].start = static_cast<double>(i) * window_seconds;
    windows[i].end = std::min(duration, windows[i].start + window_seconds);
  }

  const int hop = chroma.hop_length();
  for (int frame = 0; frame < chroma.n_frames(); ++frame) {
    const double frame_start = static_cast<double>(frame) * hop / sample_rate;
    if (frame_start >= duration) continue;
    const double frame_end = std::min(duration, static_cast<double>(frame + 1) * hop / sample_rate);
    size_t window = static_cast<size_t>(frame_start / window_seconds);
    while (window < window_count && windows[window].start < frame_end) {
      WindowMean& aggregate = windows[window];
      const double overlap =
          std::min(frame_end, aggregate.end) - std::max(frame_start, aggregate.start);
      if (overlap > 0.0) {
        ++aggregate.frame_count;
        aggregate.supported_seconds += overlap;
        for (size_t root = 0; root < aggregate.sum.size(); ++root) {
          aggregate.sum[root] +=
              static_cast<double>(chroma.data()[root * chroma.n_frames() + frame]) * overlap;
        }
      }
      ++window;
    }
  }

  double total_support = 0.0;
  for (const auto& window : windows) total_support += window.supported_seconds;
  if (std::abs(total_support - duration) > 1e-9) {
    throw std::runtime_error("fine windows do not cover the recording's frame support");
  }
  return windows;
}

std::vector<KeyCandidate> analyze_window(const WindowMean& window, const KeyConfig& config,
                                         bool fine) {
  if (window.frame_count == 0 || (fine && !(window.supported_seconds > 0.0))) {
    if (fine) throw std::runtime_error("fine window has no overlapping chroma frame support");
    throw std::runtime_error("empty or invalid eight-second chroma window");
  }
  std::array<float, 12> mean{};
  for (size_t root = 0; root < mean.size(); ++root) {
    const double divisor =
        fine ? window.supported_seconds : static_cast<double>(window.frame_count);
    mean[root] = static_cast<float>(window.sum[root] / divisor);
    if (!std::isfinite(mean[root])) throw std::runtime_error("non-finite window chroma");
  }
  const KeyAnalyzer local(mean, config);
  if (local.all_candidates().size() != 24) {
    throw std::runtime_error("window did not return 24 keys");
  }
  return local.all_candidates();
}

void print_windows(const std::vector<WindowMean>& windows, const KeyConfig& config,
                   const char* profile, bool fine) {
  std::cout << '[';
  for (size_t i = 0; i < windows.size(); ++i) {
    if (i != 0) std::cout << ',';
    const WindowMean& window = windows[i];
    std::cout << "{\"start\":" << window.start << ",\"end\":" << window.end
              << ",\"selected_profile\":\"" << profile
              << "\",\"frame_count\":" << window.frame_count;
    if (fine) std::cout << ",\"supported_seconds\":" << window.supported_seconds;
    std::cout << ",\"candidates\":";
    print_candidates(analyze_window(window, config, fine), profile);
    std::cout << '}';
  }
  std::cout << ']';
}

}  // namespace

int main(int argc, char** argv) {
  bool include_fine_windows = false;
  const char* audio_path = nullptr;
  if (argc == 2) {
    audio_path = argv[1];
  } else if (argc == 3 && std::string(argv[1]) == "--fine-windows") {
    include_fine_windows = true;
    audio_path = argv[2];
  } else {
    std::cerr << "usage: sonare_eval_key_windows [--fine-windows] AUDIO\n";
    return 2;
  }
  try {
    using namespace sonare;
    const Audio input = Audio::from_file(audio_path);
    const Audio audio = analysis_rate_audio(input);
    if (audio.empty()) throw std::runtime_error("decoded audio is empty");
    const double duration = static_cast<double>(input.size()) / input.sample_rate();
    const float tuning = measure_tuning(audio);

    KeyConfig config;
    config.tuning = tuning;
    ChordChromaConfig chroma_config;
    chroma_config.hop_length = config.hop_length * chord_constants::kChordChromaHopMultiplier;
    chroma_config.tuning = tuning;
    chroma_config.bass_weighted = false;
    chroma_config.use_hpss = config.use_hpss;
    chroma_config.hpss_n_fft = config.n_fft;
    chroma_config.hpss_hop_length = config.hop_length;
    chroma_config.highpass_hz = config.high_pass_hz;
    const Chroma chroma = chord_chroma(audio, chroma_config);
    if (chroma.empty() || chroma.n_chroma() != 12) {
      throw std::runtime_error("production key chroma is empty or not 12-bin");
    }

    const auto global_mean = chroma.mean_energy();
    const KeyAnalyzer audio_default(audio, config);
    const KeyAnalyzer global_default(global_mean, config);
    assert_parity(audio_default, global_default);

    const KeyProfileType profile_type = selected_profile(global_mean, config);
    const char* const profile = profile_name(profile_type);
    KeyConfig local_config = config;
    local_config.profile_type = profile_type;
    const KeyAnalyzer selected_global(global_mean, local_config);
    assert_parity(audio_default, selected_global);

    const int sample_rate = audio.sample_rate();
    const int hop = chroma.hop_length();
    const auto windows = pool_by_frame_centers(chroma, duration, sample_rate, kWindowSeconds);
    const auto fine_windows =
        include_fine_windows
            ? pool_by_frame_support(chroma, duration, sample_rate, kFineWindowSeconds)
            : std::vector<WindowMean>{};
    size_t active_frames = 0;
    for (int frame = 0; frame < chroma.n_frames(); ++frame) {
      const double timestamp = static_cast<double>(frame) * chroma.hop_length() / sample_rate;
      if (timestamp < duration) ++active_frames;
    }

    std::cout
        << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "{\"version\":\"key_window_export_v1\",\"duration\":" << duration
        << ",\"duration_seconds\":" << duration << ",\"sample_rate\":" << sample_rate
        << ",\"hop_length\":" << hop << ",\"frame_count\":" << chroma.n_frames()
        << ",\"active_frame_count\":" << active_frames << ",\"tuning\":" << tuning
        << ",\"selected_profile\":\"" << profile
        << "\",\"config_fingerprint\":\"key_chroma_v1_hop_2048_highpass_80_no_hpss_no_bassblend\""
        << ",\"selector\":\"production_auto_v1_ordered_profile_score_top_plus_0.35_gap_margin_0."
           "025\""
        << ",\"global_winner\":{\"root\":" << static_cast<int>(selected_global.root())
        << ",\"mode\":\"" << (selected_global.mode() == Mode::Minor ? "minor" : "major") << "\"}"
        << ",\"global_candidates\":";
    print_candidates(selected_global.all_candidates(), profile);
    std::cout << ",\"windows\":";
    print_windows(windows, local_config, profile, false);
    if (include_fine_windows) {
      std::cout << ",\"fine_window_seconds\":" << kFineWindowSeconds
                << ",\"fine_pooling\":\"physical_frame_support_overlap\",\"fine_windows\":";
      print_windows(fine_windows, local_config, profile, true);
    }
    std::cout << "}\n";
  } catch (const std::exception& error) {
    std::cerr << "key window export failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
