#include "playback/binaural.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "acoustic/geometry.h"
#include "acoustic/image_source.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "playback/room_presets.h"
#include "rt/fractional_delay.h"
#include "rt/partitioned_convolver.h"
#include "rt/scoped_no_denormals.h"
#include "rt/seqlock_cell.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::playback {

using sonare::constants::kHalfPi;
using sonare::constants::kPi;
using sonare::constants::kSoundSpeedMps;

namespace {

constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kFullCircleDeg = 360.0f;
constexpr float kMsPerSecond = 1000.0f;
/// Partition of the late-reverberation convolver, and so the depth of its FIFO.
constexpr int kLatePartition = 128;
constexpr int kImageSourceOrder = 2;
constexpr int kLateSeedLeft = 1;
constexpr int kLateSeedRight = 2;
/// Taps of the 3rd-order Lagrange fractional delay.
constexpr int kKernelTaps = 4;
constexpr int kQ8One = 256;
constexpr int kProbeSize = 8;
constexpr int kProbeWrite = 4;
constexpr int kMaxBusPlanes = kVirtualSlotCount + 2;

/// Coefficients of the shared Lagrange kernel over delays offset .. offset+3,
/// read off an impulse so the folded filter matches `rt::lagrange3_fractional_delay`.
void lagrange_kernel(float delay, float* coeffs, int* offset) noexcept {
  const int q8 = std::max(0, static_cast<int>(std::lround(delay * static_cast<float>(kQ8One))));
  const int base = q8 / kQ8One;
  *offset = base >= 1 ? base - 1 : 0;
  const int local_q8 = q8 - *offset * kQ8One;
  std::array<float, kProbeSize> probe{};
  for (int j = 0; j < kKernelTaps; ++j) {
    probe.fill(0.0f);
    probe[static_cast<size_t>((kProbeWrite - j + kProbeSize) % kProbeSize)] = 1.0f;
    coeffs[j] = rt::lagrange3_read(probe.data(), kProbeSize, kProbeWrite, local_q8);
  }
}

/// One ear's FIR: `coeffs[m]` applies at input delay `offset + m`.
struct EarFilter {
  std::vector<float> coeffs;
  int length = 0;
  int offset = 0;
};

/// A convolved direction: a virtual speaker slot or a reflection-ring channel.
struct Source {
  SpeakerDirection world{};
  SpeakerDirection head{};
  bool has_filter = false;
  std::array<EarFilter, 2> current;
  std::array<EarFilter, 2> previous;
  /// `history_len` past input samples, then the current block.
  std::vector<float> history;
};

/// One image source folded onto the reflection ring.
struct ReflectionTap {
  int delay = 0;
  int ring_a = 0;
  int ring_b = 0;
  float gain_a = 0.0f;
  float gain_b = 0.0f;
};

struct SlotDelay {
  std::vector<float> buffer;
  size_t write = 0;
  std::vector<ReflectionTap> taps;
};

struct LateEar {
  std::unique_ptr<rt::PartitionedConvolver> convolver;
  std::array<float, kLatePartition> in{};
  std::array<float, kLatePartition> out{};
};

float wrap_degrees(float deg) noexcept {
  float wrapped = std::fmod(deg, kFullCircleDeg);
  if (wrapped < 0.0f) wrapped += kFullCircleDeg;
  return wrapped;
}

void fir(const EarFilter& filter, const float* block, float* out, int frames) noexcept {
  const float* c = filter.coeffs.data();
  const int length = filter.length;
  for (int k = 0; k < frames; ++k) {
    const float* x = block + k - filter.offset;
    float acc = 0.0f;
    for (int m = 0; m < length; ++m) acc += c[m] * x[-m];
    out[k] = acc;
  }
}

}  // namespace

SpeakerDirection head_relative_direction(SpeakerDirection world, const HeadPose& pose) noexcept {
  // Components along (front, right, up).
  const float az = world.azimuth_deg * kDegToRad;
  const float el = world.elevation_deg * kDegToRad;
  const float vf = std::cos(el) * std::cos(az);
  const float vr = std::cos(el) * std::sin(az);
  const float vu = std::sin(el);

  const float cy = std::cos(pose.yaw_deg * kDegToRad);
  const float sy = std::sin(pose.yaw_deg * kDegToRad);
  const float cp = std::cos(pose.pitch_deg * kDegToRad);
  const float sp = std::sin(pose.pitch_deg * kDegToRad);
  const float cr = std::cos(pose.roll_deg * kDegToRad);
  const float sr = std::sin(pose.roll_deg * kDegToRad);

  // Yaw turns the front toward the right, about the up axis.
  const float f1 = cy * vf + sy * vr;
  const float r1 = -sy * vf + cy * vr;
  const float u1 = vu;
  // Pitch tilts the front toward the up axis, about the right axis.
  const float f2 = cp * f1 + sp * u1;
  const float u2 = -sp * f1 + cp * u1;
  const float r2 = r1;
  // Roll lowers the right axis, about the front axis.
  const float r3 = cr * r2 - sr * u2;
  const float u3 = sr * r2 + cr * u2;
  const float f3 = f2;

  return {std::atan2(r3, f3) * kRadToDeg, std::asin(std::clamp(u3, -1.0f, 1.0f)) * kRadToDeg};
}

struct BinauralRenderer::Impl {
  rt::SeqlockCell<HeadPose> pose_cell;
  rt::SeqlockCell<HeadPose>::Reader pose_reader{pose_cell.reader()};
  rt::SeqlockCell<BinauralParams> params_cell;
  rt::SeqlockCell<BinauralParams>::Reader params_reader{params_cell.reader()};

  bool prepared = false;
  int max_block = 0;
  int taps = 0;
  int history_len = 0;
  std::unique_ptr<HrtfSet> hrtf;
  int slot_count = 0;
  std::vector<Source> sources;  ///< slots, then the ring when the room exists
  std::vector<float> scratch_left;
  std::vector<float> scratch_right;
  std::vector<float> fir_new;
  std::vector<float> fir_old;

  bool room_available = false;
  std::vector<SlotDelay> slot_delays;
  std::array<std::vector<float>, kReflectionRingDirections> ring;
  std::array<LateEar, 2> late;
  int late_pos = 0;
  std::vector<float> late_in;
  std::array<std::vector<float>, 2> late_out;
  bool room_gain_valid = false;
  float room_gain = 0.0f;
  /// False once a silent room was skipped, so its state is cleared on re-entry.
  bool room_state_live = false;

  void build_filters(Source& source, SpeakerDirection direction) noexcept;
  void render_source(Source& source, const float* input, const HeadPose& pose, float* left,
                     float* right, int frames) noexcept;
  void clear_room_state() noexcept;
  void process_chunk(const float* const* bus, float* left, float* right, int frames) noexcept;
};

void BinauralRenderer::Impl::build_filters(Source& source, SpeakerDirection direction) noexcept {
  float itd = 0.0f;
  hrtf->interpolate(direction.azimuth_deg, direction.elevation_deg, scratch_left.data(),
                    scratch_right.data(), &itd);
  // Positive ITD delays the left ear; the near ear stays undelayed.
  const int far_ear = itd > 0.0f ? 0 : 1;
  const float far_delay = std::fabs(itd);
  for (int ear = 0; ear < 2; ++ear) {
    const float* h = ear == 0 ? scratch_left.data() : scratch_right.data();
    EarFilter& f = source.current[static_cast<size_t>(ear)];
    if (ear != far_ear || far_delay == 0.0f) {
      std::copy(h, h + taps, f.coeffs.begin());
      f.length = taps;
      f.offset = 0;
      continue;
    }
    std::array<float, kKernelTaps> kernel{};
    lagrange_kernel(far_delay, kernel.data(), &f.offset);
    f.length = taps + kKernelTaps - 1;
    std::fill(f.coeffs.begin(), f.coeffs.begin() + f.length, 0.0f);
    for (int t = 0; t < taps; ++t) {
      for (int j = 0; j < kKernelTaps; ++j) {
        f.coeffs[static_cast<size_t>(t + j)] += kernel[static_cast<size_t>(j)] * h[t];
      }
    }
  }
}

void BinauralRenderer::Impl::render_source(Source& source, const float* input, const HeadPose& pose,
                                           float* left, float* right, int frames) noexcept {
  float* block = source.history.data() + history_len;
  std::copy(input, input + frames, block);

  const SpeakerDirection direction = head_relative_direction(source.world, pose);
  bool crossfade = false;
  if (!source.has_filter || direction.azimuth_deg != source.head.azimuth_deg ||
      direction.elevation_deg != source.head.elevation_deg) {
    if (source.has_filter) {
      std::swap(source.current, source.previous);
      crossfade = true;
    }
    build_filters(source, direction);
    source.head = direction;
    source.has_filter = true;
  }

  const float inv_frames = 1.0f / static_cast<float>(frames);
  for (int ear = 0; ear < 2; ++ear) {
    float* out = ear == 0 ? left : right;
    fir(source.current[static_cast<size_t>(ear)], block, fir_new.data(), frames);
    if (crossfade) {
      fir(source.previous[static_cast<size_t>(ear)], block, fir_old.data(), frames);
      for (int k = 0; k < frames; ++k) {
        const float w = static_cast<float>(k + 1) * inv_frames;
        out[k] += fir_old[static_cast<size_t>(k)] +
                  w * (fir_new[static_cast<size_t>(k)] - fir_old[static_cast<size_t>(k)]);
      }
    } else {
      for (int k = 0; k < frames; ++k) out[k] += fir_new[static_cast<size_t>(k)];
    }
  }

  std::copy(source.history.begin() + frames, source.history.begin() + frames + history_len,
            source.history.begin());
}

void BinauralRenderer::Impl::clear_room_state() noexcept {
  for (auto& delay : slot_delays) {
    std::fill(delay.buffer.begin(), delay.buffer.end(), 0.0f);
    delay.write = 0;
  }
  for (size_t i = static_cast<size_t>(slot_count); i < sources.size(); ++i) {
    std::fill(sources[i].history.begin(), sources[i].history.end(), 0.0f);
    sources[i].has_filter = false;
  }
  for (auto& ear : late) {
    ear.in.fill(0.0f);
    ear.out.fill(0.0f);
    if (ear.convolver) ear.convolver->reset();
  }
  late_pos = 0;
}

void BinauralRenderer::Impl::process_chunk(const float* const* bus, float* left, float* right,
                                           int frames) noexcept {
  std::fill(left, left + frames, 0.0f);
  std::fill(right, right + frames, 0.0f);

  const BinauralParams params = params_reader.try_load();
  const HeadPose pose = params.head_tracking_enabled ? pose_reader.try_load() : HeadPose{};
  const float target_gain =
      room_available && params.room_enabled ? db_to_linear(params.room_mix_db) : 0.0f;
  if (!room_gain_valid) {
    room_gain = target_gain;
    room_gain_valid = true;
  }
  const float gain_start = room_gain;
  const float gain_step = (target_gain - gain_start) / static_cast<float>(frames);
  room_gain = target_gain;
  const bool room_active = gain_start > 0.0f || target_gain > 0.0f;
  if (room_active && !room_state_live) clear_room_state();
  room_state_live = room_active;

  if (room_active) {
    for (auto& channel : ring) std::fill(channel.begin(), channel.begin() + frames, 0.0f);
    std::fill(late_in.begin(), late_in.begin() + frames, 0.0f);
    for (int s = 0; s < slot_count; ++s) {
      SlotDelay& delay = slot_delays[static_cast<size_t>(s)];
      const size_t size = delay.buffer.size();
      const float* in = bus[s];
      for (int k = 0; k < frames; ++k) {
        delay.buffer[(delay.write + static_cast<size_t>(k)) % size] = in[k];
        late_in[static_cast<size_t>(k)] += in[k];
      }
      for (const ReflectionTap& tap : delay.taps) {
        float* ring_a = ring[static_cast<size_t>(tap.ring_a)].data();
        float* ring_b = ring[static_cast<size_t>(tap.ring_b)].data();
        size_t read = (delay.write + size - static_cast<size_t>(tap.delay)) % size;
        for (int k = 0; k < frames; ++k) {
          const float x = delay.buffer[read];
          ring_a[k] += tap.gain_a * x;
          ring_b[k] += tap.gain_b * x;
          if (++read == size) read = 0;
        }
      }
      delay.write = (delay.write + static_cast<size_t>(frames)) % size;
    }
    for (auto& channel : ring) {
      for (int k = 0; k < frames; ++k) {
        channel[static_cast<size_t>(k)] *= gain_start + gain_step * static_cast<float>(k + 1);
      }
    }

    // The FIFO delays by one partition, which the IR head trim already removed.
    for (int k = 0; k < frames; ++k) {
      const float gain = gain_start + gain_step * static_cast<float>(k + 1);
      for (size_t ear = 0; ear < late.size(); ++ear) {
        late[ear].in[static_cast<size_t>(late_pos)] = late_in[static_cast<size_t>(k)];
        late_out[ear][static_cast<size_t>(k)] = gain * late[ear].out[static_cast<size_t>(late_pos)];
      }
      if (++late_pos == kLatePartition) {
        for (auto& ear : late) ear.convolver->process_block(ear.in.data(), ear.out.data());
        late_pos = 0;
      }
    }
    for (int k = 0; k < frames; ++k) {
      left[k] += late_out[0][static_cast<size_t>(k)];
      right[k] += late_out[1][static_cast<size_t>(k)];
    }
  }

  for (int s = 0; s < slot_count; ++s) {
    render_source(sources[static_cast<size_t>(s)], bus[s], pose, left, right, frames);
  }
  if (room_active) {
    for (int r = 0; r < kReflectionRingDirections; ++r) {
      render_source(sources[static_cast<size_t>(slot_count + r)],
                    ring[static_cast<size_t>(r)].data(), pose, left, right, frames);
    }
  }

  const float* direct_left = bus[slot_count];
  const float* direct_right = bus[slot_count + 1];
  for (int k = 0; k < frames; ++k) {
    left[k] += direct_left[k];
    right[k] += direct_right[k];
  }
}

BinauralRenderer::BinauralRenderer() : impl_(std::make_unique<Impl>()) {}
BinauralRenderer::~BinauralRenderer() = default;

void BinauralRenderer::prepare(double sample_rate, int max_block_size, const HrtfSet& hrtf,
                               const HeadphoneSlotSet& slots, RoomPreset room) {
  if (!(sample_rate > 0.0) || max_block_size <= 0 || slots.count < 1 ||
      slots.count > kVirtualSlotCount) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "binaural: invalid sample rate, block size or slot set");
  }
  Impl& im = *impl_;
  const int rate = static_cast<int>(std::lround(sample_rate));
  im.prepared = false;
  im.max_block = max_block_size;
  im.hrtf = std::make_unique<HrtfSet>(hrtf.resampled(rate));
  im.taps = im.hrtf->taps();

  float max_itd = 0.0f;
  for (int e = 0; e < im.hrtf->elevation_count(); ++e) {
    for (int a = 0; a < im.hrtf->azimuth_count(); ++a) {
      max_itd = std::max(max_itd, std::fabs(im.hrtf->itd_samples(e, a)));
    }
  }
  const int max_offset = static_cast<int>(std::ceil(max_itd)) + 1;
  const int filter_capacity = im.taps + kKernelTaps - 1;
  im.history_len = max_offset + filter_capacity;

  RoomPresetSpec spec;
  im.room_available = room_preset_spec(room, &spec);
  im.slot_count = slots.count;
  const int source_count = slots.count + (im.room_available ? kReflectionRingDirections : 0);
  im.sources.assign(static_cast<size_t>(source_count), Source{});
  for (int i = 0; i < source_count; ++i) {
    Source& source = im.sources[static_cast<size_t>(i)];
    source.world =
        i < slots.count
            ? virtual_slot_direction(slots.slots[i])
            : SpeakerDirection{static_cast<float>(i - slots.count) * kReflectionRingStepDeg, 0.0f};
    for (auto* set : {&source.current, &source.previous}) {
      for (EarFilter& f : *set) f.coeffs.assign(static_cast<size_t>(filter_capacity), 0.0f);
    }
    source.history.assign(static_cast<size_t>(im.history_len + max_block_size), 0.0f);
  }
  im.scratch_left.assign(static_cast<size_t>(im.taps), 0.0f);
  im.scratch_right.assign(static_cast<size_t>(im.taps), 0.0f);
  im.fir_new.assign(static_cast<size_t>(max_block_size), 0.0f);
  im.fir_old.assign(static_cast<size_t>(max_block_size), 0.0f);

  im.slot_delays.clear();
  for (auto& channel : im.ring) channel.clear();
  for (auto& ear : im.late) ear.convolver.reset();
  im.late_in.clear();
  for (auto& out : im.late_out) out.clear();

  if (im.room_available) {
    const acoustic::ShoeboxRoom shoebox = acoustic::uniform_shoebox(spec.dims, spec.absorption);
    const acoustic::Vec3 listener = listener_position(spec);
    im.slot_delays.assign(static_cast<size_t>(slots.count), SlotDelay{});
    for (int s = 0; s < slots.count; ++s) {
      SlotDelay& delay = im.slot_delays[static_cast<size_t>(s)];
      const acoustic::Vec3 source =
          virtual_speaker_position(spec, virtual_slot_direction(slots.slots[s]));
      const float direct = acoustic::length(source - listener);
      int max_delay = 0;
      for (const acoustic::ImageSource& image :
           acoustic::shoebox_image_sources(shoebox, {source, listener}, kImageSourceOrder)) {
        if (image.order < 1 || image.reflection.empty()) continue;
        double beta_power = 0.0;
        for (float beta : image.reflection) beta_power += static_cast<double>(beta) * beta;
        const float beta = static_cast<float>(
            std::sqrt(beta_power / static_cast<double>(image.reflection.size())));
        const acoustic::Vec3 arrival = image.position - listener;
        const float azimuth = wrap_degrees(std::atan2(arrival.y, arrival.x) * kRadToDeg);
        const float position = azimuth / kReflectionRingStepDeg;
        const int lower = static_cast<int>(std::floor(position));
        const float frac = position - static_cast<float>(lower);
        const float gain = beta * direct / image.distance;
        ReflectionTap tap;
        tap.delay = std::max(1, static_cast<int>(std::lround((image.distance - direct) /
                                                             kSoundSpeedMps * sample_rate)));
        tap.ring_a = lower % kReflectionRingDirections;
        tap.ring_b = (lower + 1) % kReflectionRingDirections;
        tap.gain_a = gain * std::cos(frac * kHalfPi);
        tap.gain_b = gain * std::sin(frac * kHalfPi);
        delay.taps.push_back(tap);
        max_delay = std::max(max_delay, tap.delay);
      }
      delay.buffer.assign(static_cast<size_t>(max_delay + max_block_size + 1), 0.0f);
    }
    for (auto& channel : im.ring) channel.assign(static_cast<size_t>(max_block_size), 0.0f);

    // Late tails, normalised so a source at the virtual-speaker distance has a
    // unit direct path, like the direct HRIRs.
    const float mixing_ms = std::sqrt(acoustic::room_volume(spec.dims));
    const int mixing_sample =
        static_cast<int>(std::lround(mixing_ms / kMsPerSecond * static_cast<float>(sample_rate)));
    const float norm = 4.0f * kPi * spec.speaker_distance_m;
    const acoustic::Vec3 centre{spec.dims.length * 0.5f, spec.dims.width * 0.5f,
                                kListenerEarHeightM};
    acoustic::RirSynthConfig config;
    config.mixing_time_ms = mixing_ms;
    config.max_seconds = kLateTailMaxSeconds;
    for (size_t ear = 0; ear < im.late.size(); ++ear) {
      config.seed = static_cast<unsigned>(ear == 0 ? kLateSeedLeft : kLateSeedRight);
      const acoustic::RirSynthResult result =
          acoustic::synthesize_rir(shoebox, {centre, listener}, rate, config);
      if (result.rir.empty()) {
        throw SonareException(ErrorCode::InvalidParameter,
                              "binaural: " + acoustic::first_error_text(result.diagnostics));
      }
      const int length = static_cast<int>(result.rir.size());
      // Trim the FIFO depth off the head so the path reports no latency.
      std::vector<float> ir(static_cast<size_t>(std::max(0, length - kLatePartition)), 0.0f);
      for (int n = 0; n < static_cast<int>(ir.size()); ++n) {
        const int src = n + kLatePartition;
        if (src >= mixing_sample)
          ir[static_cast<size_t>(n)] = norm * result.rir[static_cast<size_t>(src)];
      }
      im.late[ear].convolver = std::make_unique<rt::PartitionedConvolver>(
          rt::PartitionedConvolverConfig{kLatePartition});
      im.late[ear].convolver->set_impulse_response(ir);
    }
    im.late_in.assign(static_cast<size_t>(max_block_size), 0.0f);
    for (auto& out : im.late_out) out.assign(static_cast<size_t>(max_block_size), 0.0f);
  }

  im.prepared = true;
  reset();
}

void BinauralRenderer::set_params(const BinauralParams& params) noexcept {
  impl_->params_cell.store(params);
}

void BinauralRenderer::set_head_pose(const HeadPose& pose) noexcept {
  impl_->pose_cell.store(pose);
}

void BinauralRenderer::process(const float* const* bus, float* left, float* right,
                               int frames) noexcept {
  if (frames <= 0 || left == nullptr || right == nullptr) return;
  Impl& im = *impl_;
  if (!im.prepared || bus == nullptr) {
    std::fill(left, left + frames, 0.0f);
    std::fill(right, right + frames, 0.0f);
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  const int planes = im.slot_count + 2;
  std::array<const float*, kMaxBusPlanes> chunk_bus{};
  for (int done = 0; done < frames;) {
    const int n = std::min(im.max_block, frames - done);
    for (int p = 0; p < planes; ++p) chunk_bus[static_cast<size_t>(p)] = bus[p] + done;
    im.process_chunk(chunk_bus.data(), left + done, right + done, n);
    done += n;
  }
}

void BinauralRenderer::reset() noexcept {
  Impl& im = *impl_;
  for (Source& source : im.sources) {
    std::fill(source.history.begin(), source.history.end(), 0.0f);
    source.has_filter = false;
  }
  im.clear_room_state();
  im.room_gain_valid = false;
  im.room_state_live = true;
}

int BinauralRenderer::latency_samples() const noexcept { return 0; }

}  // namespace sonare::playback
