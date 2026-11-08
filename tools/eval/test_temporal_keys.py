"""Numerical checks for the frozen temporal-key diagnostic, not analyzer accuracy."""

from __future__ import annotations

import itertools
import math
import unittest

import infer_temporal_keys as temporal

PROFILE = "KrumhanslSchmuckler"


def candidates(scores: list[float], profile: str = PROFILE) -> list[dict[str, object]]:
    rows = []
    for state, score in enumerate(scores):
        rows.append(
            {
                "root": state // 2,
                "mode": "minor" if state % 2 else "major",
                "correlation": score,
                "profile": profile,
            }
        )
    return rows


def export(duration: float, windows: list[tuple[float, float, list[float]]]) -> dict[str, object]:
    return {
        "duration": duration,
        "selected_profile": PROFILE,
        "global_candidates": candidates([0.0] * 24),
        "windows": [
            {
                "start": start,
                "end": end,
                "selected_profile": PROFILE,
                "candidates": candidates(scores),
            }
            for start, end, scores in windows
        ],
    }


def brute_force(
    emissions: list[list[float]], durations: list[float]
) -> tuple[list[int], list[list[float]]]:
    states = range(24)
    transition = lambda left, right: math.log(
        temporal.STAY_PROBABILITY if left == right else temporal.OTHER_PROBABILITY
    )
    log_e = temporal._log_emissions(emissions, durations)
    paths = list(itertools.product(states, repeat=len(emissions)))
    scores = []
    for path in paths:
        score = -math.log(24) + sum(log_e[t][state] for t, state in enumerate(path))
        score += sum(transition(path[t - 1], path[t]) for t in range(1, len(path)))
        scores.append(score)
    maximum = max(scores)
    total = sum(math.exp(score - maximum) for score in scores)
    posterior = []
    for t in range(len(emissions)):
        posterior.append(
            [
                sum(
                    math.exp(score - maximum)
                    for path, score in zip(paths, scores)
                    if path[t] == state
                )
                / total
                for state in states
            ]
        )
    return posterior


class TemporalKeyTests(unittest.TestCase):
    def test_reindexes_candidates_by_root_and_mode_not_input_order(self) -> None:
        rows = candidates([float(i) for i in range(24)])
        rows.reverse()
        distribution = temporal._candidate_distribution(rows)
        self.assertEqual(max(range(24), key=distribution.__getitem__), 23)

    def test_forward_backward_matches_576_path_enumeration(self) -> None:
        first = [0.0] * 24
        second = [0.0] * 24
        first[4], first[6], first[19] = 0.3, 0.25, 0.05
        second[4], second[6], second[19] = 0.01, 0.11, 0.32
        emissions = [
            temporal._softmax([value / temporal.TEMPERATURE for value in first]),
            temporal._softmax([value / temporal.TEMPERATURE for value in second]),
        ]
        durations = [8.0, 3.25]
        expected_marginals = brute_force(emissions, durations)
        measured = temporal._hmm_posterior(emissions, durations)
        for measured_row, expected_row in zip(measured, expected_marginals, strict=True):
            for actual, expected in zip(measured_row, expected_row, strict=True):
                self.assertAlmostEqual(actual, expected, places=12)

    def test_uniform_evidence_has_uniform_marginals_and_stable_ties(self) -> None:
        emissions = [[1 / 24] * 24 for _ in range(3)]
        posterior = temporal._hmm_posterior(emissions, [8.0, 8.0, 2.0])
        self.assertEqual([max(range(24), key=row.__getitem__) for row in posterior], [0, 0, 0])
        for row in posterior:
            for value in row:
                self.assertAlmostEqual(value, 1 / 24, places=12)

    def test_duration_power_uses_short_final_cell(self) -> None:
        emissions = [[0.9] + [0.1 / 23] * 23, [0.01] + [0.99 / 23] * 23]
        log_values = temporal._log_emissions(emissions, [8.0, 0.5])
        self.assertAlmostEqual(log_values[1][0], math.log(0.01) * 0.5 / 8.0)
        self.assertAlmostEqual(log_values[1][1], math.log(0.99 / 23) * 0.5 / 8.0)

    def test_transposition_preserves_posterior_argmax(self) -> None:
        scores = [[0.0] * 24 for _ in range(3)]
        scores[0][4], scores[1][6], scores[2][8] = 0.5, 0.45, 0.5
        probabilities = [
            temporal._softmax([value / temporal.TEMPERATURE for value in row]) for row in scores
        ]
        posterior = temporal._hmm_posterior(probabilities, [8.0] * 3)
        path = [max(range(24), key=row.__getitem__) for row in posterior]
        shift = 5
        transposed = []
        for row in probabilities:
            shifted = [0.0] * 24
            for state, value in enumerate(row):
                root, mode = divmod(state, 2)
                shifted[((root + shift) % 12) * 2 + mode] = value
            transposed.append(shifted)
        shifted_posterior = temporal._hmm_posterior(transposed, [8.0] * 3)
        shifted_path = [max(range(24), key=row.__getitem__) for row in shifted_posterior]
        self.assertEqual(
            shifted_path, [((state // 2 + shift) % 12) * 2 + state % 2 for state in path]
        )

    def test_three_predictions_keep_global_and_timed_intervals(self) -> None:
        global_scores = [0.0] * 24
        global_scores[0] = 0.5
        local_a = [0.0] * 24
        local_b = [0.0] * 24
        local_a[0], local_b[2] = 0.4, 0.4
        result = temporal.build_predictions(
            [{"id": "control", "export": export(9.0, [(0, 8, local_a), (8, 9, local_b)])}]
        )
        predictions = result["predictions"]
        self.assertEqual(len(predictions["raw_global"]["records"][0]["intervals"]), 1)
        for method in ("independent_8s", "hmm_8s"):
            intervals = predictions[method]["records"][0]["intervals"]
            self.assertEqual([(item["start"], item["end"]) for item in intervals], [(0, 8), (8, 9)])
        self.assertAlmostEqual(
            sum(item["mass"] for item in result["diagnostics"]["control"]["occupancy"]), 1.0
        )

    def test_invalid_candidate_coverage_duplicate_state_and_profile_fail_closed(self) -> None:
        rows = candidates([0.0] * 24)
        rows[0]["root"] = 99
        with self.assertRaisesRegex(ValueError, "root"):
            temporal._candidate_distribution(rows)
        rows = candidates([0.0] * 24)
        rows[1]["root"] = 0
        rows[1]["mode"] = "major"
        with self.assertRaisesRegex(ValueError, "duplicate"):
            temporal._candidate_distribution(rows)
        rows = candidates([0.0] * 24, "Temperley")
        with self.assertRaisesRegex(ValueError, "profile"):
            temporal._candidate_distribution(rows, PROFILE)

    def test_invalid_time_grid_or_nonfinite_duration_fails_closed(self) -> None:
        payload = export(9.0, [(0, 7.9, [0.0] * 24), (8, 9, [0.0] * 24)])
        with self.assertRaisesRegex(ValueError, "exact 8-second"):
            temporal._validate_export(payload)
        payload = export(float("nan"), [(0, 1, [0.0] * 24)])
        with self.assertRaisesRegex(ValueError, "duration"):
            temporal._validate_export(payload)


if __name__ == "__main__":
    unittest.main()
