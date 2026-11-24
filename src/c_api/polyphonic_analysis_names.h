#pragma once

#include <array>

#include "c_api/sonare_c_internal.h"

namespace sonare_c_detail {

// The analysis composes several core configs into one public record.
inline constexpr std::array<CName, 26> kPolyphonicAnalysisNames{{
    {"n_fft", "config.n_fft"},
    {"hop_length", "config.hop_length"},
    {"win_length", "config.win_length"},
    {"cents_per_bin", "config.cents_per_bin"},
    {"f0_min_hz", "config.f0_min_hz"},
    {"f0_max_hz", "config.f0_max_hz"},
    {"max_polyphony", "config.max_polyphony"},
    {"min_frame_peak_ratio", "config.min_frame_peak_ratio"},
    {"min_separation_cents", "config.min_separation_cents"},
    {"subtraction_factor", "config.subtraction_factor"},
    {"max_jump_cents", "config.max_jump_cents"},
    {"min_ridge_peak_ratio", "config.min_ridge_peak_ratio"},
    {"reattack_ratio", "config.reattack_ratio"},
    {"audio", "samples"},
    {"ref_hz", "config.cent_ref_hz"},
    {"max_hz", "config.cent_max_hz"},
    {"n_harmonics", "config.salience_harmonics"},
    {"alpha_hz", "config.salience_alpha_hz"},
    {"beta_hz", "config.salience_beta_hz"},
    {"inharmonicity", "config.salience_inharmonicity"},
    {"min_duration_ms", "config.min_ridge_duration_ms"},
    {"config.n_harmonics", "config.mask_harmonics"},
    {"min_partials", "config.inharmonicity_min_partials"},
    {"max_residual_bins", "config.inharmonicity_max_residual_bins"},
    {"max_inharmonicity", "config.inharmonicity_max_stretch"},
    {"config.segmenter", "config"},
}};

}  // namespace sonare_c_detail
