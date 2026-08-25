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
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from corpus import load_corpus  # noqa: E402

from . import probes, purity, takes  # noqa: E402
from .bed import Bed  # noqa: E402
from .loss import ShapeLoss  # noqa: E402
from .partials import Track  # noqa: E402
from .render import Signals, load_knob_dump, read_overrides, write_overrides  # noqa: E402
from .search import Descent, ablate, prune, split_notes, summarise  # noqa: E402
from .spectro import Spectro  # noqa: E402

CAPTURE_DIR = Path(__file__).resolve().parents[1] / "capture"


def build(args):
    """A loss, its signal source, and the note grid, all from the capture."""
    cap = json.loads((CAPTURE_DIR / f"{args.capture}.json").read_text())
    corpus = load_corpus(args.corpus, args.timbre)
    gate_s = float(cap["gate_ms"]) / 1000.0
    preroll = float(cap.get("preroll_ms", 0)) / 1000.0
    seconds = round(corpus.slot_s + preroll, 3)
    spectro = Spectro(sample_rate=corpus.sample_rate, seconds=seconds)
    sigs = Signals(corpus_root=Path(args.corpus), program=int(cap["program"]),
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
                     note_off_s=preroll + gate_s, velocities=vels)
    return cap, corpus, sigs, loss, notes


def cmd_score(args):
    _cap, _corpus, _sigs, loss, notes = build(args)
    ov = read_overrides(Path(args.overrides).read_text()) if args.overrides else {}
    text = ",".join(f"{k}={v!r}" for k, v in sorted(ov.items()))
    base = loss.score("", notes=notes)
    print(f"shipped   {base}")
    if text:
        print(f"overrides {loss.score(text, notes=notes)}")


def cmd_fit(args):
    _cap, _corpus, _sigs, loss, notes = build(args)
    base = load_knob_dump(args.knobs, tuple(args.namespaces.split(","))
                          if args.namespaces else ())
    deny = set(Path(args.deny).read_text().split()) if args.deny else set()
    fit_notes, hold_notes = split_notes(notes)
    d = Descent(loss=loss, base=base, fit_notes=fit_notes, hold_notes=hold_notes,
                deny=deny, workers=args.workers, passes=args.passes)
    start = read_overrides(Path(args.start).read_text()) if args.start else None
    moves = d.run(start)
    kept, report = prune(loss, base, moves, fit_notes, hold_notes,
                         workers=args.workers)
    print("\n" + summarise(report["contributions"], base, moves))
    print(f"\nbefore  fit {report['before']['fit']:.3f}  hold {report['before']['hold']:.3f}")
    print(f"pruned  fit {report['after']['fit']:.3f}  hold {report['after']['hold']:.3f}"
          f"   ({len(kept)} of {len(moves)} moves kept)")
    out = write_overrides({**base, **kept}, base)
    print("\n" + out)
    if args.out:
        Path(args.out).write_text(out + "\n")


def cmd_ablate(args):
    _cap, _corpus, _sigs, loss, notes = build(args)
    base = load_knob_dump(args.knobs, tuple(args.namespaces.split(","))
                          if args.namespaces else ())
    moves = read_overrides(Path(args.overrides).read_text())
    fit_notes, hold_notes = split_notes(notes)
    scores, (f0, h0) = ablate(loss, base, moves, fit_notes, hold_notes, args.workers)
    print(f"fitted set  fit {f0:.3f}  hold {h0:.3f}\n")
    print(summarise(scores, base, moves))


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


def cmd_takes(args):
    """Chords, pedal and repeats, against a reference, from an audition page.

    The note corpus holds one note struck alone with the pedal up, so nothing in
    it can show what an instrument does when more than one string is moving.
    This reads what `make_audition.py` wrote, which is the only material in the
    harness that can.
    """
    page = takes.load(args.page)
    ref = page["_reference"]
    if ref is None:
        raise SystemExit(f"{args.page} has no reference source; render it without "
                         "--model-only")
    items = {it["id"]: it for it in page["_manifest"]["items"]}
    for tid, item in items.items():
        tracks = page.get(tid)
        if not tracks or ref not in tracks:
            continue
        body = takes.window_for(item, "body")
        try:
            ring = takes.window_for(item, "ringing")
        except ValueError as exc:
            print(f"\n== {tid}: {item['label']}\n   skipped: {exc}")
            continue
        rows = []
        for src in tracks:
            if src == ref:
                continue
            vals, g = takes.band_error(tracks, src, ref, body, ring)
            rows.append((f"{src} ({g:+.1f} dB on body)", vals))
        if not rows:
            continue
        print(f"\n== {tid}: {item['label']}")
        print(f"   body {body[0]:.1f}-{body[1]:.1f} s, "
              f"measured {ring[0]:.1f}-{ring[1]:.1f} s")
        print(takes.report(rows))


def main(argv=None):
    # The target options live on a parent parser so they are accepted on either
    # side of the subcommand; typed after it is what a person reaches for first.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--capture", default="piano", help="capture definition id")
    common.add_argument("--corpus", required=True, help="directory holding manifest.json")
    common.add_argument("--timbre", default="", help="which timbre of the capture")
    common.add_argument("--notes", default="", help="subset of the capture's notes")
    common.add_argument("--velocities", default="",
                        help="subset of the capture's velocities")
    common.add_argument("--lib", default="", help="SONARE_LIB_PATH for model renders")
    common.add_argument("--cache", default="/tmp/voicematch-shape")
    common.add_argument("--no-bed", action="store_true",
                        help="skip the recorded-floor subtraction")
    common.add_argument("--workers", type=int, default=7)

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

    s = sub.add_parser("probe", parents=[common])
    s.add_argument("--overrides", default="")
    s.set_defaults(fn=cmd_probe)

    s = sub.add_parser("purity", parents=[common])
    s.add_argument("--overrides", default="")
    s.set_defaults(fn=cmd_purity)

    # Reads a rendered audition page rather than the corpus, so it needs none of
    # the corpus options -- but they are harmless and keep one invocation shape.
    s = sub.add_parser("takes", parents=[common])
    s.add_argument("--page", required=True,
                   help="audition directory holding manifest.json and the takes")
    s.set_defaults(fn=cmd_takes)

    args = p.parse_args(argv)
    args.fn(args)


if __name__ == "__main__":
    main()
