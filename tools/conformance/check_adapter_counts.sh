#!/bin/sh
# Count grep matches. A count change lists the current matches for review.
set -eu

guard=${1-}
excluded_paths=''
case "$guard" in
  broad-catches)
    roots='src/wasm bindings/node/src/addon'
    extensions='cpp h cc hh cxx hpp'
    pattern='(^|[^[:alnum:]_])catch[[:space:]]*\([[:space:]]*(\.\.\.[[:space:]]*\)|const[[:space:]]+std::exception[[:space:]]*&)'
    ;;
  zero-empty-fallbacks)
    roots='bindings/node/src bindings/wasm/src'
    extensions='ts tsx js jsx mts cts mjs cjs'
    pattern="(\?\?|\|\|)[[:space:]]*(0([^[:alnum:]_.\$]|\$)|''|\"\")"
    ;;
  universal-literals)
    roots='src include'
    extensions='c cpp cc cxx h hh hpp hxx mm inc inl ipp tpp'
    excluded_paths='src/util/constants.h
src/mastering/stereo/binaural_ring.inc
src/midi/synth/gs_classic_models.inc
src/midi/synth/gs_efx_bindings.h
src/midi/synth/gs_efx_tables.h
src/midi/synth/singing_pitch_compensation.inc
src/util/parameter_bag_keys.h
src/wasm/c_entry_names.h
src/wasm/field_spelling.h'
    numbers='(3[.]14159|6[.]2831|1[.]5707|0?[.]70710678|1[.]41421356)[[:digit:]]*[fFlL]?'
    names='k(Pi|TwoPi|HalfPi|InvPi|Sqrt2|InvSqrt2|ButterworthQ)[DFL]?'
    pattern="(^|[^[:alnum:]_.])$numbers([^[:alnum:]_.]|$)|(^|[^[:alnum:]_])(M_PI|_USE_MATH_DEFINES)([^[:alnum:]_]|$)|(^|[^[:alnum:]_:])$names[[:space:]]*(=[^=]|[{])|(^|[^[:alnum:]_])(float|double|auto)[[:blank:]]+$names[[:blank:]]*[(]|^[[:space:]]*#[[:space:]]*define[[:space:]]+$names([^[:alnum:]_]|$)"
    ;;
  *) echo "usage: $0 broad-catches|zero-empty-fallbacks|universal-literals" >&2; exit 2 ;;
esac

baseline="tests/conformance/$guard.count"
expected=$(cat "$baseline")
case "$expected" in
  ''|*[!0-9]*) echo "$guard: invalid count in $baseline" >&2; exit 2 ;;
esac

matches=$(mktemp)
trap 'rm -f "$matches"' 0

# Count and scan the same files, including the exclusions.
set -- -type f '(' -name "*.${extensions%% *}"
for extension in ${extensions#* }; do
  set -- "$@" -o -name "*.$extension"
done
set -- "$@" ')'
for path in $excluded_paths; do
  set -- "$@" ! -path "$path"
done

scanned=0
for root in $roots; do
  files=0
  if [ -d "$root" ]; then
    paths=$(find "$root" "$@" -print)
    if [ -n "$paths" ]; then
      files=$(printf '%s\n' "$paths" | wc -l | tr -d '[:space:]')
    fi
  fi
  printf '%s: %s files scanned in %s\n' "$guard" "$files" "$root"
  if [ "$files" -eq 0 ]; then
    echo "$guard: no source files found in $root" >&2
    exit 1
  fi
  scanned=$((scanned + files))
  find "$root" "$@" -exec sh -c '
    pattern=$1
    shift
    grep -EnoH -e "$pattern" "$@" || [ "$?" -eq 1 ]
  ' sh "$pattern" {} + >> "$matches"
done

actual=$(wc -l < "$matches" | tr -d '[:space:]')
printf '%s: %s matches across %s files scanned\n' "$guard" "$actual" "$scanned"
if [ "$actual" -gt "$expected" ]; then
  echo "$guard: count went up from $expected to $actual. Matching sites:" >&2
  cat "$matches" >&2
  exit 1
fi
if [ "$actual" -lt "$expected" ]; then
  echo "$guard: count went down from $expected to $actual. Lower $baseline to $actual." >&2
  exit 1
fi
