"""mastering_chain_config_from_json: the params as the core wrote them, refusals by class."""

from __future__ import annotations

import json

import pytest

import libsonare
from libsonare import ErrorCode

from ._helpers import raises_coded


def _document(config: libsonare.MasteringChainConfig) -> str:
    return json.dumps({"version": 2, "params": config})


def test_a_preset_round_trips_through_its_document() -> None:
    preset = libsonare.mastering_preset_params(libsonare.mastering_preset_names()[0])
    assert libsonare.mastering_chain_config_from_json(_document(preset)) == preset


def test_a_document_without_params_is_refused_as_invalid_format() -> None:
    with raises_coded(ErrorCode.INVALID_FORMAT):
        libsonare.mastering_chain_config_from_json('{"version": 2}')


def test_a_version_1_document_is_refused_as_invalid_format() -> None:
    preset = libsonare.mastering_preset_params(libsonare.mastering_preset_names()[0])
    document = json.dumps({"version": 1, "params": preset})
    with raises_coded(ErrorCode.INVALID_FORMAT, match="version"):
        libsonare.mastering_chain_config_from_json(document)


def test_an_earlier_key_spelling_is_refused_as_invalid_format() -> None:
    document = json.dumps({"version": 2, "params": {"dynamics.multibandComp.slope": "lr4"}})
    with raises_coded(ErrorCode.INVALID_FORMAT, match="crossover.slope"):
        libsonare.mastering_chain_config_from_json(document)


def test_a_malformed_document_is_refused_as_invalid_format() -> None:
    with raises_coded(ErrorCode.INVALID_FORMAT):
        libsonare.mastering_chain_config_from_json("{")


@pytest.mark.parametrize("document", [b"{}", {"version": 2}, None])
def test_a_non_string_document_is_a_type_error(document: object) -> None:
    with pytest.raises(TypeError):
        libsonare.mastering_chain_config_from_json(document)  # type: ignore[arg-type]
