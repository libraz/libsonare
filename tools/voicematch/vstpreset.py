"""Turn a `.vstpreset` into the saved state an audio unit will actually read.

`docs/oracles.md` says `--au-preset` reaches a timbre only in a plugin whose
audio-unit build keeps its state under `Processor State` and `Controller
State`. Those two keys are one vendor's convention. A sampler that hosts
third-party libraries usually keeps everything in one blob of its own instead,
accepts the dictionary it is handed and ignores what was put in it — so a
4 MB rack arrives as an empty one, renders silence or whatever the plugin
loads by default, and nothing anywhere reports a failure.

The route for such a plugin is a saved class-info dictionary, which is what an
`.aupreset` is, passed through the capture definition's `state` field. Getting
one normally needs a host that can both load the instrument and save audio-unit
state. This converts instead: a `.vstpreset`'s `Comp` chunk is the component
state the plugin itself produced, and it is byte-compatible with what the
audio-unit build keeps under its own key, because both are one `getState()`
output under different wrappers.

**The conversion is verified rather than assumed.** A plugin that ingested the
blob re-serialises to a size unlike both its default and the input; one that
ignored it comes back at exactly the default. That check is the whole reason
this is a tool and not a paragraph: the failure it catches has no other
symptom.
"""

from __future__ import annotations

import argparse
import plistlib
import re
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from au_oracle import find_aubounce  # noqa: E402

VSTPRESET_MAGIC = b"VST3"

#: Where a plugin's own state usually lives in an audio unit's class-info
#: dictionary. `aubounce info <plugin> --values` lists the keys it really uses.
DEFAULT_STATE_KEY = "vstdata"

_SIZE_LINE = re.compile(r"^\s*(\S+)\s+\((\d+) bytes\)\s*$")


class VstPresetError(RuntimeError):
    """A `.vstpreset` that cannot be read, or a conversion that did not take."""


def read_vstpreset(path: Path | str) -> dict[str, bytes]:
    """Every chunk of a `.vstpreset`, keyed by its four-character id.

    The component state is `Comp`; `Cont` is the controller's and is commonly
    empty; `Info` is an XML description of the preset.
    """
    raw = Path(path).read_bytes()
    if raw[:4] != VSTPRESET_MAGIC:
        raise VstPresetError(f"{path}: not a .vstpreset (magic {raw[:4]!r})")
    list_offset = struct.unpack_from("<q", raw, 40)[0]
    if raw[list_offset : list_offset + 4] != b"List":
        raise VstPresetError(f"{path}: no chunk list at offset {list_offset}")
    count = struct.unpack_from("<i", raw, list_offset + 4)[0]
    chunks: dict[str, bytes] = {}
    for i in range(count):
        entry = list_offset + 8 + i * 20
        chunk_id = raw[entry : entry + 4].decode("ascii", "replace")
        offset, size = struct.unpack_from("<qq", raw, entry + 4)
        chunks[chunk_id] = raw[offset : offset + size]
    return chunks


def component_codes(triple: str) -> tuple[int, int, int]:
    """`type`, `subtype` and `manufacturer` for a `type:subtype:manufacturer` triple.

    An audio unit identifies itself by three four-character codes packed
    big-endian, which is what `aubounce list` prints with colons between them.
    """
    parts = triple.split(":")
    if len(parts) != 3 or any(len(p) != 4 for p in parts):
        raise VstPresetError(
            f"{triple!r} is not a type:subtype:manufacturer triple of three "
            "four-character codes — `aubounce list` prints them"
        )
    return tuple(int.from_bytes(p.encode("ascii"), "big") for p in parts)  # type: ignore[return-value]


def build_aupreset(state: bytes, *, triple: str, name: str, key: str = DEFAULT_STATE_KEY) -> bytes:
    """An `.aupreset` carrying @p state under @p key, as a binary-free XML plist."""
    au_type, subtype, manufacturer = component_codes(triple)
    return plistlib.dumps(
        {
            "type": au_type,
            "subtype": subtype,
            "manufacturer": manufacturer,
            "version": 0,
            "name": name,
            key: state,
        },
        fmt=plistlib.FMT_XML,
    )


def state_sizes(text: str) -> dict[str, int]:
    """The `<key> (<n> bytes)` entries of an `aubounce info --values` report."""
    sizes: dict[str, int] = {}
    for line in text.splitlines():
        m = _SIZE_LINE.match(line)
        if m:
            sizes[m.group(1)] = int(m.group(2))
    return sizes


def _read_state_sizes(
    plugin: str, *, aubounce: Path, settle_ms: int, state: Path | None
) -> dict[str, int]:
    argv = [str(aubounce), "info", plugin, "--settle-ms", str(settle_ms), "--values"]
    if state is not None:
        argv += ["--state", str(state)]
    done = subprocess.run(argv, capture_output=True, text=True, check=False)
    if done.returncode != 0:
        raise VstPresetError(f"aubounce info failed: {done.stderr.strip() or done.stdout.strip()}")
    return state_sizes(done.stdout)


def verify(
    plugin: str,
    preset: Path,
    *,
    key: str = DEFAULT_STATE_KEY,
    settle_ms: int = 20000,
    aubounce: Path | None = None,
) -> tuple[int, int]:
    """Load @p preset and report the plugin's state size with and without it.

    Raises when the two agree: that is a plugin which accepted the dictionary
    and kept its default, which is the silent failure this exists to catch.
    """
    binary = aubounce or find_aubounce()
    default = _read_state_sizes(plugin, aubounce=binary, settle_ms=settle_ms, state=None)
    loaded = _read_state_sizes(plugin, aubounce=binary, settle_ms=settle_ms, state=preset)
    before, after = default.get(key, 0), loaded.get(key, 0)
    if after == before:
        raise VstPresetError(
            f"{plugin} came back at its default {key} size ({before} bytes) with the "
            f"preset loaded, so it ignored the blob. Check which key it uses: "
            f"`aubounce info {plugin} --values`"
        )
    return before, after


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("preset", type=Path, help="the .vstpreset to convert")
    parser.add_argument(
        "--plugin", required=True, help="the plugin's type:subtype:manufacturer triple"
    )
    parser.add_argument("--out", type=Path, help="destination .aupreset (default: alongside)")
    parser.add_argument(
        "--key",
        default=DEFAULT_STATE_KEY,
        help=f"class-info key to carry the state (default {DEFAULT_STATE_KEY})",
    )
    parser.add_argument("--chunk", default="Comp", help="which .vstpreset chunk to take")
    parser.add_argument("--name", help="preset name (default: the input's stem)")
    parser.add_argument("--settle-ms", type=int, default=20000, help="settle time for the check")
    parser.add_argument(
        "--no-verify",
        action="store_true",
        help="skip loading the result to check it took",
    )
    args = parser.parse_args(argv)

    chunks = read_vstpreset(args.preset)
    if args.chunk not in chunks:
        raise SystemExit(f"{args.preset}: no {args.chunk} chunk; it carries {sorted(chunks)}")
    state = chunks[args.chunk]
    out = args.out or args.preset.with_suffix(".aupreset")
    out.write_bytes(
        build_aupreset(state, triple=args.plugin, name=args.name or args.preset.stem, key=args.key)
    )
    print(f"{out}  {args.chunk} {len(state)} bytes under {args.key}")

    if args.no_verify:
        print("not verified — load it and compare the state size before believing it")
        return 0
    before, after = verify(args.plugin, out, key=args.key, settle_ms=args.settle_ms)
    print(f"verified: {args.key} {before} bytes by default, {after} with the preset")
    return 0


if __name__ == "__main__":
    sys.exit(main())
