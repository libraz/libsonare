// Prints, for one recording, the monophonic transcription's pitch track and
// notes, and optionally the notes the same stages produce along a reference F0
// contour, so an evaluation can separate tracker errors from segmentation errors.
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/audio.h"
#include "editing/note_model/note_transcriber.h"
#include "editing/pitch_editor/f0_provider.h"

namespace {
using namespace sonare;
using editing::note_model::TranscribeConfig;
using editing::note_model::TranscribedNote;
using editing::pitch_editor::F0Track;

struct ReferenceContour {
  std::vector<double> times_sec;
  std::vector<float> f0_hz;
};

/// Reads `time_sec,f0_hz` rows; a non-positive F0 marks an unvoiced frame.
ReferenceContour read_contour(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open reference contour " + path);
  ReferenceContour contour;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream row(line);
    std::string time_field;
    std::string f0_field;
    if (!std::getline(row, time_field, ',') || !std::getline(row, f0_field)) {
      throw std::runtime_error("malformed contour row: " + line);
    }
    const double time_sec = std::stod(time_field);
    if (!contour.times_sec.empty() && time_sec <= contour.times_sec.back()) {
      throw std::runtime_error("contour times must increase");
    }
    contour.times_sec.push_back(time_sec);
    contour.f0_hz.push_back(std::stof(f0_field));
  }
  if (contour.times_sec.size() < 2) throw std::runtime_error("contour has fewer than two rows");
  return contour;
}

/// The reference contour resampled to @p grid's frames by nearest row; frames
/// farther than one reference step from any row are unvoiced.
F0Track oracle_track(const F0Track& grid, const ReferenceContour& contour) {
  F0Track track;
  track.sample_rate = grid.sample_rate;
  track.hop_length = grid.hop_length;
  track.frame_rate_hz = grid.frame_rate_hz;
  const double step = contour.times_sec[1] - contour.times_sec[0];
  const double frame_sec = grid.samples_per_frame() / static_cast<double>(grid.sample_rate);
  size_t row = 0;
  for (int frame = 0; frame < grid.n_frames(); ++frame) {
    const double time_sec = frame * frame_sec;
    while (row + 1 < contour.times_sec.size() && std::abs(contour.times_sec[row + 1] - time_sec) <=
                                                     std::abs(contour.times_sec[row] - time_sec)) {
      ++row;
    }
    const bool near = std::abs(contour.times_sec[row] - time_sec) <= step;
    const float hz = near ? contour.f0_hz[row] : 0.0f;
    const bool voiced = std::isfinite(hz) && hz > 0.0f;
    track.f0_hz.push_back(voiced ? hz : 0.0f);
    track.voiced.push_back(voiced);
  }
  return track;
}

void print_notes(const std::vector<TranscribedNote>& notes, int sample_rate) {
  std::cout << '[';
  for (size_t i = 0; i < notes.size(); ++i) {
    const TranscribedNote& note = notes[i];
    std::cout << (i ? "," : "")
              << "{\"onset_sec\":" << static_cast<double>(note.onset_sample) / sample_rate
              << ",\"offset_sec\":" << static_cast<double>(note.offset_sample) / sample_rate
              << ",\"midi\":" << static_cast<int>(note.note) << ",\"hz\":" << note.median_hz << '}';
  }
  std::cout << ']';
}

}  // namespace

int main(int argc, char** argv) {
  const char* audio_path = nullptr;
  const char* contour_path = nullptr;
  if (argc == 2) {
    audio_path = argv[1];
  } else if (argc == 4 && std::string(argv[2]) == "--reference-f0") {
    audio_path = argv[1];
    contour_path = argv[3];
  } else {
    std::cerr << "usage: sonare_eval_note_oracle AUDIO [--reference-f0 CSV]\n";
    return 2;
  }
  try {
    const Audio audio = Audio::from_file(audio_path);
    if (audio.empty()) throw std::runtime_error("decoded audio is empty");
    const TranscribeConfig config;

    // The same tracker settings transcription uses, so the exported track and the
    // oracle grid match the frames the default path segments.
    PitchConfig pitch_config =
        editing::pitch_editor::default_editing_pitch_config(audio.sample_rate());
    const auto range = editing::note_model::resolve_transcribe_f0_range(config);
    pitch_config.fmin = range.fmin;
    pitch_config.fmax = range.fmax;
    pitch_config.center = true;
    editing::pitch_editor::PyinF0Provider provider(pitch_config);
    const F0Track detected = provider.detect(audio);
    const double frame_sec =
        detected.samples_per_frame() / static_cast<double>(detected.sample_rate);

    std::cout << std::setprecision(9) << "{\"sample_rate\":" << audio.sample_rate()
              << ",\"frame_sec\":" << frame_sec << ",\"track\":{\"f0_hz\":[";
    for (int i = 0; i < detected.n_frames(); ++i) {
      std::cout << (i ? "," : "") << (detected.voiced[i] ? detected.f0_hz[i] : 0.0f);
    }
    std::cout << "]},\"notes\":";
    print_notes(editing::note_model::transcribe_notes(audio, config), audio.sample_rate());
    if (contour_path != nullptr) {
      const F0Track oracle = oracle_track(detected, read_contour(contour_path));
      std::cout << ",\"oracle_notes\":";
      print_notes(editing::note_model::transcribe_monophonic_track(audio, oracle, config),
                  audio.sample_rate());
    }
    std::cout << "}\n";
  } catch (const std::exception& error) {
    std::cerr << "sonare_eval_note_oracle: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
