#!/usr/bin/env python3
"""Print a compact JSON analysis of an audio file."""

from __future__ import annotations

import json
import sys

import libsonare


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} INPUT_AUDIO", file=sys.stderr)
        return 2

    with libsonare.Audio.from_file(sys.argv[1]) as audio:
        result = audio.analyze()
    payload = {
        "bpm": result.bpm,
        "key": result.key.name,
        "chords": [chord.name for chord in result.chords],
        "sections": [section.name for section in result.sections],
    }
    print(json.dumps(payload, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
