# Capture and reference profiles — `capture.py`, `profile.py`

The oracle routes in [oracles.md](oracles.md) compare one probe at a time. A corpus is the other half: the whole note-by-velocity grid of a real instrument, measured once, so a voice is fitted to properties rather than to three notes.

**Capturing needs macOS, the commercial plugin being captured, and `aubounce`, and a fresh clone has none of the three.** That is how the committed references in `reference/` were produced and it is not how they are consumed: `profile.py compare`, `autofit.py` and `--diagnose` all read `reference/<id>.json`, which is committed, so a voice can be scored, fitted and diagnosed against a captured instrument with no plugin present. What needs the plugin is re-capturing — measuring an instrument nobody has measured yet, or re-measuring one after a plugin update.

Without it, the reference comes from somewhere else and everything downstream is unchanged: `--sf2` plays a SoundFont through fluidsynth on any platform, and `--oracle-wav` takes a WAV of the probe rendered by anything at all. `export-probe` writes the score to render. The audition page has the same shape: `make_audition.py --model-only` needs no plugin and produces a set that plays rather than compares.

## Where the audio lives

Five places, and which one a file belongs in follows from whether it can be committed and whether it can be regenerated.

| | what is there | committed |
|---|---|---|
| `capture/<id>.json` | the capture **method**: the grid, the gate, the timbre ids, the GM program the model answers it with | **yes** |
| `capture/<id>.local.json` | the capture **identity**: the plugin's component triple and each timbre's preset | no — gitignored |
| `reference/<id>.json` | the measurements taken from that capture — the actual calibration target | **yes** |
| `.cache/voicematch/capture/<id>/` | the captured WAVs, one per (timbre, note, velocity), plus their manifest | no, and cannot be |
| `.cache/voicematch/audition/<name>/` | the listening sets rendered from a capture | no |

**The identity is split out on purpose.** `capture.py` reads `<id>.json` and folds `<id>.local.json` over it (`merge_overlay`, matching timbres by `id`), so `plugin` and `timbres[].preset` reach the loader without being named in a tracked file. Everything a fit, a comparison or a diagnosis needs is on the tracked side; only re-capturing needs the overlay. A definition's own `_identity` key says which sibling holds it.

**The captured audio cannot enter the repository**: a sample library's licence covers what is rendered from it. It also should not, even where a licence would allow it — what a physical model has to reproduce is a handful of measured properties, not four hundred megabytes of one particular instrument. `reference/<id>.json` is those properties, and it is what a fit and a gate are held against.

`SONARE_VOICEMATCH_ROOT` moves that whole scratch root off the checkout, which is worth doing: a capture is expensive — several seconds per note, in real time, with retries — so one worth re-measuring is kept rather than re-rendered, and `profile.py --corpus <path>` reads a corpus from wherever it was kept.

Renders that are cheap to reproduce stay in `out/` and `out/au_cache/`, both gitignored: probe WAVs, comparison output, and the plugin render cache keyed on everything that defines a sound.

## Shipped capture definitions

One `capture/<id>.json` per instrument, and `reference/<id>.json` beside it for each that has been measured; `make voice-status` is the roll-up of which bank voice each answers and how far it has got. They are not listed here — a hand-written index of a directory drifts from it silently, and this one already had, still naming four long after the directory had outgrown them.

What is worth reading across them is that the gate is an argument rather than a default, and every definition makes it in its own `_stimulus` note. The grand's is long with a short tail, because on a piano note-off is the damper and the free decay a model has to match happens while the key is still down. The harpsichord's is shorter, because its jack drops the moment the key is released and what follows is a fast damping plus the jack's own noise. On a synth rack the same question splits the rack: the slots that stop at note-off record half a second of tail and the ones that release record two.

## Writing a capture definition for a new plugin

Every field is discoverable, and none of them should be guessed. `aubounce` answers the first four questions and `capture.py calibrate` answers the fifth.

```sh
cargo install --git https://github.com/libraz/aubounce   # once

# 1. What is installed, and its component triple.
aubounce list

# 2. What is inside it. A rack keeps its contents in a chunk only it can parse;
#    --strings decompresses what is compressed and reports what is readable, so
#    it says what a rack contains without knowing the format. Unlabelled: it
#    does not say which slot holds what.
aubounce info "<rack plugin triple>" --settle-ms 4000 --strings harpsi

# 3. Which MIDI channel plays which slot. This is the one that cannot be
#    guessed and the one a multitimbral rack never publishes: --probe-slots
#    raises one solo control at a time and plays the channels that sound.
#    --probe-channels is the fallback when a plugin has no solo per slot.
aubounce info "<rack plugin triple>" --settle-ms 4000 --probe-slots

# 4. What it advertises as an effect section, which is what `dry` switches off.
aubounce info "<rack plugin triple>" --params reverb

# 5. What the host has to give it. Measured, and reported rather than asserted.
capture.py calibrate --config capture/<id>.json
```

### How a timbre is addressed, which is decided before anything is rendered

A capture's timbres are the instruments being measured, and a plugin offers them in one of three shapes. Which one it is decides what `--probe-slots` is for and what the overlay has to carry.

- **A preset per timbre.** The plugin loads one instrument per preset file, so each timbre names its own `preset` and they all play on channel 1. Nothing has to be arranged in advance.
- **One rack, one channel per slot.** The plugin cannot be told from outside which instrument to load — its preset is the whole rack rather than a slot — so the instrument set is arranged inside the plugin first: one instrument per slot, each slot on its own MIDI channel, saved once as a single preset. Every timbre then names that same file and differs only by `slot_channel`.
- **One instrument, selected from the keyboard.** Neither a preset nor a channel reaches the variants: they are one instrument's own articulations, switched by a key outside its playing range. The timbre names that key as `keyswitch` and the capture says how long before the note it is struck as `keyswitch_lead_ms`. Nothing outside the plugin can enumerate these — the switch map is the library's documentation and the render is the only confirmation — so identify each one by rendering it before writing the timbre list.

**In the second shape `slot_channel` is the only thing selecting a slot.** `source_for` threads it through to the host's `--channel` and nothing else reaches the rack, so a timbre list that leaves it at its default captures slot 1 once per timbre. The failure has no symptom worth noticing — every render is the right length, at a normal level, with an instrument audible in it — and the only sign is that two timbres come back byte-identical, so `shasum` two of them after the first note of a new rack rather than listening for a difference.

**`slot_channel` is an address; `channel` is a meaning. Never write one in the other's field.** `channel` says what a timbre's note numbers stand for, which is what `profile.is_percussion` reads to choose the metric set for the whole capture, and MIDI gives it two answers: 10 for a note that selects an instrument, 1 for a note that selects a pitch. `load_config` refuses anything else, because a third value is an address that has reached the wrong field — which is how five melodic instruments that happened to sit in slot 10 came to be measured as drum maps, each note reporting a band tilt and a crest and none reporting a fundamental. A rack that keeps a kit somewhere other than slot 10 needs both fields and they disagree: `channel: 10`, `slot_channel: 15`.

**A key switch comes OUT of the preroll, not on top of it.** The switch has to precede the note, and every consumer downstream expects the note at exactly `preroll_ms` — the onset guard below, the corpus assembly that drops the head, every metric that reads a window from it. So `au_preroll_ms` is `preroll_ms` less the lead, and the host is asked only for the remainder: the note lands where it always did whether a timbre is switched or not, which is what lets a switched timbre share one corpus with an unswitched one. The capture's `preroll_ms` therefore has to be raised to cover the lead, and raising it means re-rendering every timbre onto the one timeline. That is cheap to justify and worth proving: re-take a few cells of the existing grid at the new preroll first and check they agree sample for sample with the old ones once each has its head removed. Agreement says the corpus is regenerable; disagreement says the plugin is not deterministic and the whole grid is about to move for a reason that has nothing to do with the switch.

**A switch is supposed to select rather than sound, and a library that voices one is invisible to every check but this.** It lands where the late-onset guard expects silence rather than where it expects a note, so that guard cannot fire, and it is loud enough and on the right series to pass the peak and tone-share tests — leaving the same transient under every measurement of that timbre and of no other, which reads as the variant. `_render_note` refuses a switched render whose onset arrives more than `ONSET_SLACK_MS` *early*, and refuses rather than retries, since an audible key is deterministic. Choosing switch keys outside the instrument's playing range is what makes this pass; confirm the range rather than assuming the library's map.

**What a switch does with a request the instrument cannot answer is a measurement, not a guess.** A fretted instrument's forced-string switch is the case here: no single string spans a three-octave grid, so most of a grid is out of range for most of the switches. Measured across the whole grid, the sampled guitar captured here neither fails nor renders silence — it clamps to the nearest string that can reach the note, and an unswitched note is always played on the highest that can. That is what makes a two-timbre capture possible from one product: forcing the lowest switch gives the lowest fingering of every note and forcing nothing gives the highest, so the pair brackets the choice completely instead of sampling it, and where only one string reaches a note the two come back byte-identical — a spread of zero where the instrument has one fingering. **Establish the clamp rule by rendering the grid against every switch and comparing digests**, not by reasoning from the string's open pitch: the two answers agree here and there is nothing in the file that would have said so.

**Which slot holds what is measured, not read.** A rack does not publish its channel map, `--probe-slots` reports only which channels are placed, and a slot's name is not a description of it. Render a diagnostic note per placed channel and identify each from what comes back — speech time, tone-to-noise, where the energy sits, the partial stack — then record that reasoning in a `_`-prefixed key beside the slot list. Both multi-slot captures here were identified that way, and one of them turned out to hold five electric-organ slots among its pipe ranks.

**A slot can hold a sample that stops before the key does, and every cheap check calls it healthy.** The render begins exactly on the preroll, tracks the key it was sent, and peaks at a normal level — then goes to digital silence while the key is still down. With a three-second gate the sustain window every metric reads falls entirely on silence, so a capture written for such a slot measures nothing and says so nowhere. Peak, onset and harmonic share all pass; the only thing that shows it is the envelope over the whole render, so **trace one before choosing a gate for a slot nobody has rendered long-form**. Fourteen slots across the GM racks here are like this — one reed, every solo and ensemble bowed string, all three voices, the tuba and the muted trumpet — while their neighbours in the same rack hold for the whole gate, so it is a property of the slot rather than of the host or the score.

Shortening the gate does make them measurable: the sustain window is 30 to 90 per cent of the note capped at 0.6 to 1.8 s, and `render-grid` gives the model the capture's own `gate_ms`, so both sides stay comparable. It is still usually the wrong move. On anything whose model is a continuously excited one — a bowed string, a blown pipe — a gate short enough to fit the sample measures the attack transient and never the steady state the model exists to produce, which is a target the fit will follow as far as it goes. Prefer a source with a sustaining articulation, and record the slot's measured length as the reason rather than capturing it anyway.

### Fields

Tracked `capture/<id>.json`:

| field | what it is | default |
|---|---|---|
| `id` | names `.cache/voicematch/capture/<id>/` and `reference/<id>.json` | — |
| `label` / `audition_title` | how the capture and its audition page are titled | — |
| `program` | the GM program the model answers this reference with | `0` |
| `bank` | the GS variation bank, where the reference is one (the pipe organ) | — |
| `takes` | the audition phrase set ([audition.md](audition.md#phrase-sets)); a capture's own always wins over the generic one | `""` |
| `dimensions` | which `compare` columns the gate holds; empty means all of them | `[]` |
| `timbres[]` | `id`, `label`, and `slot_channel` — the slot, from `--probe-slots`. `channel` is the other quantity and is written only where a note number selects an instrument. `key_offset` is below; `keyswitch` is identity and belongs in the overlay | — / `channel: 1` |
| `notes` / `velocities` | the grid. Velocity is an axis in its own right on anything plucked | — |
| `note_map` | oracle-side note correspondence, where the kit is not laid out as GM ([probes.md](probes.md#a-sampled-kit-need-not-lay-its-instruments-out-the-way-gm-does)) | — |
| `groups` | which of the notes are one instrument — the tom series, the hi-hat trio — as `name: [notes]`, for the kit-relation term ([loss.md](loss.md#percussion-terms)) | `{}` |
| `settle_ms` / `realtime` | from `calibrate`, at twice the measured minimum | `4000` / `true` |
| `warmup` | strike a note and throw it away before recording. Turn it off for an instrument that rings longer than the preroll — see below | `true` |
| `gate_ms` / `tail` | how long the key is held and how long is recorded after it lifts | `8000` / `"2s"` |
| `tail_by_note` | a longer tail for the notes that need one — the cymbals | `{}` |
| `preroll_ms` / `sample_rate` | render lead-in, and the rate everything is measured at | `100` / `48000` |
| `keyswitch_lead_ms` | how long before each note a switched timbre's key is struck. Comes out of `preroll_ms`, so it has to be smaller than it — see above | `0` |
| `onset_slack_ms` | how far past the preroll a render may begin and still be the note. Widen it only for a voice that swells, and only against a measurement of its own grid — see below | `10` |
| `dry` | switch off every effect section the plugin advertises | `true` |
| `room` | whether the recording carries a space — `none` or `present`, and absent means unclassified. `none` is what stops both `measure` and a fit recording one they cannot distinguish from a long release — see below | `"unclassified"` |
| `rig` | whether an amplifier, a cabinet or a rotary speaker stands between the instrument and the microphone in this reference — `none` or `baked`, and absent means unclassified | `"unclassified"` |
| `params` | anything else, as `Name=value` | `[]` |

Untracked `capture/<id>.local.json`, folded over the above:

| field | what it is |
|---|---|
| `plugin` | the triple from `aubounce list` |
| `label` | the product name, if the tracked label is generic |
| `timbres[]` | `id` (matching the tracked one) plus `preset` and a product-specific `label`, and `keyswitch` where the variants are switched from the keyboard — which key selects which articulation is the library's own map and names it as surely as a preset path does |

**A rack's slot names are not a description of what the slot does.** On the harpsichord rack the slot called `Digi` decays over 36 seconds and the one called `Ambient` decays in 3. That is why that capture sets `dry: false` and measures each timbre's room instead: the switch cannot be trusted where the effects are per-slot sends rather than a section the plugin advertises. Measure before believing a name, and record which slots came out dry — a slot's own room is what a later `compare` corrects the model with.

**`dry` cannot answer the room question either, and neither can the measurement.** `dry` is an instruction to the host — switch off whatever the plugin advertises — so a rack that advertises nothing is written `dry: false` while being completely dry. What `room` answers is what the recording contains, and over the seconds after note-off a release and a small hall are the same falling signal. Every guard around `estimate_room` separates a room from *one note's* decay, and a release is not one note's: it is one envelope generator, so it gives the whole compass the same decay exactly as a room does and the per-note agreement test passes on it. The high-frequency plausibility gate does not catch it either — it refuses a ring that damps its highs no faster than its lows, and a release through a lowpass damps them faster, which is the signature it is looking for.

Measured on one GM rack, which is its own control: the eight slots that stop at note-off recorded no space at all, while six of the eight that release recorded one each, of 0.64 to 2.22 s, correlating 0.87 with their own release times. Six sources in one room cannot report six rooms. Left in the profile those entries are convolved onto every model render before any figure is taken, which lands hardest on `damper` — the dimension a released voice is most worth judging on. So the capture answers, and `none` is the answer that stops the estimate; absent still measures, which is what every capture written before the field did.

**The answer has to reach both readers, and for a while it reached only one.** `profile.py measure` folds `room: none` into its wetness and records no space; `corpus._dryness` read `dry` alone, and `autofit` gated its live estimate on that — so a capture that answered "no space here" still had one measured from its own note tails and convolved onto the model before every figure. `corpus.Corpus` now carries `room` beside `dry` and `rig`, all three read the same two-step from the manifest or the config it names, and only `none` skips: `unclassified` is nobody having answered and still measures.

**`dry` and `rig` are different questions, and dryness cannot answer the second.** Dryness is looked for as a tail and a cabinet has none — it is a filter rather than a space — so a close-mic'd amplified guitar reads dry with its whole rig inside it. `rig: "none"` is a reference captured at the instrument's own boundary, which is what a fit is for; `"baked"` is one recorded through an amplifier, which is an acceptance target and never a fit target, because a rig is nonlinear and offers no inverse to correct with the way a room does ([voicing.md](../../../src/midi/synth/docs/voicing.md)).

**Absent means unclassified, never "no rig."** Every definition written before the field simply never answered, and no later reader can tell the two apart from the audio. So `autofit.py` refuses a fit against anything but `none` on a family that could carry a rig — the electric guitars and basses, the electric keyboards, the rotary organs — and `--allow-rigged-oracle` is the way past it. `profile.py compare`, the auditions and `--diagnose` are unaffected: they read the reference rather than moving the voice towards it.

**No comparison dimension survives a baked rig, so there is no subset to exempt.** The refusal above looks blunt enough to invite carving out the dimensions an amplifier "cannot reach" — a filter and a compressor should leave the sub-fundamental and the decay alone. They do not. Measured on one voice against itself, binding the rig moves `body` by 41 dB (−76.8 to −35.6: a nonlinearity rectifies, and rectification is difference tones below the note) and the h2–h6 stack by 11 dB, and on a program whose rig runs hotter it stretches the decay by 1.4 dB/s. Every number a rigged `compare` prints belongs to the chain. What it is still good for is a residual too large for any rig to be responsible for and a shape no rig has — but that is an argument made per finding, not a list of safe dimensions.

**`profile.py rig` is how the answer is measured, and what it can decide is narrower than it looks.** Every signature it reads is something a rig *adds*, so it can show one present and cannot show one absent: a dull instrument recorded flat and a bright one recorded through a cabinet both come back dull. `none` therefore needs an A/B the product can supply — the fingered bass got its answer by switching the plugin's amplifier stage off and measuring what that removed. A rack that publishes no such switch cannot produce a `none` for any of its slots however clean the spectrum looks, and the honest outcome there is that the answer stays unclassified. That asymmetry is the right one: marking a rigged reference `none` is the failure the field exists to prevent and it is silent, while leaving a direct one unclassified costs only a fit that could have run.

Two signatures, one per rig kind, and each has a null that is not an answer:

- **A cabinet is a steep skirt with a knee**, and the slope is what a merely dark source cannot imitate — a speaker in a box falls at 30–50 dB per octave above 4–5 kHz where a pickup or a mic'd body falls at 6–20. Stronger still with siblings, which is what `--against` is for: a cabinet is *one filter after everything*, so instruments through it share a high-frequency shape while instruments each dull for their own reason do not. On the GM rack the overdriven, distortion and harmonics slots agree within 2.0–3.8 dB RMS above 2 kHz and differ from every unamplified sibling by 10–24, all three falling at 38–41 dB/oct from a knee at 4 kHz. Three instruments do not arrive at one shape that closely by coincidence.
- **A rotary is anti-correlated across the mics** — the horn turns away from one as it faces the other. In-phase modulation at a few dB is a tonewheel beating against itself, which every drawbar organ does and no rig is responsible for. Check the channel separation first: a dual-mono render correlates at +1 by construction, and reading that as "no rotor" is a negative nothing earned.
- **The null that means nothing:** a rig's signature lives where the source has energy. A drawbar organ generates nine harmonics and nothing above them, so a cabinet after it has no band to shape and leaves no skirt to find. That null says the instrument is unmeasurable this way, not that it is clean.

**A sampler's key range and its sounding range are different things, and `key_offset` is where they part company.** One electric bass here answers on keys 71–109 and sounds four octaves below the key it is sent; its playable zone begins at the key that sounds a five-string's low B. So the grid stays written in the pitch that sounds — the model's own — and `timbres[].key_offset` is added on the way to the plugin. Establish the offset with a harmonic comb against a range of octaves rather than by reading the strongest low partial: on that instrument the strongest partial below 400 Hz is the third harmonic at two of three probe keys, which reports the note a fifth or an octave-and-a-fifth too high. Sweep the whole 0–127 before calling a zone missing, for the same reason — a probe over the range a bass *sounds* in measures digital zero and reads as an unplayable instrument.

**`warmup` prevents one failure and can cause another, so it is per capture.** The discarded strike is meant to land beside the recorded note; on an instrument that rings longer than the preroll it lands underneath it. On the electric bass above, the preroll reads −41.2 dB against a −26.8 dB onset with the warmup on and digital zero with it off, and on the softest velocity row the contamination sits 7 dB under the note it would be measured from — through the whole gate, since a bass decays slower than it is contaminated. What the warmup was guarding against there was 1.9 dB of onset level, indistinguishable from the instrument's own round robin. Turn it off when the tail is longer than the preroll; keep it on otherwise.

**A `groups` entry is a set, never a sequence.** The kit-relation term compares the sorted contrasts inside a family, so the order the notes are written in is not read and a kit whose layout disagrees with GM's needs no map to be grouped correctly. Which relations a family holds is measured from the reference's own rows rather than declared beside it, so a family whose members this capture cannot tell apart quietly scores nothing — which is why the drum capture declares thirteen families and leaves out the whistle pair, whose lengths differ by 0.31 doublings and are the 50 ms gate rather than the instrument. A test refuses a family that names an uncaptured note, and one that holds no relation at all in its own reference.

**A `_`-prefixed key is a note to the next reader and is ignored by the loader.** Every shipped definition uses them to record why a value is what it is — which slots were left out and why, what the velocity axis is there to settle, why the gate is the length it is. That is the right place for it: the reasoning belongs next to the number, not in a commit message.

## `capture.py`

| command | what it does |
|---|---|
| `calibrate` | measures the host settings this plugin needs (`--note`, `--velocity` pick the probe) |
| `corpus` | renders the note × velocity × timbre grid, one note per process, resumable (`--no-resume`, `--limit`) |
| `verify` | re-reads the corpus and reports what is wrong with it |

All three take `--config`, `--out` (default `.cache/voicematch/capture/<id>`) and `--verbose`.

`capture.py`'s own finding is worth keeping: the piano sampler fails to load its samples on roughly one render in three, and *more* settling time does not help — 4 s and 16 s both produced the real peak while 8 s in between produced silence. So the capture retries, and decides "too quiet" from the loudest velocity already recorded for that same note rather than from an absolute floor, since a real note at the top of the keyboard at velocity 24 is quiet enough that any fixed threshold either lets a failure through or rejects real notes.

### A render can come back loud, plausible, and not the note

The quiet retry above catches a render whose samples did not arrive. The drum kit turned out to have the opposite failure, which every level test passes.

Re-rendering 120 slots of that grid and correlating each against its original, 39 came back a **completely different signal** — correlation 0.000, 4 to 25 dB *louder* than the real note, beginning 150 to 843 ms into the file after a stretch of digital silence, where every correct render begins at exactly the 100 ms preroll. Whole instruments were affected: all six velocities of the closed hi-hat and all six of the mute triangle, whose peaks then read −7.99, −8.03, −8.04, −0.57, −0.79, −0.32 dBFS against a clean ramp that ascends smoothly. Every slot that was *not* one of these reproduced bit for bit, so the plugin is deterministic and the corrupt renders are not a round robin.

Nothing could have caught it from the level, because the failure is loud, and nothing did: it sat in a committed reference profile and in every number derived from it. What separates the two populations perfectly is **when** the audio starts. So `_render_note` now reads the written WAV back, finds the first sample over an absolute `ONSET_FLOOR_DBFS` (−80 dBFS, far under anything an instrument radiates and far over a preroll that is exactly zero), and retries a render that begins more than `ONSET_SLACK_MS` past the preroll. The measured onset is recorded per slot as `onset_ms`, so a manifest can be audited without re-rendering anything.

**A voice that swells does defeat it, and a capture that measures the defeat may widen the guard with `onset_slack_ms`.** The threshold is absolute, so what it finds is the note-on only where the preroll is exact zeros: on a sampled choir whose preroll carries the library's own bed at −97 to −105 dBFS, beneath the −80 dBFS line, the crossing sits partway up the swell instead and moves with velocity. Measured across that grid it rises monotonically from 102–105 ms at velocity 127 to 107–123 ms at velocity 32, at every one of seven notes, while the peak descends smoothly 4 to 6 dB a step — so the refusals land on one end of the velocity axis rather than on particular notes. The default stays at the width the drum grid measured; a capture that widens it records what it measured, and the manifest carries the width the run used so `verify` re-checks each render against the guard that admitted it.

Widening is the declaration, not the workaround. A grid left with cells missing and the reason in a prose note is invisible to everything mechanical downstream: the eight cells this refused were enough for `autofit --diagnose` to refuse the voice entirely, on a `corpus_pattern` check that only counts.

**The general shape is worth carrying to the next capture.** A guard written against one failure mode says nothing about its opposite, and a corpus is exactly where that goes unnoticed: 282 files nobody listens to, reduced to a committed table of numbers that all look like numbers. `calibrate` renders one note twice and reports agreement, which is the right check and cannot see a failure that strikes a seventh of the grid.

## `profile.py`

| command | what it does |
|---|---|
| `measure` | corpus WAVs → `reference/<id>.json`, the committed calibration target |
| `render-grid` | renders the model over the capture's own grid, as one more timbre of the corpus (`--timbre`, default `model`) |
| `compare` | diffs the model's grid against the reference, per dimension (`--gate`, `--write-gate`, `--margin`) |
| `agree` | measures a second, independent reference over the same grid and reports which dimensions the two agree on |
| `dynamics` | tabulates the pp→ff swing rather than one velocity at a time |

All take `--config`, `--corpus`, `--profile` and `--program`; all but `measure` and `render-grid` take `--timbre` and `--notes`.

`render-grid` is what puts the model into the same corpus as one more timbre, so nothing downstream needs a special case for it.

On a kit, `compare` follows its table with one line per family and relation — the reference's spread against the model's, in doublings, sorted by what is costing most. It exists for the same reason `dynamics` does, on the other axis: `compare` scores one instrument at a time, so a kit whose every piece is individually plausible can still have its tom series half as wide as the reference's. Printed rather than gated, because the gate is a median across all 282 hits and folding a six-tom finding into that loses it; `--w-kit` is where the same measurement drives a search ([loss.md](loss.md#percussion-terms)).

`dynamics` exists because `compare` scores one velocity at a time, so a model whose every velocity is individually plausible can still have the wrong **dynamics** — the axis a pianist actually plays. It tabulates the swing rather than the absolute value: level in dB, and the two partial bands relative to the fundamental, so a swing that is merely "louder" is separated from one that is "brighter". A physical model gets this out of its excitation solver rather than out of a curve, so a mismatch here is structural and `--diagnose` is where to take it.

### `measure` reads the timbres the capture declares, not what the corpus holds

A corpus outlives the definition that filled it. `corpus --resume` keeps a timbre it does not recognise rather than throwing away renders that cost hours in real time, which is what lets `render-grid` put the model's own grid in the same directory — and also means an instrument **re-captured from a different product still has the old one on disk under the old timbre id**. Measuring both is silent in the worst direction: `committed_capture` intersects the manifest with the tracked definition, so the profile would declare one timbre and carry the statistics of two. The retired reference is normally retired for being wrong, so a bass replaced for having its rig baked in would have gone on contributing to the DI profile that replaced it. `measure` therefore skips any render whose timbre the definition does not name, which covers the model's rows by the same rule.

### What `profile.py measure` measures that `metrics.py` does not

Six things, none of which the harmonic-series assumption can express. All six are measured for every instrument, and what varies between instruments is which of them is the headline rather than which are taken:

- **Inharmonicity** — a stiff string puts partial *n* at `n·f0·√(1+Bn²)`. Fitted, not assumed, by iterating the search window down as the estimate improves. On the captured C7, B is smallest around C2 (7.6e-5) and rises toward both ends — the shape a real piano has. The harpsichord capture reads 1.5e-6 to 5.7e-6, three orders of magnitude lower, which is the measurement saying its strings are essentially harmonic. Above roughly C6 there are too few partials left under the Nyquist frequency to fit two parameters, and those notes are reported as unfitted rather than as a number; the summary curve names the notes it stopped at, because a curve that quietly stops short reads afterwards as one that covered the keyboard.
- **Stretch tuning** — cents from equal temperament. The grand capture reads the Railsback curve, −14.5 c at A0 rising to −2.7 c at C4; the harpsichord reads inside ±1.6 c everywhere, which is the same measurement reporting a plain equal temperament. A model tuned to exact equal temperament beats against a reference that is not, and the beating is the first thing anyone hears.
- **Double decay** — the knee where the fast initial fall gives way to the aftersound, found by searching the breakpoint rather than assuming one. Where it falls is set by how fast the strings of a unison drift apart, so a model with one string cannot have it.
- **Damper release** — note-off as a damper landing on a moving string. A row that never fell 40 dB inside the capture's tail is shown and left out of the median, since the number recorded for it is the length of the window.
- **Tone-to-noise** — the mechanism against the string. A model with the partial stack right and no action noise reads cleaner than any recording, and every spectral metric scores that as an improvement. The partial window is the wider of ±2 % and three FFT bins, because a relative-only window collapses below one bin in the bass and reports a clean low note as a noise burst.
- **Velocity response** — the level range from the softest blow to the hardest, and whether it is monotonic. On a plucked instrument this is most of the identity: the harpsichord capture reads 5.9–6.6 dB on three of its four slots against a piano's thirty, and a model given the wrong one of those is wrong by 20 dB in a way no timbre metric reports, since every one of them normalises each note by its own fundamental.

### The room is measured per timbre and the model is placed in it

A dry model scored against a reference carrying its building reads short in decay, fast in damper release and thin under the note, and all three are the room rather than the voice. `measure` records the space each timbre was recorded in under `rooms` in the reference profile, and `compare` convolves every model render into a matching one before measuring — the same correction the fitter has always applied on the same corpus (`autofit --room auto`), so the two halves judge one signal. The room lives in the committed profile rather than being measured at comparison time because `compare` reads that file and never the audio, and a corpus is not committed.

**A room is common to every note and an instrument's own decay is not**, so the estimate is taken per note and kept only where most of the compass agrees there is one. Measured over the whole grid at once instead, a single note carries the answer: the electric guitar's DI reports a credible 1.17 s space that way, and note by note it is 3.56 s on one note of ten and nothing on the other nine. The `room.py` guards do not catch that one — the length is believable and the notes are held for seconds — so the agreement is the guard, and the entry records `notes_measured` / `notes_probed` and the RT60 range beside the space itself.

Most references have no room to correct for and the estimator says so: of the eighty captured timbres, four measure one and three survive the agreement test — both church-organ registrations (RT60 4.3 s and 4.1 s) and the bright drawbar registration (1.4 s). `dry: false` is not evidence of a room; it means only that the plugin had no effect section to switch off.

## Holding the comparison to what it measured (`--gate`)

`profile.py compare` ends with a per-dimension summary, and it reports **two** numbers per dimension rather than one:

```
                                                  median  |median|  rows
  tuning vs the reference (cents)                    -0.15      0.91    12
  partial stack h2-h6 vs h1 (dB)                     +4.04     13.58    12
  brightness (% of the reference centroid)           -3.50     37.27    12
```

The second column exists because the first cannot fail on a defect that is symmetric across the keyboard. The brightness row once read +0.16 % while individual notes were between 26 and 660 % out — the bass dark by as much as the treble was bright — and the median of the signed errors said the voice was correct to a sixth of a percent.

```sh
profile.py compare --notes 36,60,84 --write-gate reference/piano_gate.json
profile.py compare --notes 36,60,84 --gate reference/piano_gate.json   # exit 1 on a regression
```

Both statistics are bounded, and the gate exits 1 when either is exceeded. What it exists to catch is not a bad voice but **a change that improves one dimension by breaking another**, which is the shape most of this work takes: widening the prompt-decay profile fixes the level on every note between F#2 and F#5 and brightens the same notes by 35 to 60 points of centroid. Both are real, and whoever makes that trade should decide it rather than discover it in a later listening test. Bounds are written at `--margin` (1.25× by default) with a floor under each, deliberately loose — a gate that fails on measurement noise gets switched off, and a switched-off gate catches nothing. Re-record only in the same change as the behaviour that justifies the new numbers.

**A bound is only about the rows that reached it, and the `rows` column is which.** Two censors drop a row rather than compare it — a damper that never fell 40 dB inside the tail, and a decay whose two sides stayed audible over spans too different to difference — and both fire hardest exactly where the error is largest. The surviving rows are then averaged, so a dimension can sit inside its bound while most of the keyboard contributed nothing to the number. The electric guitar's `decay`, `aftersound` and `doubling` bounds are each recorded from **18 of that capture's 50 rows**. So a gate records `rows` beside each bound and `check_gate` fails when the evidence has halved **in either direction** — evidence returning moves the median onto notes the bound was never set from, which is as much a different measurement as evidence leaving. A gate written before that field says so and holds nothing on this axis until it is re-recorded; the run's own thin dimensions are named regardless. `vel_range` and `register` are exempt from the report because their row *is* a note rather than a grid cell, so a lower count is their shape.

**One of the dimensions has to be able to see gain.** Every other column is normalised — a band profile against its own loudest band, a crest against its own RMS, a decay against its own peak — which is what makes them measure timbre and also what makes all of them blind to output level. Rewriting eighteen of the kit's levels moved not one of them by a digit. `level` is the per-hit `peak_dbfs` against the reference's, and `vel_range` is not a substitute: it is a span, and a span cancels an offset by construction.

### The gated dimensions, and what a bound may not go under

A pitched `compare` reduces to seven dimensions: `stretch` (tuning), `decay`, `attack`, `stereo`, `balance` (the h2–h6 stack against h1), `centroid_pct` (brightness) and `vel_range`. `tnr` joins them on any profile measured since the tone-to-noise column existed. Two of them answer questions nothing else in this harness can, because everything else is computed on a mono mix of a sustain window:

- **`attack`** is the time the envelope first comes within `ONSET_SLACK_DB` of its loudest point. On a drum that is the strike; on a struck string the hammer is over milliseconds before the board reaches full level, so the number is the bloom after it — which is what separates a note that sinks in from a thump. It is not the time to the peak itself, because a note that holds a level has no peak worth the name: a clarinet's envelope crests 0.2 dB over its own mean, so its loudest window falls wherever the vibrato did — 1270 ms into a 3 s note, against 25 ms for the arrival. That index also starts every decay fit, so reading it off the maximum put the decay's origin wherever noise chose and fitted the rate over whatever followed.
- **`stereo`** is `1 - |channel correlation|` over the held note. A voice that returns one signal to both legs scores exactly `0.0` and is mono however wide the reverb around it is. Nothing else measured here can see that, and a listening test finds it immediately.

Where the reference holds several instruments of one kind, the floor under each bound is measured rather than chosen: the median disagreement **between** them is the tightest any model can be held to without failing on which one it was compared against. On the three concert grands that is 0.15–0.27 of width and 12.5–40 ms of attack.

### `make voice-gate`

Runs every gate that exists — one per `reference/<id>_gate.json` — against its capture, reading each gate's own timbre back out of it, and reports every voice outside its bounds rather than stopping at the first. It builds its own `build-autofit`, so it disturbs neither the Debug tree `ctest` reads nor the shared `build-python-shared`.

It is not in CI and not in `ci-local`. It renders a full grid per instrument, and re-recording a bound is a judgement about a trade that a CI job cannot make. **A gate is invalidated by its reference as well as by the voice**: re-measuring `reference/<id>.json` moves the numbers the bounds were recorded from, so a gate re-recorded before that re-measure is comparing against an instrument that no longer exists in the file.

## A second reference, and where the two agree (`agree`)

```sh
profile.py agree --config capture/drums.json          # the capture vs fluidsynth
```

Every bound in a `_gate.json` was decided by hand from one reference, and one reference cannot say which of its numbers are the instrument and which are its own voicing. That is not a documentation gap: working from a single reference, a tambourine was on its way to being pushed 31 dB down — inaudible — because that is where the one reference put it.

`agree` measures a second, independent reference over the same grid (fluidsynth over the bundled SoundFont, already wired for the fit's oracle) and reports, per dimension, how many hits the two agree on within a stated tolerance. It writes nothing. It is not a better reference and is never a target; the only question asked of it is where it and the capture say the same thing. **A dimension the two agree on has a target in it and is worth gating. One they do not is a voicing decision — measure it, print it, and do not fit to it**, because the number a single reference gives there is that library's opinion and a fit handed it will follow it as far as it goes.

## Listening to the result

```sh
make_audition.py --config capture/harpsichord.json  # render the phrases
python tools/audition/serve.py                      # serve it, and everything else
```

`--model-only` skips the reference renders and auditions the model against itself; `--timbres` and `--only` narrow what the config named. A capture is one way in — the whole of it, from the program to the timbres, in one file — and not the only one: [audition.md](audition.md) is the page for auditioning a voice the bank has and no capture covers.

The set a capture names belongs to an instrument rather than to the tool, and each covers what its own instrument is hard to get right and no metric here reports.

- **`drums`** — the mute group (an open hat choked by a closed one and by the pedal), sixteenths landing on their own ring, a flam and a roll, a tom fill in the capture's *measured* pitch order, ten seconds of cymbal wash, and a bar of eights. Every one of those lives in the relation between hits, which a probe striking one note every two seconds cannot produce — not because the search failed but because the question was never put.
- **`sustained`** — what happens *between* notes on anything bowed or blown: a slurred scale, repeated notes each articulated, leaps across the compass, and a swell under CC11. The whole harness measures isolated held notes, which is the least characteristic thing a wind or a bowed string does. The register comes from the capture's own program, so the set is a shape rather than one instrument's notes.

`make_audition.py` renders the same phrases through libsonare and through each captured timbre, at one shared gain per take so the level difference between them survives, and writes the manifest [`tools/audition`](../../audition/README.md) reads. The phrases are chosen for what a metric will not report: a chord that exposes inharmonicity as beating, an arpeggio, a note struck again while it is still ringing, and a passage under the sustain pedal — couplings between strings rather than properties of one, which the per-note analysis in `metrics.py` never sees.

**A phrase selects a rack's slot with the channel it is written on, and the corpus does not.** The two paths reach the plugin differently: the corpus has aubounce play the notes, so `--channel` carries the slot, while a phrase is a MIDI file, and a file supplies its own channels — aubounce refuses `--channel` alongside `--midi` rather than dropping it. So the audition writes one score per timbre, on that timbre's `slot_channel`, and only the model keeps the take's own (which is what makes a drum note a drum). The failure this prevents is silent by construction: every render has the right length, a normal level and an instrument audible in it, and the only sign is that two slots of one rack come back byte-identical. Two of the four shipped captures have timbres off channel 1 — the harpsichord's `baroque`, `concert-8-4` and `room`, and both of the organ's choruses — so a page whose registrations sound alike is worth a `shasum` before it is worth an explanation.

**A page's references are worth keeping.** `--archive-references DIR` writes every reference render the run produced, and `--reference-from DIR` builds the next page out of it, so a phrase set that has been captured once needs the plugin only when the phrases themselves change. The renders are deterministic on a sampler that passes `calibrate`'s repeat check, so an archived take and a fresh one are byte-identical and the archive can be trusted as the reference rather than as a copy of it.

**A phrase that asks for silence cannot be captured.** `sustained`'s swell takes CC11 to 0, and a sampler that implements expression as a gain renders true silence there; `au_oracle` reads that as the dropout it guards against — a disk-streaming sampler starving mid-note — retries, and skips the take. The guard cannot tell the two apart, and on an organ the phrase is arguably wrong anyway: a closed swell box attenuates a chorus by 15 to 20 dB rather than muting it. Either the take declares its silence or its floor comes up; until one of them happens the swell is a model-only take.
