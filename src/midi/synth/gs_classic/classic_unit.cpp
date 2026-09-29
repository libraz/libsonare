#include "midi/synth/gs_classic/classic_unit.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "CDSPResampler.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::midi::synth::gs_classic {

namespace {

/// Transition band in percent of the narrower rate's spectral space, and stop-band dB.
constexpr double kTransitionBandPercent = 20.0;
constexpr double kStopBandDb = 96.0;
/// Host time over which `prepare()` walks the resampler pair for its deepest shortfall.
constexpr double kShortfallProbeSeconds = 0.5;
constexpr float kLargestByte = 127.0f;

std::unique_ptr<r8b::CDSPResampler> make_resampler(double from, double to, int max_in) {
  return std::make_unique<r8b::CDSPResampler>(from, to, max_in, kTransitionBandPercent, kStopBandDb,
                                              r8b::fprMinPhase);
}

}  // namespace

const GsClassicKernelTable& gs_classic_extension_kernels() noexcept {
  static const GsClassicKernelTable kTable = {
      {gs_classic_section_state_size, gs_classic_section_reset, gs_classic_render_section},
      {nullptr, nullptr, gs_classic_render_shaper},
      {gs_classic_envelope_state_size, gs_classic_envelope_reset, gs_classic_render_envelope},
      {nullptr, nullptr, gs_classic_render_gain_computer},
      {nullptr, nullptr, gs_classic_render_vca},
      {gs_classic_pitch_state_size, nullptr, gs_classic_render_pitch},
  };
  return kTable;
}

GsClassicUnit::GsClassicUnit(const GsClassicModelSet& models, const GsClassicType& type)
    : models_(&models), type_(&type) {}

GsClassicUnit::~GsClassicUnit() = default;

void GsClassicUnit::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "GsClassicUnit needs a positive sample rate and block size");
  }
  prepared_ = false;
  max_block_ = static_cast<std::size_t>(max_block_size);
  for (auto& up : up_) up = make_resampler(sample_rate, kGsClassicSampleRateHz, max_block_size);
  const int drawn_max = up_[0]->getMaxOutLen(max_block_size);
  for (auto& down : down_) down = make_resampler(kGsClassicSampleRateHz, sample_rate, drawn_max);
  const int returned_max = down_[0]->getMaxOutLen(drawn_max);
  if (!graph_.prepare(*models_, *type_, static_cast<std::size_t>(drawn_max),
                      &gs_classic_extension_kernels())) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "GsClassicUnit: type " + std::to_string(type_->type) +
                              " is not a graph the classic engine can draw");
  }

  // Walk zeros through the pair one host sample at a time; the widest gap between
  // samples taken and samples returned is what the FIFO has to hold in advance.
  const auto probe = static_cast<std::size_t>(std::ceil(sample_rate * kShortfallProbeSeconds));
  double zero = 0.0;
  std::size_t returned = 0;
  std::size_t shortfall = 0;
  for (std::size_t taken = 1; taken <= probe; ++taken) {
    double* drawn = nullptr;
    const int m = up_[0]->process(&zero, 1, drawn);
    double* back = nullptr;
    returned += static_cast<std::size_t>(down_[0]->process(drawn, m, back));
    if (taken > returned) shortfall = std::max(shortfall, taken - returned);
  }
  prefill_ = shortfall;

  for (int c = 0; c < 2; ++c) {
    host_in_[c].assign(max_block_, 0.0);
    drawn_[c].assign(static_cast<std::size_t>(drawn_max), 0.0);
    fifo_[c].assign(prefill_ + static_cast<std::size_t>(returned_max) + max_block_, 0.0);
  }
  prepared_ = true;
  reset();
}

void GsClassicUnit::prime() noexcept {
  for (auto& fifo : fifo_) std::fill(fifo.begin(), fifo.end(), 0.0);
  fifo_read_ = 0;
  fifo_size_ = prefill_;
}

void GsClassicUnit::reset() {
  if (!prepared_) return;
  for (auto& up : up_) up->clear();
  for (auto& down : down_) down->clear();
  graph_.reset();
  prime();
}

void GsClassicUnit::process(float* const* channels, int num_channels, int num_samples) {
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  ensure_prepared(prepared_, "GsClassicUnit");
  rt::ScopedNoDenormals no_denormals;
  float* left = channels[0];
  float* right = num_channels > 1 ? channels[1] : channels[0];
  const bool stereo = right != left;
  const std::size_t capacity = fifo_[0].size();

  for (std::size_t done = 0; done < static_cast<std::size_t>(num_samples);) {
    const std::size_t n = std::min(max_block_, static_cast<std::size_t>(num_samples) - done);
    for (std::size_t i = 0; i < n; ++i) {
      host_in_[0][i] = left[done + i];
      host_in_[1][i] = right[done + i];
    }
    double* raised[2] = {nullptr, nullptr};
    int drawn = 0;
    for (int c = 0; c < 2; ++c) {
      drawn = up_[c]->process(host_in_[c].data(), static_cast<int>(n), raised[c]);
    }
    const auto count = static_cast<std::size_t>(drawn);
    graph_.process(raised[0], raised[1], drawn_[0].data(), drawn_[1].data(), count);
    bool finite = true;
    for (std::size_t i = 0; i < count && finite; ++i) {
      finite = std::isfinite(drawn_[0][i]) && std::isfinite(drawn_[1][i]);
    }
    if (!finite) {
      // A non-finite sample would stay in the graph's recursions for good.
      graph_.reset();
      std::fill(drawn_[0].begin(), drawn_[0].begin() + drawn, 0.0);
      std::fill(drawn_[1].begin(), drawn_[1].begin() + drawn, 0.0);
      note_non_finite_discard();
    }
    for (int c = 0; c < 2; ++c) {
      double* back = nullptr;
      const auto m = static_cast<std::size_t>(down_[c]->process(drawn_[c].data(), drawn, back));
      std::vector<double>& fifo = fifo_[c];
      for (std::size_t k = 0; k < m && fifo_size_ + k < capacity; ++k) {
        fifo[(fifo_read_ + fifo_size_ + k) % capacity] = back[k];
      }
      if (c == 1) fifo_size_ = std::min(capacity, fifo_size_ + m);
    }
    if (fifo_size_ < n) ++underruns_;
    for (std::size_t i = 0; i < n; ++i) {
      double l = 0.0;
      double r = 0.0;
      if (i < fifo_size_) {
        const std::size_t at = (fifo_read_ + i) % capacity;
        l = fifo_[0][at];
        r = fifo_[1][at];
      }
      if (stereo) {
        left[done + i] = static_cast<float>(l);
        right[done + i] = static_cast<float>(r);
      } else {
        left[done + i] = static_cast<float>(0.5 * (l + r));
      }
    }
    const std::size_t taken = std::min(n, fifo_size_);
    fifo_read_ = (fifo_read_ + taken) % capacity;
    fifo_size_ -= taken;
    done += n;
  }
}

bool GsClassicUnit::set_parameter(unsigned int param_id, float value) {
  if (param_id >= kGsClassicByteSlots || std::isnan(value)) return false;
  const float clamped = std::min(std::max(value, 0.0f), kLargestByte);
  graph_.set_byte(param_id, static_cast<uint8_t>(std::lround(clamped)));
  return true;
}

bool GsClassicUnit::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  return param_id < kGsClassicByteSlots;
}

std::vector<rt::ParamDescriptor> GsClassicUnit::parameter_descriptors() const {
  std::vector<rt::ParamDescriptor> out;
  out.reserve(kGsClassicByteSlots);
  for (unsigned int slot = 0; slot < kGsClassicByteSlots; ++slot) {
    out.push_back({"byte" + std::to_string(slot), slot});
  }
  return out;
}

}  // namespace sonare::midi::synth::gs_classic
