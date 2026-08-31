# Oracles — the four references, and the room they may come with

The model side is fixed: libsonare's GM fallback bank, rendered through the working tree's dylib. The oracle side has four sources, and every one of them is accepted by `compare`, `autofit` and `room-match` through the same interface, so nothing downstream knows which was used.

| source | flag | needs | when |
|---|---|---|---|
| fluidsynth over a GM SoundFont | *(default)* | fluidsynth | any platform, any program, zero setup |
| an externally rendered WAV | `--oracle-wav` | you render the probe somewhere | a VST, a hardware synth, a recorded player |
| a captured single-note corpus | `--corpus` | a `capture.py corpus` run | the route to use when a capture exists |
| an AudioUnit instrument on this machine | `--au` | macOS + the plugin + `aubounce` | no manual render step at all |

## fluidsynth (the default)

`assets/MuseScore_General.sf3`, downloaded from the OSUOSL MuseScore mirror; override with `--sf2` or `VOICEMATCH_SF2`. Rendered **dry** (`-R 0 -C 0`), since reverb tails would contaminate release and noise metrics — so anything a room measurement finds on this route is the instrument, not a space.

## `--corpus`: the captured grid

When a capture exists, this is the route to use. `--corpus` points at the directory `capture.py corpus` wrote (or its `manifest.json`) and the probe *becomes* the capture: its notes, its velocities and its eight-second gate come from the manifest, and the oracle is the captured audio assembled onto that same timeline, one slot per recording.

```sh
autofit.py --spec tools/voicematch/specs/piano_corpus.json --program 0 \
    --corpus <capture dir> --corpus-timbre grand-227 \
    --notes 36,60,84 --velocities 56,120 \
    --optimizer cmaes --max-evals 200 --workers 4
```

It matters more than a convenience flag sounds, because until it existed this harness ran **two pipelines over the same instrument with different stimuli**. `profile.py compare` reported against the captured grid — fifteen notes, four velocities, held eight seconds — and had no search in it. `autofit.py` had the search and scored a stimulus of its own: three notes at one velocity, held two. A value fitted on the second cannot be read off the first, and neither table can say so.

Three properties, all of which the rest of the harness depends on:

- **Slot spacing is the capture's own length**, not a chosen gap, so a note's analysis window is precisely the audio recorded for it and never reaches into its neighbour.
- **The capture's preroll is dropped on assembly**, so a captured onset lands exactly where the model's does. Keeping it would place every reference onset a preroll late and read as an attack that arrives slow on every note of the grid.
- **The oracle is silence wherever the corpus has no recording**, so a grid cut down with `--notes` / `--velocities` stays on the same timeline as the full one — a fit and its hold-out are laid out by the same rule.

The grid is not free: sixty slots of ten seconds is ten minutes of audio per render, so the run prints what it is about to cost before it starts. Cut it with `--notes` / `--velocities`; a six-slot grid is one minute per render and enough to move the decay and level terms.

The capture's dryness is read from the config it names. A dry capture is not given a room; a wet one is measured and the model is placed in a matching space before any metric is taken, exactly as for a supplied WAV.

## `--oracle-wav`: your own rendering

Export the probe, render it wherever the sound you are chasing actually lives, and hand the file back:

```sh
# 1. Write the probe SMF (plus probe.json describing its timeline).
voicematch.py export-probe --programs 0 --pattern sustain
#    -> out/p000_sustain/probe.mid

# 2. Render probe.mid at 48 kHz with the reference instrument — a VST in a DAW,
#    a plugin host, a hardware synth, or a player recorded against the same score.

# 3. Score, or fit, against it.
voicematch.py compare --programs 0 --oracle-wav /path/to/rendered.wav
autofit.py --spec specs/piano.json --program 0 --oracle-wav /path/to/rendered.wav
```

What the harness handles for you:

- **Bit depth and channel count** — PCM 8/16/24/32 and IEEE float 32/64, mono or stereo, including `WAVE_FORMAT_EXTENSIBLE`. A DAW bounce rarely comes out 16-bit.
- **Lead-in silence** — the WAV does not have to start on the first sample. The render's onset-strength function is correlated against an impulse train at the score's note onsets, and the measured offset is removed (reported as `lead-in removed: +NNN ms`). Pin it with `--oracle-offset` or switch it off with `--oracle-no-align`.
- **Length** — trimmed or zero-padded to the probe's timeline.

Sample rate is **not** silently converted: a rate converter's passband error lands in every harmonic the fit reads, so a mismatch is an error telling you to re-render at 48 kHz. `--oracle-resample` overrides that when re-rendering is not an option.

**A stereo reference is summed to mono, and summing a spaced pair comb-filters it.** Two close mics are decorrelated by construction — that is what spacing is for — so their sum has notches where the path difference is half a wavelength, and a notch landing on a partial reads as several dB of harmonic error a mono model cannot reproduce. `--mono-mode left` (or `loudest`) takes one channel and has no sum in it. The default is still `mean`, and not because it is right: every committed profile in `reference/` was measured through one and cannot be re-measured without the plugin it came from, so changing the reduction would silently redefine what those files contain. A run reports the inter-channel correlation when it is low enough for this to matter.

One thing the harness cannot fix, and which will quietly bias a fit:

- **A different note set.** Render the probe SMF as exported. Transposing it, or playing "roughly the same notes" by hand, breaks the per-note analysis windows.

Render the reference dry where you can. Where you cannot — see [Ambience](#ambience-references-that-come-with-a-room).

## `--au`: a plugin on this machine

An AudioUnit instrument installed here can be the oracle directly, with no manual render step in between. [aubounce](https://github.com/libraz/aubounce) hosts it; `au_oracle.py` puts the result behind the same interface as `render_oracle_fluidsynth`, so `compare`, `autofit` and `room-match` all take `--au` without knowing anything changed.

```sh
voicematch.py compare --programs 0 \
    --au "<piano plugin triple>" \
    --au-preset "<close-mic preset>" \
    --au-dry
```

- `--au` takes a plugin name or a `type:subtype:manufacturer` triple; `aubounce list` prints both.
- `--au-preset` takes a `.vstpreset` path or a unique fragment of one, resolved under the macOS preset roots. An ambiguous fragment lists the candidates rather than picking one, because which piano was captured is the whole point.
- `--au-dry` switches off every effect section the plugin advertises. Worth preferring over a preset that merely sounds dry: it is the difference between measuring the instrument and measuring the instrument plus a room, and `room.py` then has nothing to correct.
- `--au-param "Name=value"` sets anything else, repeatable.
- Renders are cached under `out/au_cache/` keyed by the probe, the preset's digest and every host setting, so re-running a comparison costs nothing and a changed preset is a different recording rather than a stale hit. `--au-no-cache` forces a re-render.

The binary is found via `AUBOUNCE`, then `PATH`, then a sibling `../aubounce` checkout.

### A plugin whose state is not where Steinberg keeps it

**`--au-preset` reaches a timbre only in a plugin whose audio-unit build stores its state under `Processor State` and `Controller State`.** Those two keys are one vendor's convention, not a property of audio units. A plugin that keeps its state elsewhere accepts the dictionary and ignores what was put in it — so a `.vstpreset` produces an empty rack that renders silence, or worse, whatever the plugin loads by default. **Nothing reports a failure**, which is why `aubounce info <plugin>` lists the keys a plugin actually uses and why that is worth reading before authoring a capture.

A sampler that hosts third-party libraries typically keeps everything in one blob of its own. Where that is the case the route is a **saved class-info dictionary** — an `.aupreset` is exactly that — passed through the capture definition's `state` field rather than `preset`. `AuSource` digests it for the cache key on the same terms as a preset, since one edited in place is a different sound at the same path.

Getting that file needs a host, and the host that can save the plugin state is not always the one that can load the instrument. A DAW that can save a **`.vstpreset`** is enough, because its `Comp` chunk is the component state the plugin itself produced and that is byte-compatible with what the audio-unit build keeps under its own key — both are one `getState()` output under different wrappers. `vstpreset.py` does the substitution:

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/vstpreset.py \
    <rack>.vstpreset --plugin <type:subtype:manufacturer> --out <rack>.aupreset --settle-ms 30000
```

- **The conversion is verified rather than assumed**, which is why this is a tool and not a paragraph. It loads the result and reads the state size back: a plugin that ingested the blob re-serialises to a size unlike both its default and the input, while one that ignored it comes back at exactly the default, and the run fails saying so. A sampler hosting a third-party guitar library measured 4 195 bytes with an empty rack and 4 013 818 with a 4 013 825-byte `Comp` chunk in it, having reported that same default after being handed the `.vstpreset` directly.
- **`--key` names where the state goes**, defaulting to `vstdata`; `aubounce info <plugin> --values` says which key a given plugin publishes. Only that one key is written — an empty second key loads as an empty rack in a plugin that reads it, which is indistinguishable from a preset that took.
- **`--strings` is not evidence either way.** A plugin's own default state carries instrument names too, so a `.vstpreset` that was ignored still reports plausible ones, and they change between instances for reasons of the plugin's own. The state size is the only reading that separates the two.

**Confirm the slot map after any of this, by rendering.** Two timbres that come back byte-identical mean the channel is not selecting anything, and that failure has no other symptom: every render is the right length, at a normal level, with an instrument audible in it.

**A `state` path is expanded before it reaches the host.** It was not always, and the way that failed is the shape everything on this page has: `AuSource.identity` expands the path to digest the file while `argv` passed the `~` through, so the cache key named a real preset and the plugin was handed one that does not exist — and a plugin handed a missing state loads its empty default and renders it at a plausible length.

**A plugin shares the host's stdout, and some of them write to it.** One sampler here prints a slot-manager error above every render, which is harmless in itself and fatal to a whole-stream JSON parse. What that produced was not an error message but a wrong diagnosis: every probe came back peak 0.0000, so the settle sweep reported the library as never loading at any settle time it tried, and the recipe that fell out of it would have been "this plugin cannot be captured". `au_oracle.summary_json` scans stdout for the first JSON object carrying `peak` instead, so a plugin printing prose — or JSON of its own — is stepped over.

### Five ways a plugin produces a plausible file rather than an error

None of them announces itself. Three are detected:

| what happens | what you get | the guard |
|---|---|---|
| rendered faster than real time (`--au-no-realtime`) | the note starts correctly and goes silent in the middle | aubounce reports `dropout_ms`; a non-zero value is refused |
| not given long enough to load (`--au-settle-ms`) | the right length, the right shape, a peak three orders of magnitude too small | a peak below the floor is refused |
| leaking on load | energy before the first note | reported, not refused — a plugin is allowed a tail |

Two cannot be, because nothing in the file distinguishes them from a good render, so they are prevented instead:

| what happens | what you get | how it is avoided |
|---|---|---|
| the plugin's first note is not its steady one | a probe whose softest hit — the one a drum fit is validated on — is louder and thinner than the same velocity struck again | aubounce's `--warmup` strikes one note and discards it before recording. On by default; `--au-no-warmup` opts out, and a capture whose instrument outrings the preroll has to ([capture.md](capture.md#fields)) |
| the probe's program change is obeyed | the wrong instrument, at the right length, with a clean preroll and a healthy peak | the program change is stripped. `--au-gm` keeps it, for a plugin that really is a GM synth |

Measured on the piano sampler, and the numbers are why the defaults are what they are: rendered at full speed a 2 s C4 loses 1544 ms out of its middle; at `--au-settle-ms 1000` it renders a peak of 0.0011 where the real one is 0.158. Two seconds of settling is enough and the default is twice that. `capture.py calibrate` measures all of this for a given plugin rather than assuming it.

The program change is the one to know about before capturing a drum kit, because it is loud and it looks fine. A probe is written for a GM synth, where the program number selects the kit; a multitimbral rack reads it as an instruction to load a different program into that slot, and loads it asynchronously — so the first note of the probe beats the load and every later one does not. Measured on a sampled GM kit, one snare struck at 64 / 100 / 127 with the program change left in: peaks 0.610, 0.171, 0.365 and a probe that says the snare gets quieter as you hit it harder. Stripped: 0.610, 0.927, 0.898, with RMS ascending. No guard fires on either.

## Ambience: references that come with a room

Some instruments are never heard dry. A church organ is voiced *for* its building; a sampled or VST cathedral organ has that space baked into the samples and no switch turns it off. Comparing a dry model against such a reference makes every metric lie in the same direction, and the effect is not subtle: on a 2 s hall, the organ's release reads **1220 ms short**, and a fitter handed that loss will spend every envelope knob chasing the building.

So the room is treated as a nuisance parameter (`room.py`). When the oracle is an external WAV, the harness measures the reverberation from the tails between its notes and convolves the *model* with a response fitted to reproduce that same measurement, before any metric is taken. On the same 2 s hall the release delta goes to **0 ms**, and what is left in the loss is timbre. It is on by default and `--room none` switches it off.

What it measures, and what it refuses to:

- **Reverberation time** per octave band, from the −5…−25 dB slope of a Schroeder backward integration. Recovered to within ~25 % on a synthetic round trip.
- **High-frequency ratio** (4 kHz RT60 over 500 Hz). Every real absorber damps highs faster than lows, so a ratio near or above 1 is the *instrument's* ring, not a room — a dry reference organ measures 0.98 — and the measurement is discarded.
- Decays under 0.35 s are reported as no room at all: at that length an instrument's own release is indistinguishable from a small studio, and inventing a room is far more damaging than missing one.

Only an external WAV is measured. The fluidsynth route renders with its effect units off, so anything measured there is the instrument.

**Use `--pattern room-probe` when the reference is wet.** A T20 fit needs the tail to fall 25 dB, which takes `RT60 × 25/60` seconds of silence — 1.0 s for a 2.4 s hall, 1.7 s for a 4 s cathedral. The `sustain` pattern leaves one second after a two-second note, so anything past a small hall is measured on a decay that gets cut off before it has fallen far enough, and what little tail there is arrives mixed with the instrument's own ring. `room-probe` inverts both: 0.25 s notes with 4 s gaps, so the excitation stops long before the room does and the tail is the room. Nothing in it is an analysis note, so it is a probe to measure the space with, not to fit timbre on.

`estimate_room` records the shortest silence it actually had (`Room.tail_window_s`), and `autofit.py` prints a warning naming both numbers when the probe was too short for the decay it measured — the RT60 is then biased low, since what fits in the window is the steepest early part of the decay.

### Writing the room back into libsonare (`room-match`)

libsonare already gives each program a default ambience: the GS power-on CC91 of 40, weighted per program by `gm_fallback_sends` (a church organ is scaled 2.2×, a bass 0.4×). `room-match` measures a reference's space and searches libsonare's own controls for the settings that land closest:

```sh
voicematch.py room-match --programs 19 --oracle-wav /path/to/cathedral-organ.wav
```

```
oracle room: RT60 1.95s  tail level +13.3dB  HF ratio 0.72
closest match: CC91 10, reverb_decay 0.28 (gs_effects.kReverbDecayScale=0.4)
  reached RT60 2.07s  tail +15.0dB   residual 0.648
  -> gm_fallback_sends reverb_scale for program 19: 0.25
```

It searches rather than inverting a formula because the two controls are coupled and neither maps to RT60 linearly — measured on program 19, the tank decay gives 1.12 s at 0.28 and 6.45 s at 0.98, while the send moves the measured RT60 too because the instrument's own release sits inside the window. Around 26 renders, one per grid point.

The ambience weights are themselves fittable (`gm_fallback_map.kSendsChurchOrganRev` and friends), so a program whose reference is always a wet one can have its room fitted alongside its timbre.

`--model-sends R,C,D` controls the CC91/93/94 written into the probe for the **model** render — default `0,0,0`, fully dry; `gs` leaves libsonare's power-on ambience in place. It is a `compare` flag, not a `room-match` one.
