#!/bin/sh
# Sync a persistent source mirror to HEAD by content: only files HEAD changed get a
# new mtime, so an incremental build in the mirror rebuilds only what moved, and a
# build there is exactly HEAD whatever the working tree holds.
#
#     sh tools/head_mirror.sh <mirror dir>
set -eu
mirror=${1:?usage: head_mirror.sh <mirror dir>}
case "$mirror" in
/ | "" | .) echo "head_mirror.sh: refusing mirror path '$mirror'" >&2; exit 1 ;;
esac
repo=$(git rev-parse --show-toplevel)
stage="$mirror.stage"
rm -rf "$stage"
mkdir -p "$stage" "$mirror"
git -C "$repo" archive HEAD | tar -x -C "$stage"
rsync -rlc --delete --exclude /.HEAD "$stage"/ "$mirror"/
rm -rf "$stage"
git -C "$repo" rev-parse --short HEAD >"$mirror/.HEAD"
echo "head mirror $mirror at $(cat "$mirror/.HEAD")"
