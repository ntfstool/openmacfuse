#!/bin/sh
# Guard against the old project name coming back: fails if the old name appears anywhere in tracked files.
# (The word is assembled from parts so that this file does not contain it.)
cd "$(dirname "$0")/.." || exit 2
OLD="n""fuse"
hits=$(git grep -nIi "$OLD" | grep -vi "confus")
if [ -n "$hits" ]; then
	echo "check-names: the old name '$OLD' is still used:"
	echo "$hits"
	exit 1
fi
echo "check-names: ok"
