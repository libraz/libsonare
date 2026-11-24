"""mastering_chain_config_from_json: the params as the core wrote them, refusals by class."""

from __future__ import annotations

import json
import re

import pytest

import libsonare
from libsonare import ErrorCode

from ._helpers import chain_unknown_key_text, raises_coded


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
    with raises_coded(
        ErrorCode.INVALID_FORMAT,
        match=re.escape(chain_unknown_key_text("unknown key params.dynamics.multibandComp.slope"))
        + "$",
    ):
        libsonare.mastering_chain_config_from_json(document)


@pytest.mark.parametrize("value", ["{}", '"x"', "null", "[]"])
@pytest.mark.parametrize(
    ("template", "path"),
    [
        ('{{"params":{{"eq":{{"tilt":{{"undeclared":{}}}}}}}}}', "params.eq.tilt.undeclared"),
        ('{{"params":{{"undeclared":{{"deeper":{}}}}}}}', "params.undeclared"),
    ],
)
def test_an_undeclared_key_is_refused_at_its_first_undeclared_parent(
    value: str, template: str, path: str
) -> None:
    with raises_coded(
        ErrorCode.INVALID_FORMAT,
        match=re.escape(chain_unknown_key_text(f"unknown key {path}")) + "$",
    ):
        libsonare.mastering_chain_config_from_json(template.format(value))


def test_annotation_keys_are_accepted_whatever_they_hold() -> None:
    document = '{"params":{"eq":{"tilt":{"$annotation":{"anything":null}}},"x-extra":[]}}'
    assert libsonare.mastering_chain_config_from_json(
        document
    ) == libsonare.mastering_chain_config_from_json('{"params":{}}')


def test_a_malformed_document_is_refused_as_invalid_format() -> None:
    with raises_coded(ErrorCode.INVALID_FORMAT):
        libsonare.mastering_chain_config_from_json("{")


@pytest.mark.parametrize("document", [b"{}", {"version": 2}, None])
def test_a_non_string_document_is_a_type_error(document: object) -> None:
    with pytest.raises(TypeError):
        libsonare.mastering_chain_config_from_json(document)  # type: ignore[arg-type]
