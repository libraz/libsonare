# audition — an A/B listening page for a directory of renders

```sh
python tools/audition/serve.py [<audition-dir> ...]
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

## Several sets, one server

Name more than one directory and the page gets a control to move between them.
Each is a separate instrument or a separate experiment — a piano set and a
harpsichord set have different takes and different references — so they stay
separate sets rather than one long list, and one server serves them all.

Name none and they are discovered under the scratch root the rest of the
harness renders into — `.cache/voicematch/` here, or wherever
`SONARE_VOICEMATCH_ROOT` points. None of it is committed. **A fresh clone has
nothing there, and that is a supported state:** the reference side of a
comparison is captured from a commercial plugin and cannot be redistributed, so
anyone can render the model side and nobody can render the reference side
without owning the plugin. A set holding one version of each take is therefore
expected rather than broken — the page drops the comparison controls for it and
plays instead. With no renders at all the page opens and says where to put
some. Nothing here requires a reference to exist.

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

Notes and picks live in this browser's local storage, keyed by the set. Two
sets do not share them; re-rendering a set keeps them.

## The manifest

`manifest.json` in the served directory:

```json
{
  "title": "libsonare piano vs the sampled reference",
  "notes": "shown under the title",
  "sources": {
    "model":     { "label": "libsonare NativeSynth" },
    "grand-227": { "label": "227 cm concert grand, close" }
  },
  "items": [
    {
      "id": "single-c4",
      "label": "Single note — C4, mf",
      "sub": "attack, free decay, damper",
      "group": "one note at a time",
      "tracks": { "model": "single-c4/model.wav", "grand-227": "single-c4/grand-227.wav" }
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
phrases rendered through libsonare and through a reference plugin, with one
shared gain per take. Which instrument it renders comes from the capture
definition it is given, which names the phrase set and the GM program:

```sh
rye run --pyproject bindings/python/pyproject.toml python \
    tools/voicematch/make_audition.py \
    --config tools/voicematch/capture/harpsichord.json \
    --out .cache/voicematch/audition/harpsichord
```

Writing it under the scratch root is what lets `serve.py` find it with no
argument.

Add `--model-only` to skip the reference renders. That needs no plugin, so it
is the form that works from a plain clone; the result plays rather than
compares.
