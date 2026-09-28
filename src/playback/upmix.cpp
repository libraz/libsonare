#include "playback/upmix.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "core/window.h"
#include "mastering/multiband/crossover.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::playback {

using sonare::constants::kEpsilon;
using sonare::constants::kInvSqrt2;

namespace {

constexpr double kSecondsPerMs = 1e-3;
constexpr float kStatsTimeConstantMs = 100.0f;
constexpr float kRampMs = 20.0f;
constexpr double kAmbienceDelayMs = 10.0;
constexpr float kLfeCutoffHz = 120.0f;
constexpr float kAllpassGain = 0.5f;
constexpr int kAllpassStages = 4;
constexpr std::array<double, kAllpassStages> kAllpassMsLeft{4.1, 6.3, 9.7, 13.1};
constexpr std::array<double, kAllpassStages> kAllpassMsRight{4.7, 5.9, 10.3, 12.7};
/// Pll + Prr below this is silence: phi = 1 (no ambience).
constexpr double kPowerFloor = static_cast<double>(kEpsilon) * kEpsilon;
/// -60 dB in energy: the remaining-tail fraction at the decay horizon.
constexpr double kDecayEnergyThreshold = 1e-6;
/// Measurement window a drain is checked over; the horizon clears it entirely.
constexpr double kDecayWindowMs = 5.0;
/// Longest impulse response simulated when measuring a decay, in seconds.
constexpr double kDecaySimulationSeconds = 2.0;

// STFT output streams, in the order the frame loop writes them.
enum Stream { kFrontL, kFrontR, kCentre, kAmbL, kAmbR, kAnti, kStreamCount };

// Plane indices shared by 5.1 and 7.1 (WAVE_FORMAT_EXTENSIBLE order).
constexpr int kPlaneL = 0;
constexpr int kPlaneR = 1;
constexpr int kPlaneC = 2;
constexpr int kPlaneLfe = 3;
constexpr int kPlaneLs = 4;
constexpr int kPlaneRs = 5;
constexpr int kPlaneLss = 6;
constexpr int kPlaneRss = 7;

int ms_to_frames(double ms, double sample_rate) {
  return std::max(1, static_cast<int>(std::lround(ms * kSecondsPerMs * sample_rate)));
}

/// Schroeder all-pass `y = -g*v + v[n-d]`, `v = x + g*v[n-d]`.
struct Allpass {
  std::vector<float> buf;
  int pos = 0;

  void prepare(int delay) {
    buf.assign(static_cast<size_t>(delay), 0.0f);
    pos = 0;
  }
  void reset() noexcept {
    std::fill(buf.begin(), buf.end(), 0.0f);
    pos = 0;
  }
  float process(float x) noexcept {
    const float delayed = buf[static_cast<size_t>(pos)];
    const float v = x + kAllpassGain * delayed;
    buf[static_cast<size_t>(pos)] = v;
    if (++pos == static_cast<int>(buf.size())) pos = 0;
    return delayed - kAllpassGain * v;
  }
};

struct AllpassChain {
  std::array<Allpass, kAllpassStages> stages;

  void prepare(const std::array<double, kAllpassStages>& ms, double sample_rate) {
    for (int s = 0; s < kAllpassStages; ++s) {
      stages[static_cast<size_t>(s)].prepare(ms_to_frames(ms[static_cast<size_t>(s)], sample_rate));
    }
  }
  void reset() noexcept {
    for (auto& s : stages) s.reset();
  }
  float process(float x) noexcept {
    for (auto& s : stages) x = s.process(x);
    return x;
  }
};

/// Integer delay line of a fixed length.
struct Delay {
  std::vector<float> buf;
  int pos = 0;

  void prepare(int delay) {
    buf.assign(static_cast<size_t>(delay), 0.0f);
    pos = 0;
  }
  void reset() noexcept {
    std::fill(buf.begin(), buf.end(), 0.0f);
    pos = 0;
  }
  float process(float x) noexcept {
    const float y = buf[static_cast<size_t>(pos)];
    buf[static_cast<size_t>(pos)] = x;
    if (++pos == static_cast<int>(buf.size())) pos = 0;
    return y;
  }
};

/// First index T at which the energy remaining in h[T..] is -60 dB of the
/// total: what sustained input leaves behind T frames after it stops.
int decay_length(const std::vector<float>& h) {
  double total = 0.0;
  for (float v : h) total += static_cast<double>(v) * v;
  const double threshold = total * kDecayEnergyThreshold;
  double tail = 0.0;
  for (int i = static_cast<int>(h.size()) - 1; i >= 0; --i) {
    tail += static_cast<double>(h[static_cast<size_t>(i)]) * h[static_cast<size_t>(i)];
    if (tail > threshold) return i + 1;
  }
  return 0;
}

mastering::multiband::CrossoverConfig lfe_crossover_config() {
  mastering::multiband::CrossoverConfig config;
  config.cutoffs_hz = {kLfeCutoffHz};
  config.slope = mastering::multiband::CrossoverSlope::LR4;
  config.mode = mastering::multiband::CrossoverMode::LinkwitzRiley;
  return config;
}

}  // namespace

struct Upmixer::Impl {
  ChannelLayout output = ChannelLayout::FivePointOne;
  double sample_rate = 48000.0;
  int max_block = 0;
  int n = 0;
  int hop = 0;
  int bins = 0;
  int decay = 0;

  // STFT.
  std::unique_ptr<FFT> fft;
  std::vector<float> window;
  std::vector<float> inv_norm;  // 1 / sum of squared windows, per hop position
  std::vector<float> in_l, in_r;
  int in_count = 0;
  int zero_run = 0;
  std::vector<float> frame;
  std::vector<std::complex<float>> spec_l, spec_r;
  std::array<std::vector<std::complex<float>>, kStreamCount> spec_out;
  std::array<std::vector<float>, kStreamCount> ola;
  std::array<std::vector<float>, kStreamCount> ready;
  int ready_pos = 0;

  // Smoothed per-bin statistics.
  std::vector<double> p_ll, p_rr, p_lr_re, p_lr_im;
  double stats_keep = 0.0;
  double stats_take = 1.0;

  // Bypass: the input delayed by N.
  std::vector<float> byp_l_line, byp_r_line;
  int byp_pos = 0;

  // Rear decorrelation: ambience L/R through their own chains, the anti-phase
  // part through the opposite side's chain so Ls and Rs receive it decorrelated.
  AllpassChain ap_amb_l, ap_amb_r, ap_anti_ls, ap_anti_rs;
  Delay dl_amb_l, dl_amb_r, dl_anti_ls, dl_anti_rs;

  mastering::multiband::Crossover lfe_filter{lfe_crossover_config()};
  mastering::multiband::CrossoverScratch lfe_scratch;

  // Block scratch.
  std::array<std::vector<float>, kStreamCount> blk;
  std::vector<float> blk_byp_l, blk_byp_r, blk_mono;

  UpmixParams params;
  int ramp_len = 1;
  int ramp_pos = 0;  // 0 = bypass, ramp_len = processed

  void clear() noexcept {
    std::fill(in_l.begin(), in_l.end(), 0.0f);
    std::fill(in_r.begin(), in_r.end(), 0.0f);
    in_count = 0;
    zero_run = n - hop;
    for (auto& v : ola) std::fill(v.begin(), v.end(), 0.0f);
    for (auto& v : ready) std::fill(v.begin(), v.end(), 0.0f);
    ready_pos = 0;
    for (auto* v : {&p_ll, &p_rr, &p_lr_re, &p_lr_im}) std::fill(v->begin(), v->end(), 0.0);
    std::fill(byp_l_line.begin(), byp_l_line.end(), 0.0f);
    std::fill(byp_r_line.begin(), byp_r_line.end(), 0.0f);
    byp_pos = 0;
    ap_amb_l.reset();
    ap_amb_r.reset();
    ap_anti_ls.reset();
    ap_anti_rs.reset();
    dl_amb_l.reset();
    dl_amb_r.reset();
    dl_anti_ls.reset();
    dl_anti_rs.reset();
    lfe_filter.reset();
    ramp_pos = params.enabled ? ramp_len : 0;
  }

  void compute_masks() noexcept {
    const double cw = params.center_width;
    const double fa = params.front_ambience;
    const double amb_rear_share = 1.0 - fa;
    for (int k = 0; k < bins; ++k) {
      const auto ks = static_cast<size_t>(k);
      const std::complex<float> xl = spec_l[ks];
      const std::complex<float> xr = spec_r[ks];
      const double lr = static_cast<double>(xl.real());
      const double li = static_cast<double>(xl.imag());
      const double rr = static_cast<double>(xr.real());
      const double ri = static_cast<double>(xr.imag());
      p_ll[ks] = stats_keep * p_ll[ks] + stats_take * (lr * lr + li * li);
      p_rr[ks] = stats_keep * p_rr[ks] + stats_take * (rr * rr + ri * ri);
      p_lr_re[ks] = stats_keep * p_lr_re[ks] + stats_take * (lr * rr + li * ri);
      p_lr_im[ks] = stats_keep * p_lr_im[ks] + stats_take * (li * rr - lr * ri);

      const double sum = p_ll[ks] + p_rr[ks];
      double phi = 1.0;
      double q = 0.0;
      double w_c = 1.0;
      if (sum >= kPowerFloor) {
        const double cross = std::hypot(p_lr_re[ks], p_lr_im[ks]);
        const double geo = std::sqrt(p_ll[ks] * p_rr[ks]);
        // The relative floor makes a one-sided (hard-panned) bin fully coherent.
        const double rel_floor = kEpsilon * sum;
        phi = std::min(1.0, (cross + rel_floor) / (geo + rel_floor));
        if (cross > 0.0) q = std::clamp(-p_lr_re[ks] / cross, 0.0, 1.0);
        const double side = p_ll[ks] > p_rr[ks] ? 1.0 : (p_ll[ks] < p_rr[ks] ? -1.0 : 0.0);
        const double psi = std::max(0.0, 1.0 - 2.0 * cross / sum) * side;
        w_c = std::max(0.0, 1.0 - std::abs(psi) / cw);
      }
      const double direct_in_phase = phi * (1.0 - q);
      const double ambience = 1.0 - phi;
      // Each output takes the power sum of its shares of the same bin.
      const auto g_front =
          static_cast<float>(std::sqrt(direct_in_phase * (1.0 - w_c) + ambience * fa));
      const auto g_centre = static_cast<float>(std::sqrt(direct_in_phase * w_c)) * kInvSqrt2;
      const auto g_amb = static_cast<float>(std::sqrt(ambience * amb_rear_share));
      const auto g_anti = static_cast<float>(std::sqrt(phi * q)) * kInvSqrt2;
      spec_out[kFrontL][ks] = g_front * xl;
      spec_out[kFrontR][ks] = g_front * xr;
      spec_out[kCentre][ks] = g_centre * (xl + xr);
      spec_out[kAmbL][ks] = g_amb * xl;
      spec_out[kAmbR][ks] = g_amb * xr;
      spec_out[kAnti][ks] = g_anti * (xl - xr);
    }
  }

  void run_frame() noexcept {
    const auto nn = static_cast<size_t>(n);
    if (zero_run >= n) {
      // Silent analysis frame: the synthesis contribution is exactly zero, so
      // only the statistics decay as they would have.
      for (int k = 0; k < bins; ++k) {
        const auto ks = static_cast<size_t>(k);
        p_ll[ks] = stats_keep * p_ll[ks];
        p_rr[ks] = stats_keep * p_rr[ks];
        p_lr_re[ks] = stats_keep * p_lr_re[ks];
        p_lr_im[ks] = stats_keep * p_lr_im[ks];
      }
    } else {
      for (size_t i = 0; i < nn; ++i) frame[i] = in_l[i] * window[i];
      fft->forward(frame.data(), spec_l.data());
      for (size_t i = 0; i < nn; ++i) frame[i] = in_r[i] * window[i];
      fft->forward(frame.data(), spec_r.data());
      compute_masks();
      for (int s = 0; s < kStreamCount; ++s) {
        fft->inverse(spec_out[static_cast<size_t>(s)].data(), frame.data());
        auto& acc = ola[static_cast<size_t>(s)];
        for (size_t i = 0; i < nn; ++i) acc[i] += frame[i] * window[i];
      }
    }
    const auto hh = static_cast<size_t>(hop);
    for (int s = 0; s < kStreamCount; ++s) {
      auto& acc = ola[static_cast<size_t>(s)];
      auto& out = ready[static_cast<size_t>(s)];
      for (size_t i = 0; i < hh; ++i) out[i] = acc[i] * inv_norm[i];
      std::copy(acc.begin() + static_cast<std::ptrdiff_t>(hh), acc.end(), acc.begin());
      std::fill(acc.end() - static_cast<std::ptrdiff_t>(hh), acc.end(), 0.0f);
    }
    std::copy(in_l.begin() + static_cast<std::ptrdiff_t>(hh), in_l.end(), in_l.begin());
    std::copy(in_r.begin() + static_cast<std::ptrdiff_t>(hh), in_r.end(), in_r.begin());
    ready_pos = 0;
    in_count = 0;
  }

  void process_chunk(const float* left, const float* right, float* const* out,
                     int frames) noexcept {
    const auto base = static_cast<size_t>(n - hop);
    for (int i = 0; i < frames; ++i) {
      const auto is = static_cast<size_t>(i);
      for (int s = 0; s < kStreamCount; ++s) {
        blk[static_cast<size_t>(s)][is] =
            ready[static_cast<size_t>(s)][static_cast<size_t>(ready_pos)];
      }
      ++ready_pos;
      const float l = left[i];
      const float r = right[i];
      blk_byp_l[is] = byp_l_line[static_cast<size_t>(byp_pos)];
      blk_byp_r[is] = byp_r_line[static_cast<size_t>(byp_pos)];
      byp_l_line[static_cast<size_t>(byp_pos)] = l;
      byp_r_line[static_cast<size_t>(byp_pos)] = r;
      if (++byp_pos == n) byp_pos = 0;
      in_l[base + static_cast<size_t>(in_count)] = l;
      in_r[base + static_cast<size_t>(in_count)] = r;
      zero_run = (l == 0.0f && r == 0.0f) ? std::min(zero_run + 1, n) : 0;
      if (++in_count == hop) run_frame();
    }

    const bool seven = output == ChannelLayout::SevenPointOne;
    const int planes = channel_count(output);
    for (int i = 0; i < frames; ++i) {
      const auto is = static_cast<size_t>(i);
      const float amb_l = dl_amb_l.process(ap_amb_l.process(blk[kAmbL][is]));
      const float amb_r = dl_amb_r.process(ap_amb_r.process(blk[kAmbR][is]));
      const float anti_ls = dl_anti_ls.process(ap_anti_ls.process(blk[kAnti][is])) * kInvSqrt2;
      const float anti_rs = dl_anti_rs.process(ap_anti_rs.process(blk[kAnti][is])) * kInvSqrt2;
      out[kPlaneL][i] = blk[kFrontL][is];
      out[kPlaneR][i] = blk[kFrontR][is];
      out[kPlaneC][i] = blk[kCentre][is];
      if (seven) {
        out[kPlaneLs][i] = amb_l * kInvSqrt2 + anti_ls;
        out[kPlaneRs][i] = amb_r * kInvSqrt2 + anti_rs;
        out[kPlaneLss][i] = amb_l * kInvSqrt2;
        out[kPlaneRss][i] = amb_r * kInvSqrt2;
      } else {
        out[kPlaneLs][i] = amb_l + anti_ls;
        out[kPlaneRs][i] = amb_r + anti_rs;
      }
      blk_mono[is] = 0.5f * (blk_byp_l[is] + blk_byp_r[is]);
    }

    float* mono_ptr = blk_mono.data();
    lfe_filter.split_into(&mono_ptr, 1, frames, lfe_scratch);
    const float* low = lfe_scratch.band_channels[0][0];
    const float lfe_gain = params.lfe_from_upmix ? 1.0f : 0.0f;
    for (int i = 0; i < frames; ++i) out[kPlaneLfe][i] = low[i] * lfe_gain;

    const int target = params.enabled ? ramp_len : 0;
    if (ramp_pos == target && ramp_pos == ramp_len) return;
    if (ramp_pos == target) {
      for (int ch = 0; ch < planes; ++ch) std::fill(out[ch], out[ch] + frames, 0.0f);
      std::copy(blk_byp_l.begin(), blk_byp_l.begin() + frames, out[kPlaneL]);
      std::copy(blk_byp_r.begin(), blk_byp_r.begin() + frames, out[kPlaneR]);
      return;
    }
    const float inv_len = 1.0f / static_cast<float>(ramp_len);
    for (int i = 0; i < frames; ++i) {
      if (ramp_pos < target) {
        ++ramp_pos;
      } else if (ramp_pos > target) {
        --ramp_pos;
      }
      const float w = ramp_pos == ramp_len ? 1.0f : static_cast<float>(ramp_pos) * inv_len;
      const float bw = 1.0f - w;
      for (int ch = 0; ch < planes; ++ch) out[ch][i] *= w;
      out[kPlaneL][i] += bw * blk_byp_l[static_cast<size_t>(i)];
      out[kPlaneR][i] += bw * blk_byp_r[static_cast<size_t>(i)];
    }
  }
};

int upmix_latency_frames(double sample_rate) noexcept {
  const double frame = static_cast<double>(kUpmixFrameSeconds) * sample_rate;
  const long exponent = std::lround(std::log2(frame));
  return 1 << static_cast<int>(exponent);
}

Upmixer::Upmixer() : impl_(std::make_unique<Impl>()) {}
Upmixer::~Upmixer() = default;

void Upmixer::prepare(double sample_rate, int max_block_size, ChannelLayout output) {
  Impl& m = *impl_;
  m.output = output;
  m.sample_rate = sample_rate;
  m.max_block = std::max(1, max_block_size);
  m.n = upmix_latency_frames(sample_rate);
  m.hop = m.n / 4;
  m.bins = m.n / 2 + 1;
  const auto nn = static_cast<size_t>(m.n);
  const auto hh = static_cast<size_t>(m.hop);
  const auto bb = static_cast<size_t>(m.bins);
  const auto blk = static_cast<size_t>(m.max_block);

  m.fft = std::make_unique<FFT>(m.n);
  m.fft->prepare(true, true, false);
  m.window = create_window(WindowType::Hann, m.n, true);
  m.inv_norm.assign(hh, 0.0f);
  for (size_t j = 0; j < hh; ++j) {
    double acc = 0.0;
    for (size_t i = j; i < nn; i += hh) acc += static_cast<double>(m.window[i]) * m.window[i];
    m.inv_norm[j] = static_cast<float>(1.0 / acc);
  }
  m.in_l.assign(nn, 0.0f);
  m.in_r.assign(nn, 0.0f);
  m.frame.assign(nn, 0.0f);
  m.spec_l.assign(bb, {});
  m.spec_r.assign(bb, {});
  for (auto& v : m.spec_out) v.assign(bb, {});
  for (auto& v : m.ola) v.assign(nn, 0.0f);
  for (auto& v : m.ready) v.assign(hh, 0.0f);
  for (auto& v : m.blk) v.assign(blk, 0.0f);
  for (auto* v : {&m.p_ll, &m.p_rr, &m.p_lr_re, &m.p_lr_im}) v->assign(bb, 0.0);
  const double frame_rate = sample_rate / static_cast<double>(m.hop);
  m.stats_take = rt::one_pole_alpha_from_time_ms(kStatsTimeConstantMs, frame_rate);
  m.stats_keep = 1.0 - m.stats_take;

  m.byp_l_line.assign(nn, 0.0f);
  m.byp_r_line.assign(nn, 0.0f);
  m.blk_byp_l.assign(blk, 0.0f);
  m.blk_byp_r.assign(blk, 0.0f);
  m.blk_mono.assign(blk, 0.0f);

  m.ap_amb_l.prepare(kAllpassMsLeft, sample_rate);
  m.ap_amb_r.prepare(kAllpassMsRight, sample_rate);
  m.ap_anti_ls.prepare(kAllpassMsRight, sample_rate);
  m.ap_anti_rs.prepare(kAllpassMsLeft, sample_rate);
  const int ambience_delay = ms_to_frames(kAmbienceDelayMs, sample_rate);
  m.dl_amb_l.prepare(ambience_delay);
  m.dl_amb_r.prepare(ambience_delay);
  m.dl_anti_ls.prepare(ambience_delay);
  m.dl_anti_rs.prepare(ambience_delay);

  m.lfe_filter.prepare(sample_rate, m.max_block, 1);
  m.lfe_filter.prepare_scratch(m.lfe_scratch, 1, m.max_block);

  m.ramp_len = ms_to_frames(kRampMs, sample_rate);

  // Decay horizons, measured on the prepared filters' impulse responses.
  const auto sim_len = static_cast<size_t>(kDecaySimulationSeconds * sample_rate);
  std::vector<float> h(sim_len, 0.0f);
  int chain_decay = 0;
  for (const auto* ms : {&kAllpassMsLeft, &kAllpassMsRight}) {
    AllpassChain chain;
    chain.prepare(*ms, sample_rate);
    for (size_t i = 0; i < sim_len; ++i) h[i] = chain.process(i == 0 ? 1.0f : 0.0f);
    chain_decay = std::max(chain_decay, decay_length(h));
  }
  mastering::multiband::Crossover probe(lfe_crossover_config());
  probe.prepare(sample_rate, m.max_block, 1);
  mastering::multiband::CrossoverScratch probe_scratch;
  probe.prepare_scratch(probe_scratch, 1, m.max_block);
  std::fill(h.begin(), h.end(), 0.0f);
  h[0] = 1.0f;
  std::vector<float> lfe_h(sim_len, 0.0f);
  for (size_t start = 0; start < sim_len; start += blk) {
    const int count = static_cast<int>(std::min(blk, sim_len - start));
    float* ptr = h.data() + start;
    probe.split_into(&ptr, 1, count, probe_scratch);
    std::copy(probe_scratch.band_channels[0][0], probe_scratch.band_channels[0][0] + count,
              lfe_h.begin() + static_cast<std::ptrdiff_t>(start));
  }
  m.decay = std::max(chain_decay + ambience_delay, decay_length(lfe_h)) +
            ms_to_frames(kDecayWindowMs, sample_rate);

  m.clear();
}

void Upmixer::set_params(const UpmixParams& params, bool immediate) noexcept {
  Impl& m = *impl_;
  m.params = params;
  if (immediate) m.ramp_pos = params.enabled ? m.ramp_len : 0;
}

void Upmixer::process(const float* left, const float* right, float* const* out,
                      int frames) noexcept {
  if (out == nullptr || frames <= 0) return;
  rt::ScopedNoDenormals guard;
  Impl& m = *impl_;
  const int planes = channel_count(m.output);
  std::array<float*, 8> ptrs{};
  for (int done = 0; done < frames;) {
    const int count = std::min(m.max_block, frames - done);
    for (int ch = 0; ch < planes; ++ch) ptrs[static_cast<size_t>(ch)] = out[ch] + done;
    m.process_chunk(left + done, right + done, ptrs.data(), count);
    done += count;
  }
}

void Upmixer::reset() noexcept { impl_->clear(); }

int Upmixer::latency_samples() const noexcept { return impl_->n; }

int Upmixer::decay_frames() const noexcept { return impl_->decay; }

ChannelLayout Upmixer::output_layout() const noexcept { return impl_->output; }

}  // namespace sonare::playback
