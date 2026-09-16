"""One non-finite sample must not outlive the block that carried it.

`StreamingEqualizer` holds four families of recursive state across a block
boundary: biquad history, the dynamic-band detector, the auto-threshold follower
and the auto-gain smoother. Each is checked by feeding one non-finite sample in
one block and looking for the first block that is bit-identical to a clean-run
control.

The block that carried the sample is EXPECTED to come out non-finite; that is the
positive control, and asserting it keeps an implementation that quietly sanitizes
its input from passing.

Every case asserts the equalizer is doing something to the signal before it reads
a recovery result: an unconfigured equalizer is a passthrough, and a passthrough
recovers instantly for reasons that say nothing about the state under test.
"""

from __future__ import annotations

import math

import pytest

from libsonare import StreamingEqualizer

SR = 48000
BLOCK = 512
POISON_BLOCK = 1
POISON_INDEX = 100

# Blocks the stream is allowed to take to rejoin its control. A cell returns to
# its post-reset value at once; the stream rejoins only once the smoother behind
# that cell has re-converged, so each bound follows the slowest time constant in
# its path. Measured first-identical blocks are 66 / 25 / 261 / 82, and each bound
# sits about half again above its own: the crossing is where an exponential tail
# passes one float ULP, and a one-ULP difference in the platform's sin() moved the
# same fixture's crossing by 45%.
BIQUAD_RECOVERY_BLOCKS = 120
DETECTOR_RECOVERY_BLOCKS = 45
AUTO_THRESHOLD_RECOVERY_BLOCKS = 330
AUTO_GAIN_RECOVERY_BLOCKS = 150
# Blocks run past the bound, so a run that misses it still shows how far it got.
HORIZON_SLACK = 20

PEAK = {"type": "peak", "frequencyHz": 1000.0, "gainDb": 9.0, "q": 1.0, "enabled": True}
LOW_SHELF = {"type": "lowShelf", "frequencyHz": 120.0, "gainDb": -6.0, "q": 0.707, "enabled": True}
DYNAMIC_PEAK = dict(
    PEAK,
    dynamic=True,
    thresholdDb=-30.0,
    ratio=4.0,
    rangeDb=12.0,
    attackMs=5.0,
    releaseMs=80.0,
)
AUTO_THRESHOLD_PEAK = dict(DYNAMIC_PEAK, autoThreshold=True)


def _block(index: int) -> list[float]:
    return [
        0.5 * math.sin(2.0 * math.pi * 440.0 * (index * BLOCK + i) / SR)
        + 0.25 * math.sin(2.0 * math.pi * 97.0 * (index * BLOCK + i) / SR)
        for i in range(BLOCK)
    ]


def _run(bands, block_count, *, poison=None, auto_gain=False):
    """Returns every output block plus the band-0 gain and auto-gain the run ended on."""
    with StreamingEqualizer(SR, max_block_size=BLOCK) as eq:
        for index, band in enumerate(bands):
            eq.set_band(index, band)
        if auto_gain:
            eq.set_auto_gain(True)
        outputs = []
        for k in range(block_count):
            samples = _block(k)
            if poison is not None and k == POISON_BLOCK:
                samples[POISON_INDEX] = poison
            outputs.append(eq.process_mono(samples))
        return outputs, eq.spectrum().band_gain_db[0], eq.last_auto_gain_db


def _control_effect(control) -> float:
    """Largest change the control run makes. Zero means the EQ is a passthrough."""
    largest = 0.0
    for k in range(len(control)):
        source = _block(k)
        for i in range(BLOCK):
            largest = max(largest, abs(control[k][i] - source[i]))
    return largest


def _first_identical_block(control, poisoned) -> int:
    """First block after the poison that is bit-identical, or the block count if none is."""
    for k in range(POISON_BLOCK + 1, len(control)):
        if control[k] == poisoned[k]:
            return k
    return len(control)


def _assert_bounded(control, poisoned, recovery_blocks: int) -> None:
    """The carrying block is non-finite, every later block is clean, and the stream rejoins.

    Bounding the rejoin block is the whole claim: "rejoins eventually" is also
    true of a handle that rejoins on its last measured block.
    """
    # Positive control: an implementation that sanitized the input instead of its
    # own state would leave this block finite and pass everything below.
    assert any(not math.isfinite(v) for v in poisoned[POISON_BLOCK])
    for k in range(POISON_BLOCK + 1, len(poisoned)):
        assert all(math.isfinite(v) for v in poisoned[k]), f"block {k} carries non-finite output"
    assert _first_identical_block(control, poisoned) <= recovery_blocks


@pytest.mark.parametrize("poison", [math.nan, math.inf, -math.inf])
def test_biquad_state_recovers_from_one_non_finite_sample(poison):
    control, _, _ = _run([PEAK, LOW_SHELF], BIQUAD_RECOVERY_BLOCKS + HORIZON_SLACK)
    assert _control_effect(control) > 0.0

    poisoned, _, _ = _run([PEAK, LOW_SHELF], BIQUAD_RECOVERY_BLOCKS + HORIZON_SLACK, poison=poison)
    _assert_bounded(control, poisoned, BIQUAD_RECOVERY_BLOCKS)


def test_dynamic_detector_state_recovers_from_one_non_finite_sample():
    control, control_gain_db, _ = _run([DYNAMIC_PEAK], DETECTOR_RECOVERY_BLOCKS + HORIZON_SLACK)
    assert _control_effect(control) > 0.0
    # The detector has to be moving the band off its static gain, or its state is
    # not under test.
    assert control_gain_db != pytest.approx(PEAK["gainDb"])

    poisoned, poisoned_gain_db, _ = _run(
        [DYNAMIC_PEAK], DETECTOR_RECOVERY_BLOCKS + HORIZON_SLACK, poison=math.nan
    )
    _assert_bounded(control, poisoned, DETECTOR_RECOVERY_BLOCKS)
    assert poisoned_gain_db == control_gain_db


def test_auto_threshold_follower_recovers_from_one_non_finite_sample():
    # The follower is reinitialised by a floor-sentinel comparison, which a
    # non-finite value answers false to forever, so it needs the rule even though
    # it looks like it reseeds itself.
    control, control_gain_db, _ = _run(
        [AUTO_THRESHOLD_PEAK], AUTO_THRESHOLD_RECOVERY_BLOCKS + HORIZON_SLACK
    )
    assert _control_effect(control) > 0.0
    assert control_gain_db != pytest.approx(PEAK["gainDb"])

    poisoned, poisoned_gain_db, _ = _run(
        [AUTO_THRESHOLD_PEAK], AUTO_THRESHOLD_RECOVERY_BLOCKS + HORIZON_SLACK, poison=math.nan
    )
    _assert_bounded(control, poisoned, AUTO_THRESHOLD_RECOVERY_BLOCKS)
    assert poisoned_gain_db == control_gain_db


def test_auto_gain_smoother_recovers_from_one_non_finite_sample():
    control, _, control_auto_gain_db = _run(
        [PEAK, LOW_SHELF], AUTO_GAIN_RECOVERY_BLOCKS + HORIZON_SLACK, auto_gain=True
    )
    assert _control_effect(control) > 0.0
    # Auto-gain has to be compensating something, or its smoother is not under test.
    assert control_auto_gain_db != 0.0

    poisoned, _, poisoned_auto_gain_db = _run(
        [PEAK, LOW_SHELF],
        AUTO_GAIN_RECOVERY_BLOCKS + HORIZON_SLACK,
        poison=math.nan,
        auto_gain=True,
    )
    _assert_bounded(control, poisoned, AUTO_GAIN_RECOVERY_BLOCKS)
    assert poisoned_auto_gain_db == control_auto_gain_db


def test_non_finite_discard_count_reports_the_state_the_eq_discarded():
    """`process_stereo` scrubs nothing, so a NaN sample poisons the band's own state.

    Unlike the mixer's block entry, `sonare_eq_process` passes the caller's
    buffer straight to the core: the poison goes in as supplied and the
    recursive cells behind the band take it.
    """
    with StreamingEqualizer(SR, max_block_size=64) as eq:
        eq.set_band(0, PEAK)
        # Without an enabled band the EQ holds no recursive state, so its
        # count would stay at zero for a reason unrelated to the entry.
        assert eq.non_finite_discard_count() == 0

        left = [0.25] * 64
        right = [0.25] * 64
        # Control: an ordinary block counts nothing, so the rise below is
        # attributable to the poison rather than to processing at all.
        eq.process_stereo(left, right)
        assert eq.non_finite_discard_count() == 0

        left[8] = math.nan
        right[8] = math.nan
        eq.process_stereo(left, right)
        # Both channels lost their cells and the block still adds one: moving
        # by two would report a stereo stream as twice as degraded as a mono
        # one for the same defect, over a width the caller passed rather than
        # asked for.
        assert eq.non_finite_discard_count() == 1
