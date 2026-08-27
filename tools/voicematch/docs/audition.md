# Auditioning — listening to a voice, against a reference or alone

The metrics reduce a render to about a dozen scalars and `shape` compares two spectrograms cell by cell. Neither hears a chord bloom, a note struck again before it has stopped, a damper landing, or a hi-hat choked by the next one, because every measurement in this harness is taken on one isolated note. Those are the things a listening page is for.

```sh
rye run --pyproject bindings/python/pyproject.toml \
    python tools/voicematch/make_audition.py --program 40
python tools/audition/serve.py
```

## The index is the bank

What is auditioned is named as a **GM program, a variation bank or a kit**, and `bank.py` resolves everything else about it. A capture is an attachment to an entry, never the reason the entry exists: where one covers the voice its phrase set and its reference timbres are used, and where none does the page holds the model alone and plays rather than compares. Four captures exist and the bank has 128 programs, so most voices are in the second case — which is an ordinary set to `serve.py`, not a broken one.

That is the way round it has to be. Addressed by capture, a voice could be auditioned only where somebody owned a plugin and had captured it, which left the whole bank but four voices with no page at all — including every voice nobody has a reference for and every voice whose reference is not worth buying.

| you want | flag |
|---|---|
| one program | `--program 40` |
| several | `--programs 0-7,40,73` |
| the whole bank | `--programs all` |
| a program's variations | `--banks 0,8,16` (default: every bank the library voices apart) |
| a drum kit, on channel 10 | `--kits 0` |
| a capture, as the subject | `--config capture/harpsichord.json` |

Every run writes one subdirectory per voice under `--out` (default `.cache/voicematch/audition/`), named `p040-violin`, `p019b008-church-organ`, `kit000-standard-kit`, and merges `bank.json` by slug so a directory can be filled a few voices at a time. `--config` used to write flat, straight into `--out`, and that was a defect rather than a convenience: the manifest then landed on the path every other single-voice run also writes to, so auditioning a second instrument took the first one's page and left its takes on disk with nothing to name or group them. A caller who wants an exact path passes `--out` and gets the voice's slug under it.

**One voice is one page.** The question a page was built to settle belongs on the version buttons, not in the directory name — a baseline, a candidate and a re-fit are three versions of one voice, and giving each its own directory produced five piano "sets" that the picker showed side by side with nothing saying which was live. Candidates come from [`calibrations.json`](status.md#unadopted-settings) with `--calibrations`, which is what makes them survive the directory.

**A measurement run is not a page.** `--probe` writes under the probe root, which `serve.py` does not glob, and marks the manifest so a probe named explicitly on the command line is still skipped. A component-isolation set — a voice rendered with parts of it switched off — is a legitimate thing to produce and a nonsense thing to put on a picker beside the real pages of the same voice, where it reads as another candidate.

**A page of a wet reference renders the model the way it ships.** `write_smf` zeroes CC91/93/94 by default, which is what a dry-versus-dry metric needs and exactly wrong here: against a reference carrying its building it puts a bone-dry model beside a wet one, and the low end — where a registration question is decided — is then decided by the building rather than by the voice. So where the capture is not dry the sends are left at their GS power-on values, weighted per program by `gm_fallback_sends`, which is libsonare's own ambience and the only ambience a listener ever gets from it. A dry capture is compared dry and its page is unchanged. `profile.py room-match` is what says how close that ambience gets.

**`--model-sends` overrides that inference**, because the capture decides what a *comparison* needs and the question on a page is not always a comparison. `gs` leaves the sends alone whatever the reference does, `dry` always zeroes them, and `auto` is the rule above. The case it exists for: the GS tank is shared by every program, so a change made for one voice has to be checked on another — and the voice a listener knows best usually has a dry capture, whose page would render at CC91 0 and hold the one setting the question is about perfectly inert. That looks exactly like a page of subtly different versions.

A run also writes `bank.json` beside the pages: what each holds, which capture it found, which patch answered. Nothing reads it — `serve.py` discovers sets from the filesystem — so it exists to be diffed against last week's, which is the only way to see that a voice changed patch without listening to all of them.

## What a build can and cannot say

Which variation banks a program has, and which patch answers it, comes from the library itself through `catalogue.dump_catalogue`, and only a `-DBUILD_TUNING=ON` build answers. Without one the run says so and carries on: every program is listed at bank 0 and no patch is named. That is a reading the run did not get rather than a voice that is not there, so it is a note on stderr and not a failure.

Point `--lib` at a tuning build to get both the patch names and the `--variant` override layer.

## Phrase sets

A set is chosen for what it catches by ear that a metric will not report. Three are written for one instrument and name their notes; five are generic — one per `ToneClass` — and fill their notes in from `patterns.registers_for_program`, which is what lets a piccolo and a contrabass get their own compass out of the same phrases.

| set | for | what it is written to catch |
|---|---|---|
| `piano` | the grand | the pedal, sympathetic resonance, a restruck string |
| `harpsichord` | the harpsichord | the flat velocity response, the trill, the jack and damper between notes |
| `drums` | a kit | the mute group, a flam, the tom series, a cymbal past where every metric stops |
| `sustained` | bowed and blown | the slur, tonguing, the swell, leaps across the compass |
| `struck` | a hammered string | decay against register, a held third's beating, a restruck string |
| `plucked` | a plucked string | the strum's 25 ms spread, the re-pluck, the damped stroke |
| `modal` | bars, bells, membranes | the ring in full, two bars beating, the roll, the compass a transposed mode bank fails |
| `noise` | no pitch at all | one trigger heard whole, the velocity response, a retrigger |

**A capture's own set wins over the generic one**, and not only because it was chosen for the instrument: the reference archive is keyed by take id, so a captured voice given a different set would find none of its references and drop to model-only without a word.

### The musical take

Every take above isolates one thing, and a voice can hold all of them and still be unpleasant across ten seconds of a real line — how a chord bloom sits under a moving part, whether a decay leaves room for the next entry, where the register balance goes on the way up. So every page carries one passage of Bach, about ten seconds, judged by ear and by nothing else. `--no-music` leaves it off.

The note data is **committed** under `excerpts/`, so a clone renders it with nothing installed; `make excerpts` re-cuts it from a sibling corpus at `$SONARE_BACH_ROOT`. One passage per tone class, since every program needs one and four have a capture — a keyboard prelude for a struck string, a lute praeludium for a plucked one, a cello prelude for anything bowed or blown, a fugue for bars and bells. A capture may name its own with a `music` key, which is how the organ gets a chorale with a pedal part and the harpsichord a fugue rather than an arpeggiated prelude.

The passage moves by **whole octaves** to reach a program's compass, and only when it does not already fit: anything but an octave puts it in a different key, and centring every passage on the program's middle register transposes a keyboard prelude that Bach put where a keyboard is. A take that moved says so under its label, since a page read weeks later cannot otherwise tell a transposed passage from a voice whose register is wrong.

**It says nothing about velocity response.** The corpus carries one velocity per piece, so the `dynamics-*` takes remain the only thing that measures dynamics — a musical take does not retire them.

`--only take-id,take-id` narrows a page to the takes a question needs. Rendering `--programs all` in full is about 1500 renders and several gigabytes of WAV under the scratch root; a sweep across the bank is usually two or three takes wide.

## Calibration settings, per voice

"Is this constant better at 0 or at 4" is not a question the metrics can settle, and the answer has to be heard against the same phrase and the same reference. Two ways to put one on a page.

**Recorded, per voice — `calibrations.json`.** A voice keeps its own ordered list of named settings, each an override string and a line saying what it is for:

```json
"p073-flute": {
  "variants": [
    { "name": "breathier",
      "overrides": "concert_flute.flute.breath_noise=0.9",
      "note": "not enough air in the attack" }
  ]
}
```

The key is the voice's slug — the same string the directory is called, the picker shows and the address carries. A key matching no voice in the bank is a typo, and `test_calibration.py` fails on it; a run also names any recorded voice it did not resolve, before several hundred renders start.

The file is tracked, because an override string is knob names and numbers and no part of it names a commercial product. That is what makes a calibration question reopenable from a clone, which a `--variant` flag in somebody's shell history is not. `autofit.py --out result.json` writes a paste-ready override string, which is where most entries come from.

**`--calibrations`** is what reads it — off by default, since every setting is another render of every take and the override layer needs a `-DBUILD_TUNING=ON` library. `--calibrations FILE` reads a different one.

**Ad-hoc, for the run — `--variant NAME=OVERRIDES`.** Repeatable, and applied to every voice in the run. Recorded settings come first on the page and these follow. A name declared in both is refused rather than resolved: the page labels its version buttons with the name and nothing else, so whichever won, a note written about it would name the other just as well.

A setting with an empty override string is a legitimate thing to want — a second copy of the baseline, which is what blind mode needs a control for.

### What the page can and cannot show you

Each setting renders in its own interpreter, because the override table is read when the library loads and a second setting in the same process would silently get the first's values.

The run hashes every render — **the baseline among them** — and warns when no setting changed the render on any take. That is what a library built without the override layer produces, and it looks exactly like a page of subtly different versions. Comparing the variants against each other would miss it whenever a voice has only one recorded setting, which is the common case. A setting that overrides nothing is left out of the comparison rather than counted, since it is identical to the baseline in a working build as much as in a broken one.

A silent guard is worth as much as a loud one only if it can go either way: with the same page built on a tuning build and a key the voice actually consults, it says nothing, and the two renders differ by 0.92 full-scale.

## The reference side

Reference renders come from `.cache/voicematch/audition-references/` by default and fall through to the plugin only for a take the archive does not hold. A model render takes seconds and can always be made again; a reference render is a real-time pass through a commercial plugin and is the one part of a page that cannot be reproduced from this repository. Kept per page it was both the bulk of the disk and the reason nobody dared delete a page.

- `--reference-from DIR` — read them from here; empty string to always render
- `--archive-references DIR` — keep this run's for the next page
- `--model-only` — skip them even where one exists

The archive stores a take under a gain computed from its reference renders alone, so it does not move when a page's candidates get louder, and divides it back out on the way in. Only a take whose every reference came from the plugin in one run is written, so nothing in it has been through 16 bits twice.

## Levels, and what the page does with them

Every version of a take is written at **one shared gain**, so their level difference survives into the comparison — normalising each on its own would erase exactly what a register-balance or velocity-curve error looks like. The listening page has `match loudness` for when that difference is in the way, and says how much it applied.

The rest of the page — the transport, blind mode, the addresses, the export — is [`tools/audition/README.md`](../../audition/README.md).
