"""Numerical and validation tests for continuous-time key inference."""

from __future__ import annotations

import itertools
import math
import unittest

import infer_continuous_keys as continuous
import infer_temporal_keys as temporal

PROFILE = "KrumhanslSchmuckler"


def candidates(scores: list[float], profile: str = PROFILE) -> list[dict[str, object]]:
    return [
        {
            "root": state // 2,
            "mode": "minor" if state % 2 else "major",
            "correlation": score,
            "profile": profile,
        }
        for state, score in enumerate(scores)
    ]


def export(duration: float, score_rows: list[list[float]]) -> dict[str, object]:
    whole = math.ceil(duration / 8.0)
    windows = []
    for index in range(whole):
        start = index * 8.0
        end = min(start + 8.0, duration)
        windows.append(
            {
                "start": start,
                "end": end,
                "selected_profile": PROFILE,
                "candidates": candidates(score_rows[0]),
            }
        )
    fine = []
    for index, scores in enumerate(score_rows):
        start = float(index)
        end = min(start + 1.0, duration)
        fine.append(
            {
                "start": start,
                "end": end,
                "selected_profile": PROFILE,
                "frame_count": 1,
                "supported_seconds": end - start,
                "candidates": candidates(scores),
            }
        )
    return {
        "duration": duration,
        "selected_profile": PROFILE,
        "global_candidates": candidates(score_rows[0]),
        "windows": windows,
        "fine_window_seconds": 1.0,
        "fine_pooling": continuous.FINE_POOLING,
        "fine_windows": fine,
    }


def transition_matrix(delta: float) -> list[list[float]]:
    diagonal, off_diagonal = continuous._continuous_transition(delta)
    return [
        [diagonal if source == target else off_diagonal for target in range(24)]
        for source in range(24)
    ]


def matmul(left: list[list[float]], right: list[list[float]]) -> list[list[float]]:
    return [
        [math.fsum(left[row][k] * right[k][column] for k in range(24)) for column in range(24)]
        for row in range(24)
    ]


def enumerate_posteriors(emissions: list[list[float]], durations: list[float]) -> list[list[float]]:
    log_evidence = continuous._log_evidence(emissions, durations)
    deltas = continuous._cell_center_deltas(durations)
    matrices = [transition_matrix(delta) for delta in deltas]
    paths = list(itertools.product(range(24), repeat=len(emissions)))
    log_weights = []
    for path in paths:
        log_weight = -math.log(24) + math.fsum(
            log_evidence[index][state] for index, state in enumerate(path)
        )
        log_weight += math.fsum(
            math.log(matrices[index][path[index]][path[index + 1]])
            for index in range(len(matrices))
        )
        log_weights.append(log_weight)
    maximum = max(log_weights)
    weights = [math.exp(value - maximum) for value in log_weights]
    total = math.fsum(weights)
    return [
        [
            math.fsum(
                weight for path, weight in zip(paths, weights, strict=True) if path[index] == state
            )
            / total
            for state in range(24)
        ]
        for index in range(len(emissions))
    ]


class ContinuousKeyTests(unittest.TestCase):
    def assert_posteriors_close(
        self, actual: list[list[float]], expected: list[list[float]], places: int = 12
    ) -> None:
        self.assertEqual(len(actual), len(expected))
        for actual_row, expected_row in zip(actual, expected, strict=True):
            for left, right in zip(actual_row, expected_row, strict=True):
                self.assertAlmostEqual(left, right, places=places)

    def test_eight_second_transition_recovers_frozen_matrix(self) -> None:
        diagonal, off_diagonal = continuous._continuous_transition(8.0)
        self.assertAlmostEqual(diagonal, temporal.STAY_PROBABILITY, places=15)
        self.assertAlmostEqual(off_diagonal, temporal.OTHER_PROBABILITY, places=15)
        self.assertAlmostEqual(24 * off_diagonal + (diagonal - off_diagonal), 1.0, places=15)

    def test_transition_prior_has_semigroup_composition(self) -> None:
        composed = matmul(transition_matrix(1.35), transition_matrix(2.6))
        direct = transition_matrix(3.95)
        for composed_row, direct_row in zip(composed, direct, strict=True):
            for actual, expected in zip(composed_row, direct_row, strict=True):
                self.assertAlmostEqual(actual, expected, places=14)

    def test_forward_backward_matches_all_576_two_cell_paths(self) -> None:
        first = [0.0] * 24
        second = [0.0] * 24
        first[4], first[6], first[19] = 0.31, 0.23, 0.08
        second[4], second[6], second[19] = 0.03, 0.16, 0.34
        emissions = [
            temporal._softmax([value / temporal.TEMPERATURE for value in first]),
            temporal._softmax([value / temporal.TEMPERATURE for value in second]),
        ]
        durations = [1.0, 0.37]
        self.assert_posteriors_close(
            continuous._continuous_hmm_posteriors(emissions, durations),
            enumerate_posteriors(emissions, durations),
        )

    def test_forward_backward_matches_all_13824_variable_gap_three_cell_paths(self) -> None:
        score_rows = [[0.0] * 24 for _ in range(3)]
        score_rows[0][4], score_rows[0][6] = 0.3, 0.17
        score_rows[1][7], score_rows[1][19] = 0.29, 0.18
        score_rows[2][2], score_rows[2][15] = 0.22, 0.31
        emissions = [
            temporal._softmax([value / temporal.TEMPERATURE for value in row]) for row in score_rows
        ]
        durations = [0.42, 1.0, 0.43]
        self.assert_posteriors_close(
            continuous._continuous_hmm_posteriors(emissions, durations),
            enumerate_posteriors(emissions, durations),
        )

    def test_single_cell_posterior_uses_duration_powered_emission(self) -> None:
        scores = [0.0] * 24
        scores[0], scores[1], scores[8] = 0.8, 0.35, 0.1
        emission = temporal._softmax([score / temporal.TEMPERATURE for score in scores])
        duration = 0.25
        posterior = continuous._continuous_hmm_posteriors([emission], [duration])[0]
        expected = temporal._softmax(
            [
                math.log(probability) * duration / continuous.EMISSION_REFERENCE_SECONDS
                for probability in emission
            ]
        )
        self.assert_posteriors_close([posterior], [expected])
        self.assertLess(max(posterior) - min(posterior), max(emission) - min(emission))

    def test_large_center_gap_keeps_transition_self_mass_in_log_domain(self) -> None:
        emissions = [
            temporal._softmax([0.6 / temporal.TEMPERATURE] + [0.0] * 23),
            temporal._softmax([0.3 / temporal.TEMPERATURE] + [0.0] * 23),
            temporal._softmax([0.0] * 23 + [0.7 / temporal.TEMPERATURE]),
        ]
        durations = [1.0, 20_000.0, 1.0]
        posterior = continuous._continuous_hmm_posteriors(emissions, durations)
        expected = [
            temporal._softmax(
                [
                    math.log(probability) * duration / continuous.EMISSION_REFERENCE_SECONDS
                    for probability in emission
                ]
            )
            for emission, duration in zip(emissions, durations, strict=True)
        ]
        self.assert_posteriors_close(posterior, expected)

    def test_uniform_evidence_is_stable_for_partial_final_cell(self) -> None:
        emissions = [[1.0 / 24.0] * 24 for _ in range(4)]
        posterior = continuous._continuous_hmm_posteriors(emissions, [1.0, 1.0, 1.0, 0.13])
        self.assertEqual([max(range(24), key=row.__getitem__) for row in posterior], [0] * 4)
        for row in posterior:
            for value in row:
                self.assertAlmostEqual(value, 1.0 / 24.0, places=14)

    def test_partial_tail_uses_cell_center_elapsed_time_and_eight_second_emission_rate(
        self,
    ) -> None:
        durations = [1.0, 0.2]
        self.assertEqual(continuous._cell_center_deltas(durations), [0.6])
        emissions = [[0.9] + [0.1 / 23] * 23, [0.01] + [0.99 / 23] * 23]
        log_values = continuous._log_evidence(emissions, durations)
        self.assertAlmostEqual(log_values[1][0], math.log(0.01) * 0.2 / 8.0)
        self.assertAlmostEqual(log_values[1][1], math.log(0.99 / 23) * 0.2 / 8.0)

    def test_transposition_preserves_posterior_labels(self) -> None:
        scores = [[0.0] * 24 for _ in range(4)]
        scores[0][0], scores[1][2], scores[2][7], scores[3][9] = 0.4, 0.31, 0.29, 0.4
        emissions = [
            temporal._softmax([value / temporal.TEMPERATURE for value in row]) for row in scores
        ]
        durations = [1.0, 1.0, 1.0, 0.3]
        posterior = continuous._continuous_hmm_posteriors(emissions, durations)
        transpose_by = 5
        transposed = [[0.0] * 24 for _ in emissions]
        for frame, row in enumerate(emissions):
            for state, probability in enumerate(row):
                root, mode = divmod(state, 2)
                target = ((root + transpose_by) % 12) * 2 + mode
                transposed[frame][target] = probability
        shifted_posterior = continuous._continuous_hmm_posteriors(transposed, durations)
        actual = [max(range(24), key=row.__getitem__) for row in shifted_posterior]
        expected = [
            ((max(range(24), key=row.__getitem__) // 2 + transpose_by) % 12) * 2
            + max(range(24), key=row.__getitem__) % 2
            for row in posterior
        ]
        self.assertEqual(actual, expected)

    def test_builds_independent_and_hmm_predictions_with_frozen_profile(self) -> None:
        rows = [[0.0] * 24 for _ in range(3)]
        rows[0][0], rows[1][2], rows[2][4] = 0.4, 0.35, 0.38
        result = continuous.build_predictions([{"id": "synthetic", "export": export(2.4, rows)}])
        self.assertEqual(set(result["predictions"]), {"independent_1s", "hmm_ct_1s"})
        self.assertEqual(result["predictions"]["independent_1s"]["method_id"], "independent_1s")
        for method in ("independent_1s", "hmm_ct_1s"):
            intervals = result["predictions"][method]["records"][0]["intervals"]
            self.assertEqual(
                [(row["start"], row["end"]) for row in intervals], [(0, 1), (1, 2), (2, 2.4)]
            )
        self.assertEqual(result["protocol"]["parameter_search"], False)
        self.assertEqual(result["protocol"]["physical_pooling"], "physical_frame_support_overlap")

    def test_invalid_grid_profile_support_candidates_and_duplicate_ids_fail_closed(self) -> None:
        rows = [[0.0] * 24, [0.0] * 24]
        cases = []
        bad = export(1.4, rows)
        bad["fine_window_seconds"] = 0.5
        cases.append((bad, "fine_window_seconds"))
        bad = export(1.4, rows)
        bad["fine_window_seconds"] = True
        cases.append((bad, "fine_window_seconds"))
        bad = export(1.4, rows)
        bad["fine_window_seconds"] = math.nan
        cases.append((bad, "fine_window_seconds"))
        bad = export(1.4, rows)
        bad["fine_windows"][0]["end"] = 0.9
        cases.append((bad, "one-second cells"))
        bad = export(1.4, rows)
        bad["fine_windows"][0]["selected_profile"] = "Temperley"
        cases.append((bad, "profile"))
        bad = export(1.4, rows)
        bad["fine_windows"][0]["supported_seconds"] = 0.95
        cases.append((bad, "support"))
        bad = export(1.4, rows)
        bad["fine_windows"][0]["frame_count"] = 0
        cases.append((bad, "positive integer"))
        bad = export(1.4, rows)
        bad["fine_windows"][0]["candidates"][0]["root"] = 12
        cases.append((bad, "root"))
        for payload, expected in cases:
            with self.subTest(expected=expected), self.assertRaisesRegex(ValueError, expected):
                continuous._validate_fine_export(payload)

        payload = export(1.4, rows)
        with self.assertRaisesRegex(ValueError, "duplicate export id"):
            continuous.build_predictions(
                [{"id": "same", "export": payload}, {"id": "same", "export": payload}]
            )

    def test_old_eight_second_inference_remains_callable_and_unmodified(self) -> None:
        score_rows = [[0.0] * 24, [0.0] * 24]
        old_export = export(9.0, score_rows)
        old_export.pop("fine_windows")
        old_export.pop("fine_window_seconds")
        old_export.pop("fine_pooling")
        old = temporal.build_predictions([{"id": "old", "export": old_export}])
        self.assertEqual(set(old["predictions"]), {"raw_global", "independent_8s", "hmm_8s"})


if __name__ == "__main__":
    unittest.main()
