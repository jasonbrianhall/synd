#!/bin/sh
# export_public.sh - copy the publishable part of this repository into DIR (the public "synd" repository): every
# tracked file except the original game files (_references/) and the code generated from them (gen/), which
# are Bullfrog's and must not be published.  The public repository keeps its own README.md, LICENSE and
# .gitignore and screenshots/ (public/ here holds them) and its own history.
#
#   tools/export_public.sh DIR
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$1"
[ -n "$DIR" ] || { echo "usage: tools/export_public.sh DIR"; exit 2; }
mkdir -p "$DIR"
cd "$ROOT"
git ls-files | grep -v -e '^_references/' -e '^gen/' -e '^public/' -e '^README\.md$' -e '^\.gitignore$' |
    while IFS= read -r f; do
        mkdir -p "$DIR/$(dirname "$f")"
        cp -p "$f" "$DIR/$f"
    done
cp -p public/README.md public/LICENSE public/.gitignore "$DIR/"
mkdir -p "$DIR/screenshots"
cp -p public/screenshots/*.png "$DIR/screenshots/"
echo "exported to $DIR"
