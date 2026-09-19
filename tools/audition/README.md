# audition — an A/B listening page for a directory of renders

```sh
python tools/audition/serve.py [<audition-dir> ...]
```

Opens a browser on a page that plays every version of a take from one transport, so a switch costs nothing — no second or two spent stopping one file and starting another, which is long enough for the ear to lose what it was holding.

A switch seeks back to the start, so the attack is the first thing heard. Turn `restart from the top when I switch` off and the versions stay sample-aligned instead: the switch becomes a gain change on a sound that never stopped, which is the only way to compare a sustain or a decay.

Standard library only, no build step, nothing specific to this repository. Point it at any directory of renders.

The page is in English and Japanese and opens in whichever the browser asks for; the toggle in the header moves it and is remembered. Only the page's own words move — a take's label, an engine's name and a source key are data and stay as they are.

## Which side of the comparison is sounding

**A source's `role` owns a colour, and it holds everywhere.** The library's own renders are one colour and the reference is the other; the block a version sits in, the button that selects it, the banner above the transport, every trace on the waveform and the ramp the spectrogram is painted with all take it from the same place. Flipping between the two sides recolours the page, so there is never a moment where a screenshot — or a memory of what was just heard — could be of either one.

The banner says it in words as well: the role, then the source's full label and detail. A key on a button is what the URL carries and what a report has to name, and it is four characters that describe nothing.

**One key swaps between the two sides.** `tab`, or the button beside the breadcrumbs. Stepping with the digits means knowing which digit is the reference on this take, and that number moves with every voice; the two sides do not. Whichever version of a role was last chosen is the one it comes back to, so flipping between a candidate setting and the reference does not drop you on the baseline each time.

Blind mode withholds all of this on purpose: the names, the path, the version in the address, the colours and the legend.

## Which slot, and where its reference came from

The line above the banner says what the page is *about*, as opposed to which render is sounding: the slot, and the reference's provenance.

```
SLOT       GM 82 · program 81 · bank 0 · patch lead_saw
REFERENCE  a sample library · <the product> · no room in it   [target: the module | not what this slot is aimed at]
```

Both numbers, because the printed map counts from one and every MIDI file counts from zero — a reference timbre labelled `[GM 082]` and a manifest saying program 81 are the same slot, and that off-by-one is the one thing about an address nobody should have to hold.

**A kit is one part holding forty-odd instruments, so its slot has a second level.** `kit 0` names the rhythm part and identifies nothing on its own — on a kit page the note number *is* the instrument, so the line also names what the open take strikes, from the same GM drum map the rest of the harness names drums from. A hi-hat take is three notes of that map and a fill is six, and each of them is versioned on its own as `dNNN`.

**The reference side carries what kind of source answered it, not only which product.** The capture definition classifies it — the module itself, a dedicated instrument, or a sample library — and `tools/voicematch/policy.json` says which of those the slot is aimed at. Where they disagree the line says so: forty slots name a sound the machine invented, whose only possible target is the machine, and a library's idea of one sounds exactly like a finished voice. A slot the policy holds to have no usable reference says that instead, with the name the machine gives the address, and is not confused with one nobody has captured yet.

Both facts are resolved when the page is served rather than written into a render, so a page rendered months ago tells you what the policy says today. The product name comes from the untracked half of the capture definition; a clone without it still gets the class. Blind mode drops the whole reference half.

## Saying what you heard

The panel under the pictures asks **two things at a time, in a listener's words**, and `not sure` is always the third answer and is recorded as one. It can be sent from at any point — a narrowing that stopped early is worth more than one that guessed.

The first question is whether anything bothers you at all. The second is the verdict, and it is deliberately separate from the diagnosis: *it is recognisably the instrument and I would still change it* is the state most of the bank is in, and with only "fine" and "wrong" to choose from it had to be filed as one or the other. A voice that barely sounds gets its own answer, because that is a missing mechanism rather than a mis-set constant and nothing else on the page can say so.

Nothing asks about an attack transient or a decay envelope. A form built on the names of the parameters underneath asks the listener to translate on the harness's behalf, and a mistranslation arrives as a confident, wrong, machine-readable tag.

A note carries what was sounding when it was written — the take, the version and its role, the playhead, which strike that is, and the options in force — so it never has to be reconstructed at the other end by counting strikes against a phrase set. `attach what is sounding right now` is what puts it there, and turning it off leaves the note about the voice rather than about the moment.

Notes land in `<scratch root>/feedback/<set>.jsonl`, one JSON line per note, appended — so two tabs open on two voices cannot lose each other's. The path is printed at startup and again under the panel. `undo the last one` removes the most recent line.

```json
{"at": "2026-09-20T05:30:11+00:00",
 "grade": "acceptable", "tag": "tone/dark",
 "answers": [{"q": "off", "tag": "off", "said": "気になるところがある"}],
 "text": "A線から上が痩せて聞こえる",
 "conditions": {"set": "p040-violin", "take": "single-long", "version": "model",
                "playhead": 3.42, "hit": {"n": 1, "notes": [{"note": 67, "velocity": 96}]}}}
```

`grade` is the verdict and `tag` is the finest point the narrowing reached; they are separate because `tone/dark` is the same string whether the voice is shippable or unrecognisable. The strings in `answers` are what the listener actually saw, in the language they saw it in, so a tag can be checked against the question it came from.

The scratch root is untracked, which is the same reasoning as the renders themselves: a note is taken against renders that cannot be redistributed, and a note whose subject no longer exists is worse than no note.

## Reading the notes back

```sh
python3 tools/audition/heard.py                 # every voice with notes, newest first
python3 tools/audition/heard.py p040-violin     # one voice, in full
python3 tools/audition/heard.py --grade broken  # only the verdict that means a mechanism is missing
python3 tools/audition/heard.py --json
```

A note is dated and the voice under it moves, so each is checked against when that voice's patch last changed in `tools/bank-versions.json`: one taken before that describes a render which is no longer in the tree, and is marked. **A mark does not say the change answered it** — nothing here knows that — it says listen again before acting. The headline verdict per voice is therefore the worst one still standing rather than the worst ever recorded, and a voice whose every note predates its last move reports that instead of a finding.

**A kit is dated per drum note rather than as a kit**, because that is how it is versioned: the log records which strike the note was taken on and which notes that strike held, so a verdict on the snare is attributed to `d038` and says nothing about the hi-hat in the same kit. A strike holding several notes — a flam, a fill — is dated by the newest of them, since one drum moving under it is enough to make the description stale.

A page built without a tuning build names no patch, so its notes cannot be dated at all; those are never marked, and unmarked means *nothing known* rather than *current*.

## Addressing one render

Every set, take and version has an address, and the page rewrites it as it is navigated:

```
http://127.0.0.1:8730/#piano-body/single-c4/E_strike
http://127.0.0.1:8730/?set=piano-body&take=single-c4&v=E_strike
```

Either form opens on exactly that render, and an address arriving at a page already open on the bank switches it to the listening surface — it names a render and is therefore a request to listen to it. The fragment is what the page writes back. `serve.py` prints the per-set form for each set it finds. `copy what I hear` puts the whole set of conditions, and a link that reproduces them, on the clipboard.

The version is left out of the address in blind mode, along with the path and the label: an address bar is visible, and hiding the name is the whole point of that mode.

## Choosing what to listen to

`V`, or the control in the header. A native select over a hundred and eighty entries shows one at a time and says nothing but the name, so choosing what to open next meant already knowing the answer. Every row here carries the three facts that decide whether a voice is worth opening, and they are rendered by the same functions the bank view uses rather than a second set that can drift from them:

- **how far the calibration has got** — the stage bar and its number, the same rung the bank sorts by
- **whether anybody has listened** — a count of the notes in that voice's log, and its colour is the worst verdict among them. **Blank means nobody has**, which is most of the bank, and is the point: a list whose every row claims a status says nothing, while the handful carrying a number is where a session starts
- **the two claims the top of the ladder is gated on** — `res` that the structural residual was diagnosed, `ear` that somebody listened and said it is the instrument. Green is current, amber unverified because a shared unit moved, red stale because the voice itself moved under it. **No voice in this bank carries `ear`**, so an empty column there is the bank's own answer rather than a missing readout.

Typing filters over the name, the program number, the engine and the patch at once — a voice is looked for by number at least as often as by name. `↓` drops from the box into the rows, `enter` opens, `esc` closes. Both columns are on the bank's rows too.

## The bank view

`listen` in the header is one of two views. `bank` is the other: every GM program and GS variation the library voices, whatever has been rendered — the engine answering it, how far its calibration has got, and whether a reference has been captured for it at all.

Three bands, because one list of a hundred and eighty-eight rows only ever answers the question you were already on:

- **The masthead is the overview.** The totals, and the distribution across the six stages as a proportional bar with a countable chip per step. Every figure and every chip is also a filter, so "which ones have something unadopted" is one click rather than a search.
- **The toolbar narrows** by synthesis method, by whether a reference exists, by whether anything is unwritten, and by name, engine or patch. Sorting by stage puts the worked-on voices at the top; the GM family headings belong to address order and come off under any other sort.
- **The table lists, beside an inspector.** A row carries what fits on a line. The inspector carries what does not: the coverage denominator and which dimensions are excused or missing, which dimensions sit outside the references' own spread and by how much, every unadopted setting, and the rung this voice is on with the one move that would raise it. It is always on screen — it holds the ladder and the method legend when nothing is selected — so selecting a row never resizes the table beside it.

Absence is kept quiet and presence is not: most voices have no reference, and a hundred and eighty-four copies of the same word in a warning colour would bury the four that say something.

It reads `tools/voice-status.json`, which is committed and generated by `tools/voicematch/status.py`; the page drops the view rather than erroring when there is none, since this server is also usable against any directory of renders outside this repository. What the stages mean is [`tools/voicematch/docs/status.md`](../voicematch/docs/status.md); the stage names are translated for display and the generated prose beside them is not.

**A fresh clone opens on it.** There are no renders to listen to and the bank still says which voices exist and which need a reference, neither of which requires anything to have been rendered. Otherwise the view is whichever one the last session was left on — except behind an address.

The listening surface keeps a line of the same information in its header: the engine, the stage, and anything unadopted for the voice being heard. A page otherwise says what a voice sounds like and nothing about why it is open.

## Several sets, one server

Name more than one directory and the page gets a control to move between them. Each is a separate instrument or a separate experiment — a piano set and a harpsichord set have different takes and different references — so they stay separate sets rather than one long list, and one server serves them all.

A directory with a `manifest.json` is a set whatever it contains; one without is a set only if it holds no other set. That distinction matters because the default output directory is the parent of every named one, so the take directories of the set written there sit beside the other sets and match the same glob.

Name none and they are discovered under the scratch root the rest of the harness renders into — `.cache/voicematch/` here, or wherever `SONARE_VOICEMATCH_ROOT` points. None of it is committed. **A fresh clone has nothing there, and that is a supported state:** the reference side of a comparison is captured from a commercial plugin and cannot be redistributed, so anyone can render the model side and nobody can render the reference side without owning the plugin. A set holding one version of each take is therefore expected rather than broken — the page drops the comparison controls for it, plays instead, and asks how the sound is rather than how it compares. With no renders at all the page opens and says where to put some. Nothing here requires a reference to exist.

## What it shows

- **All versions overlaid** on the waveform, each in its role's colour, the active one solid and the rest dimmed, so a level or envelope difference is visible before it is audible.
- **A log-frequency spectrogram** of the active version, which is where an inharmonicity or a decay-rate difference shows itself as a shape rather than as a number. Its ramp is built from the sounding version's own role colour.
- **Levels as captured.** Versions of a take are expected to be written at one shared gain so their level difference survives; `match the loudness of the versions` equalises them when that difference is in the way, and says how much gain it applied.

Everything that is set once and then left alone is behind `options`. What stays on the listening surface is what is being listened to, and what is being said about it.

## Keys

`?` shows them on either view.

| key | |
|---|---|
| `space` | play / pause |
| `tab` | swap between the two sides of the comparison |
| `1`…`9`, `←` `→` | pick a version |
| `↑` `↓` | previous / next take |
| `L` | loop |
| `M` | match loudness |
| `S` | restart on switch |
| `B` | blind |
| `R` | reveal this take, or reshuffle if already revealed |

On the bank the rows are what is navigated, since nothing is sounding there:

| key | |
|---|---|
| `↑` `↓`, `J` `K` | move down the rows |
| `home` `end` | first, last |
| `enter` | listen to this voice, where a page has been rendered |
| `/` | find; `esc` leaves the box |

Dragging across the waveform sets a loop region; a click with no drag seeks. The digits count in the order the versions are shown, which is by role, not the order the manifest happens to list them in.

**Blind mode** hides which version is which and shuffles them per take, so a preference is a preference rather than an expectation. Whichever version is selected when you move on is recorded as the pick, and the running tally is in the transport bar. Picks live in this browser's local storage, keyed by the set; re-rendering a set keeps them. `record this result` writes the tally and the per-take picks into the same log as every other note — a run is a result, not an impression, and it used to leave the page only through a download nobody remembered to make.

## The manifest

`manifest.json` in the served directory:

```json
{
  "title": "libsonare piano vs the sampled reference",
  "notes": "shown under the title",
  "sources": {
    "model":     { "label": "libsonare NativeSynth", "role": "model" },
    "grand-227": { "label": "227 cm concert grand, close", "role": "reference",
                   "detail": "the plugin it was captured from" }
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

`tracks` maps a source key to a path relative to the served directory; every key of every take gets its own switch button, labelled with the key. Takes with the same `group` are listed under one heading. Nothing but `id` and `tracks` is required.

`group` at the top level, beside `title`, is the heading the set picker files this set under. It is optional, and a set that declares none stays in an ungrouped run at the top of the list — which is right for a handful of hand-assembled directories and no use at all once a whole instrument bank is being served.

**`role` is what the colours, the swap key and the take list's markers are built on**, so a manifest that declares it gets all of them and one that does not gets a single unlabelled row of buttons in a neutral colour. `model` is shown first and `reference` second. A source's `label` and `detail` are shown for whichever version is selected, in full — so they can say as much as they need to without a segmented button ellipsising the part that distinguishes them.

With no `manifest.json`, one is inferred from the layout: each subdirectory is a take and the audio files inside it are its versions.

Any format the browser decodes will play. 16-bit PCM WAV is the safe choice — a 32-bit float `WAVE_FORMAT_EXTENSIBLE` file, which is what most offline render tools write by default, is decoded by some browsers and not others.

## The page's own files

`index.html`, `style.css`, and one ES module per job: `app.js` wires the two views and the keys, `i18n.js` holds every string the page shows in both languages and the question tree, `player.js` is the transport, `scope.js` draws the two pictures, `listen.js` is the listening surface, `bank.js` is the bank, `feedback.js` is the panel that takes a note, and `state.js` is what they all read.

`test_page.py` checks the agreements a server cannot: every element the script reaches for exists, every class it uses has a rule, every string it asks for is declared in both languages, and the question tree has no unreachable node or dead end. It reads every module rather than the entry point, since most of what builds an element is in the others.

## In this repository

`tools/voicematch/make_audition.py` writes a manifest of this shape: the same phrases rendered through libsonare and through a reference plugin, with one shared gain per take. What it renders is named as a GM program, a variation bank or a drum kit, and the library's own voice list is the index:

```sh
rye run --pyproject bindings/python/pyproject.toml python \
    tools/voicematch/make_audition.py --program 6
```

One subdirectory per voice is written under `.cache/voicematch/audition/`, which is the scratch root, which is what lets `serve.py` find them with no argument.

Most voices have no captured reference — there are four captures and a hundred and twenty-eight programs — and those pages hold the model alone and play rather than compare, which needs no plugin and works from a plain clone. `--model-only` forces that for a voice which does have one. The full set of flags is `tools/voicematch/docs/audition.md`.
