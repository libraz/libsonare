#include "mastering/saturation/cab_voicing.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::mastering::saturation {
namespace {

using constants::kButterworthQ;

// Dynamic cardioid: the pronounced upper-mid peak that makes a close-miked cab
// cut, over a firm low end.
constexpr MicVoicing kMicDynamic{5500.0f, 4.0f, 1.05f, 100.0f, 4.0f, 3000.0f, -6.0f};
// Ribbon: an early, gentle top-end roll-off and the figure-8's strong close-range
// bass lift; smooth enough off-axis that moving it darkens less than a cardioid.
constexpr MicVoicing kMicRibbon{2500.0f, 1.5f, 0.72f, 90.0f, 7.0f, 2600.0f, -3.5f};
// Condenser: the most extended top and the flattest low end, so it also hears
// more of the cab's own edge.
constexpr MicVoicing kMicCondenser{8000.0f, 3.0f, 1.5f, 110.0f, 2.5f, 3400.0f, -5.0f};

constexpr CabVoicing kCabGuitar4x12{75.0f, 110.0f, 2.0f, 3800.0f, 4800.0f};
constexpr CabVoicing kCabBass8x10{40.0f, 80.0f, 3.0f, 2200.0f, 3500.0f};

}  // namespace

CabVoicing cab_voicing(CabModel model) noexcept {
  return model == CabModel::kBass8x10 ? kCabBass8x10 : kCabGuitar4x12;
}

MicVoicing mic_voicing(MicModel model) noexcept {
  switch (model) {
    case MicModel::kRibbon:
      return kMicRibbon;
    case MicModel::kCondenser:
      return kMicCondenser;
    default:
      return kMicDynamic;
  }
}

CabDesign design_cab_stage(CabModel cab_model, MicModel mic_model, float axis, float distance_cm,
                           float presence_db, double sample_rate) {
  const CabVoicing cab = cab_voicing(cab_model);
  CabDesign out;
  float rolloff_hz = cab.rolloff_hz;
  out.mic = mic_model != MicModel::kNone;
  if (out.mic) {
    const MicVoicing mic = mic_voicing(mic_model);
    const float off_axis = std::clamp(axis, 0.0f, 1.0f);
    // Off-axis is not just a shelf: on a real cab, moving toward the cone edge
    // pulls the whole top-end corner down with it.
    rolloff_hz *= mic.rolloff_scale * (1.0f - kOffAxisRolloffScale * off_axis);
    // Proximity holds at the reference distance and falls off inversely as the
    // mic backs away.
    const float distance = std::max(kMicReferenceDistanceCm, distance_cm);
    const float proximity_db = mic.proximity_db * kMicReferenceDistanceCm / distance;
    out.mic_prox = rt::rbj_low_shelf(rt::frequency_to_w0(mic.proximity_hz, sample_rate),
                                     kButterworthQ, proximity_db);
    out.mic_presence = rt::rbj_peak(rt::frequency_to_w0(mic.presence_hz, sample_rate), 1.2f,
                                    mic.presence_db * (1.0f - off_axis));
    // Off-axis darkening and distance HF loss both land on the same top-end
    // shelf, so the pair costs one biquad rather than two.
    const float distance_db = std::max(
        kDistanceHfLossFloorDb,
        kDistanceHfLossPerDoubleDb * std::log2(std::max(1.0f, distance / kMicReferenceDistanceCm)));
    out.mic_top = rt::rbj_high_shelf(rt::frequency_to_w0(mic.off_axis_hz, sample_rate),
                                     kButterworthQ, mic.off_axis_db * off_axis + distance_db);
  }
  out.hp = rt::rbj_highpass(rt::frequency_to_w0(cab.highpass_hz, sample_rate), kButterworthQ);
  out.bump = rt::rbj_peak(rt::frequency_to_w0(cab.bump_hz, sample_rate), 1.0f, cab.bump_db);
  out.presence = rt::rbj_peak(rt::frequency_to_w0(cab.presence_hz, sample_rate), 1.0f, presence_db);
  // 4th-order Butterworth roll-off: the steep top-end cut is the single
  // strongest "cabinet" cue.
  out.lp1 =
      rt::rbj_lowpass(rt::frequency_to_w0(rolloff_hz, sample_rate), rt::butterworth_stage_q(4, 0));
  out.lp2 =
      rt::rbj_lowpass(rt::frequency_to_w0(rolloff_hz, sample_rate), rt::butterworth_stage_q(4, 1));
  return out;
}

std::vector<float> render_cab_design(const CabDesign& design, const std::vector<float>& input) {
  // Stage order matches the realtime chain exactly (see AmpSim::process_cab):
  // the capsule sits between the cabinet's presence peak and its roll-off pair,
  // and a design without a capsule steps neither the mic biquads nor their
  // states.
  rt::BiquadState hp, bump, presence, mic_prox, mic_presence, mic_top, lp1, lp2;
  hp.set(design.hp);
  bump.set(design.bump);
  presence.set(design.presence);
  mic_prox.set(design.mic_prox);
  mic_presence.set(design.mic_presence);
  mic_top.set(design.mic_top);
  lp1.set(design.lp1);
  lp2.set(design.lp2);

  std::vector<float> out(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    float s = hp.process(input[i]);
    s = bump.process(s);
    s = presence.process(s);
    if (design.mic) {
      s = mic_prox.process(s);
      s = mic_presence.process(s);
      s = mic_top.process(s);
    }
    s = lp1.process(s);
    out[i] = lp2.process(s);
  }
  return out;
}

}  // namespace sonare::mastering::saturation
