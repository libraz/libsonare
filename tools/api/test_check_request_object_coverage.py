"""Self-test for check_request_object_coverage.py's declaration reader.

Run from the repository root:
``python3 -m unittest tools/api/test_check_request_object_coverage.py``.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_request_object_coverage as cov

GENERIC_OVERLOADS = """
export function applyProcessor<P extends SoloProcessor>(
  request: ApplyProcessorRequest<P>,
): Result;
export function applyProcessor<P extends SoloProcessor>(
  samples: Float32Array,
  sampleRate: number,
): Result;
export function applyProcessor(...args: unknown[]): Result {
  return run(args);
}
"""

NESTED_GENERIC = """
export function analyzePair<A extends Map<string, Set<Pair<A>>>>(
  request: AnalyzePairRequest<A>,
): Result {
  return run(request);
}
"""

ARROW_IN_CONSTRAINT = """
export function withCallback<F extends (x: number) => void = (x: number) => void>(
  request?: WithCallbackRequest<F>,
): void {}
"""

GENERIC_ONLY_POSITIONAL = """
export function lookup<T extends string>(name: T): number {
  return 0;
}
"""

GENERIC_NO_ARGUMENTS = """
export function catalog<T = string>(): T[] {
  return [];
}
"""


class DiscoveryTest(unittest.TestCase):
    def test_finds_a_function_declared_only_with_type_parameters(self) -> None:
        self.assertEqual(cov.function_names(GENERIC_ONLY_POSITIONAL), ["lookup"])

    def test_finds_a_function_with_nested_type_arguments(self) -> None:
        self.assertEqual(cov.function_names(NESTED_GENERIC), ["analyzePair"])

    def test_lists_an_overloaded_function_once(self) -> None:
        self.assertEqual(cov.function_names(GENERIC_OVERLOADS), ["applyProcessor"])


class RequestOverloadTest(unittest.TestCase):
    def test_generic_request_overload_counts(self) -> None:
        self.assertTrue(cov.has_request_overload(GENERIC_OVERLOADS, "applyProcessor"))

    def test_nested_type_arguments_do_not_end_the_parameter_list_early(self) -> None:
        self.assertTrue(cov.has_request_overload(NESTED_GENERIC, "analyzePair"))

    def test_arrow_in_a_constraint_is_not_a_closing_bracket(self) -> None:
        self.assertTrue(cov.has_request_overload(ARROW_IN_CONSTRAINT, "withCallback"))

    def test_generic_positional_function_is_not_covered(self) -> None:
        self.assertFalse(cov.has_request_overload(GENERIC_ONLY_POSITIONAL, "lookup"))

    def test_request_type_after_the_first_parameter_does_not_count(self) -> None:
        text = "export function f(samples: Float32Array, options: FRequest): void {}"
        self.assertFalse(cov.has_request_overload(text, "f"))

    def test_name_prefix_does_not_borrow_a_sibling_overload(self) -> None:
        text = "export function fStereo(request: FStereoRequest): void {}\nexport function f(x: number): void {}"
        self.assertFalse(cov.has_request_overload(text, "f"))


class ZeroArgumentTest(unittest.TestCase):
    def test_generic_zero_argument_function(self) -> None:
        self.assertTrue(cov.takes_no_arguments(GENERIC_NO_ARGUMENTS, "catalog"))

    def test_generic_positional_function_takes_arguments(self) -> None:
        self.assertFalse(cov.takes_no_arguments(GENERIC_ONLY_POSITIONAL, "lookup"))


if __name__ == "__main__":
    unittest.main()
