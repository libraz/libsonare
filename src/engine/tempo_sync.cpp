#include "engine/tempo_sync.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "core/fft.h"
#include "core/window.h"
#include "effects/phase_vocoder.h"
#include "transport/musical_time.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::engine {
namespace {

using sonare::constants::kSpectrumEpsilon;
using sonare::constants::kTwoPi;
using sonare::constants::kTwoPiD;

float wrap_phase(float phase) {
  if (!std::isfinite(phase)) return 0.0f;
  return std::remainder(phase, kTwoPi);
}

double wrap_phase(double phase) {
  if (!std::isfinite(phase)) return 0.0;
  return std::remainder(phase, kTwoPiD);
}

std::vector<float> stretch_segment(const float* source, size_t source_samples,
                                   size_t target_samples,
                                   const TempoSyncWarpBakeConfig& bake_config) {
  StreamingPhaseVocoderConfig config;
  config.sample_rate = bake_config.sample_rate;
  config.n_fft = bake_config.n_fft;
  config.hop_length = bake_config.hop_length;
  config.phase_lock = bake_config.phase_lock;

  const float rate =
      static_cast<float>(static_cast<double>(source_samples) / static_cast<double>(target_samples));
  StreamingPhaseVocoder stretcher(config);
  stretcher.reserve(source_samples, target_samples + static_cast<size_t>(config.n_fft));

  std::vector<float> out(target_samples + static_cast<size_t>(config.n_fft), 0.0f);
  size_t written = stretcher.process_into(source, source_samples, rate, out.data(), out.size());
  written += stretcher.finalize_into(rate, out.data() + written, out.size() - written);
  out.resize(std::min(written, target_samples));
  if (out.size() < target_samples) out.resize(target_samples, 0.0f);
  return out;
}

std::vector<std::vector<float>> stretch_segment_channels(const std::vector<const float*>& sources,
                                                         size_t source_samples,
                                                         size_t target_samples,
                                                         const TempoSyncWarpBakeConfig& config) {
  const int channels = static_cast<int>(sources.size());
  const int n_fft = config.n_fft;
  const int hop = config.hop_length;
  const int n_bins = n_fft / 2 + 1;
  const int pad = n_fft / 2;
  const float rate =
      static_cast<float>(static_cast<double>(source_samples) / static_cast<double>(target_samples));
  const size_t padded_length = source_samples + static_cast<size_t>(n_fft);
  const int input_frames =
      std::max(2, 1 + static_cast<int>((padded_length - static_cast<size_t>(n_fft)) /
                                       static_cast<size_t>(hop)));
  const int output_frames = std::max(
      1,
      static_cast<int>(std::ceil(static_cast<double>(input_frames) / static_cast<double>(rate))));

  FFT fft(n_fft);
  const std::vector<float>& analysis_win_short = get_window_cached(WindowType::Hann, n_fft, true);
  const std::vector<float>& synthesis_win = get_window_cached(WindowType::Hann, n_fft, false);
  std::vector<float> window_product(static_cast<size_t>(n_fft));
  for (int i = 0; i < n_fft; ++i) {
    window_product[static_cast<size_t>(i)] =
        analysis_win_short[static_cast<size_t>(i)] * synthesis_win[static_cast<size_t>(i)];
  }

  std::vector<std::vector<std::complex<float>>> spectra(
      static_cast<size_t>(channels),
      std::vector<std::complex<float>>(static_cast<size_t>(input_frames * n_bins)));
  std::vector<float> frame(static_cast<size_t>(n_fft), 0.0f);
  std::vector<std::complex<float>> bins(static_cast<size_t>(n_bins));
  for (int ch = 0; ch < channels; ++ch) {
    for (int frame_index = 0; frame_index < input_frames; ++frame_index) {
      const size_t start = static_cast<size_t>(frame_index) * static_cast<size_t>(hop);
      for (int i = 0; i < n_fft; ++i) {
        const int64_t raw_index =
            static_cast<int64_t>(start) + static_cast<int64_t>(i) - static_cast<int64_t>(pad);
        frame[static_cast<size_t>(i)] =
            raw_index >= 0 && static_cast<size_t>(raw_index) < source_samples
                ? sources[static_cast<size_t>(ch)][static_cast<size_t>(raw_index)] *
                      analysis_win_short[static_cast<size_t>(i)]
                : 0.0f;
      }
      fft.forward(frame.data(), bins.data());
      std::copy(bins.begin(), bins.end(),
                spectra[static_cast<size_t>(ch)].begin() +
                    static_cast<std::ptrdiff_t>(frame_index * n_bins));
    }
  }

  std::vector<std::vector<float>> ola(
      static_cast<size_t>(channels),
      std::vector<float>(static_cast<size_t>(output_frames * hop + n_fft), 0.0f));
  std::vector<float> window_sum(static_cast<size_t>(output_frames * hop + n_fft), 0.0f);
  std::vector<std::complex<float>> synth_bins(static_cast<size_t>(n_bins));
  std::vector<float> ref_mag(static_cast<size_t>(n_bins), 0.0f);
  std::vector<float> ref_phase(static_cast<size_t>(n_bins), 0.0f);
  std::vector<float> ref_inst_freq(static_cast<size_t>(n_bins), 0.0f);
  std::vector<std::vector<float>> channel_mag(static_cast<size_t>(channels),
                                              std::vector<float>(static_cast<size_t>(n_bins)));
  std::vector<std::vector<float>> channel_phase(static_cast<size_t>(channels),
                                                std::vector<float>(static_cast<size_t>(n_bins)));
  std::vector<double> phase_acc(static_cast<size_t>(n_bins), 0.0);
  std::vector<int> peaks;
  std::vector<int> nearest_peak(static_cast<size_t>(n_bins), -1);
  const double time_step = static_cast<double>(hop) / static_cast<double>(config.sample_rate);

  auto spectrum_at = [&](int ch, int frame_index, int bin) -> const std::complex<float>& {
    return spectra[static_cast<size_t>(ch)][static_cast<size_t>(frame_index * n_bins + bin)];
  };

  for (int t_out = 0; t_out < output_frames; ++t_out) {
    float t_in_f = static_cast<float>(t_out) * rate;
    int t_in = static_cast<int>(t_in_f);
    float frac = t_in_f - static_cast<float>(t_in);
    if (t_in >= input_frames - 1) {
      t_in = input_frames - 2;
      frac = 1.0f;
    }
    if (t_in < 0) {
      t_in = 0;
      frac = 0.0f;
    }

    for (int k = 0; k < n_bins; ++k) {
      std::complex<float> ref0{};
      std::complex<float> ref1{};
      for (int ch = 0; ch < channels; ++ch) {
        const auto frame0 = spectrum_at(ch, t_in, k);
        const auto frame1 = spectrum_at(ch, t_in + 1, k);
        ref0 += frame0;
        ref1 += frame1;
        channel_mag[static_cast<size_t>(ch)][static_cast<size_t>(k)] =
            std::abs(frame0) * (1.0f - frac) + std::abs(frame1) * frac;
        const float phase0 = std::arg(frame0);
        const float phase1 = std::arg(frame1);
        channel_phase[static_cast<size_t>(ch)][static_cast<size_t>(k)] =
            phase0 + frac * wrap_phase(phase1 - phase0);
      }
      ref_mag[static_cast<size_t>(k)] = std::abs(ref0) * (1.0f - frac) + std::abs(ref1) * frac;
      const float ref_phase0 = std::arg(ref0);
      const float ref_phase1 = std::arg(ref1);
      ref_phase[static_cast<size_t>(k)] = ref_phase0 + frac * wrap_phase(ref_phase1 - ref_phase0);
      const float bin_freq = static_cast<float>(k) * static_cast<float>(config.sample_rate) /
                             static_cast<float>(n_fft);
      const float expected_advance = kTwoPi * bin_freq * static_cast<float>(time_step);
      const float phase_diff = wrap_phase(ref_phase1 - ref_phase0 - expected_advance);
      ref_inst_freq[static_cast<size_t>(k)] =
          bin_freq + phase_diff / (kTwoPi * static_cast<float>(time_step));
    }

    peaks.clear();
    if (config.phase_lock) {
      for (int k = 1; k < n_bins - 1; ++k) {
        if (ref_mag[static_cast<size_t>(k)] > ref_mag[static_cast<size_t>(k - 1)] &&
            ref_mag[static_cast<size_t>(k)] > ref_mag[static_cast<size_t>(k + 1)]) {
          peaks.push_back(k);
        }
      }
    }
    if (!config.phase_lock || peaks.empty()) {
      for (int k = 0; k < n_bins; ++k) {
        if (t_out == 0) {
          phase_acc[static_cast<size_t>(k)] =
              static_cast<double>(ref_phase[static_cast<size_t>(k)]);
        } else {
          phase_acc[static_cast<size_t>(k)] = wrap_phase(
              phase_acc[static_cast<size_t>(k)] +
              kTwoPiD * static_cast<double>(ref_inst_freq[static_cast<size_t>(k)]) * time_step);
        }
      }
      for (int k = 0; k < n_bins; ++k) {
        nearest_peak[static_cast<size_t>(k)] = k;
      }
    } else {
      int p_idx = 0;
      for (int k = 0; k < n_bins; ++k) {
        while (p_idx + 1 < static_cast<int>(peaks.size())) {
          const int boundary =
              (peaks[static_cast<size_t>(p_idx)] + peaks[static_cast<size_t>(p_idx + 1)]) / 2;
          if (k > boundary) {
            ++p_idx;
          } else {
            break;
          }
        }
        nearest_peak[static_cast<size_t>(k)] = peaks[static_cast<size_t>(p_idx)];
      }
      for (const int peak_bin : peaks) {
        if (t_out == 0) {
          phase_acc[static_cast<size_t>(peak_bin)] =
              static_cast<double>(ref_phase[static_cast<size_t>(peak_bin)]);
        } else {
          phase_acc[static_cast<size_t>(peak_bin)] = wrap_phase(
              phase_acc[static_cast<size_t>(peak_bin)] +
              kTwoPiD * static_cast<double>(ref_inst_freq[static_cast<size_t>(peak_bin)]) *
                  time_step);
        }
      }
    }

    const size_t start = static_cast<size_t>(t_out) * static_cast<size_t>(hop);
    for (int ch = 0; ch < channels; ++ch) {
      for (int k = 0; k < n_bins; ++k) {
        const int peak_bin = nearest_peak[static_cast<size_t>(k)];
        const double rotation = phase_acc[static_cast<size_t>(peak_bin)] -
                                static_cast<double>(ref_phase[static_cast<size_t>(peak_bin)]);
        const float synth_phase = static_cast<float>(wrap_phase(
            static_cast<double>(channel_phase[static_cast<size_t>(ch)][static_cast<size_t>(k)]) +
            rotation));
        synth_bins[static_cast<size_t>(k)] =
            std::polar(channel_mag[static_cast<size_t>(ch)][static_cast<size_t>(k)], synth_phase);
      }
      fft.inverse(synth_bins.data(), frame.data());
      for (int i = 0; i < n_fft; ++i) {
        ola[static_cast<size_t>(ch)][start + static_cast<size_t>(i)] +=
            frame[static_cast<size_t>(i)] * synthesis_win[static_cast<size_t>(i)];
      }
    }
    for (int i = 0; i < n_fft; ++i) {
      window_sum[start + static_cast<size_t>(i)] += window_product[static_cast<size_t>(i)];
    }
  }

  std::vector<std::vector<float>> out(static_cast<size_t>(channels),
                                      std::vector<float>(target_samples, 0.0f));
  for (int ch = 0; ch < channels; ++ch) {
    for (size_t i = 0; i < target_samples; ++i) {
      const size_t full_index = i + static_cast<size_t>(pad);
      const float sum = full_index < window_sum.size() ? window_sum[full_index] : 0.0f;
      out[static_cast<size_t>(ch)][i] =
          full_index < ola[static_cast<size_t>(ch)].size()
              ? (sum > kSpectrumEpsilon ? ola[static_cast<size_t>(ch)][full_index] / sum
                                        : ola[static_cast<size_t>(ch)][full_index])
              : 0.0f;
    }
  }
  return out;
}

void smooth_segment_joins(std::vector<float>* audio, const std::vector<size_t>& boundaries,
                          size_t requested_fade) {
  if (audio == nullptr || requested_fade == 0 || audio->empty()) return;
  std::vector<float>& out = *audio;
  for (const size_t boundary : boundaries) {
    if (boundary == 0 || boundary >= out.size()) continue;
    const size_t fade = std::min({requested_fade, boundary, out.size() - boundary});
    if (fade == 0) continue;
    const float next_anchor = out[boundary];
    for (size_t i = 0; i < fade; ++i) {
      const float a = static_cast<float>(i + 1) / static_cast<float>(fade + 1);
      const size_t left_index = boundary - fade + i;
      const size_t right_index = boundary + i;
      out[left_index] = out[left_index] * (1.0f - a) + next_anchor * a;
      out[right_index] = next_anchor * (1.0f - a) + out[right_index] * a;
    }
  }
}

}  // namespace

double tempo_sync_ppq(TempoSyncValue value) noexcept {
  return transport::note_length_ppq(value.denominator, value.modifier);
}

int64_t tempo_sync_samples(TempoSyncValue value, const transport::TransportState& state) noexcept {
  return transport::ppq_duration_to_samples(tempo_sync_ppq(value), state.bpm, state.sample_rate);
}

std::vector<float> bake_tempo_sync_warp_channel(const float* source, size_t source_samples,
                                                const std::vector<TempoSyncWarpSegment>& segments,
                                                const TempoSyncWarpBakeConfig& config) {
  SONARE_CHECK(source != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(source_samples > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.sample_rate > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_fft > 0 && config.hop_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(!segments.empty(), ErrorCode::InvalidParameter);

  size_t total_target_samples = 0;
  for (const TempoSyncWarpSegment& segment : segments) {
    SONARE_CHECK(segment.source_samples > 0 && segment.target_samples > 0,
                 ErrorCode::InvalidParameter);
    SONARE_CHECK(segment.source_offset <= source_samples &&
                     segment.source_samples <= source_samples - segment.source_offset,
                 ErrorCode::InvalidParameter);
    total_target_samples += segment.target_samples;
  }

  std::vector<float> output;
  output.reserve(total_target_samples);
  std::vector<size_t> boundaries;
  boundaries.reserve(segments.size() > 0 ? segments.size() - 1 : 0);

  for (const TempoSyncWarpSegment& segment : segments) {
    if (!output.empty()) boundaries.push_back(output.size());
    std::vector<float> stretched = stretch_segment(
        source + segment.source_offset, segment.source_samples, segment.target_samples, config);
    output.insert(output.end(), stretched.begin(), stretched.end());
  }

  if (output.size() > total_target_samples) {
    output.resize(total_target_samples);
  } else if (output.size() < total_target_samples) {
    output.resize(total_target_samples, 0.0f);
  }
  smooth_segment_joins(&output, boundaries, config.join_crossfade_samples);
  return output;
}

std::vector<std::vector<float>> bake_tempo_sync_warp_channels(
    const std::vector<const float*>& sources, size_t source_samples,
    const std::vector<TempoSyncWarpSegment>& segments, const TempoSyncWarpBakeConfig& config) {
  SONARE_CHECK(!sources.empty(), ErrorCode::InvalidParameter);
  for (const float* source : sources) {
    SONARE_CHECK(source != nullptr, ErrorCode::InvalidParameter);
  }
  SONARE_CHECK(source_samples > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.sample_rate > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_fft > 0 && config.hop_length > 0 && config.hop_length <= config.n_fft / 2,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(!segments.empty(), ErrorCode::InvalidParameter);

  size_t total_target_samples = 0;
  for (const TempoSyncWarpSegment& segment : segments) {
    SONARE_CHECK(segment.source_samples > 0 && segment.target_samples > 0,
                 ErrorCode::InvalidParameter);
    SONARE_CHECK(segment.source_offset <= source_samples &&
                     segment.source_samples <= source_samples - segment.source_offset,
                 ErrorCode::InvalidParameter);
    total_target_samples += segment.target_samples;
  }

  if (sources.size() == 1) {
    return {{bake_tempo_sync_warp_channel(sources[0], source_samples, segments, config)}};
  }

  std::vector<std::vector<float>> output(sources.size());
  for (auto& channel : output) {
    channel.reserve(total_target_samples);
  }
  std::vector<size_t> boundaries;
  boundaries.reserve(segments.size() > 0 ? segments.size() - 1 : 0);

  for (const TempoSyncWarpSegment& segment : segments) {
    if (!output.empty() && !output[0].empty()) boundaries.push_back(output[0].size());
    std::vector<const float*> segment_sources;
    segment_sources.reserve(sources.size());
    for (const float* source : sources) {
      segment_sources.push_back(source + segment.source_offset);
    }
    std::vector<std::vector<float>> stretched = stretch_segment_channels(
        segment_sources, segment.source_samples, segment.target_samples, config);
    for (size_t ch = 0; ch < output.size(); ++ch) {
      output[ch].insert(output[ch].end(), stretched[ch].begin(), stretched[ch].end());
    }
  }

  for (auto& channel : output) {
    if (channel.size() > total_target_samples) {
      channel.resize(total_target_samples);
    } else if (channel.size() < total_target_samples) {
      channel.resize(total_target_samples, 0.0f);
    }
    smooth_segment_joins(&channel, boundaries, config.join_crossfade_samples);
  }
  return output;
}

}  // namespace sonare::engine
