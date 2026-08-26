"""Command line: score, fit, ablate and probe a voice against a captured corpus.

Every subcommand takes the same target description -- a capture definition and
the corpus rendered from it -- and nothing else is instrument-specific. The GM
program, the note grid, the velocities and the gate come from the capture, so
pointing this at a different instrument is a matter of naming its capture.

    python -m shape score   --capture piano --corpus <dir>
    python -m shape fit     --capture piano --corpus <dir> --knobs dump.txt
    python -m shape ablate  --capture piano --corpus <dir> --overrides set.txt
    python -m shape probe   --capture piano --corpus <dir> --notes 36,60,84
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import replace
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from corpus import load_corpus  # noqa: E402

from . import admittance, probes, purity, struck, takes  # noqa: E402
from .bed import Bed  # noqa: E402
from profile import PERCUSSION_CHANNEL, is_percussion  # noqa: E402

from .loss import ShapeLoss  # noqa: E402
from .partials import Track  # noqa: E402
from .render import Signals, load_knob_dump, read_overrides, write_overrides  # noqa: E402
from .search import (  # noqa: E402
    Descent, ablate, prune, split_notes, split_velocities, summarise,
)
from .spectro import Spectro  # noqa: E402

CAPTURE_DIR = Path(__file__).resolve().parents[1] / "capture"


def build(args):
    """A loss, its signal source, and the note grid, all from the capture."""
    cap = json.loads((CAPTURE_DIR / f"{args.capture}.json").read_text())
    corpus = load_corpus(args.corpus, args.timbre)
    gate_s = float(cap["gate_ms"]) / 1000.0
    preroll = float(cap.get("preroll_ms", 0)) / 1000.0
    # One plane size for the whole run, so this is the grid's LONGEST slot rather
    # than a per-note window: a spectrogram bed is cached and compared cell by
    # cell, and two notes cannot share a plane at two sizes. On a grid with a
    # per-note tail that pads the short notes with silence, which costs render
    # time; the other direction would cut the long ones short.
    seconds = round(corpus.slot_s + preroll, 3)
    spectro = Spectro(sample_rate=corpus.sample_rate, seconds=seconds)
    # Which kind of instrument this is decides the channel the model renders on
    # and which terms the comparison can even ask. Read off the capture through
    # the harness's own predicate rather than from a flag here, so one answer
    # serves `profile.py` and this package alike.
    percussion = is_percussion(cap)
    sigs = Signals(corpus_root=Path(args.corpus), program=int(cap["program"]),
                   channel=PERCUSSION_CHANNEL - 1 if percussion else 0,
                   timbre=corpus.timbre,
                   gate_s=gate_s, seconds=seconds, lib_path=args.lib)
    notes = tuple(int(x) for x in args.notes.split(",")) if args.notes else corpus.notes
    # Every velocity but the softest. Fitted at one dynamic, a contact model has
    # no reason to get the others right and every knob that grades brightness or
    # contact time with velocity is unconstrained -- free to take whatever value
    # suits the single layer it was shown. The softest layer is left out because
    # at that dynamic most of the plane is the recorded floor, and the
    # comparison would mostly be mask.
    vels = tuple(int(x) for x in args.velocities.split(",")) if args.velocities \
        else tuple(sorted(corpus.velocities)[1:]) or corpus.velocities

    bed = None
    if not args.no_bed:
        cache = Path(args.cache) / f"bed-{args.capture}-{corpus.timbre}.npz"
        if cache.exists():
            bed = Bed.load(cache, spectro.scales)
        else:
            # Measured at the softest layer the capture holds. The floor scales
            # with the layer while its shape does not, and it is the shape that
            # is frozen -- so the quietest renders are the ones where the least
            # instrument is mixed into it. Measured at the loudest layer instead
            # the notes genuinely radiate up in the anchor band and the estimator
            # correctly refuses itself: 1.3 dB of across-note spread at the
            # bottom of this corpus's velocity range against 3.7 at the top.
            probe_pairs = [(n, min(corpus.velocities)) for n in corpus.notes]
            bed = Bed.measure(spectro, sigs(probe_pairs, ref=True))
            cache.parent.mkdir(parents=True, exist_ok=True)
            bed.save(cache)
        if not bed.usable:
            print(f"bed refused: across-note spread {bed.agreement_db:.1f} dB is too "
                  f"wide to be one source, so the reference has no recorded floor "
                  f"to subtract", file=sys.stderr)
            bed = None
    loss = ShapeLoss(signals=sigs, spectro=spectro, bed=bed,
                     note_off_s=preroll + gate_s, velocities=vels,
                     pitched=not percussion)
    return cap, corpus, sigs, loss, notes


def cmd_score(args):
    _cap, _corpus, _sigs, loss, notes = build(args)
    ov = read_overrides(Path(args.overrides).read_text()) if args.overrides else {}
    text = ",".join(f"{k}={v!r}" for k, v in sorted(ov.items()))
    base = loss.score("", notes=notes)
    print(f"shipped   {base}")
    if text:
        print(f"overrides {loss.score(text, notes=notes)}")


def holdout(loss, notes):
    """The fit and hold-out comparisons, split on the axis this capture has.

    A keyboard splits the notes and one loss serves both. A kit cannot: each
    piece is its own patch, so a held-out note is one no move could reach and
    the hold-out sits frozen at its starting value -- after which `prune`, which
    keeps a move only if it paid on the hold-out, drops every move the descent
    made. Splitting the velocity layers instead asks the same patch a dynamic
    the fit was not shown, which is what a hold-out is for.
    """
    if loss.pitched:
        fit_notes, hold_notes = split_notes(notes)
        return fit_notes, hold_notes, None
    fit_v, hold_v = split_velocities(loss.velocities)
    loss.velocities = fit_v
    hold = replace(loss, velocities=hold_v)
    return notes, notes, hold


def _report_prune(args, loss, base, moves, fit_notes, hold_notes, hold_loss):
    """Price every move, run the threshold ladder, and print both."""
    kept, report = prune(loss, base, moves, fit_notes, hold_notes,
                         workers=args.workers, hold_loss=hold_loss)
    print("\n" + summarise(report["contributions"], base, moves))
    print(f"\nbefore  fit {report['before']['fit']:.3f}  hold {report['before']['hold']:.3f}")
    for a in report["attempts"]:
        mark = "*" if a["keep_db"] == report["keep_db"] else " "
        thresh = "all" if a["keep_db"] is None else f"{a['keep_db']:g}"
        print(f"prune{mark} keep>{thresh:<5} fit {a['fit']:.3f}  hold {a['hold']:.3f}"
              f"   ({a['kept']} of {len(moves)} moves)")
    out = write_overrides({**base, **kept}, base)
    print("\n" + out)
    if args.out:
        Path(args.out).write_text(out + "\n")


def cmd_fit(args):
    _cap, _corpus, _sigs, loss, notes = build(args)
    base = load_knob_dump(args.knobs, tuple(args.namespaces.split(","))
                          if args.namespaces else ())
    deny = set(Path(args.deny).read_text().split()) if args.deny else set()
    fit_notes, hold_notes, hold_loss = holdout(loss, notes)
    d = Descent(loss=loss, base=base, fit_notes=fit_notes, hold_notes=hold_notes,
                deny=deny, workers=args.workers, passes=args.passes,
                hold_loss=hold_loss)
    start = read_overrides(Path(args.start).read_text()) if args.start else None
    moves = d.run(start)
    _report_prune(args, loss, base, moves, fit_notes, hold_notes, hold_loss)


def cmd_prune(args):
    """Re-select from a saved override set without repeating the descent.

    The descent is the expensive half and its result is a file, so a selection
    rule that changes -- or a set assembled by hand from several runs -- can be
    re-priced in minutes rather than in the hour the search cost.
    """
    _cap, _corpus, _sigs, loss, notes = build(args)
    base = load_knob_dump(args.knobs, tuple(args.namespaces.split(","))
                          if args.namespaces else ())
    moves = read_overrides(Path(args.overrides).read_text())
    fit_notes, hold_notes, hold_loss = holdout(loss, notes)
    _report_prune(args, loss, base, moves, fit_notes, hold_notes, hold_loss)


def cmd_ablate(args):
    _cap, _corpus, _sigs, loss, notes = build(args)
    base = load_knob_dump(args.knobs, tuple(args.namespaces.split(","))
                          if args.namespaces else ())
    moves = read_overrides(Path(args.overrides).read_text())
    fit_notes, hold_notes, hold_loss = holdout(loss, notes)
    scores, (f0, h0) = ablate(loss, base, moves, fit_notes, hold_notes, args.workers,
                              hold_loss=hold_loss)
    print(f"fitted set  fit {f0:.3f}  hold {h0:.3f}\n")
    print(summarise(scores, base, moves))


def cmd_struck(args):
    """Per piece and per band: how many things ring, and where the top went.

    The two readings a struck piece is judged on, printed side by side with the
    reference instead of folded into the score, because the decisions they drive
    are per band. A count well under the reference's says the piece is a small
    resonator bank where the instrument is a field, which is a mechanism
    finding; a positive prompt/late says the band belongs to the strike and is
    gone from the aftersound, which is the reading that separates a hard piece
    from one that measured every band level right and sounded like a drum.

    Both windows come from each side's OWN decay, so the model is not read over
    a window the reference's length chose. Comparing a count taken over a live
    aftersound with one taken over a piece that has already stopped is the
    error this is arranged to avoid.
    """
    _cap, _corpus, sigs, loss, notes = build(args)
    ov = ""
    if args.overrides:
        ov = ",".join(f"{k}={v!r}" for k, v in sorted(
            read_overrides(Path(args.overrides).read_text()).items()))
    vel = loss.velocities[len(loss.velocities) // 2]
    pairs = [(n, vel) for n in notes]
    ref, mod = sigs(pairs, ref=True), sigs(pairs, ov=ov)
    sr = loss.spectro.sample_rate
    bands = struck.STRUCK_BANDS

    print(f"velocity {vel}. modes = resonances counted in a {struck.DENSITY_SPAN_S:.2f} s "
          "slice of the aftersound;")
    print("prompt = each band's share of the strike minus its share of the "
          "aftersound, in dB.")
    print("A band the recording could not answer is left blank rather than "
          "counted as zero.")
    for n in notes:
        print(f"\nnote {n}" + "".join(f"{f'{lo}-{hi}':>13}" for lo, hi in bands))
        for label, sig in (("reference", ref[(n, vel)]), ("model", mod[(n, vel)])):
            body, late = struck.windows(sig, sr)
            counts, ok = struck.mode_count(sig, late, sr)
            prompt, alive = struck.prompt_late(sig, body, late, sr)
            print(f"{label:>10}" + "".join(
                f"{c:>7.0f}/{p:<5.0f}" if o and a else f"{'-':>13}"
                for c, o, p, a in zip(counts, ok, prompt, alive)))


def cmd_probe(args):
    _cap, _corpus, sigs, loss, notes = build(args)
    vel = loss.velocities[len(loss.velocities) // 2]
    pairs = [(n, vel) for n in notes]
    ref = sigs(pairs, ref=True)
    sets = [("shipped", "")]
    if args.overrides:
        sets.append(("overrides", ",".join(
            f"{k}={v!r}" for k, v in sorted(
                read_overrides(Path(args.overrides).read_text()).items()))))
    mods = {lab: sigs(pairs, ov=ov) for lab, ov in sets}

    print(f"velocity {vel}. tail = late residue in {probes.METAL_BAND[0]:.0f}-"
          f"{probes.METAL_BAND[1]:.0f} Hz relative to the note's early level;")
    print("colour = partials 3-10 minus 1-2 at 1.5 s; rate = mean decay in dB/s.")
    print(f"\n{'note':>6}{'':>12}{'tail':>9}{'colour':>9}{'rate':>9}")
    pooled = {lab: [] for lab, _ in [("reference", "")] + sets}
    for n in notes:
        track = Track(ref[(n, vel)], n, loss.spectro.sample_rate)
        for lab, sig in [("reference", ref[(n, vel)])] + \
                [(lab, mods[lab][(n, vel)]) for lab, _ in sets]:
            prof = probes.decay_profile(track, sig)
            pooled[lab].append(prof)
            t = probes.tail_residue(loss.spectro, sig, n,
                                    bed=loss.bed if lab == "reference" else None)
            c = probes.sustain_colour(track, sig)
            r = np.mean([x for _, x in prof]) if prof else float("nan")
            print(f"{n if lab == 'reference' else '':>6}{lab:>12}"
                  f"{t:>9.1f}{c if c is not None else float('nan'):>9.1f}{r:>9.1f}")

    print("\ndecay rate by band, pooled. The slope across these bands is what the")
    print("ear calls metallic when it is too flat.")
    bands = list(probes.DECAY_BINS)
    print(f"{'':>12}" + "".join(f"{f'{lo}-{hi}':>13}" for lo, hi in bands))
    for lab in pooled:
        cells = probes.decay_bins(pooled[lab])
        print(f"{lab:>12}" + "".join(
            f"{cells[b][0]:>9.1f}/{cells[b][1]:<3d}" if cells[b][0] is not None
            else f"{'-':>13}" for b in bands))


def cmd_purity(args):
    """How much of each render is the played string, per note and per window.

    Kept per note rather than pooled: the pooled figure for this measure changed
    sign depending on whether the ratio or the powers were averaged, which is
    what notes disagreeing looks like, and the disagreement was the finding.
    """
    _cap, _corpus, sigs, loss, notes = build(args)
    vel = loss.velocities[len(loss.velocities) // 2]
    pairs = [(n, vel) for n in notes]
    ref = sigs(pairs, ref=True)
    sets = [("shipped", "")]
    if args.overrides:
        sets.append(("overrides", ",".join(
            f"{k}={v!r}" for k, v in sorted(
                read_overrides(Path(args.overrides).read_text()).items()))))

    print(f"velocity {vel}. Harmonic over non-harmonic energy, dB. The number has")
    print("no absolute zero -- read each column against the reference above it,")
    print("never one note against another.\n")
    rp = purity.profile(loss.spectro, ref, notes, vel)
    floor_win = (loss.note_off_s + 0.9, loss.spectro.seconds)
    share = {n: purity.floor_share(loss.spectro, ref[(n, vel)], n,
                                   (3.5, 6.5), floor_win) for n in notes}
    print(f"{'':<14}" + "".join(f"{n:>7}" for n in notes))
    for w, _, _ in purity.WINDOWS:
        print(f"{'ref ' + w:<14}" + "".join(f"{rp[w].get(n, float('nan')):>7.1f}"
                                            for n in notes))
    print(f"{'floor share %':<14}" + "".join(f"{share[n]:>7.0f}" for n in notes)
          + f"   (over {purity.FLOOR_SHARE_LIMIT:.0f} means the tail column is")
    print(f"{'':<14}" + " " * (7 * len(notes)) + "    the recording, not the note)")
    for lab, ov in sets:
        mp = purity.profile(loss.spectro, sigs(pairs, ov=ov), notes, vel)
        print()
        for w, _, _ in purity.WINDOWS:
            d = {n: mp[w][n] - rp[w][n] for n in notes if n in mp[w] and n in rp[w]}
            print(f"{lab + ' ' + w:<14}" + "".join(
                f"{d.get(n, float('nan')):>+7.1f}" for n in notes))


def cmd_admittance(args):
    """The prompt decay against the aftersound, pooled by absolute frequency.

    Read the reference's spread column first. The table is only a curve if the
    notes agree on it, and where they do not the right conclusion is that the
    prompt rate is a per-note quantity here rather than a termination the model
    could be designed against.
    """
    _cap, _corpus, sigs, loss, notes = build(args)
    pairs = [(n, v) for n in notes for v in loss.velocities]
    ov = ",".join(f"{k}={v!r}" for k, v in sorted(
        read_overrides(Path(args.overrides).read_text()).items())) if args.overrides else ""
    print(admittance.report(sigs(pairs, ref=True), sigs(pairs, ov=ov)))


def clamped(tracks, item):
    """The take's ringing window, cut back to where every source still decays.

    The schedule says when the last damper lands; it cannot say when a source
    stops being an instrument. A sampled reference reaches the end of its sample
    and is faded out, and the file then runs on in silence, so a window placed
    on the schedule alone averages one side over its decay and the other over a
    fade plus a second of nothing -- and the two sides hold different amounts of
    nothing, which lands in the answer as a band difference.
    """
    ring = takes.window_for(item, "ringing")
    floor = takes.window_for(item, "floor")
    end = min(takes.usable_until(x, sr, floor, ring[0]) for x, sr in tracks.values())
    if end - ring[0] < 0.3:
        raise ValueError(
            f"take {item.get('id', '?')} has no window in which every source is "
            f"still decaying (last note-off {ring[0]:.2f} s, earliest source ends "
            f"{end:.2f} s)")
    return (ring[0], min(ring[1], end))


def cmd_takes(args):
    """Chords, pedal and repeats, against a reference, from an audition page.

    The note corpus holds one note struck alone with the pedal up, so nothing in
    it can show what an instrument does when more than one string is moving.
    This reads what `make_audition.py` wrote, which is the only material in the
    harness that can.
    """
    page = takes.load(args.page, args.reference)
    refs = takes.pick_references(page["_manifest"], args.reference)
    if not refs:
        raise SystemExit(f"{args.page} has no reference source; render it without "
                         "--model-only")
    ref = refs[0]
    items = {it["id"]: it for it in page["_manifest"]["items"]}
    # The take everything else is read against for accumulation. Derived from
    # the schedules, so a page rendered with --only still picks the plainest of
    # what it holds and says which one that was.
    base_id = takes.plainest(items)
    base_tracks = page.get(base_id) or {}
    try:
        base_window = clamped(base_tracks, items[base_id]) if base_id else None
        base_floor = takes.window_for(items[base_id], "floor") if base_id else None
        base_body = takes.window_for(items[base_id], "body") if base_id else None
    except (KeyError, ValueError):
        base_window = base_floor = base_body = None
    for tid, item in items.items():
        tracks = page.get(tid)
        if not tracks or ref not in tracks:
            continue
        print(f"\n== {tid}: {item['label']}")
        # First, and for every take: the wave as it is drawn, levels absolute.
        # An output level that is wrong by the same amount everywhere is a
        # finding of its own, and one this harness had nowhere to report -- the
        # note metrics normalise it away and the band table below divides it out
        # deliberately, so it could only be seen on the audition page, where it
        # looks like a difference in envelope rather than in level.
        phrase = takes.window_for(item, "phrase")
        print(f"   drawn envelope, {phrase[0]:.1f}-{phrase[1]:.1f} s, dBFS")
        print(takes.envelope_report(tracks, refs, phrase))

        body = takes.window_for(item, "body")
        floor = takes.window_for(item, "floor")

        # While it is sounding, which is the only window some instruments have.
        # `ringing` measures what a take leaves behind once nothing is driven,
        # and an instrument whose damper stops the string in a tenth of a second
        # leaves nothing there to read: on the harpsichord set that window is
        # empty on all nine takes, so the phrase set had no band table at all and
        # a level fitted from it before the window was clamped was fitted against
        # silence. This is a different measurement and carries its own name — the
        # colour of what is sounding, not the colour of what is left.
        srows = []
        for src in tracks:
            if src == ref:
                continue
            vals, g = takes.band_error(tracks, src, ref, body, body, floor)
            srows.append((f"{src} ({g:+.1f} dB on body)", vals))
        if srows:
            print(f"   bands against {ref} while sounding, "
                  f"{body[0]:.1f}-{body[1]:.1f} s")
            print(takes.report(srows))

        try:
            schedule = takes.window_for(item, "ringing")
            ring = clamped(tracks, item)
        except ValueError as exc:
            print(f"   aftersound band table skipped: {exc}")
            continue
        rows = []
        for src in tracks:
            if src == ref:
                continue
            vals, g = takes.band_error(tracks, src, ref, body, ring, floor)
            rows.append((f"{src} ({g:+.1f} dB on body)", vals))
        if not rows:
            continue
        cut = (f", cut from {schedule[1]:.1f} where a source stopped decaying"
               if ring[1] < schedule[1] - 0.05 else "")
        print(f"   bands against {ref}, body {body[0]:.1f}-{body[1]:.1f} s, "
              f"measured {ring[0]:.1f}-{ring[1]:.1f} s{cut}")
        print(takes.report(rows))

        # What the phrase ADDED over the plainest take, each source against its
        # own rendering of that take. The table above cannot answer this: it
        # fits one gain per take, so a voice that piles up twenty decibels too
        # much over eight strikes and a voice that gets it right are shown the
        # same. Every source is printed, references included, because how much
        # two real instruments accumulate is the only thing that says how much
        # is too much.
        if tid != base_id and base_tracks and base_window is not None:
            # Both windows open a tenth of a second after their own last damper
            # and are cut to ONE length, the shorter of the two. Same age and
            # same span, or a difference in decay rate is read as a difference
            # in accumulation -- averaging a long window over more of a decay
            # lowers it, which is a bias in whichever direction the two spans
            # happen to differ.
            span = min(ring[1] - ring[0], base_window[1] - base_window[0])
            here = (ring[0], ring[0] + span)
            there = (base_window[0], base_window[0] + span)
            brows = [(src, takes.band_buildup(tracks, base_tracks, src, here,
                                              there, floor, base_floor,
                                              body, base_body))
                     for src in tracks if src in base_tracks]
            if brows:
                print(f"   built up over {base_id}, each source against itself, "
                      f"{span:.1f} s from each last damper")
                print(takes.report(brows))


def main(argv=None):
    # The target options live on a parent parser so they are accepted on either
    # side of the subcommand; typed after it is what a person reaches for first.
    # Built fresh per subcommand rather than shared through `parents`, because
    # `takes` reads a rendered audition page and never opens the corpus. Sharing
    # one parent made `--corpus` required there and then ignored it, so a path
    # that did not exist was accepted without a word and every run read as though
    # the corpus had been consulted. A required argument that is never used is
    # worse than an absent one: it invites a value nobody checks.
    def target(*, corpus: bool):
        q = argparse.ArgumentParser(add_help=False)
        q.add_argument("--capture", default="piano", help="capture definition id")
        if corpus:
            q.add_argument("--corpus", required=True,
                           help="directory holding manifest.json")
        q.add_argument("--timbre", default="", help="which timbre of the capture")
        q.add_argument("--notes", default="", help="subset of the capture's notes")
        q.add_argument("--velocities", default="",
                       help="subset of the capture's velocities")
        q.add_argument("--lib", default="", help="SONARE_LIB_PATH for model renders")
        q.add_argument("--cache", default="/tmp/voicematch-shape")
        q.add_argument("--no-bed", action="store_true",
                       help="skip the recorded-floor subtraction")
        q.add_argument("--workers", type=int, default=7)
        return q

    common = target(corpus=True)

    p = argparse.ArgumentParser(prog="shape", description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("score", parents=[common])
    s.add_argument("--overrides", default="")
    s.set_defaults(fn=cmd_score)

    s = sub.add_parser("fit", parents=[common])
    s.add_argument("--knobs", required=True, help="SONARE_TUNING_DUMP output")
    s.add_argument("--namespaces", default="", help="comma-separated key prefixes")
    s.add_argument("--deny", default="", help="file of coordinates to leave alone")
    s.add_argument("--start", default="")
    s.add_argument("--out", default="")
    s.add_argument("--passes", type=int, default=4)
    s.set_defaults(fn=cmd_fit)

    s = sub.add_parser("ablate", parents=[common])
    s.add_argument("--knobs", required=True)
    s.add_argument("--namespaces", default="")
    s.add_argument("--overrides", required=True)
    s.set_defaults(fn=cmd_ablate)

    s = sub.add_parser("prune", parents=[common])
    s.add_argument("--knobs", required=True)
    s.add_argument("--namespaces", default="")
    s.add_argument("--overrides", required=True)
    s.add_argument("--out", default="")
    s.set_defaults(fn=cmd_prune)

    s = sub.add_parser("probe", parents=[common])
    s.add_argument("--overrides", default="")
    s.set_defaults(fn=cmd_probe)

    s = sub.add_parser("struck", parents=[common])
    s.add_argument("--overrides", default="")
    s.set_defaults(fn=cmd_struck)

    s = sub.add_parser("purity", parents=[common])
    s.add_argument("--overrides", default="")
    s.set_defaults(fn=cmd_purity)

    s = sub.add_parser("admittance", parents=[common])
    s.add_argument("--overrides", default="")
    s.set_defaults(fn=cmd_admittance)

    # Reads a rendered audition page rather than the corpus, so it is given the
    # target options WITHOUT --corpus.
    s = sub.add_parser("takes", parents=[target(corpus=False)])
    s.add_argument("--page", required=True,
                   help="audition directory holding manifest.json and the takes")
    s.add_argument("--reference", default="",
                   help="source key everything is measured against (default: the "
                        "one declaring role=reference, or the only non-model one)")
    s.set_defaults(fn=cmd_takes)

    args = p.parse_args(argv)
    args.fn(args)


if __name__ == "__main__":
    main()
