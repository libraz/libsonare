#!/usr/bin/env python3
"""Stdlib self-tests for the documented bound mirror check.

Every perturbation case drives :func:`check_documented_bound_mirrors.evaluate`
-- the function the shipping entry point calls -- over a copy of the real files
with one edit, so a break is demonstrated rather than asserted about. A copy
holds only the documents one claim reaches, and that claim alone is evaluated,
which is why the claim's floor is lowered to the subset rather than the tree.

The cases that matter most are the ones where nothing is found: an unlocatable
constant, a claim whose wording stops matching, and a constant no claim reads.
A check that measures nothing and exits 0 reads as coverage, so each of those
is required to fail with its own message.
"""

from __future__ import annotations

import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_documented_bound_mirrors",
    Path(__file__).resolve().parent / "check_documented_bound_mirrors.py",
)
assert _SPEC and _SPEC.loader
check = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(check)

CLAIMS = {claim.key: claim for claim in check.CLAIMS}

SPECTRAL = CLAIMS["spectral edit n_fft ceiling"]
SPECTRAL_CORE = "src/effects/spectral_edit.h"
SPECTRAL_DOCS = (
    "include/sonare/sonare_c_effects.h",
    "bindings/node/src/types_mastering.ts",
    "bindings/wasm/src/public_types_spectral.ts",
    "bindings/python/src/libsonare/_effects_editing.py",
)

# The offset case: one constant, one document, two claims that read it -- the
# ceiling itself and the largest legal value, which is the ceiling minus one.
HPSS_CEILING = CLAIMS["HPSS median kernel ceiling"]
HPSS_LARGEST = CLAIMS["HPSS median kernel largest legal size"]
HPSS_CORE = "src/core/spectrum.h"
HPSS_DOC = "include/sonare/sonare_c_effects.h"

# A second declaration of the same name: what makes the core unreadable is the
# ambiguity, not the absence.
SECOND_DECLARATION = "inline constexpr int kSpectralEditMaxNFft = 4096;\n"


class _CopiedTree(unittest.TestCase):
    """A throwaway tree holding named files, optionally with one edit each."""

    def tree(self, files, edits: dict[str, tuple[str, str]] | None = None) -> Path:
        root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root)
        for relative in files:
            text = (check.ROOT / relative).read_text(encoding="utf-8")
            if edits and relative in edits:
                old, new = edits[relative]
                self.assertIn(old, text, f"{relative}: the fixture anchor is gone")
                text = text.replace(old, new, 1)
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(text, encoding="utf-8")
        return root

    def spectral(self, edits=None, floor: int = 4) -> list[str]:
        root = self.tree((SPECTRAL_CORE, *SPECTRAL_DOCS), edits)
        return check.evaluate(root, (SPECTRAL._replace(floor=floor),))

    def only(self, failures: list[str], *fragments: str) -> str:
        self.assertEqual(len(failures), 1, f"expected one failure, got: {failures}")
        for fragment in fragments:
            self.assertIn(fragment, failures[0])
        return failures[0]


class ShippingTreeTest(unittest.TestCase):
    def test_the_tree_as_it_stands_passes(self) -> None:
        self.assertEqual(check.evaluate(), [])

    def test_every_registered_constant_is_located_with_a_value(self) -> None:
        for key, constant in check.CONSTANTS.items():
            with self.subTest(constant=key):
                self.assertIsInstance(check.find(check.ROOT, constant), int)

    def test_every_claim_reaches_at_least_its_floor(self) -> None:
        sites = check.collect(check.ROOT, check.doc_blocks(check.ROOT))
        for claim in check.CLAIMS:
            with self.subTest(claim=claim.key):
                self.assertGreaterEqual(len(sites[claim.key]), claim.floor)

    def test_claim_keys_are_unique(self) -> None:
        keys = [claim.key for claim in check.CLAIMS]
        self.assertEqual(len(keys), len(set(keys)))

    def test_every_claim_names_a_registered_constant(self) -> None:
        for claim in check.CLAIMS:
            for group, (key, _) in claim.groups.items():
                with self.subTest(claim=claim.key, group=group):
                    self.assertIn(key, check.CONSTANTS)

    def test_the_known_four_surface_mirror_is_covered(self) -> None:
        """The four documents that carry the spectral edit ceiling, by name."""
        sites = check.collect(check.ROOT, check.doc_blocks(check.ROOT))
        found = {site.path for site in sites[SPECTRAL.key]}
        self.assertEqual(found, set(SPECTRAL_DOCS))


class PerturbationTest(_CopiedTree):
    def test_an_untouched_copy_passes(self) -> None:
        self.assertEqual(self.spectral(), [])

    def test_moving_the_constant_reports_every_document_in_one_failure(self) -> None:
        failure = self.only(
            self.spectral({SPECTRAL_CORE: ("kSpectralEditMaxNFft = 262144", "kSpectralEditMaxNFft = 131072")}),
            "kSpectralEditMaxNFft is 131072",
            "4 document(s)",
        )
        for document in SPECTRAL_DOCS:
            self.assertIn(document, failure)

    def test_moving_one_document_alone_is_reported(self) -> None:
        stale = "bindings/python/src/libsonare/_effects_editing.py"
        self.only(
            self.spectral({stale: ("a power of two in ``[2, 262144]``", "a power of two in ``[2, 65536]``")}),
            stale,
            "says 65536",
            "1 document(s)",
        )

    def test_moving_the_c_header_alone_is_reported(self) -> None:
        """Each document is broken on its own: a guard that sees one is a quarter of a guard."""
        stale = "include/sonare/sonare_c_effects.h"
        self.only(
            self.spectral({stale: ("a power of two in [2, 262144]", "a power of two in [2, 1024]")}),
            stale,
            "says 1024",
        )

    def test_a_widened_document_is_reported_as_readily_as_a_narrowed_one(self) -> None:
        stale = "bindings/wasm/src/public_types_spectral.ts"
        self.only(
            self.spectral({stale: ("`[2, 262144]`", "`[2, 1048576]`")}),
            stale,
            "says 1048576",
        )


class OffsetTest(_CopiedTree):
    """A document stating ceiling - 1 is a mirror of the ceiling, not of nothing."""

    def test_both_claims_track_one_constant_through_its_offset(self) -> None:
        root = self.tree((HPSS_CORE, HPSS_DOC))
        claims = (HPSS_CEILING._replace(floor=3), HPSS_LARGEST._replace(floor=3))
        self.assertEqual(check.evaluate(root, claims), [])

        moved = self.tree(
            (HPSS_CORE, HPSS_DOC),
            {HPSS_CORE: ("kMaxStftNFft = 524288", "kMaxStftNFft = 262144")},
        )
        failure = self.only(check.evaluate(moved, claims), "kMaxStftNFft is 262144")
        self.assertIn("says 524288", failure)
        self.assertIn("says 524287", failure)


class UnlocatableConstantTest(_CopiedTree):
    """The vacuity case: nothing found has to be a failure, never a quiet pass."""

    def test_a_renamed_constant_fails(self) -> None:
        self.only(
            self.spectral({SPECTRAL_CORE: ("kSpectralEditMaxNFft = 262144", "kSpectralEditCeiling = 262144")}),
            SPECTRAL_CORE,
            "no single `kSpectralEditMaxNFft`",
            "leaves them unmeasured",
        )

    def test_a_deleted_constant_fails(self) -> None:
        self.only(
            self.spectral({SPECTRAL_CORE: ("inline constexpr int kSpectralEditMaxNFft = 262144;\n", "")}),
            "no single `kSpectralEditMaxNFft`",
        )

    def test_a_second_declaration_of_the_same_name_fails(self) -> None:
        """Two declarations are not a value: whichever is read first is a guess."""
        self.only(
            self.spectral(
                {
                    SPECTRAL_CORE: (
                        "inline constexpr int kSpectralEditMaxNFft = 262144;",
                        SECOND_DECLARATION + "inline constexpr int kSpectralEditMaxNFft = 262144;",
                    )
                }
            ),
            "no single `kSpectralEditMaxNFft`",
        )


class UnreachedClaimTest(_CopiedTree):
    """A claim that stops matching measures less than it reports."""

    def test_a_reworded_document_drops_below_the_floor(self) -> None:
        self.only(
            self.spectral(
                {
                    "bindings/node/src/types_mastering.ts": (
                        "a power of two in `[2, 262144]`",
                        "a power of two, no larger than 262144",
                    )
                }
            ),
            f"claim '{SPECTRAL.key}'",
            "matched 3 documents, below the 4",
        )

    def test_a_floor_above_the_tree_fails_rather_than_passing_quietly(self) -> None:
        self.only(self.spectral(floor=5), "matched 4 documents, below the 5")


class UnreadConstantTest(unittest.TestCase):
    """A constant registered and never compared is a blessing nothing earns."""

    def test_a_constant_no_claim_reads_is_reported(self) -> None:
        check.CONSTANTS["unread_example"] = check.Constant(SPECTRAL_CORE, "kSpectralEditMaxNFft")
        self.addCleanup(check.CONSTANTS.pop, "unread_example")
        failures = check.evaluate()
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("registered but no claim reads it", failures[0])


if __name__ == "__main__":
    unittest.main()
