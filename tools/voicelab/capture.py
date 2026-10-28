"""Play a stimulus and record the response of an external device in one pass.

The stimulus WAV is played to a chosen pair of output channels while the chosen input channels are
recorded at the same time on the same audio clock, so the delay between recorded channels carries
no host-side latency. Each named input group is written as its own WAV; `sonare_voicelab analyze`
reads two of them.

Channel assignment comes from a JSON device file (1-based channel numbers):

    {
      "output_device": "<name substring or index>",
      "input_device": "<name substring or index>",
      "sample_rate": 48000,
      "output_channels": [3, 4],
      "input_channels": {"dry": [5], "wet": [3]}
    }

Usage:
    capture.py --list
    capture.py --stimulus stim.wav --device-file device.json --out-dir runs/run1

Keep the device file and the recordings outside version control: they describe one machine.
"""

from __future__ import annotations

import argparse
import json
import sys
import wave
from pathlib import Path
from typing import Any, TypeAlias

import numpy as np
import numpy.typing as npt
import sounddevice as sd  # type: ignore[import-untyped]

FloatArray: TypeAlias = npt.NDArray[np.float32]

DEFAULT_TAIL_S = 1.0


def list_input_devices() -> int:
    """Prints every PortAudio device that has input channels."""
    found = 0
    for index, device in enumerate(sd.query_devices()):
        channels = int(device["max_input_channels"])
        if channels > 0:
            print(f"{index}\t{channels} in\t{device['name']}")
            found += 1
    if found == 0:
        print("no input devices found", file=sys.stderr)
        return 1
    return 0


def resolve_device(spec: str | int, want_input: bool) -> int:
    """Maps a device index or a name substring to a PortAudio device index."""
    devices = sd.query_devices()
    key = "max_input_channels" if want_input else "max_output_channels"
    if isinstance(spec, int):
        if not 0 <= spec < len(devices) or devices[spec][key] <= 0:
            raise SystemExit(f"device {spec} has no {'input' if want_input else 'output'} channels")
        return spec
    matches = [i for i, d in enumerate(devices) if spec.lower() in d["name"].lower() and d[key] > 0]
    if len(matches) != 1:
        raise SystemExit(f"device '{spec}' matched {len(matches)} devices; use an index")
    return matches[0]


def read_wav(path: Path) -> tuple[FloatArray, int]:
    """Reads a 16/24/32-bit PCM WAV as float32 frames of shape (n, channels)."""
    with wave.open(str(path), "rb") as w:
        channels, width, rate, frames = (
            w.getnchannels(),
            w.getsampwidth(),
            w.getframerate(),
            w.getnframes(),
        )
        raw = w.readframes(frames)
    if width == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    elif width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        ints = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        ints = np.where(ints >= 1 << 23, ints - (1 << 24), ints)
        data = ints.astype(np.float32) / 8388608.0
    elif width == 4:
        data = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0
    else:
        raise SystemExit(f"{path}: unsupported sample width {width * 8} bits")
    return data.reshape(-1, channels), rate


def write_wav(path: Path, data: FloatArray, rate: int) -> None:
    """Writes float frames of shape (n, channels) as a 24-bit PCM WAV."""
    ints = np.clip(np.rint(data * 8388607.0), -8388608, 8388607).astype(np.int32)
    packed = np.stack([ints & 0xFF, (ints >> 8) & 0xFF, (ints >> 16) & 0xFF], axis=-1)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(data.shape[1])
        w.setsampwidth(3)
        w.setframerate(rate)
        w.writeframes(packed.astype(np.uint8).tobytes())


def load_device_file(path: Path) -> dict[str, Any]:
    """Loads and checks the channel assignment file."""
    if not path.is_file():
        raise SystemExit(f"device file not found: {path}")
    config: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    for key in ("output_device", "input_device", "output_channels", "input_channels"):
        if key not in config:
            raise SystemExit(f"{path}: missing '{key}'")
    return config


def capture(stimulus_path: Path, device_file: Path, out_dir: Path, tail_s: float) -> int:
    """Plays the stimulus, records the assigned input groups and writes one WAV per group."""
    config = load_device_file(device_file)
    stimulus, rate = read_wav(stimulus_path)
    if "sample_rate" in config and int(config["sample_rate"]) != rate:
        raise SystemExit(f"stimulus is {rate} Hz but the device file says {config['sample_rate']}")

    out_channels: list[int] = [int(c) for c in config["output_channels"]]
    groups: dict[str, list[int]] = {
        k: [int(c) for c in v] for k, v in config["input_channels"].items()
    }
    if stimulus.shape[1] == 1:
        stimulus = np.repeat(stimulus, len(out_channels), axis=1)
    if stimulus.shape[1] != len(out_channels):
        raise SystemExit(
            f"stimulus has {stimulus.shape[1]} channels, device file assigns {len(out_channels)}"
        )
    padded = np.concatenate(
        [stimulus, np.zeros((int(tail_s * rate), stimulus.shape[1]), dtype=np.float32)]
    )

    in_channels = sorted({c for chans in groups.values() for c in chans})
    in_dev = resolve_device(config["input_device"], want_input=True)
    out_dev = resolve_device(config["output_device"], want_input=False)
    recorded = sd.playrec(
        padded,
        samplerate=rate,
        device=(in_dev, out_dev),
        input_mapping=in_channels,
        output_mapping=out_channels,
        dtype="float32",
    )
    sd.wait()

    for name, chans in groups.items():
        columns = [in_channels.index(c) for c in chans]
        write_wav(out_dir / f"{name}.wav", np.asarray(recorded[:, columns], dtype=np.float32), rate)
        print(f"wrote {out_dir / (name + '.wav')}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--list", action="store_true", help="list devices that have input channels")
    parser.add_argument("--stimulus", type=Path, help="stimulus WAV to play")
    parser.add_argument("--device-file", type=Path, help="JSON channel assignment")
    parser.add_argument("--out-dir", type=Path, help="directory for the recorded WAVs")
    parser.add_argument(
        "--tail-s", type=float, default=DEFAULT_TAIL_S, help="extra seconds recorded"
    )
    args = parser.parse_args()

    if args.list:
        return list_input_devices()
    if args.stimulus is None or args.device_file is None or args.out_dir is None:
        parser.error("--stimulus, --device-file and --out-dir are required unless --list is given")
    return capture(args.stimulus, args.device_file, args.out_dir, args.tail_s)


if __name__ == "__main__":
    sys.exit(main())
