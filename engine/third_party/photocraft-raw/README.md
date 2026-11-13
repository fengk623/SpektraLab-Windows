# PhotoCraft raw overlay

The pin and a small tracked overlay for PhotoCraft's `crates/raw`
([storytold/photocraft](https://github.com/storytold/photocraft)), the
clean-room, pure-Rust camera raw decoder and developer that
`engine/raw-photocraft` wraps for the Windows/Linux host. Nothing in this
directory is compiled in place: `engine/setup-photocraft-raw.sh` / `.ps1`
fetch the pinned source, verify it, and stage it with the overlay on top in
`build/deps/photocraft-raw`.

## Pin

`PIN` holds the commit (`e052eabf4be74612725400955d184f85d208c0f0`,
PhotoCraft 0.23.1) and the git tree id of `crates/raw` at that commit
(`ca85873ac8124df98d2b7ee4263bec9987454cbd`). The setup scripts fetch that
commit by its full SHA and check both; a checkout that does not match is
refused. `engine/raw-photocraft/src/lib.rs` names the same commit, and the
`spk_raw_photocraft` CTest fails when the two differ.

To move the pin: change `PIN`, rebase the overlay files onto the new upstream
files, update the constant in `lib.rs`, re-pin `engine/raw-photocraft/Cargo.lock`
(`engine/tools/pin_photocraft_lock.py`), and re-run the whole test matrix in
`engine/WINDOWS_PHOTOCRAFT_RAW.md`. The pin is a reviewed change: this crate
parses untrusted files.

## Why an overlay

PhotoCraft's public `develop` / `develop_sensor` return 16-bit RGB in ProPhoto
RGB with a gamma of 1.8, after an optional tone curve and a clamp to 0..1. The
engine develops scene-linear radiance, and SpektraLab's acceptance format is a
32-bit float linear TIFF, so the host needs the frame *before* tone, encoding
and quantisation. That frame exists inside `develop`, on a private path.

## Contents

Files under `files/` replace the file of the same path in `crates/raw`:

| Path | Change |
|---|---|
| `src/develop.rs` | Adds `DevelopedLinear` and `develop_sensor_linear`: the same linearization, levels, white balance, highlight clip, demosaic, colour conversion, exposure and orientation as `develop_sensor`, returning the f32 linear ProPhoto frame. The body of `develop` moves into `develop_frame`, shared by both; the 16-bit path is unchanged (its tests still compare the fused and full-frame paths bit for bit). `orient` becomes generic over the sample type. Four tests are added. |
| `src/lib.rs` | Exports `DevelopedLinear`, `develop_sensor_linear`, `develop_linear` and `LINEAR_OUTPUT_SPACE`. |
| `Cargo.toml` | A standalone manifest. Upstream's inherits its version, edition, lints and dependency versions from the PhotoCraft workspace root, which is not staged; this one states the same values. The `testgen` / `corpus` features and the dev-dependencies are left out. |

To see the change as a diff:

```sh
engine/setup-photocraft-raw.sh
git -C build/deps/photocraft-e052eabf4be74612725400955d184f85d208c0f0 \
    show pinned:crates/raw/src/develop.rs | diff -u - engine/third_party/photocraft-raw/files/src/develop.rs
```

What linear output does and does not carry is stated on `DevelopedLinear` and
`develop_sensor_linear` in `src/develop.rs`. In short: no tone curve, no black
point, no Dehaze or local tone (asking for them is an error), no clamp, no
quantisation; the white-balanced sensor data is still clipped at the white
level before demosaicing, as upstream.

The overlay is the smallest change that exposes that frame. It is a candidate
to offer upstream; until PhotoCraft has an equivalent, the overlay stays.

## Licence

PhotoCraft is licensed `MIT OR Apache-2.0`; we take it under either. The
overlay files are modified copies of upstream files and stay under the same
terms. The setup scripts stage PhotoCraft's `LICENSE-MIT`, `LICENSE-APACHE` and
`NOTICE` beside the source, and the build ships them, with the licences of the
Rust crates linked in, under `third-party/` (see
`engine/WINDOWS_PHOTOCRAFT_RAW.md`, "Licences"). Both licences are compatible
with this repository's GPL-3.0-or-later.

PhotoCraft's own statement of provenance (its `crates/raw/README.md`) is that
the decoder is implemented from public specifications, papers and observation
of files, with no code from dcraw, LibRaw, rawspeed, rawler, rawloader or
darktable, and no copied camera colour tables. That last point is why this
integration supplies LibRaw's colour matrices for non-DNG files rather than
expecting PhotoCraft to have them.
