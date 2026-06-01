#!/usr/bin/env python3
"""
K-weighting filter reference data generator for libsonare tests.

Implements ITU-R BS.1770-4 K-weighting as two biquad stages:
  Stage 1: High-frequency shelving filter (pre-filter)
           Ref frequency: 1681.974450955533 Hz, gain: +3.999843853973347 dB, Q: 0.7071752369554196
  Stage 2: RLB high-pass filter
           Ref frequency: 38.13547087613982 Hz, Q: 0.5003270373238773

The C++ implementation (src/rt/biquad_design.cpp) uses the Deman bilinear-transform
formulation.  For 48 kHz, bit-exact reference coefficients from the ITU-R BS.1770
Annex are returned directly.  For all other sample rates the same Deman design
formulas are applied.

This script:
  1. Implements the same Deman design in Python for cross-verification.
  2. Computes frequency response at representative test frequencies.
  3. Outputs tests/fixtures/k_weighting_reference.json for use by the C++ test.

Reference:
  ITU-R BS.1770-4 (2015), Annex 2 (normative)
  Mansbridge, S., Finn, S., & Reiss, J. D. (2012).
    "Implementation and Evaluation of Autonomous Multi-track Fader Control"
  Deman, B. (2013). Implementation of BS.1770 K-weighting filter.
    https://www.ebu.ch/files/live/sites/ebu/files/Publications/EBU-Tech3341.pdf
  pyloudnorm: https://github.com/csteinmetz1/pyloudnorm
"""

import json
import math
import os

# ---------------------------------------------------------------------------
# K-weighting analog prototype parameters (ITU-R BS.1770-4 Annex 2)
# ---------------------------------------------------------------------------
SHELF_FREQ_HZ = 1681.974450955533    # pre-filter shelf frequency
SHELF_GAIN_DB = 3.999843853973347    # pre-filter shelf gain (dB)
SHELF_Q = 0.7071752369554196         # pre-filter Q
HP_FREQ_HZ = 38.13547087613982       # RLB high-pass frequency
HP_Q = 0.5003270373238773            # RLB high-pass Q

# Deman exponent: vb = vh^0.499666774155
# This comes from the parametric EQ bilinear-transform derivation described
# in Deman (2013) and cross-checked against the BS.1770-4 reference coefficients.
DEMAN_EXP = 0.499666774155


def deman_high_shelf(freq_hz: float, sample_rate: float, gain_db: float, q: float) -> dict:
    """
    Compute K-weighting high-shelf biquad via Deman bilinear transform.

    Returns normalized coefficients (b0, b1, b2, a1, a2) in transposed Direct Form II
    convention matching src/rt/biquad_design.cpp::deman_high_shelf_d().
    """
    k = math.tan(math.pi * freq_hz / sample_rate)
    vh = 10.0 ** (gain_db / 20.0)
    vb = vh ** DEMAN_EXP
    a0 = 1.0 + k / q + k * k
    b0 = (vh + vb * k / q + k * k) / a0
    b1 = 2.0 * (k * k - vh) / a0
    b2 = (vh - vb * k / q + k * k) / a0
    a1 = 2.0 * (k * k - 1.0) / a0
    a2 = (1.0 - k / q + k * k) / a0
    return {"b0": b0, "b1": b1, "b2": b2, "a1": a1, "a2": a2}


def deman_highpass(freq_hz: float, sample_rate: float, q: float) -> dict:
    """
    Compute K-weighting RLB high-pass biquad via Deman bilinear transform.

    Returns normalized coefficients (b0, b1, b2, a1, a2) matching
    src/rt/biquad_design.cpp::deman_highpass_d().
    """
    k = math.tan(math.pi * freq_hz / sample_rate)
    a0 = 1.0 + k / q + k * k
    b0 = 1.0
    b1 = -2.0
    b2 = 1.0
    a1 = 2.0 * (k * k - 1.0) / a0
    a2 = (1.0 - k / q + k * k) / a0
    return {"b0": b0, "b1": b1, "b2": b2, "a1": a1, "a2": a2}


def biquad_freq_response_db(coeffs: dict, freq_hz: float, sample_rate: float) -> float:
    """
    Compute |H(e^{j omega})| in dB for a normalized biquad section.

    Uses the z-domain transfer function evaluation at z = e^{j*2*pi*f/fs}.
    H(z) = (b0 + b1*z^{-1} + b2*z^{-2}) / (1 + a1*z^{-1} + a2*z^{-2})
    """
    omega = 2.0 * math.pi * freq_hz / sample_rate
    z_inv = complex(math.cos(omega), -math.sin(omega))  # z^{-1} = e^{-j*omega}
    z_inv2 = z_inv * z_inv                               # z^{-2}

    numerator = coeffs["b0"] + coeffs["b1"] * z_inv + coeffs["b2"] * z_inv2
    denominator = 1.0 + coeffs["a1"] * z_inv + coeffs["a2"] * z_inv2

    magnitude = abs(numerator / denominator)
    if magnitude < 1e-300:
        return -math.inf
    return 20.0 * math.log10(magnitude)


def k_weighting_response_db(pre: dict, rlb: dict, freq_hz: float, sample_rate: float) -> float:
    """
    Combined K-weighting frequency response in dB (pre-filter + RLB cascaded).
    """
    return (
        biquad_freq_response_db(pre, freq_hz, sample_rate)
        + biquad_freq_response_db(rlb, freq_hz, sample_rate)
    )


def make_48k_hardcoded() -> tuple[dict, dict]:
    """
    Return the exact ITU-R BS.1770 reference coefficients at 48 kHz.

    These are the bit-exact values hardcoded in the C++ implementation and
    in the ITU-R BS.1770-4 standard itself.
    """
    pre = {
        "b0": 1.53512485958697,
        "b1": -2.69169618940638,
        "b2": 1.19839281085285,
        "a1": -1.69065929318241,
        "a2": 0.73248077421585,
    }
    rlb = {
        "b0": 1.0,
        "b1": -2.0,
        "b2": 1.0,
        "a1": -1.99004745483398,
        "a2": 0.99007225036621,
    }
    return pre, rlb


def compute_sample_rate_data(sample_rate: float) -> dict:
    """
    Compute K-weighting coefficients and frequency response for a given sample rate.

    Test frequencies chosen to cover:
      - 100 Hz: well below the high-pass; should be strongly attenuated
      - 1000 Hz: mid-band reference point (K-weighting near 0 dB)
      - 10000 Hz: in the shelf boost region
      - 20000 Hz (if below Nyquist): upper end of audible range

    Returns coefficients and response_db dict.
    """
    if sample_rate == 48000.0:
        pre, rlb = make_48k_hardcoded()
    else:
        pre = deman_high_shelf(SHELF_FREQ_HZ, sample_rate, SHELF_GAIN_DB, SHELF_Q)
        rlb = deman_highpass(HP_FREQ_HZ, sample_rate, HP_Q)

    # Test frequencies — exclude those above Nyquist
    nyquist = sample_rate / 2.0
    candidate_freqs = [100.0, 1000.0, 10000.0, 20000.0]
    test_freqs = [f for f in candidate_freqs if f < nyquist]

    response_db = {}
    for f in test_freqs:
        combined = k_weighting_response_db(pre, rlb, f, sample_rate)
        pre_stage = biquad_freq_response_db(pre, f, sample_rate)
        rlb_stage = biquad_freq_response_db(rlb, f, sample_rate)
        response_db[str(int(f))] = {
            "combined_db": combined,
            "pre_db": pre_stage,
            "rlb_db": rlb_stage,
        }

    return {
        "coefficients": {
            "pre": pre,
            "rlb": rlb,
        },
        "response_db": response_db,
    }


def main() -> None:
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.normpath(os.path.join(script_dir, "..", ".."))
    out_path = os.path.join(repo_root, "tests", "fixtures", "k_weighting_reference.json")

    sample_rates = [22050.0, 44100.0, 48000.0, 96000.0]

    # Build the reference data dict
    data: dict = {}
    for sr in sample_rates:
        key = str(int(sr))
        sr_data = compute_sample_rate_data(sr)
        data[key] = sr_data

        pre = sr_data["coefficients"]["pre"]
        rlb = sr_data["coefficients"]["rlb"]
        print(f"\n--- {int(sr)} Hz ---")
        print(f"  pre:  b0={pre['b0']:.15f}  b1={pre['b1']:.15f}  b2={pre['b2']:.15f}")
        print(f"        a1={pre['a1']:.15f}  a2={pre['a2']:.15f}")
        print(f"  rlb:  b0={rlb['b0']:.15f}  b1={rlb['b1']:.15f}  b2={rlb['b2']:.15f}")
        print(f"        a1={rlb['a1']:.15f}  a2={rlb['a2']:.15f}")
        for freq_str, resp in sr_data["response_db"].items():
            print(f"  {int(freq_str):6d} Hz: combined={resp['combined_db']:+8.4f} dB"
                  f"  pre={resp['pre_db']:+7.4f} dB  rlb={resp['rlb_db']:+7.4f} dB")

    # Cross-verify: 48 kHz hardcoded vs Deman should match to high precision
    pre_48_hc, rlb_48_hc = make_48k_hardcoded()
    pre_48_dm = deman_high_shelf(SHELF_FREQ_HZ, 48000.0, SHELF_GAIN_DB, SHELF_Q)
    rlb_48_dm = deman_highpass(HP_FREQ_HZ, 48000.0, HP_Q)

    print("\n--- 48 kHz: hardcoded vs Deman delta ---")
    for key in ["b0", "b1", "b2", "a1", "a2"]:
        delta_pre = abs(pre_48_hc[key] - pre_48_dm[key])
        delta_rlb = abs(rlb_48_hc[key] - rlb_48_dm[key])
        print(f"  pre[{key}] delta={delta_pre:.3e}   rlb[{key}] delta={delta_rlb:.3e}")

    # Also compute frequency response of Deman-derived 48 kHz for the JSON
    # so the test can verify the hardcoded path matches Deman within tolerance
    data["48000"]["deman_coefficients"] = {
        "pre": pre_48_dm,
        "rlb": rlb_48_dm,
    }
    deman_resp = {}
    for f in [100.0, 1000.0, 10000.0]:
        deman_resp[str(int(f))] = {
            "combined_db": k_weighting_response_db(pre_48_dm, rlb_48_dm, f, 48000.0),
            "pre_db": biquad_freq_response_db(pre_48_dm, f, 48000.0),
            "rlb_db": biquad_freq_response_db(rlb_48_dm, f, 48000.0),
        }
    data["48000"]["deman_response_db"] = deman_resp

    output = {
        "metadata": {
            "description": "K-weighting filter reference data for libsonare tests",
            "standard": "ITU-R BS.1770-4 (2015)",
            "method": "Deman bilinear-transform design (matches src/rt/biquad_design.cpp)",
            "deman_exponent": DEMAN_EXP,
            "shelf_freq_hz": SHELF_FREQ_HZ,
            "shelf_gain_db": SHELF_GAIN_DB,
            "shelf_q": SHELF_Q,
            "hp_freq_hz": HP_FREQ_HZ,
            "hp_q": HP_Q,
            "note_48k": (
                "48 kHz uses the bit-exact ITU-R BS.1770 reference coefficients. "
                "The Deman-derived coefficients are also included for comparison."
            ),
        },
        "data": data,
    }

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(output, f, indent=2)
    print(f"\nWrote {out_path}")


if __name__ == "__main__":
    main()
