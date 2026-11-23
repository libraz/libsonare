"""Verifying the file a contract case promised its run would write.

Checks the artifact against its declared kind, the payload field that
announces its size or sample rate, and either the manifest's SHA-256 or, for a
WAV whose last bit differs between builds, a committed reference compared within
a tolerance scaled by the reference's peak.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import sys
import wave
from pathlib import Path
from typing import Any

# Run directly to record a reference, so the tools directory is not on sys.path yet.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from _repo import REPO_ROOT
from cli_contract_payload import parse_single_json

WAV_REFERENCES = REPO_ROOT / "tests" / "conformance" / "cli_contract_wav_references.json"


def _wav_samples(data: bytes) -> tuple[tuple[int, int, int, int], list[float]]:
    """The (rate, channels, sample width, frames) header and the samples at full scale 1.0."""
    with wave.open(io.BytesIO(data), "rb") as source:
        header = (
            source.getframerate(),
            source.getnchannels(),
            source.getsampwidth(),
            source.getnframes(),
        )
        frames = source.readframes(header[3])
    width = header[2]
    scale = float(1 << (8 * width - 1))
    if width == 1:
        return header, [(byte - 128) / scale for byte in frames]
    return header, [
        int.from_bytes(frames[offset : offset + width], "little", signed=True) / scale
        for offset in range(0, len(frames), width)
    ]


def _check_wav_within_tolerance(
    data: bytes,
    reference_key: str,
    artifact: dict[str, Any],
    compared: str,
    label: str,
    report: list[tuple[str, str]],
) -> None:
    """Hold a WAV to its committed reference: header exact, samples within the tolerance."""
    references = json.loads(WAV_REFERENCES.read_text(encoding="utf-8"))
    entry = references.get(reference_key)
    if not isinstance(entry, dict) or not isinstance(entry.get("wav_base64"), str):
        report.append(("fail", f"{label}: no reference {reference_key} in {WAV_REFERENCES.name}"))
        return
    reference = base64.b64decode(entry["wav_base64"])
    if hashlib.sha256(reference).hexdigest() != artifact["reference_sha256"]:
        report.append(
            (
                "fail",
                f"{label}: reference {reference_key} does not match its reference_sha256",
            )
        )
        return
    expected_header, expected = _wav_samples(reference)
    try:
        actual_header, actual = _wav_samples(data)
    except (EOFError, wave.Error) as exc:
        report.append(("fail", f"{label}: artifact is not a readable WAV ({exc})"))
        return
    produced = f" (compared {compared})" if compared else ""
    if actual_header != expected_header:
        message = (
            f"{label}: WAV (rate, channels, width, frames) {actual_header} does not match "
            f"reference {expected_header}{produced}"
        )
        report.append(("fail", message))
        return
    peak = max((abs(value) for value in expected), default=0.0)
    tolerance = artifact["tolerance_of_peak"] * peak
    worst, position = max(
        ((abs(a - b), index) for index, (a, b) in enumerate(zip(actual, expected, strict=True))),
        default=(0.0, 0),
    )
    if worst > tolerance:
        message = (
            f"{label}: sample {position} is {worst:.3g} from the reference, past the "
            f"tolerance {tolerance:.3g} ({artifact['tolerance_of_peak']} of peak "
            f"{peak:.3g}){produced}"
        )
        report.append(("fail", message))


def _check_artifact(
    artifact_name: str,
    contract: dict[str, Any],
    payload: Any,
    paths: dict[str, str],
    label: str,
    report: list[tuple[str, str]],
    compared: str = "",
) -> None:
    """Verify one case's artifact. ``payload`` is None for a case printing text.

    ``compared`` names the binaries under comparison, so a tolerance failure says
    which builds disagreed with the reference.

    A text-mode case writes the same file its JSON-mode sibling does but reports
    nothing to cross-check the size against, so the payload-coupled assertions
    are skipped there and the bytes alone are pinned. None cannot mean "the
    payload failed to parse": the caller reaches this only with a parsed payload
    or with a case that has none.
    """
    if artifact_name == "none":
        return
    artifact = contract["artifacts"].get(artifact_name)
    if artifact is None:
        report.append(("fail", f"{label}: unknown artifact {artifact_name}"))
        return
    path = Path(paths[artifact["placeholder"]])
    if not path.is_file():
        report.append(("fail", f"{label}: expected artifact {path} was not created"))
        return
    data = path.read_bytes()
    if artifact.get("kind") == "wav":
        try:
            with wave.open(str(path), "rb") as input_wav:
                sample_rate = input_wav.getframerate()
                frame_count = input_wav.getnframes()
        except (EOFError, wave.Error) as exc:
            report.append(("fail", f"{label}: artifact is not a readable WAV ({exc})"))
            return
        sample_rate_key = artifact["sample_rate_key"]
        if payload is not None and (
            not isinstance(payload, dict) or payload.get(sample_rate_key) != sample_rate
        ):
            payload_rate = payload.get(sample_rate_key) if isinstance(payload, dict) else None
            report.append(
                (
                    "fail",
                    f"{label}: payload {sample_rate_key}={payload_rate!r} does not equal WAV sample rate {sample_rate}",
                )
            )
        if (
            isinstance(payload, dict)
            and isinstance(payload.get("samples"), int)
            and payload["samples"] != frame_count
        ):
            report.append(
                (
                    "fail",
                    f"{label}: payload samples={payload['samples']!r} does not equal WAV frames {frame_count}",
                )
            )
        if "tolerance_of_peak" in artifact:
            _check_wav_within_tolerance(
                data,
                f"{contract['path']}.{artifact_name}",
                artifact,
                compared,
                label,
                report,
            )
            return
        actual_digest = hashlib.sha256(data).hexdigest()
        if actual_digest != artifact["sha256"]:
            report.append(
                (
                    "fail",
                    f"{label}: artifact SHA-256 {actual_digest} does not match {artifact['sha256']}",
                )
            )
        return
    if artifact.get("kind") == "json":
        try:
            parsed_artifact = parse_single_json(data.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as exc:
            report.append(("fail", f"{label}: artifact is not one JSON document ({exc})"))
            return
        if not isinstance(parsed_artifact, dict) or set(parsed_artifact) != set(artifact["keys"]):
            actual_keys = sorted(parsed_artifact) if isinstance(parsed_artifact, dict) else None
            report.append(
                (
                    "fail",
                    f"{label}: JSON artifact keys {actual_keys!r} do not match {artifact['keys']!r}",
                )
            )
        actual_digest = hashlib.sha256(data).hexdigest()
        if actual_digest != artifact["sha256"]:
            report.append(
                (
                    "fail",
                    f"{label}: artifact SHA-256 {actual_digest} does not match {artifact['sha256']}",
                )
            )
        return
    bytes_key = artifact["bytes_key"]
    if payload is not None and (
        not isinstance(payload, dict) or payload.get(bytes_key) != len(data)
    ):
        payload_bytes = payload.get(bytes_key) if isinstance(payload, dict) else None
        message = (
            f"{label}: payload {bytes_key}={payload_bytes!r} does not equal "
            f"artifact size {len(data)}"
        )
        report.append(
            (
                "fail",
                message,
            )
        )
    actual_digest = hashlib.sha256(data).hexdigest()
    if actual_digest != artifact["sha256"]:
        report.append(
            (
                "fail",
                (f"{label}: artifact SHA-256 {actual_digest} does not match {artifact['sha256']}"),
            )
        )
    try:
        parse_single_json(data.decode(artifact["encoding"]))
    except (UnicodeDecodeError, ValueError) as exc:
        report.append(("fail", f"{label}: artifact is not one JSON document ({exc})"))


def _record_reference(argv: list[str] | None = None) -> int:
    """Store a WAV as the reference a tolerance artifact is compared against."""
    parser = argparse.ArgumentParser(description=_record_reference.__doc__)
    parser.add_argument("key", help="<command path>.<artifact name>, e.g. voice-change.preset_wav")
    parser.add_argument("wav", type=Path)
    parser.add_argument("--provenance", required=True, help="the build that wrote the WAV")
    args = parser.parse_args(argv)
    data = args.wav.read_bytes()
    _wav_samples(data)
    references = json.loads(WAV_REFERENCES.read_text(encoding="utf-8"))
    references[args.key] = {
        "provenance": args.provenance,
        "wav_base64": base64.b64encode(data).decode("ascii"),
    }
    text = json.dumps(dict(sorted(references.items())), indent=2) + "\n"
    temporary = WAV_REFERENCES.with_suffix(".json.tmp")
    temporary.write_text(text, encoding="utf-8")
    temporary.replace(WAV_REFERENCES)
    print(f"{args.key}: reference_sha256 {hashlib.sha256(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(_record_reference())
