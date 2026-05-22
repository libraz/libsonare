#pragma once

/// @file spectrogram.h
/// @brief Meter-oriented spectrogram views.

#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"

namespace sonare::analysis::meter {

struct MeterSpectrogramConfig {
  StftConfig stft;
  float db_ref = 1.0f;
  float db_amin = 1e-10f;
  float top_db = -1.0f;
};

struct MeterSpectrogramResult {
  std::vector<float> frequencies;
  std::vector<float> times;
  std::vector<float> magnitude;
  std::vector<float> power;
  std::vector<float> db;
  int n_bins = 0;
  int n_frames = 0;
  int n_fft = 0;
  int hop_length = 0;
  int sample_rate = 0;
};

MeterSpectrogramResult spectrogram(const Audio& audio, const MeterSpectrogramConfig& config = {});

}  // namespace sonare::analysis::meter
