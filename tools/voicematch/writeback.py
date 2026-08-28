"""Putting a fitted value back into the source it came from.

Three destinations, because the three kinds of knob are declared three ways. A
source knob and a `SONARE_TUNABLE` both have a literal to replace, which
`materialize` splices in place. A patch field has none — the program table
builds most patches through helper lambdas taking positional arguments, so no
literal in it belongs to a named field — and is written as an explicit
`o.<patch>.<field> = <value>f;`, the idiom that table already uses for its own
exceptions. A drum note gets the same treatment into the drum table, which names
every note directly.

`restore` is the safety net: the pristine text of every touched file is
snapshotted before a fit starts and written back in a `finally`, so an exception
or a Ctrl-C never leaves the tree perturbed. It undoes this process's own writes
and nothing else — see `write_edits`, which is what records them.
"""

from __future__ import annotations

import functools
import re
import sys
from pathlib import Path

from _repo import REPO_ROOT
from catalogue import DRUM_PATCH_KEY
from knobs import Knob, format_value


def materialize(
    knobs: list[Knob], values: list[float], pristine: dict[Path, str],
    *, source_only: bool = True, full: bool = False,
) -> dict[Path, str]:
    """Produce each touched file's text with its knob values spliced in.

    Splicing is computed against the pristine text (never re-matched on an
    already-edited buffer) so multiple knobs in one file cannot interfere.

    `source_only` (the default) skips runtime knobs, which reach the library
    through the environment during a fit and must not perturb the tree — but
    the final write-back clears it, so a fitted runtime knob lands in the
    source as its new `SONARE_TUNABLE` default rather than in a shell variable
    someone has to remember.

    Two modes, because the two callers want opposite things from a knob that
    has not moved:

    - The report wants the smallest diff, so by default a knob the fit left
      where it started is skipped rather than rewritten with the same number:
      the two spellings of a value are not always the same text (`0.10f` in the
      source against a formatted `0.1`), and a hundred-knob spec would otherwise
      produce a diff full of lines that change nothing, hiding the handful that
      do. A file left with no surviving knob does not appear in the result.
    - `full` is what the render path needs: every file the spec's source knobs
      target, spliced with every one of them. Dropping a file because this
      candidate happens to format back to its start value would leave the
      *previous* candidate's text on disk, and the render would then score a
      vector that was never assembled — silently, and only for the knobs whose
      optimum sits near the compiled-in default, which is most of them.
    """
    by_file: dict[Path, list[tuple[Knob, float]]] = {}
    for knob, value in zip(knobs, values):
        if knob.file is None:
            continue
        if source_only and knob.tunable is not None:
            continue
        if not full and format_value(value) == format_value(knob.start_value):
            continue
        by_file.setdefault(knob.file, []).append((knob, value))
    result: dict[Path, str] = {}
    for path, items in by_file.items():
        text = pristine[path]
        # Splice from the end so earlier offsets stay valid.
        for knob, value in sorted(items, key=lambda kv: kv[0].span_start, reverse=True):
            text = text[: knob.span_start] + format_value(value) + text[knob.span_end :]
        result[path] = text
    return result


def write_edits(edited: dict[Path, str], written: dict[Path, str]) -> None:
    """Write each file and record what was put there, for `restore` to check."""
    for path, text in edited.items():
        path.write_text(text)
        written[path] = text


def restore(pristine: dict[Path, str], written: dict[Path, str]) -> None:
    """Undo this process's own edits, and only those.

    A fit runs for hours in the tree its user is working in, so restoring every
    snapshotted file would roll back whatever else was edited meanwhile. Most of
    those files were never written at all: a runtime knob's declaration file is
    snapshotted only so the final report can diff it, and the render path writes
    source-knob files exclusively.

    A file is put back only while it still holds exactly what this process wrote
    to it. Anything else means it was edited after the fit touched it, and that
    edit is worth more than a clean tree — so it is reported and left alone.
    """
    for path, text in sorted(written.items()):
        if path not in pristine:
            continue
        if path.exists() and path.read_text() != text:
            print(f"note: {path} was edited after the fit wrote it — left as it is; "
                  f"its pristine text was NOT restored", file=sys.stderr)
            continue
        path.write_text(pristine[path])
    written.clear()


# The files that build the per-program override table, in the order they are
# searched for a patch's configuration block.
PROGRAM_TABLE_FILES = (
    "src/midi/synth/gm_fallback_programs_physical.h",
    "src/midi/synth/gm_fallback_programs_keyed.h",
    "src/midi/synth/gm_fallback_programs_percussion.h",
    "src/midi/synth/gm_fallback_programs_synth.h",
    "src/midi/synth/gm_fallback_programs_sfx.h",
    "src/midi/synth/gm_fallback_programs_variations.h",
)

# Where the per-drum-note voicing table is built.
DRUM_TABLE_FILE = "src/midi/synth/gm_fallback_drums.cpp"

# Where the tuning layer forms every indexed key, and therefore the only place
# that knows which path elements are arrays.
TUNING_LAYER_FILE = "src/midi/synth/patch_tuning.cpp"

#: An indexed key as that layer spells it: a string literal naming the array,
#: immediately concatenated with the index. The literal is captured whole and
#: the array is its last element, because an array's own name can carry digits
#: (`shell_t60_s`) and a pattern that assumes otherwise silently drops it.
_INDEXED_KEY = re.compile(r'"([a-z_0-9.]+)"\s*\+\s*(?:std::to_string\(|index\b)')


def override_patch_names() -> list[str]:
    """Every `ProgramOverrides` member name, from the X-macro list that defines them."""
    header = (REPO_ROOT / "src/midi/synth/gm_fallback_data.h").read_text()
    block = re.search(r"#define SONARE_GM_OVERRIDE_PATCHES\(X\)(.*?)\n\n", header, re.S)
    if block is None:
        raise ValueError("SONARE_GM_OVERRIDE_PATCHES not found in gm_fallback_data.h")
    return re.findall(r"X\((\w+)\)", block.group(1))


@functools.lru_cache(maxsize=1)
def array_members() -> tuple[str, ...]:
    """Path elements that address an array member of a patch struct.

    A tuning key spells these without brackets (`pipe_organ.ranks2.level`)
    because a key travels through a shell variable, where `[` and `]` would need
    quoting — so a write-back has to put the brackets back, and has to know which
    elements take them.

    Read from the layer that forms the keys rather than listed here. A list
    written by hand is a mirror of that file and it drifted: two percussion
    arrays were added to the library and not to the list, so a fitted drum value
    was written as `mode_ratios1`, which is not a member of anything. The tree
    stopped compiling, which is the good case — the bad one is a name that
    happens to exist.
    """
    text = (REPO_ROOT / TUNING_LAYER_FILE).read_text()
    found = [k.rsplit('.', 1)[-1] for k in _INDEXED_KEY.findall(text)]
    if not found:
        raise ValueError(f"no indexed tuning keys found in {TUNING_LAYER_FILE}")
    return tuple(sorted(set(found)))


def key_to_member_path(path: str) -> str:
    """Turn a tuning key's field path into the C++ member expression it names."""
    arrays = array_members()
    out = []
    for element in path.split("."):
        # The name may carry digits of its own, so the index is the trailing
        # run and the name is everything up to the last non-digit.
        m = re.fullmatch(r"([a-z_0-9]*[a-z_])(\d+)", element)
        if m is not None and m.group(1) in arrays:
            out.append(f"{m.group(1)}[{m.group(2)}]")
        else:
            out.append(element)
    return ".".join(out)


def patch_field_assignments(
    knobs: list[Knob], values: list[float]
) -> tuple[dict[str, list[tuple[str, float]]], dict[int, list[tuple[str, float]]], list[str]]:
    """Group the fitted patch-field knobs by the patch they belong to.

    Returns (assignments per `ProgramOverrides` member, assignments per drum
    note, keys belonging to neither). A drum note has its own assignment site —
    the drum table names each note as `t[38]` — so it is written back like a
    program patch, just into a different file. What is left over is the family
    patches (`fam3.`), which a loop builds from a table with no per-patch line to
    update; their values are reported for the developer to place.
    """
    named = set(override_patch_names())
    per_patch: dict[str, list[tuple[str, float]]] = {}
    per_drum: dict[int, list[tuple[str, float]]] = {}
    other: list[str] = []
    for knob, value in zip(knobs, values):
        if knob.tunable is None or knob.file is not None:
            continue  # a source knob or a SONARE_TUNABLE: written back directly
        if format_value(value) == format_value(knob.start_value):
            continue  # the fit left it where it was; do not add a line saying so
        patch, _, path = knob.tunable.partition(".")
        if not path:
            continue
        drum = DRUM_PATCH_KEY.fullmatch(patch)
        if patch in named:
            per_patch.setdefault(patch, []).append((key_to_member_path(path), value))
        elif drum is not None:
            per_drum.setdefault(int(drum.group(1)), []).append(
                (key_to_member_path(path), value)
            )
        else:
            other.append(knob.tunable)
    return per_patch, per_drum, other


def _splice_field_lines(
    text: str, anchor: re.Pattern, prefix: str, fields: list[tuple[str, float]]
) -> str:
    """Set `<prefix><path> = <value>f;` for each field in one patch's block.

    A line already assigning that member is replaced in place; the rest are
    appended after the block's last `anchor` line, at its indentation. Appending
    after the anchor rather than at a fixed offset is what keeps the assignment
    inside the block that builds the patch and ahead of whatever the table does
    with it afterwards.
    """
    append: list[str] = []
    for path, value in sorted(fields):
        member = f"{prefix}{path}"
        existing = re.compile(rf"^([ \t]*){re.escape(member)}\s*=\s*[^;]+;[ \t]*$", re.M)
        line = f"{member} = {format_value(value)}f;"
        if existing.search(text):
            text = existing.sub(lambda m, s=line: f"{m.group(1)}{s}", text, count=1)
        else:
            append.append(line)
    if append:
        last = None
        for m in anchor.finditer(text):
            last = m
        indent = re.match(r"[ \t]*", last.group(0)).group(0)
        block = "\n".join(indent + line for line in append)
        text = text[: last.end()] + "\n" + block + text[last.end() :]
    return text


def _current_text(path: Path, base: dict[Path, str] | None) -> str:
    """The text to edit: what this run last produced for the file, else the disk."""
    for key in (path, path.resolve()) if base else ():
        if key in base:
            return base[key]
    return path.read_text()


def _patch_site(text: str, patch: str) -> tuple[re.Pattern, str, int, int] | None:
    """Locate the block that builds one patch: anchor, member prefix and extent.

    Two spellings build a patch, and only one of them mentions its name where a
    fitted value can be appended. Most tables address it directly (`o.violin =
    bowed(...)` followed by `o.violin.cutoff_hz = ...`), and the anchor is
    `o.<patch>` anywhere in the file. The keyed and percussion tables bind a
    reference first (`NativeSynthPatch& vb = o.vibraphone;`) and set every field
    through it, so nothing else in the block spells the patch name at all — a
    third of the melodic bank is built this way. There the alias is the anchor,
    and the block is bounded by the next reference binding, so a two-letter
    alias cannot match a line that belongs to the next patch.

    Returns None when the file does not build this patch.
    """
    direct = re.compile(rf"^[ \t]*o\.{re.escape(patch)}\b.*$", re.M)
    if direct.search(text):
        return direct, f"o.{patch}.", 0, len(text)
    decl = re.compile(
        rf"^[ \t]*NativeSynthPatch&\s*(\w+)\s*=\s*o\.{re.escape(patch)}\s*;[ \t]*$", re.M
    ).search(text)
    if decl is None:
        return None
    alias = decl.group(1)
    following = re.compile(r"^[ \t]*NativeSynthPatch&\s*\w+\s*=", re.M).search(
        text, decl.end()
    )
    end = following.start() if following is not None else len(text)
    return re.compile(rf"^[ \t]*{re.escape(alias)}\b.*$", re.M), f"{alias}.", decl.start(), end


def write_patch_fields(
    per_patch: dict[str, list[tuple[str, float]]], base: dict[Path, str] | None = None
) -> dict[Path, str]:
    """Splice fitted patch-field values into the program table sources.

    A patch field has no `SONARE_TUNABLE` declaration to rewrite — the table
    builds most of them through helper lambdas taking positional arguments
    (`o.violin = bowed(0.12f, 0.55f, ...)`), so there is no literal that belongs
    to a named field. What the table does have is the idiom the builders already
    use for the exceptions: a plain `o.<patch>.<field> = <value>f;` after the
    call. A fitted field is written as one of those, replacing the line if it is
    already there and appending it to the end of the patch's block if it is not.

    `base` supplies the current text of a file the same run has already edited,
    which is how a `SONARE_TUNABLE` splice and a patch-field line landing in one
    file both survive: read from disk instead and the second write would be
    computed from text that no longer reflects the first.

    Returns the new text of each file touched; the caller writes and diffs them.
    """
    edited: dict[Path, str] = {}
    unplaced: list[str] = []
    sources = [(REPO_ROOT / name) for name in PROGRAM_TABLE_FILES]
    texts = {path: _current_text(path, base) for path in sources if path.exists()}

    for patch, fields in sorted(per_patch.items()):
        site = next(
            ((p, s) for p, t in texts.items() if (s := _patch_site(t, patch)) is not None),
            None,
        )
        if site is None:
            unplaced.extend(f"{patch}.{path}" for path, _ in fields)
            continue
        target, (anchor, prefix, lo, hi) = site
        text = texts[target]
        texts[target] = (
            text[:lo] + _splice_field_lines(text[lo:hi], anchor, prefix, fields) + text[hi:]
        )
        edited[target] = texts[target]

    if unplaced:
        print(f"note: no assignment site found for {len(unplaced)} patch fields "
              f"({', '.join(unplaced[:3])}...); reported only", file=sys.stderr)
    return edited


def write_drum_fields(
    per_note: dict[int, list[tuple[str, float]]], base: dict[Path, str] | None = None
) -> dict[Path, str]:
    """Splice fitted drum-note values into the drum table.

    The drum table addresses each note directly (`t[38] = d.snare;`) and already
    carries per-note corrections in exactly the idiom a fit needs
    (`t[46].amp_env.release_ms = 40.0f;`), so a fitted field is written as one of
    those after the note's own line — the same treatment a program patch gets in
    `write_patch_fields`, into a different file.

    A note assigned only as part of a chain (`t[41] = t[43] = ... = d.tom;`) is
    still anchored, since the whole chain is one statement and appending after it
    is correct for any of the notes in it. Everything the table builds this way
    lands ahead of the clamp pass at the end, so a written value is clamped
    exactly as a hand-written one would be.

    `base` carries an edit this run has already made to the same file, as in
    `write_patch_fields`.
    """
    path = (REPO_ROOT / DRUM_TABLE_FILE).resolve()
    if not path.exists():
        print(f"note: {DRUM_TABLE_FILE} not found; drum-note values reported only",
              file=sys.stderr)
        return {}
    text = _current_text(path, base)
    original = text
    unplaced: list[str] = []
    for note, fields in sorted(per_note.items()):
        anchor = re.compile(rf"^[ \t]*(?:t\[\d+\]\s*=\s*)*t\[{note}\][^\n]*$", re.M)
        if anchor.search(text) is None:
            unplaced.extend(f"d{note:03d}.{field}" for field, _ in fields)
            continue
        text = _splice_field_lines(text, anchor, f"t[{note}].", fields)
    if unplaced:
        print(f"note: the drum table has no line for {len(unplaced)} fields "
              f"({', '.join(unplaced[:3])}...); reported only", file=sys.stderr)
    return {path: text} if text != original else {}
