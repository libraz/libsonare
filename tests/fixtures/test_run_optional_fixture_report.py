"""Regression tests for reading report-only observations out of sonare_tests output.

The runner reads complete `<Warning>` elements from Catch2's XML reporter,
because the console reporter wraps at 80 columns and can split a token such as
`F-measure`. These pin that long messages survive and that a timeout's
truncated output still yields every warning before the cut.

Stdlib only; no build needed. Run directly:

    python3 tests/fixtures/test_run_optional_fixture_report.py
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import run_optional_fixture_report as runner

_STRUCTURE = (
    "Report-only structure fixture harmonix/harmonix/0956_tonightimgettingoveryou_"
    "extended_mix_with_a_deliberately_long_file_name.m4a features both HR.5F 0.25; "
    "HR3F 0.5; pairwise_f 0.75; min_hr3f 0; min_pairwise_f 0"
)
_BEAT = (
    "Report-only beat fixture beatles/beatles/07_14_tomorrow_never_knows.m4a "
    "F-measure 0.142149; threshold 0"
)


def _warning(message: str) -> str:
    escaped = message.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    return (
        '    <Warning filename="tests/analysis/music_dataset_test.cpp" line="995">\n'
        f"      {escaped}\n"
        "    </Warning>\n"
    )


def _document(*warnings: str) -> str:
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<Catch2TestRun name="sonare_tests" filters="&quot;[music_dataset]&quot;">\n'
        '  <TestCase name="Music optional fixtures" tags="[music_dataset]">\n'
        + "".join(warnings)
        + '    <OverallResult success="true" skips="0"/>\n'
        "  </TestCase>\n"
        "</Catch2TestRun>\n"
    )


def test_long_messages_yield_observations() -> None:
    assert len(_STRUCTURE) > 200
    observations = runner.extract_observations_from_report_output(
        _document(_warning(_STRUCTURE), _warning(_BEAT))
    )
    by_metric = {item["metric"]: item for item in observations}
    assert set(by_metric) == {"structure", "beat"}, observations
    structure = by_metric["structure"]
    assert structure["fixture"].endswith("deliberately_long_file_name.m4a"), structure
    assert structure["hr3f"] == 0.5
    assert structure["pairwise_f"] == 0.75
    beat = by_metric["beat"]
    assert beat["fixture"] == "beatles/beatles/07_14_tomorrow_never_knows.m4a", beat
    assert abs(beat["f_measure"] - 0.142149) < 1e-9


def test_truncated_output_keeps_complete_warnings_only() -> None:
    full = _document(_warning(_BEAT), _warning(_STRUCTURE))
    cut = full.index("HR3F")
    observations = runner.extract_observations_from_report_output(full[:cut])
    assert [item["metric"] for item in observations] == ["beat"], observations


def test_escaped_characters_are_restored() -> None:
    message = (
        "Report-only BPM fixture a&b/x.wav measured 120 BPM; expected 120 +/- 4%; relative error 0%"
    )
    observations = runner.extract_observations_from_report_output(_document(_warning(message)))
    assert [item["fixture"] for item in observations] == ["a&b/x.wav"], observations


def test_filter_tag_is_appended_to_suite_tag() -> None:
    assert runner.suite_filter("music", "[beat]") == "[music_dataset][beat]"
    assert runner.suite_filter("music", None) == "[music_dataset]"


def test_median_offset_is_parsed() -> None:
    late = _BEAT + "; median_offset_ms 12.5"
    early = _BEAT.replace("07_14", "07_15") + "; median_offset_ms -8.25"
    plain = _BEAT.replace("07_14", "07_16")
    observations = runner.extract_observations_from_report_output(
        _document(_warning(late), _warning(early), _warning(plain))
    )
    offsets = [item.get("median_offset_ms") for item in observations]
    assert offsets == [12.5, -8.25, None], observations
    assert all(abs(item["f_measure"] - 0.142149) < 1e-9 for item in observations)


def test_notes_observation_is_parsed() -> None:
    message = (
        "Report-only notes fixture mir1k/mir1k/Ada_01.wav F-measure_note 0.812; "
        "F-measure_note_offset 0.5; F-measure_onset 0.9; truth 30; predicted 33; "
        "min_f_note 0.7; min_f_note_offset 0.4"
    )
    observations = runner.extract_observations_from_report_output(_document(_warning(message)))
    assert len(observations) == 1, observations
    item = observations[0]
    assert item["metric"] == "notes", item
    assert item["fixture"] == "mir1k/mir1k/Ada_01.wav", item
    assert item["f_note"] == 0.812, item
    assert item["f_note_offset"] == 0.5, item
    assert item["f_onset"] == 0.9, item
    assert item["truth_count"] == 30 and isinstance(item["truth_count"], int), item
    assert item["predicted_count"] == 33 and isinstance(item["predicted_count"], int), item
    assert item["min_f_note"] == 0.7, item
    assert item["min_f_note_offset"] == 0.4, item


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"ok   {test.__name__}")
        except (AssertionError, AttributeError) as error:
            failed += 1
            print(f"FAIL {test.__name__}: {error}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
