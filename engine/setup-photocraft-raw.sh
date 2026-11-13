#!/usr/bin/env bash
# setup-photocraft-raw.sh -- PhotoCraft's `crates/raw`, pinned, verified and
# overlaid, staged where engine/raw-photocraft builds it from.
#
#   engine/setup-photocraft-raw.sh [deps-dir]   (default: <repo>/build/deps)
#
# 1. Fetches storytold/photocraft at the commit in
#    engine/third_party/photocraft-raw/PIN (by full SHA, so git verifies the
#    content) into <deps>/photocraft-<commit>, and checks that commit and the
#    git tree id of crates/raw against the pin. An existing checkout that does
#    not match is refused, never "fixed".
# 2. Stages <deps>/photocraft-raw: crates/raw/src and the crate's README as
#    committed upstream (written out of the object database, not the working
#    tree), PhotoCraft's LICENSE-MIT / LICENSE-APACHE / NOTICE and Cargo.lock,
#    then the tracked overlay in engine/third_party/photocraft-raw/files on
#    top (a standalone Cargo.toml and the linear f32 output). The stage is
#    derived, so a stale one is rebuilt.
#
# engine/raw-photocraft/Cargo.toml names the default location
# (<repo>/build/deps/photocraft-raw) as a path dependency, so a build that
# uses another deps directory must stage there too. Nothing is downloaded at
# CMake time; engine/cmake/PhotocraftRaw.cmake only verifies this stage.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
deps="${1:-$here/../build/deps}"
mkdir -p "$deps"
deps="$(cd "$deps" && pwd)"
command -v sha256sum >/dev/null || sha256sum() { shasum -a 256 "$@"; }

third="$here/third_party/photocraft-raw"
overlay="$third/files"
pin() { sed -n "s/^$1=//p" "$third/PIN" | tr -d '\r'; }
commit="$(pin commit)"
raw_tree="$(pin raw_tree)"
if [[ ! "$commit" =~ ^[0-9a-f]{40}$ || ! "$raw_tree" =~ ^[0-9a-f]{40}$ ]]; then
  echo "engine/third_party/photocraft-raw/PIN does not hold a commit and a tree id" >&2
  exit 1
fi
if [[ ! -f "$overlay/Cargo.toml" || ! -f "$overlay/src/develop.rs" || ! -f "$overlay/src/lib.rs" ]]; then
  echo "Missing tracked PhotoCraft raw overlay: $overlay" >&2
  exit 1
fi

url=https://github.com/storytold/photocraft.git
checkout="$deps/photocraft-$commit"
stage="$deps/photocraft-raw"

verify_checkout() {   # $1 = a bare-ish fetch of the pinned commit
  [[ "$(git -C "$1" rev-parse 'pinned^{commit}' 2>/dev/null)" == "$commit" ]] &&
  [[ "$(git -C "$1" rev-parse 'pinned:crates/raw' 2>/dev/null)" == "$raw_tree" ]]
}

if [[ -d "$checkout" ]]; then
  verify_checkout "$checkout" || { echo "$checkout does not hold PhotoCraft $commit; not touched." >&2; exit 1; }
else
  rm -rf "$checkout.partial"
  git init -q --bare "$checkout.partial"
  # One commit, no history. The server sends exactly the objects of that
  # commit; git names them by their hash, which is the verification.
  git -C "$checkout.partial" fetch -q --depth 1 "$url" "$commit"
  git -C "$checkout.partial" update-ref refs/tags/pinned FETCH_HEAD
  verify_checkout "$checkout.partial" || {
    rm -rf "$checkout.partial"
    echo "$url $commit does not match its pin." >&2
    exit 1
  }
  mv "$checkout.partial" "$checkout"
fi

# The stamp names everything the stage is made of.
stamp="$(
  echo "commit=$commit"
  echo "raw_tree=$raw_tree"
  (cd "$overlay" && find . -type f | LC_ALL=C sort | while read -r f; do
    # Text files; a checkout with CRLF must stamp the same as one with LF.
    echo "overlay ${f#./} $(tr -d '\r' < "$f" | sha256sum | cut -d' ' -f1)"
  done)
)"
if [[ -f "$stage/.spk-pin" && "$(cat "$stage/.spk-pin")" == "$stamp" ]]; then
  echo "PhotoCraft raw $commit staged in $stage" >&2
  echo "$stage"
  exit 0
fi

rm -rf "$stage.partial"
mkdir -p "$stage.partial"
git -C "$checkout" archive --format=tar -o "$stage.partial/raw.tar" pinned crates/raw/src crates/raw/README.md
tar -xf "$stage.partial/raw.tar" -C "$stage.partial" --strip-components=2
rm "$stage.partial/raw.tar"
for notice in LICENSE-MIT LICENSE-APACHE NOTICE Cargo.lock; do
  git -C "$checkout" show "pinned:$notice" > "$stage.partial/$notice"
done
mv "$stage.partial/Cargo.lock" "$stage.partial/photocraft-Cargo.lock"
# The overlay replaces whole files; every one it replaces must exist upstream,
# so a pin that renamed them fails here instead of leaving two copies.
(cd "$overlay" && find . -type f | LC_ALL=C sort) | while read -r f; do
  f="${f#./}"
  if [[ "$f" != Cargo.toml && ! -f "$stage.partial/$f" ]]; then
    echo "The overlay replaces $f, which PhotoCraft $commit does not have." >&2
    exit 1
  fi
  mkdir -p "$stage.partial/$(dirname "$f")"
  tr -d '\r' < "$overlay/$f" > "$stage.partial/$f"
done
printf '%s\n' "$stamp" > "$stage.partial/.spk-pin"
rm -rf "$stage"
mv "$stage.partial" "$stage"
echo "PhotoCraft raw $commit staged in $stage" >&2
echo "$stage"
