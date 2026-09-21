"""The shipped mixer-scene JSON Schema against the scenes the library emits.

`tests/conformance/check_result_schema_surfaces.py` already holds the schema's
field paths to the writer's own published list, so a key the writer emits cannot
be missing here. What that check cannot see is the half a path list does not
carry: whether the TYPE the schema declares for a leaf is the type the writer
puts there, and whether the schema keeps any power once it is written -- an
`additionalProperties: false` that never meets an unknown key, or an `enum` that
never meets a value outside it, reads exactly like one that does.

So this walks real scene documents rather than a fixture: the built-in presets,
and a scene the assistant suggested. A fixture written alongside the schema would
share its author's idea of the shape, which is the one thing a check may not
share with what it checks.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

import libsonare

ROOT = Path(__file__).resolve().parents[3]
SCHEMA = json.loads((ROOT / "schemas/mixer-scene.schema.json").read_text(encoding="utf-8"))

_JSON_TYPES: dict[str, tuple[type, ...]] = {
    "object": (dict,),
    "array": (list,),
    "string": (str,),
    "number": (int, float),
    "integer": (int,),
    "boolean": (bool,),
}


def _resolve(node: Any) -> Any:
    """Follow a local `$ref`, which is how the strip/bus/insert defs are shared."""
    while isinstance(node, dict) and "$ref" in node:
        target: Any = SCHEMA
        for part in node["$ref"][2:].split("/"):
            target = target[part]
        node = target
    return node


def _check(value: Any, node: Any, where: str, findings: list[str]) -> None:
    """Report every disagreement between one document node and its schema node."""
    node = _resolve(node)
    declared = node.get("type")
    if declared is not None:
        expected = _JSON_TYPES[declared]
        # bool is an int in Python, so a boolean would satisfy "number" and an
        # integer field would accept True without this.
        if isinstance(value, bool) != (declared == "boolean") or not isinstance(value, expected):
            findings.append(f"{where}: {type(value).__name__} is not {declared}")
            return
    if "const" in node and value != node["const"]:
        findings.append(f"{where}: {value!r} is not the required {node['const']!r}")
    if "enum" in node and value not in node["enum"]:
        findings.append(f"{where}: {value!r} is outside {node['enum']}")
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in node and value < node["minimum"]:
            findings.append(f"{where}: {value} is under the minimum {node['minimum']}")
        if "maximum" in node and value > node["maximum"]:
            findings.append(f"{where}: {value} is over the maximum {node['maximum']}")
    if isinstance(value, dict):
        properties = node.get("properties", {})
        for key in node.get("required", []):
            if key not in value:
                findings.append(f"{where}.{key}: required but absent")
        for key, item in value.items():
            if key not in properties:
                if node.get("additionalProperties") is False:
                    findings.append(f"{where}.{key}: not declared by the schema")
                continue
            _check(item, properties[key], f"{where}.{key}", findings)
    elif isinstance(value, list) and "items" in node:
        for index, item in enumerate(value):
            _check(item, node["items"], f"{where}[{index}]", findings)


def _findings(document: dict) -> list[str]:
    findings: list[str] = []
    _check(document, SCHEMA, "scene", findings)
    return findings


def _preset_scene(name: str) -> dict:
    document = json.loads(libsonare.mixing_scene_preset_json(name))
    return document.get("scene", document)


def test_every_builtin_preset_scene_satisfies_the_shipped_schema() -> None:
    """A scene the library itself emits must validate, or the schema is wrong."""
    names = libsonare.mixing_scene_preset_names()
    assert names, "no built-in scene presets: the walk below would range over nothing"
    for name in names:
        assert _findings(_preset_scene(name)) == [], name


def test_a_suggested_scene_satisfies_the_shipped_schema() -> None:
    """The assistant writes scenes the presets do not: sends, buses, insert params."""
    scene = _preset_scene("vocalReverbSend")
    # The preset set is small and hand-written, so it is checked against a
    # document that was built rather than authored: sends and an aux bus are the
    # parts a preset list could omit entirely and leave the schema untested for.
    assert any(strip.get("sends") for strip in scene["strips"])
    assert any(bus.get("role") == "aux" for bus in scene["buses"])
    assert _findings(scene) == []


@pytest.mark.parametrize(
    ("mutate", "expected"),
    [
        (lambda s: s["strips"][0].update(faderDB=1.0), "not declared by the schema"),
        (lambda s: s["strips"][0].update(faderDb="loud"), "is not number"),
        (lambda s: s["strips"][0].update(width=7.0), "over the maximum"),
        (lambda s: s["strips"][0].update(muted=1), "is not boolean"),
        (lambda s: s.update(version=2), "is not the required 1"),
        (
            lambda s: s["strips"][0]["inserts"].append({"slot": "middle", "processor": "x"}),
            "outside ['pre', 'post']",
        ),
        (lambda s: s["strips"][0]["inserts"].append({"slot": "pre"}), "required but absent"),
        (
            lambda s: s["strips"][0]["inserts"][0].update(params=json.dumps({"ratio": 4})),
            "is not object",
        ),
    ],
)
def test_the_schema_refuses_what_it_is_written_to_refuse(mutate, expected: str) -> None:
    """Each guard meets a value it must reject, so none of them is decorative.

    The wrong-case `faderDB` is the one that matters most for a generated scene:
    the parser ignores an unknown key, so without `additionalProperties: false`
    it is a fader silently left at 0 rather than an error.
    """
    scene = _preset_scene("vocalReverbSend")
    assert _findings(scene) == []  # the mutation is the only difference
    mutate(scene)
    findings = _findings(scene)
    assert any(expected in finding for finding in findings), findings


# (field path, the loader's maximum, the substring its refusal message carries).
# Every one is a range the loader rejects outside of rather than clamping into,
# which is what makes a schema maximum the difference between "validates" and
# "loads".
_BOUNDED = [
    (("strips", 0, "panMode"), 2, "panMode enum is out of range"),
    (("strips", 0, "panLaw"), 3, "panLaw enum is out of range"),
    (("strips", 0, "channelDelaySamples"), 192000, "channelDelaySamples must be in"),
    (("strips", 0, "metering", "truePeakOversample"), 16, "truePeakOversample must be in"),
]


def _place(scene: dict, path: tuple, value: object) -> None:
    node: Any = scene
    for step in path[:-1]:
        node = node[step] if isinstance(step, int) else node.setdefault(step, {})
    node[path[-1]] = value


@pytest.mark.parametrize(("path", "maximum", "message"), _BOUNDED, ids=lambda v: str(v)[:40])
def test_the_schema_and_the_loader_agree_on_where_each_bound_sits(
    path: tuple, maximum: int, message: str
) -> None:
    """One past the maximum must be refused by BOTH, and the maximum by neither.

    Asserting only the rejection would pass against a schema whose maximum sits
    anywhere at or below the loader's, silently narrowing what validates; the
    at-the-bound half is what pins the two to the same number. And the two arms
    are handed the SAME document -- an earlier draft validated the bare scene
    and gave the loader a `{"scene": ...}` wrapper, which the parser reads as a
    document with no strips, so it built an empty mixer and raised nothing. Both
    arms passed on inputs that were never the same object.

    The loader's own message does not survive to Python (every failure arrives as
    one RuntimeError), so attribution comes from the differential instead: the
    only thing that changed between the accepted and the refused run is this one
    field, which is a stronger claim than matching a string would be.
    """
    del message  # the loader's specific wording does not reach this surface
    scene = _preset_scene("vocalReverbSend")

    _place(scene, path, maximum)
    assert _findings(scene) == []  # the schema admits the boundary value
    libsonare.Mixer.from_scene_json(json.dumps(scene)).close()  # and so does the loader

    _place(scene, path, maximum + 1)
    findings = _findings(scene)
    assert any("over the maximum" in finding for finding in findings), findings
    with pytest.raises(RuntimeError):
        libsonare.Mixer.from_scene_json(json.dumps(scene))
