# Reference sources and capture workflow

The reference side supplies audio or committed measurements against which a model render is read. The command and capture context select the model's GM/GS address; the reference may come from a captured instrument, a SoundFont, an external render, or an AudioUnit.

## Sources

`autofit.py` accepts a captured corpus; `profile.py` reads capture definitions and committed profiles. Both `autofit.py` and `voicematch.py` accept `--sf2` for the configured fluidsynth SoundFont, `--oracle-wav` for a WAV rendered from an exported probe, or `--au` for an AudioUnit rendered through `aubounce`. `voicematch.py export-probe` writes the MIDI for an external renderer. An external WAV with a different sample rate is refused unless `--oracle-resample` is supplied; lead-in alignment can be disabled with `--oracle-no-align`.

A hardware module reached through `--oracle-wav` must be rendered dry (reverb, chorus, delay and the insertion effect come up non-zero from reset and nothing enforces it) with the tone map selected explicitly, or the model is matched to a room the instrument never had.

A stereo reference is summed to mono, and summing a spaced pair comb-filters it; `--mono-mode left` or `loudest` takes one channel, and the default stays `mean` because every committed profile was measured through it and changing the reduction would redefine them.

`--au-preset` reaches a timbre only in a plugin whose audio-unit build keeps its state under `Processor State` and `Controller State`; a plugin that keeps it elsewhere accepts the dictionary, ignores it and renders silence or its default with no failure, so read `aubounce info <plugin>` for the keys before authoring a capture.

For such a plugin, pass a saved class-info dictionary (an `.aupreset`) through the capture definition's `state` field, and convert a `.vstpreset` with `vstpreset.py`, whose `Comp` chunk is byte-compatible with the audio-unit state; the conversion is accepted only if the re-serialised state size differs from the plugin's default, and `--strings` is not evidence either way.

An `--au` render yields a plausible file rather than an error in several ways, each guarded:
- A render faster than real time (`--au-no-realtime`) drops out mid-note and is refused on a non-zero `dropout_ms`.
- Too short a settle (`--au-settle-ms`) renders a peak far too small and is refused below the peak floor.
- Energy before the first note is reported and not refused.
- The first note of a plugin is not its steady one, so aubounce's `--warmup` strikes and discards one note; `--au-no-warmup` opts out.
- The probe's program change is stripped, since a multitimbral rack loads a different program on it; `--au-gm` keeps it for a real GM synth.
- A `state` path is expanded before it reaches the host, and `au_oracle.summary_json` takes the first JSON object carrying `peak` from stdout because a plugin may print to it.
- Confirm the slot map by rendering: two timbres that come back byte-identical mean the channel selects nothing.

A captured corpus keeps fitting on the capture's measured grid. `capture.py` needs macOS, the target plugin, and `aubounce`. Profile-based comparisons read committed `reference/<id>.json`; corpus fitting and diagnosis read captured WAVs and use profile metadata where available. Reusing captured audio does not require the plugin. A SoundFont, WAV, or live plugin remains an oracle for a probe, not a tracked product identity.

`source_class` declares `module`, `dedicated`, or `library`; omission means unclassified. The category belongs in tracked capture metadata; the product name and preset belong in the local overlay. A module capture must not declare `room: present`; importing its recorded samples measures dryness instead of inferring a room from their instrument tails.

## Storage

| path | role | tracked |
|---|---|---|
| `capture/<id>.json` | capture method, grid, model address, measurement bounds, and source category | yes |
| `capture/<id>.local.json` | plugin component triple, product labels, presets, and key-switch identity | no |
| `reference/<id>.json` | measurements extracted from captured WAVs and the capture provenance | yes |
| `${SONARE_VOICEMATCH_ROOT}/capture/<id>/` | corpus WAVs and `manifest.json` | no |
| `${SONARE_VOICEMATCH_ROOT}/{audition,audition-references,feedback}/` | sibling directories for listening renders, reference cache, and feedback logs | no |
| `out/` and `out/au_cache/` | probe renders, reports, and temporary oracle renders | no |

Set `SONARE_VOICEMATCH_ROOT` to move capture or audition scratch data; without it the default root is the repository's untracked `.cache/voicematch/`. Captured WAVs stay untracked because they contain licensed product output. Keep them for corpus fitting, diagnosis, re-extraction, and listening; committed profiles support profile-based comparisons and gates.

`capture.py` folds `<id>.local.json` over `<id>.json` by timbre id when a re-capture needs product identity. Downstream tools read the capture/manifest or committed profile appropriate to their route; they do not infer product identity from a path or preset name.

## Capture definition

The definition is the single source for the instrument being measured: its GM program and optional GS bank, note and velocity grid, timbre ids, phrase set, dimensions, and capture conditions. Do not distribute those decisions between a config, a command line, and a source constant.

| field | meaning |
|---|---|
| `id`, `label`, `audition_title` | stable capture identity and display names |
| `program`, `bank` | GM program and optional GS variation answered by the model |
| `takes`, `dimensions` | phrase set and profile dimensions to compare or gate |
| `timbres[]` | reference timbre ids plus channel and product-independent routing |
| `notes`, `velocities` | the measured grid; velocity is a real axis for plucked and struck sources |
| `note_map` | captured note to model note correspondence for an instrument layout that differs from GM |
| `groups` | named percussion families used by kit relation terms |
| `settle_ms`, `realtime`, `warmup` | host preparation and first-note handling |
| `gate_ms`, `tail`, `tail_by_note`, `preroll_ms`, `onset_slack_ms` | per-grid recording window and onset alignment |
| `sample_rate`, `keyswitch_lead_ms` | sample rate and key-switch timing taken from the preroll |
| `dry`, `room`, `rig` | host effect request, recorded room classification, and recorded amplifier/cabinet/rotary classification |
| `source_class`, `params` | reference category and additional host parameters |

`channel` says what a note number means: channel 1 is pitched and channel 10 is percussion, where a note number selects an instrument. `slot_channel` addresses a timbre inside a multitimbral rack and must not be placed in `channel`; a rack can therefore use `channel: 10` with a different `slot_channel`.

A timbre may name a `keyswitch` in the local overlay and a `keyswitch_lead_ms` in the tracked definition. The switch is struck before the note inside the preroll, so the note onset stays on the shared corpus timeline. `key_map` or `key_offset` maps an instrument's playable compass; a mapped grid note with no reachable source key is rejected during config loading.

`dry` asks the host to disable every effect section the plugin advertises. It does not prove the recording has no room, and it does not classify an amplifier or cabinet. Use `room` and `rig` for those recorded properties.

## Capture workflow

Run capture commands from the repository root with the invocation shown in the README; `--help` defines the complete option set.

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/capture.py identify --config tools/voicematch/capture/<id>.json
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/capture.py calibrate --config tools/voicematch/capture/<id>.json
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/capture.py corpus --config tools/voicematch/capture/<id>.json
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/capture.py verify --config tools/voicematch/capture/<id>.json
```

`identify` is for multitimbral racks and reports slot contents without editing the definition. `calibrate` measures settle, real-time rendering, repeatability, onset, room, and level conditions. `corpus` renders one note per process over the declared note × velocity × timbre grid, appends each completed render to the manifest, and resumes from that manifest. `verify` re-reads the corpus and reports silence, clipping, onset, or velocity failures that can leave a plausible WAV on disk.

Turn the corpus into the committed profile and model comparison with the same capture definition:

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/profile.py measure --config tools/voicematch/capture/<id>.json
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/profile.py render-grid --config tools/voicematch/capture/<id>.json
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/profile.py compare --config tools/voicematch/capture/<id>.json --timbre <timbre>
```

`measure` writes `reference/<id>.json`; `render-grid` adds the model to the capture's own grid; `compare` reports dimensions, kit relations, coverage, and optional gate status. `agree` compares a second percussion reference; it is not wired for pitched captures. `dynamics` reads velocity swing, `takes` measures phrase interactions, `status` reports missing capture work, `rig` reports rig evidence without proving absence or writing a classification, and `room-match` searches the model's room send against a phrase reference.

To import recorded samples directly, run `rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/import_sf2.py tools/voicematch/capture/<id>.json`. The SoundFont path belongs in the local overlay. This route refuses synthesis generators, records only original-pitch cells, does not invent a velocity axis, and rejects a gate longer than the available recording. Declare `preroll_ms: 0`; the manifest retains extracted/missing reach. Module imports additionally check that the samples do not measure a room. This is a corpus import, separate from a live fluidsynth oracle.

## Probes

Single-note probes are generated by `voicematch.py` and by `autofit.py`; available pattern names are `drum`, `drum-holdout`, `drum-sequence`, `room-probe`, `scale`, `staccato`, `sustain`, and `velocity`. A captured corpus supplies its own notes, velocities, gate, tails, and timbre identity so fitting and profile comparison use the same stimulus.

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/voicematch.py export-probe --programs 40 --pattern sustain
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/voicematch.py compare --programs 40 --pattern sustain --sf2 tools/voicematch/assets/MuseScore_General.sf3
```

`--drum-note` moves a probe to channel 10 and uses the selected kit program; a drum probe uses percussion measurements rather than pitched harmonic and intonation terms. `--notes` and `--velocities` narrow a probe, but a fit's held-out axes must remain disjoint from the requested fit grid. The measured term definitions and coverage rules are in [Measurements](measurements.md).

The model and oracle are rendered from the same MIDI. `--oracle-wav` can replace fluidsynth, `--au` can render a live AudioUnit, and `--room auto` measures a wet oracle and places the model in a matching room before analysis; `--room none` compares the rendered signals without that correction. These controls answer how the probe was produced and do not change the model's GM/GS address.

Room estimation refuses what it cannot trust: a decay under 0.35 s is no room, a 4 kHz-over-500 Hz RT60 ratio near or above 1 is the instrument's ring and is discarded, and on a one-note-per-render grid a room measured on only a minority of the notes is refused because a room is common to every note.

Use `--pattern room-probe` when the reference is wet (0.25 s notes with 4 s gaps, so the tail is the room) and never to fit timbre; `autofit.py` warns when the silence was shorter than the decay it measured. `voicematch.py room-match` searches libsonare's send controls for the settings closest to a reference's measured room.

`toneclass.py` decides which notes a program is probed at, and the same classification picks its metric set and loss weights. `--drum-gate-ms` lengthens the 50 ms drum hit for the few notes (the record scratch and the whistles) whose length is the gate.

`LONG_DECAY_DRUM_NOTES` in `metrics_hit.py` gives the cymbals an eight-second probe gap, the capture's `tail_by_note` records them for eight seconds, and `HIT_LONG_MAX_SEC` is the analysis ceiling they are measured to; a shipped capture's `tail_by_note` naming a note outside its grid fails a test.

`drum-sequence` exists to fire the mute group (notes 42, 44 and 46 choke one another), which an isolated strike never does; it is read only as a whole-timeline comparison, never per hit.

## Note mapping

The model's note layout is canonical: it stays on the GM/GS layout that ships to users. `note_map` is applied to the oracle side only, mapping each captured note to the model note that answers it. The reference row, recorded window, tail, and family membership remain keyed by the captured note, so a map cannot silently calibrate the product to a source's non-GM layout.

`groups` are sets of captured percussion notes such as toms or hi-hats. They enable relation terms between family members; they do not impose an ordering and they do not replace `note_map`. A single percussion note cannot establish a kit relation, so a kit weight requires a grid that covers a declared family.

## Rig and room

`room` and `rig` are independent answers. `room` is `none`, `present`, or `unclassified`; `rig` is `none`, `baked`, or `unclassified`. Absent values default to unclassified; config loading rejects unrecognised values. Neither omission nor unreadable legacy metadata establishes a clean or direct recording.

`room: none` tells profile measurement and corpus comparison not to invent a room from a note tail. `room: present` or `unclassified` permits room estimation from the reference, and comparison convolves the model with the measured room before reading timbre. A wet reference therefore stays wet for comparison without turning reverberation into voice parameters.

`rig: none` means the recording stops at the instrument boundary, such as a direct electric-string signal, and is the fit target. `rig: baked` means an amplifier, cabinet, or rotary speaker is in the recording and is an acceptance target, never a fit target. A rig has no inverse correction. An unclassified rig refuses fitting only for programs that can carry a rig; other program families may fit while the capture remains unclassified.

Direct `--oracle-wav` and `--au` routes have no capture field to answer the rig question, so they warn on rig-capable programs and proceed as unclassified; they do not warn for families that cannot carry a rig. The built-in GM/SF2 oracle is known to include the rig for rig-capable programs and is refused by the fit on that route, because no capture metadata can turn it into a direct signal. A captured corpus uses its explicit `rig` value instead.

`--allow-rigged-oracle` overrides the fit refusal and writes a warning; values learned from an instrument-plus-rig reference describe that combined chain. `--diagnose`, `profile.py compare`, and auditions may read a rigged reference because they inspect or accept the shipped product boundary. `--grid` is not exempt because it evaluates the objective that a fit would search.

## Profiles

`reference/<id>.json` stores measured rows, validity and bandwidth, capture method/grid/model address, and room or rig provenance for profile comparisons and regression gates. Commercial product identity is excluded. Corpus fitting and diagnosis remeasure captured WAVs; the committed profile supplies bandwidth metadata to those runs.

`profile.py compare` reads the model over the same grid and reports residuals per dimension and per declared percussion family. `--gate` checks a previously recorded bound, while `--write-gate` records the current summary with its requested margin; numerical quality and bank acceptance remain separate decisions in [Acceptance](acceptance.md).

`profile.py agree` reports which dimensions two independent percussion references support. Use that evidence to classify a dimension or leave it unavailable; a missing reference cell is not a zero-error model cell. The fit's freshly measured coverage and term eligibility follow [Measurements](measurements.md); corpus requirements are in [Fitting](fitting.md).
