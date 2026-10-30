# spklib on Windows

The library helper of SpektraLab 2.0 (`spklib-host`, MIT, [JamesQiu2005/spektralab_library](https://github.com/JamesQiu2005/spektralab_library)) ported to Windows x64. `main` carries the macOS binary in `vendor/spklib/macos/`; this folder carries the Windows port while it is reviewed. Design: [RFC-W002](../../../docs/rfc/RFC-W002-spklib-windows.md). Requirements and acceptance: [PRD-W001](../../../docs/prd/PRD-W001-capture-one-library-on-windows.md).

| | |
|---|---|
| **Base** | spektralab_library `afafee63ba8be068271eb18a88a020c2a41d206b` (0.3.1) |
| **Patches** | `patches/0001…0003` (`git format-patch`): the platform layer and Windows paths; `ZPARENT = -1`; `build-windows.cmd`, the manifest, `fetch.py`, `tests/c1_acceptance.py`, 0.3.2. They are what the merge request to spektralab_library will contain. |
| **Toolchain** | llvm-mingw 20261006, ucrt, x86_64, SHA-256 `317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a` |
| **SQLite** | 3.54.1 amalgamation, SHA3-256 `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30` (checked by `third_party/sqlite/fetch.py`) |
| **Pins for CI** | `BUILD-INFO.env`, read by `.github/workflows/spklib-windows.yml` |

## Rebuilding

In a clone of spektralab_library at `afafee6`:

```
git am <this folder>\patches\*.patch
python third_party\sqlite\fetch.py
set LLVM_MINGW=<llvm-mingw-20261006-ucrt-x86_64 folder>
set SPKLIB_COMMIT=12a036ad5189
build-windows.cmd test
build-windows.cmd release
```

`release` writes `build\windows-x64\release\spklib-host.exe` and its `.sha256`. It is built without a timestamp or local paths, so the same sources and toolchain give the same bytes; the CI job checks that once a binary is vendored here.

## Acceptance

Against a real Capture One for Windows catalog and session: see the section below, filled in from the runs on the developer's PC (`tests/c1_acceptance.py` and spklib-tests' real-catalog pass). Until it is filled in, the Windows port is built and tested by CI only.
