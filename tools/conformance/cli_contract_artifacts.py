"""Verifying the file a contract case promised its run would write.

Checks the artifact against its declared kind, the payload field that
announces its size or sample rate, and either the manifest's SHA-256 or, for a
DSP output, a committed reference with a numeric tolerance. Artifacts can
also require exact agreement between the two front-ends on the same host.
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
from cli_contract_payload import _compare_values, parse_single_json

WAV_REFERENCES = REPO_ROOT / "tests" / "conformance" / "cli_contract_wav_references.json"
JSON_REFERENCES = REPO_ROOT / "tests" / "conformance" / "cli_contract_json_references.json"


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
    if len(frames) != header[1] * width * header[3]:
        raise wave.Error("PCM data is shorter than its frame count")
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
    scale = 1 << (8 * expected_header[2] - 1)
    exact_values: set[float] = set()
    if "silence" in artifact.get("exact_pcm", []):
        exact_values.add(0.0)
    if "full_scale" in artifact.get("exact_pcm", []):
        exact_values.update((-1.0, (scale - 1) / scale))
    if not exact_values.issubset(expected):
        report.append(("fail", f"{label}: reference does not contain the declared exact PCM codes"))
        return

    worst, position = 0.0, 0
    for index, (actual_sample, reference_sample) in enumerate(zip(actual, expected, strict=True)):
        if reference_sample in exact_values:
            if actual_sample != reference_sample:
                report.append(
                    (
                        "fail",
                        (
                            f"{label}: sample {index} PCM code {actual_sample * scale:g} must equal "
                            f"{reference_sample * scale:g}{produced}"
                        ),
                    )
                )
                return
        else:
            worst, position = max((worst, position), (abs(actual_sample - reference_sample), index))
    if "tolerance_lsb" in artifact:
        tolerance = artifact["tolerance_lsb"] / scale
        units = f"{artifact['tolerance_lsb']} LSB"
    else:
        peak = max((abs(value) for value in expected), default=0.0)
        tolerance = artifact["tolerance_of_peak"] * peak
        units = f"{artifact['tolerance_of_peak']} of peak {peak:.3g}"
    if worst > tolerance:
        report.append(
            (
                "fail",
                (
                    f"{label}: sample {position} is {worst * scale:g} LSB from the reference, "
                    f"past the tolerance ({units}){produced}"
                ),
            )
        )


def _check_json_within_tolerance(
    actual: Any,
    reference_key: str,
    tolerance: float,
    label: str,
    report: list[tuple[str, str]],
) -> None:
    references = json.loads(JSON_REFERENCES.read_text(encoding="utf-8"))
    if reference_key not in references:
        report.append(("fail", f"{label}: no reference {reference_key} in {JSON_REFERENCES.name}"))
        return
    mismatch = _compare_values(actual, references[reference_key], label, tolerance, 0.0)
    if mismatch:
        report.append(("fail", mismatch))


def _compare_artifact_outputs(
    native: dict[tuple[str, str], bytes],
    python: dict[tuple[str, str], bytes],
    report: list[tuple[str, str]],
) -> None:
    for path, case_id in sorted(native.keys() & python.keys()):
        if native[path, case_id] != python[path, case_id]:
            report.append(("fail", f"surface comparison: {path}.{case_id}: artifact bytes differ"))


def _check_artifact(
    artifact_name: str,
    contract: dict[str, Any],
    payload: Any,
    paths: dict[str, str],
    label: str,
    report: list[tuple[str, str]],
    compared: str = "",
) -> bytes | None:
    """Verify the artifact and return its bytes when the manifest requires same-host parity.

    ``payload`` is None for a case printing text. ``compared`` names the binaries under comparison, so a tolerance failure says
    which builds disagreed with the reference.

    A text-mode case writes the same file its JSON-mode sibling does but reports
    nothing to cross-check the size against, so the payload-coupled assertions
    are skipped there. None cannot mean "the
    payload failed to parse": the caller reaches this only with a parsed payload
    or with a case that has none.
    """
    if artifact_name == "none":
        return None
    artifact = contract["artifacts"].get(artifact_name)
    if artifact is None:
        report.append(("fail", f"{label}: unknown artifact {artifact_name}"))
        return None
    path = Path(paths[artifact["placeholder"]])
    if not path.is_file():
        report.append(("fail", f"{label}: expected artifact {path} was not created"))
        return None
    data = path.read_bytes()
    peer_bytes = data if artifact.get("compare_bytes") else None
    reference_key = f"{contract['path']}.{artifact_name}"
    if artifact.get("kind") == "wav":
        try:
            with wave.open(str(path), "rb") as input_wav:
                sample_rate = input_wav.getframerate()
                frame_count = input_wav.getnframes()
        except (EOFError, wave.Error) as exc:
            report.append(("fail", f"{label}: artifact is not a readable WAV ({exc})"))
            return None
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
        if "reference_sha256" in artifact:
            _check_wav_within_tolerance(
                data,
                reference_key,
                artifact,
                compared,
                label,
                report,
            )
            return peer_bytes
        actual_digest = hashlib.sha256(data).hexdigest()
        if actual_digest != artifact["sha256"]:
            report.append(
                (
                    "fail",
                    f"{label}: artifact SHA-256 {actual_digest} does not match {artifact['sha256']}",
                )
            )
        return peer_bytes
    if artifact.get("kind") == "json":
        try:
            parsed_artifact = parse_single_json(data.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as exc:
            report.append(("fail", f"{label}: artifact is not one JSON document ({exc})"))
            return None
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
        return peer_bytes
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
    try:
        parsed_artifact = parse_single_json(data.decode(artifact["encoding"]))
    except (UnicodeDecodeError, ValueError) as exc:
        report.append(("fail", f"{label}: artifact is not one JSON document ({exc})"))
        return peer_bytes
    if "tolerance_of_samples" in artifact:
        _check_json_within_tolerance(
            parsed_artifact, reference_key, artifact["tolerance_of_samples"], label, report
        )
    else:
        actual_digest = hashlib.sha256(data).hexdigest()
        if actual_digest != artifact["sha256"]:
            report.append(
                (
                    "fail",
                    f"{label}: artifact SHA-256 {actual_digest} does not match {artifact['sha256']}",
                )
            )
    return peer_bytes


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
