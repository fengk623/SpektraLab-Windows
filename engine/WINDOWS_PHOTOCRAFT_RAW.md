# PhotoCraft RAW decode in the Windows/Linux host

The host can develop RAW files with PhotoCraft's pure-Rust decoder as well as
with LibRaw. This file is the design record, the boundary contract, the
support state and the way to verify it. The LibRaw paths are unchanged and
remain the reference: [WINDOWS_NATIVE_RAW.md](WINDOWS_NATIVE_RAW.md).

Baselines: SpektraLab `Windows_Linux` at
`894915fb7f93656d2951eaf11a8be35e4ff60246`; PhotoCraft at
`e052eabf4be74612725400955d184f85d208c0f0` (0.23.1), `crates/raw` tree
`ca85873ac8124df98d2b7ee4263bec9987454cbd`.

## What was found in PhotoCraft, and what follows from it

Read from the pinned source, before any code was written:

1. **The public `develop` returns gamma-1.8 16-bit ProPhoto, clamped to 0..1**
   (and after the camera profile's tone curve, when there is one). The linear
   f32 frame exists only inside it. The engine develops scene-linear radiance,
   so the frame has to be taken before tone, encoding and quantisation: a
   small tracked overlay adds that output
   ([third_party/photocraft-raw](third_party/photocraft-raw/README.md)).
2. **PhotoCraft has colour calibration for DNG only** (the file's own
   matrices) and for one measured body (Nikon D4). It is a clean-room decoder
   and carries no camera colour tables. For ARW, RAF, NEF, CR2 and the rest it
   either fits a matrix and a tone curve to the camera's embedded JPEG, which
   is a display rendering and not scene-linear, or treats the white-balanced
   camera channels as sRGB primaries. Neither is acceptable as engine input,
   so the host supplies the calibration: LibRaw's XYZ -> camera matrix
   (`cam_xyz`, the D65 Adobe matrix the host's Kelvin/tint already uses),
   handed to PhotoCraft as a DNG-style ColorMatrix. PhotoCraft then applies it
   by the DNG specification's method (Bradford adaptation from the as-shot
   white to D50). A file with no calibration from either side is refused.
3. **Canon CR3 is not decoded** by this PhotoCraft revision (it falls back to
   the embedded JPEG upstream; here it is `unsupported`). Canon coverage is
   CR2. Nikon HE/HE*, Sony "Compressed RAW 2", compressed Pentax PEF and
   compressed Olympus ORF are likewise not decoded. The crate's own matrix of
   what it decodes is its `README.md`, staged in `build/deps/photocraft-raw`.
4. **Sensor data is clipped at the white-balanced white level before
   demosaicing** (multipliers normalised to a minimum of 1), the same
   convention as LibRaw `compatible16`. Clipped highlights are neutral and are
   not reconstructed. What the float frame adds over `compatible16` is
   everything after that point: no 16-bit quantisation, colours outside
   ProPhoto keep their negative components, and exposure gain or matrix
   overshoot can exceed 1.

## Shape

```
RAW file --(read whole, C++)--> bytes
   LibRaw open_file, metadata only: make/model, sizes, flip, cam_xyz, cam_mul
bytes + options --C ABI--> spk-photocraft-raw (Rust static library)
   photocraft_raw::decode            container, decompression  -> Sensor
   colour: DNG's own | LibRaw matrix | refused
   white balance: file as-shot | LibRaw as-shot | caller multipliers | refused
   photocraft_raw::develop_sensor_linear   levels, WB, clip, demosaic,
                                           camera -> XYZ D50 -> ProPhoto, orient
   <-- result handle: f32 RGB, report, warnings
copy into the host's frame; free the handle
```

| piece | where |
|---|---|
| Pin, overlay, standalone manifest | `engine/third_party/photocraft-raw/` |
| Fetch, verify, stage | `engine/setup-photocraft-raw.sh`, `.ps1` -> `build/deps/photocraft-raw` |
| Rust wrapper and C ABI | `engine/raw-photocraft/` (`src/lib.rs`), `Cargo.lock`, `rust-toolchain.toml` |
| C header | `engine/src/io/photocraft_raw_abi.h` |
| C++ bridge (no LibRaw) | `engine/src/io/raw_photocraft.{hpp,cpp}` |
| PhotoCraft pixels + LibRaw metadata | `spk::io::decode_raw_photocraft_calibrated` in `engine/src/io/raw_decoder.{hpp,cpp}` |
| Which decoder | `engine/host/raw_backend.{hpp,cpp}` |
| Host boundary | `decode_linear_prophoto` in `engine/host/image_io.cpp` |
| Linear ProPhoto ICC profile | `engine/src/io/linear_prophoto_icc.{hpp,cpp}` |
| Float TIFF | `encode_tiff_file(..., bits = 32, ...)` in `engine/host/output.cpp` |
| RAW -> TIFF entry | `spektralab-host raw-to-tiff`, `engine/host/raw_tiff_cli.{hpp,cpp}` |
| Build | `engine/cmake/PhotocraftRaw.cmake`; `-DSPEKTRALAB_PHOTOCRAFT_RAW=OFF` builds without it |

### Why a static library and a C ABI

One process, one binary, as before: the Windows package does not gain a file,
and the frame is handed over in memory. The alternatives were a helper
executable (a temporary file or a pipe per frame, a second binary to stage,
sign and audit) or a DLL (a second runtime file with its own search path).
The cost is a second language in the link, which is what the boundary rules
below are for.

### The boundary

* **No path and no handle crosses it.** The host reads the file (a
  `std::filesystem::path`, so non-ASCII names work on Windows) and passes
  bytes. The Rust side does no I/O. The source is opened for reading only.
* **Ownership.** `spk_pcraw_develop` always returns a result handle. The
  handle and every pointer its accessors return belong to the library until
  `spk_pcraw_free`. The C++ bridge holds the handle in a `unique_ptr`, copies
  the pixels into the caller's `std::vector<float>`, and frees it on every
  path. Nothing allocated on one side is freed on the other.
* **Status and errors.** Every result has a status (`ok`, `not_raw`,
  `unsupported`, `malformed`, `limit_exceeded`, `invalid_argument`,
  `internal`) and, when not `ok`, a message. The bridge adds `unavailable`
  (built without the library) and `io`. On failure the caller's image is
  left as it was.
* **No panic leaves the library.** Each entry point runs under
  `catch_unwind`; a panic becomes `internal` with its message. PhotoCraft's
  own code forbids `unsafe` and denies `unwrap` / `expect` / `panic`.
* **Versioned.** Both sides state the ABI version and the struct sizes; a
  mismatch is `invalid_argument`, and `static_assert`s in the header and
  `const` assertions in `lib.rs` fix the layouts at compile time.
* **Limits.** PhotoCraft checks declared sizes against limits before it
  allocates. The wrapper's defaults are 2^28 pixels and 8 GiB for one buffer
  and for peak working memory (PhotoCraft's own 2 GiB default refuses a
  102 MP frame once it has to be rotated). A file over 2 GiB is not read.
* **Threads.** The decode runs on a rayon pool of `raw_worker_limit()`
  threads (`SPEKTRALAB_RAW_THREADS`, default `min(8, logical CPUs)`), built
  for the call and torn down after it.

What is **not** bounded: a real out-of-memory inside the budget aborts the
process (Rust's allocation failure handler; C++ `bad_alloc` in the bridge is
caught and reported). The budget, not a recovery path, is the protection.

### Memory

The frame exists twice for a moment: in the library's result and in the
host's vector (12 bytes a pixel each), plus PhotoCraft's own working set
while it develops (the CFA plane, 4 bytes a pixel, and the f32 frame; a
second f32 frame when the orientation moves pixels). For a 102 MP frame that
is about 1.2 GiB per copy. `raw-to-tiff` reports the process's peak at three
points. No measurement on a real 102 MP file has been made yet.

## Pixel and colour contract

* Linear ProPhoto RGB (ROMM primaries, D50 white), 32-bit float, top row
  first, orientation applied, not clamped, finite.
* Black level, linearization and white level: PhotoCraft's reading of the
  file. They can differ from LibRaw's for the same camera; the report gives
  both sides' numbers where it has them.
* White balance: the file's as-shot neutral as PhotoCraft reads it; LibRaw's
  as-shot multipliers when PhotoCraft reads none; the caller's multipliers
  for a custom Temperature/Tint (computed by the host from LibRaw's matrix,
  as for the LibRaw paths). Never a grey-world estimate.
* Camera -> D50: the DNG's own ColorMatrix / ForwardMatrix for a DNG;
  LibRaw's D65 matrix through the DNG specification's method for everything
  else.
* Exposure: sensor range scaled to 0..1, multipliers normalised to a minimum
  of 1, matrix normalised so the balanced white has Y = 1. `BaselineExposure`
  (DNG) is **not** applied by the host, to keep the LibRaw paths' convention
  (LibRaw ignores it); the report records its value. The wrapper can apply it
  (`apply_baseline_exposure`).
* Not applied: any tone curve, black point, look, Dehaze or local tone.
  Film curves and display transforms stay where they are, after the decode.

### Differences from the LibRaw path

| | LibRaw `compatible16` | PhotoCraft |
|---|---|---|
| Sample storage | 16-bit, then float | float throughout after the sensor data |
| Range | clamped to 0..1 | not clamped: negatives and values above 1 kept |
| Chromatic adaptation | none: LibRaw's matrix maps the balanced white to the output white directly | Bradford, from the as-shot white to D50 (DNG specification) |
| Demosaic | LibRaw's AHD (Bayer), its own X-Trans | PhotoCraft's AHD (Bayer), its own edge-directed method for X-Trans |
| `BaselineExposure` | ignored | ignored by the host (available) |
| Image area | LibRaw's crop | PhotoCraft's crop; a difference is reported as a warning |
| Orientation | LibRaw's `flip` | the file's TIFF orientation as PhotoCraft reads it; a difference is reported |

Measured on the synthetic DNG fixture (`spk_raw_photocraft_calibrated`; a
local build, see "What has and has not been verified"): neutral patches
agree to 2.3e-4; colour patches differ by up to 0.026, the largest being a
saturated blue that lies outside ProPhoto, where `compatible16` clamps the
negative green to 0 and PhotoCraft keeps -0.176. No comparison on a camera
file exists yet.

## The linear TIFF

`raw-to-tiff` writes what the engine would be handed:

* **TIFF-01** RGB, three samples, 32-bit IEEE float (SampleFormat 3),
  uncompressed, classic little-endian TIFF, strips of at most 1 MiB. A frame
  over 4 GiB is refused (classic TIFF). `--format u16` writes 16-bit
  integers instead: clamp to 0..1, times 65535, round to nearest; the report
  counts what was clipped.
* **TIFF-02** ICC profile embedded: v4.3 matrix/TRC, ROMM colorants (the
  working-space matrix), D50, an identity curve. The bytes are fixed (a
  constant creation date), so the ICC Profile ID is the same in every build:
  `266b19b34ea10d114519d955b34e0142`, description
  `SpektraLab Linear ProPhoto RGB (ROMM, D50, gamma 1.0)`. The host
  recognises exactly that profile on a float TIFF and takes the samples as
  they are; any other profile goes through the usual conversion. So the TIFF
  is processed once: no second white balance, orientation, colour conversion
  or transfer decode.
* **TIFF-03** The frame's own size, pixels upright, Orientation 1. Make,
  model, lens, DateTimeOriginal, ISO, exposure time, aperture and focal
  length are written when the source has them; the report lists the missing
  ones.
* **TIFF-04** Levels, demosaic, as-shot white balance, exposure and the
  camera -> D50 conversion as stated above; the report records the white
  balance source and multipliers, the calibration source, the levels, the
  matrix used and `BaselineExposure`.

A float TIFF reads back bit for bit (`spk_host_raw_photocraft`, and
`raw-to-tiff`'s own verification on every run).

## Which decoder develops a file

One rule, in `engine/host/raw_backend.hpp`, consulted by the one function
every decode goes through (`decode_linear_prophoto`): opening a file, dropping
it on the window, stepping through a folder and a batch export cannot
disagree.

1. `decode.raw_backend` on the wire: `libraw` or `photocraft` is obeyed.
2. `SPEKTRALAB_RAW_BACKEND` in the host's environment: `libraw` or
   `photocraft` is obeyed. This is how a tester runs the whole app on
   PhotoCraft without a frontend change.
3. The accepted list: PhotoCraft for a camera (as LibRaw names it) and
   compression mode (LibRaw's decoder name for the file) that has passed
   acceptance on real samples. LibRaw for everything else.

No fallback in either direction: a file sent to PhotoCraft that it does not
develop fails with its reason.

**The accepted list is empty.** Acceptance needs real camera files on a real
GPU and has not happened. Until an entry is added, the app behaves as before
unless one of the first two rules is used.

### Migration state

| Input | Default path | To move it to PhotoCraft |
|---|---|---|
| Every RAW the app opens today | LibRaw (`compatible16` / `headroom`) | pass acceptance below, add an `AcceptedRaw` entry with its record |
| Canon CR3; Nikon HE/HE*; Phase One IIQ; Sony ARQ; formats PhotoCraft does not decode | LibRaw (where LibRaw decodes them) | needs decoder support in PhotoCraft first |
| Thumbnails and embedded previews | LibRaw | not part of this change |

Sample status (TEST-01). None has been run: no camera file is available in
the environment this was developed in.

| Sample | Compression / depth | LibRaw path | PhotoCraft path |
|---|---|---|---|
| Sony α7R V ARW | to be read from the file | pending | pending |
| Fujifilm ~102 MP RAF | to be read from the file | pending (GFX100RF compatible16 / headroom accepted 2026-10-29) | pending |
| Canon RAW (CR2; CR3 is not decoded by PhotoCraft) | to be read from the file | pending | pending |
| Nikon lossless NEF | to be read from the file | pending | pending |
| DNG | to be read from the file | pending | pending |
| Nikon HE/HE*, Phase One IIQ, Sony ARQ | | as recorded in RAW-SUPPORT-20261029.md | not decoded by PhotoCraft |

The procedure and the record template are
`desktop/docs/validation/RAW-PHOTOCRAFT.md`.

## Build

```sh
engine/setup-deps.sh                 # includes setup-photocraft-raw.sh
engine/build-linux.sh --test         # or engine/build-windows-cross.sh
```

On Windows: `engine/setup-photocraft-raw.ps1`, then the usual build.

* Rust: `engine/raw-photocraft/rust-toolchain.toml` pins 1.97.0 (PhotoCraft
  needs 1.95, edition 2024) and the `x86_64-pc-windows-gnu` target. rustup
  installs both on first use. CMake starts cargo in the crate directory so
  the pin applies.
* Crates: `engine/raw-photocraft/Cargo.lock`, held to the versions PhotoCraft
  itself locks (`engine/tools/pin_photocraft_lock.py --check`, run by CI).
  CMake builds with `--locked`.
* Targets: native on Linux; `x86_64-pc-windows-gnu` for the MinGW host, from
  Linux, with no Rust linker (a static library needs none);
  `x86_64-pc-windows-msvc` for the MSVC test job.
* The cross-built exe's import check allows four more system DLLs for Rust's
  standard library (`ntdll`, `userenv`, `bcryptprimitives`,
  `api-ms-win-core-synch-l1-2-0`).

## Licences

PhotoCraft: `MIT OR Apache-2.0`; its `LICENSE-MIT`, `LICENSE-APACHE` and
`NOTICE` ship in the host's `licenses/`. The Rust crates linked through it
are listed, with every licence file they ship, in
`licenses/Rust-crates-NOTICES.txt`, generated at build time from
`cargo metadata` by `engine/tools/rust_crate_notices.py`. That script is also
the gate: a crate whose licence expression cannot be met from its allowed
list (MIT, Apache-2.0, BSD-2/3-Clause, ISC, Zlib, 0BSD, Unlicense, CC0-1.0,
Unicode-3.0, BSL-1.0) fails the build. All are compatible with this
repository's GPL-3.0-or-later.

Read from PhotoCraft's lock file at the pin, the crates under `crates/raw`
are: `rayon`, `rayon-core`, `crossbeam-*`, `either` (MIT OR Apache-2.0);
`flate2`, `crc32fast`, `miniz_oxide`, `adler2`, `simd-adler32` (MIT and/or
Apache-2.0, Zlib, 0BSD); `zlib-rs` (Zlib); `zune-jpeg`, `zune-core` (MIT OR
Apache-2.0 OR Zlib); `jxl-oxide` and its `jxl-*` crates (MIT OR Apache-2.0);
`brotli-decompressor`, `alloc-no-stdlib`, `alloc-stdlib` (BSD-3-Clause and/or
MIT); `bytemuck`, `cfg-if`, `log`, `once_cell`, `tracing`, `tracing-core`,
`pin-project-lite` (MIT and/or Apache-2.0, Zlib). The licence expressions in
this paragraph are from memory of those crates; the build's generated notice
is the authority. CI's builds pass the gate with 34 linked crates. One of
them, `alloc-stdlib`, is published without a licence file; the notice takes
the text from `alloc-no-stdlib`, published from the same repository.

The decoder parses untrusted files. PhotoCraft pins `jxl-oxide` exactly for
that reason and so does the staged manifest.

## Verify

Automated, no camera file needed:

| Test | What it establishes |
|---|---|
| `photocraft-overlay` (CI job) | PhotoCraft's own `crates/raw` unit tests, with the overlay applied, in PhotoCraft's workspace with PhotoCraft's lock file; includes the four tests the overlay adds (linear equals the encoded frame within one 16-bit code for Bayer, X-Trans, LinearRaw and monochrome at four orientations; values below 0 and above 1 survive; tone left out; limits) |
| `spk_photocraft_raw_cargo` | The wrapper: a synthetic DNG develops to an independent reference (the DNG mapping written out in `engine/tools/make_photocraft_fixtures.py`), negatives and values above 1 survive, orientation, explicit white balance, a non-DNG needs the caller's matrix and white balance and then matches, damaged and oversized input, a bounded pool, the reported matrix reproduces the frame, the C ABI including bad calls |
| `spk_raw_photocraft` | The same across the real C boundary from C++; the library is the pinned commit; a non-ASCII path; failures leave the caller's image alone |
| `spk_raw_photocraft_calibrated` | PhotoCraft pixels with LibRaw metadata; the metadata contract the neutral picker and lens correction read; the two decoders side by side |
| `spk_host_raw_photocraft` | The backend rule; both decoders behind `decode_linear_prophoto`; the environment override; the float TIFF's tags, profile and bit-exact read-back; `raw-to-tiff` in f32 and u16; no intermediate file left after a failure; an ordinary file opens after a failed RAW |
| `spk_photocraft_fixtures` | The tracked fixtures are what the generator writes |
| "raw-to-tiff under wine" (CI step) | The cross-built Windows exe decodes with PhotoCraft and its float TIFF reads back exactly |

By hand, with camera files (the acceptance that is still open):

```powershell
# One file, both decoders, new output names each run.
& .\spektralab-host.exe raw-to-tiff --input D:\samples\A7R5.ARW `
    --output D:\out\A7R5-photocraft.tif --report D:\out\A7R5-photocraft.json --backend photocraft
& .\spektralab-host.exe raw-to-tiff --input D:\samples\A7R5.ARW `
    --output D:\out\A7R5-libraw.tif --report D:\out\A7R5-libraw.json --backend libraw
# The whole app on PhotoCraft:
$env:SPEKTRALAB_RAW_BACKEND = 'photocraft'; & 'C:\Program Files\SpektraLab\spektralab.exe'
```

## What has and has not been verified

**CI, on the personal fork** (`fengk623/SpektraLab-Windows`, branch
`codex/windows-photocraft-raw`, manual `workflow_dispatch`, `publish_windows`
off). Run
[43372366886](https://github.com/fengk623/SpektraLab-Windows/actions/runs/43372366886)
on `f284ff655bf3712c4b391a3b8455d2da944f31f8`, all seven jobs green, the
publish job skipped:

| Job | Result |
|---|---|
| Engine + host (Linux, lavapipe) | CTest 51/51 (46 before this change), the five new tests among them, built `--locked` with the real crates; host smoke passed |
| Engine + host (Windows x64, cross-built) | MinGW link of the Rust library; imports `ADVAPI32 KERNEL32 SHELL32 USER32 USERENV WS2_32 api-ms-win-core-synch-l1-2-0 bcryptprimitives msvcrt ntdll ole32`, all on the allowlist; host smoke under wine passed; `raw-to-tiff` under wine: PhotoCraft `e052eabf`, float TIFF read back exactly |
| Engine (Windows x64, MSVC, CTest) | 40 tests registered (36 before), 15 run (11 before), all passed on real Windows: the bridge test including Chinese file names, the calibrated test, the fixtures check, and the crate's own `cargo test --locked`; `setup-photocraft-raw.ps1` staged the source |
| PhotoCraft raw crate with the overlay | PhotoCraft's own `crates/raw` unit tests on the overlaid source, with PhotoCraft's lock file: 166 passed, 1 ignored (upstream's), 0 failed |
| Frontend checks | vitest 463 passed, 1 skipped; cargo 24 passed; Playwright 61 passed (unchanged counts) |
| Package (Linux) | built |
| Package (Windows NSIS) | installer 39.4 MB (37.3 MB before); silent install; installed and portable hosts answer `hello`; portable 961 files (957 before: the four new licence files) |

Also from CI: the lock file check (33 registry packages, each at PhotoCraft's
locked version) on all three engine jobs, and the licence gate (34 linked
Rust crates, all on the allowed list).

Two earlier runs on the branch are part of the record. Run 43369606838
(`b67a291`): everything above passed except `spk_raw_photocraft` on MSVC,
which failed in the test's own message formatting (`path::string()` throws on
Windows for a Chinese file name); the test was fixed, not the code under test.
Run 43370920743 (`2c85dba`): engine jobs green; superseded before packaging.

**Local** (Linux, GCC 13, Rust 1.97.0), before any of that, with sequential
stand-ins for `rayon` and the compressed-tile decoders because the
development workspace could not download crates: the overlay's `develop.rs`
tests, the wrapper's tests, the bridge and calibrated tests against a local
LibRaw 0.22.2, and the host boundary test with stand-ins for the codec, lens
and engine libraries. With the working-space fast path removed, the host test
fails on the bit-exact read-back. The side-by-side figures quoted above
(2.3e-4, 0.026) are from that local run of `spk_raw_photocraft_calibrated`;
CI runs the same test and enforces its bounds but its log was not read for
the figures.

**Not verified anywhere**, because it needs camera files and a Windows machine
with a GPU: every real camera file; compressed sensor data of any kind;
X-Trans; colour against the LibRaw path on real scenes; orientation and image
area on real files; lens correction and the neutral picker on a PhotoCraft
frame; custom white balance on a non-DNG; timing and memory at 45 MP and
102 MP; in-app adjustment and full-size export with PhotoCraft as the
decoder. The hosted Windows runner has no Vulkan driver, so even the engine
itself only runs under lavapipe in CI.

## Known limitations

* Colour for non-DNG files depends on LibRaw's matrix table. A camera LibRaw
  has no matrix for is refused by this path.
* LibRaw keeps a DNG's matrices outside `cam_xyz`, so the host's Kelvin/tint
  (custom white balance, as-shot temperature, the neutral picker) is
  unavailable for DNG. That is the existing behaviour with LibRaw as the
  decoder too; PhotoCraft does not change it.
* Highlights are clipped at the white-balanced white level, not
  reconstructed. `headroom`'s unclipped channels have no PhotoCraft
  equivalent.
* The frame is copied once across the boundary; peak memory is about two
  frames plus PhotoCraft's working set.
* A decode cannot be cancelled part-way; a superseded result is discarded
  when it arrives, as with LibRaw.
* Classic TIFF only: a linear TIFF over 4 GiB (about 357 MP in float) is
  refused.
* About 241 MP composites: within the pixel limit; rendering them is tracked
  separately and untested here.
