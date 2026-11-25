#!/bin/sh
# Count grep matches. A count change lists the current matches for review.
set -eu

guard=${1-}
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
  *) echo "usage: $0 broad-catches|zero-empty-fallbacks" >&2; exit 2 ;;
esac

baseline="tests/conformance/$guard.count"
expected=$(cat "$baseline")
case "$expected" in
  ''|*[!0-9]*) echo "$guard: invalid count in $baseline" >&2; exit 2 ;;
esac

matches=$(mktemp)
trap 'rm -f "$matches"' 0

# Build both selectors from the same set of extensions.
set --
for extension in $extensions; do
  set -- "$@" --include="*.$extension"
done

scanned=0
for root in $roots; do
  files=0
  if [ -d "$root" ]; then
    for extension in $extensions; do
      paths=$(find "$root" -type f -name "*.$extension" -print)
      if [ -n "$paths" ]; then
        files=$((files + $(printf '%s\n' "$paths" | wc -l)))
      fi
    done
  fi
  printf '%s: %s files scanned in %s\n' "$guard" "$files" "$root"
  if [ "$files" -eq 0 ]; then
    echo "$guard: no source files found in $root" >&2
    exit 1
  fi
  scanned=$((scanned + files))
  grep -rEnoH "$@" "$pattern" "$root" >> "$matches" || {
    status=$?
    [ "$status" -eq 1 ] || exit "$status"
  }
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
