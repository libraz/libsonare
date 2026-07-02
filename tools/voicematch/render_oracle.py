"""Oracle-side renderer: SMF bytes -> audio through a reference GM synth.

Route A (default, always available): fluidsynth fast-render with a GM
SoundFont (assets/MuseScore_General.sf3 unless overridden via --sf2 or
VOICEMATCH_SF2). Route B (plugin-hosted oracle via DawDreamer + a real VST,
see probe_grand.py) can slot in later behind the same interface: SMF bytes in,
(frames, 2) float32 out.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

import numpy as np

from wavio import read_wav

ASSETS_DIR = Path(__file__).resolve().parent / "assets"
DEFAULT_SF2 = ASSETS_DIR / "MuseScore_General.sf3"


def default_soundfont() -> Path:
    """Resolve the oracle SoundFont (env override, then the bundled asset)."""
    env = os.environ.get("VOICEMATCH_SF2")
    if env:
        return Path(env)
    return DEFAULT_SF2


def render_oracle_fluidsynth(
    smf_bytes: bytes,
    total_seconds: float,
    sr: int = 48000,
    *,
    soundfont: Path | None = None,
    gain: float = 0.5,
) -> np.ndarray:
    """Render SMF bytes to a (frames, 2) float32 array via fluidsynth.

    fluidsynth stops at the SMF end-of-track marker, so the caller must have
    written the file with enough `end_pad` to cover the release tail. Output is
    trimmed/zero-padded to exactly `total_seconds`.
    """
    sf2 = soundfont if soundfont is not None else default_soundfont()
    if not sf2.exists():
        raise FileNotFoundError(
            f"oracle SoundFont not found: {sf2} (set VOICEMATCH_SF2 or pass --sf2)"
        )
    with tempfile.TemporaryDirectory(prefix="voicematch_") as tmp:
        mid_path = Path(tmp) / "in.mid"
        wav_path = Path(tmp) / "out.wav"
        mid_path.write_bytes(smf_bytes)
        cmd = [
            "fluidsynth",
            "-ni",                # no shell, no MIDI driver
            "-r", str(sr),
            "-g", str(gain),
            "-R", "0",            # dry render: reverb/chorus tails would
            "-C", "0",            # contaminate release and TNR metrics
            "-F", str(wav_path),  # fast render to file
            str(sf2),
            str(mid_path),
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0 or not wav_path.exists():
            raise RuntimeError(f"fluidsynth failed (rc={proc.returncode}):\n{proc.stderr.strip()}")
        audio, got_sr = read_wav(wav_path)
    if got_sr != sr:
        raise RuntimeError(f"fluidsynth rendered at {got_sr} Hz, expected {sr}")
    want = int(round(total_seconds * sr))
    if audio.shape[0] >= want:
        return audio[:want]
    pad = np.zeros((want - audio.shape[0], audio.shape[1]), dtype=np.float32)
    return np.concatenate([audio, pad], axis=0)
