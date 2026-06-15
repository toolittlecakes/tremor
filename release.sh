#!/usr/bin/env bash
# Cut a tremor release: bump VERSION, tag, publish a GitHub release, and point
# the Homebrew tap formula at the new tag. Run from the tremor repo root.
#
#   ./release.sh 0.2.0
#
# Assumes the tap repo sits next to this one (../homebrew-tremor); override with
# TAP_DIR=/path/to/homebrew-tremor.
set -euo pipefail

REPO_SLUG="toolittlecakes/tremor"
TAP_DIR="${TAP_DIR:-$(cd "$(dirname "$0")/.." && pwd)/homebrew-tremor}"
FORMULA="$TAP_DIR/Formula/tremor.rb"

die() { echo "error: $*" >&2; exit 1; }

V="${1:-}"
[[ "$V" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "usage: ./release.sh <major.minor.patch>"
command -v gh >/dev/null || die "gh CLI not found"
[[ -f VERSION ]]    || die "run from the tremor repo root (no VERSION here)"
[[ -f "$FORMULA" ]] || die "tap formula not found at $FORMULA (set TAP_DIR)"
[[ -z "$(git status --porcelain)" ]] || die "working tree not clean — commit or stash first"
git rev-parse "v$V" >/dev/null 2>&1 && die "tag v$V already exists"

echo "$V" > VERSION
git add VERSION
git commit -q -m "chore: release v$V"
git tag -a "v$V" -m "tremor v$V"
git push -q origin main
git push -q origin "v$V"

REV="$(git rev-parse HEAD)"
gh release create "v$V" --repo "$REPO_SLUG" --title "v$V" --generate-notes

# Repoint the formula. macOS sed needs the empty -i argument.
/usr/bin/sed -i '' \
  -e "s|tag:[[:space:]]*\"v[0-9.]*\"|tag:      \"v$V\"|" \
  -e "s|revision:[[:space:]]*\"[0-9a-f]*\"|revision: \"$REV\"|" \
  -e "s|version[[:space:]]*\"[0-9.]*\"|version \"$V\"|" \
  "$FORMULA"

git -C "$TAP_DIR" add Formula/tremor.rb
git -C "$TAP_DIR" commit -q -m "tremor $V"
git -C "$TAP_DIR" push -q

echo "released v$V  ·  users update with:  brew update && brew upgrade tremor"
