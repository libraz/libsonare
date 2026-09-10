"""Verifying the file a contract case promised its run would write.

Checks the artifact against its declared kind, the payload field that
announces its size or sample rate, and the manifest's SHA-256.
"""

from __future__ import annotations

import hashlib
import wave
from pathlib import Path
from typing import Any

from cli_contract_payload import parse_single_json


def _check_artifact(
    artifact_name: str,
    contract: dict[str, Any],
    payload: Any,
    paths: dict[str, str],
    label: str,
    report: list[tuple[str, str]],
) -> None:
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
        if not isinstance(payload, dict) or payload.get(sample_rate_key) != sample_rate:
            payload_rate = (
                payload.get(sample_rate_key) if isinstance(payload, dict) else None
            )
            report.append(
                (
                    "fail",
                    f"{label}: payload {sample_rate_key}={payload_rate!r} does not equal WAV sample rate {sample_rate}",
                )
            )
        if isinstance(payload, dict) and isinstance(payload.get("samples"), int):
            if payload["samples"] != frame_count:
                report.append(
                    (
                        "fail",
                        f"{label}: payload samples={payload['samples']!r} does not equal WAV frames {frame_count}",
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
    if artifact.get("kind") == "json":
        try:
            parsed_artifact = parse_single_json(data.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as exc:
            report.append(
                ("fail", f"{label}: artifact is not one JSON document ({exc})")
            )
            return
        if not isinstance(parsed_artifact, dict) or set(parsed_artifact) != set(
            artifact["keys"]
        ):
            actual_keys = (
                sorted(parsed_artifact) if isinstance(parsed_artifact, dict) else None
            )
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
    if not isinstance(payload, dict) or payload.get(bytes_key) != len(data):
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
                f"{label}: artifact SHA-256 {actual_digest} does not match "
                f"{artifact['sha256']}",
            )
        )
    try:
        parse_single_json(data.decode(artifact["encoding"]))
    except (UnicodeDecodeError, ValueError) as exc:
        report.append(("fail", f"{label}: artifact is not one JSON document ({exc})"))
