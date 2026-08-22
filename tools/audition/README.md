# audition — an A/B listening page for a directory of renders

```sh
python tools/audition/serve.py <audition-dir>
```

Opens a browser on a page that plays every version of a take from one
transport, so a switch costs nothing — no second or two spent stopping one file
and starting another, which is long enough for the ear to lose what it was
holding.

A switch seeks back to the start, so the attack is the first thing heard.
Turn `switch restarts` off and the versions stay sample-aligned instead: the
switch becomes a gain change on a sound that never stopped, which is the only
way to compare a sustain or a decay.

Standard library only, no build step, nothing specific to this repository.
Point it at any directory of renders.

## What it shows

- **All versions overlaid** on the waveform, the active one solid and the rest
  dimmed, so a level or envelope difference is visible before it is audible.
- **A log-frequency spectrogram** of the active version, which is where an
  inharmonicity or a decay-rate difference shows itself as a shape rather than
  as a number.
- **Levels as captured.** Versions of a take are expected to be written at one
  shared gain so their level difference survives; `match loudness` equalises
  them when that difference is in the way, and says how much gain it applied.

## Keys

| key | |
|---|---|
| `space` | play / pause |
| `1`…`9`, `←` `→` | switch version |
| `↑` `↓` | previous / next take |
| `L` | loop |
| `M` | match loudness |
| `S` | switch restarts |
| `B` | blind mode |
| `R` | reveal this take, or reshuffle if already revealed |

Dragging across the waveform sets a loop region; a click with no drag seeks.

**Blind mode** hides which version is which and shuffles them per take, so a
preference is a preference rather than an expectation. Whichever version is
selected when you move on is recorded as the pick, and the running tally is in
the transport bar. `export notes` writes the picks and the per-take notes to a
JSON file.

Notes and picks live in this browser's local storage, keyed by the manifest
title. Two different manifests do not share them; re-rendering the same
manifest keeps them.

## The manifest

`manifest.json` in the served directory:

```json
{
  "title": "libsonare piano vs The Grand 3",
  "notes": "shown under the title",
  "sources": {
    "model":    { "label": "libsonare NativeSynth" },
    "c7-close": { "label": "The Grand 3 — Yamaha C7, close" }
  },
  "items": [
    {
      "id": "single-c4",
      "label": "Single note — C4, mf",
      "sub": "attack, free decay, damper",
      "group": "one note at a time",
      "tracks": { "model": "single-c4/model.wav", "c7-close": "single-c4/c7-close.wav" }
    }
  ]
}
```

`tracks` maps a source key to a path relative to the served directory; every
key of every take that appears in `sources` gets its label from there and its
own switch button. Takes with the same `group` are listed under one heading.
Nothing but `id` and `tracks` is required.

With no `manifest.json`, one is inferred from the layout: each subdirectory is
a take and the audio files inside it are its versions.

Any format the browser decodes will play. 16-bit PCM WAV is the safe choice —
a 32-bit float `WAVE_FORMAT_EXTENSIBLE` file, which is what most offline
render tools write by default, is decoded by some browsers and not others.

## In this repository

`tools/voicematch/make_audition.py` writes a manifest of this shape: the same
piano phrases rendered through libsonare and through a reference plugin, with
one shared gain per take.
