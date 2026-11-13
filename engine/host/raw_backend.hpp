// raw_backend.hpp -- which decoder develops a RAW file.
//
// Two decoders sit behind decode_linear_prophoto: LibRaw (compatible16 /
// headroom, io/raw_decoder.hpp) and PhotoCraft (io/raw_photocraft.hpp, with
// LibRaw's colour matrix for non-DNG files). This is the one place that
// decides between them, so opening a file, dropping it on the window,
// stepping through a folder and a batch export all get the same answer for
// the same file: they all decode through decode_linear_prophoto.
//
// The rule:
//   1. `decode.raw_backend` on the wire: "libraw" or "photocraft" is obeyed;
//      "auto" (the default) goes on.
//   2. SPEKTRALAB_RAW_BACKEND in the host's environment: "libraw" or
//      "photocraft" is obeyed, for testing a build without touching the app;
//      unset, empty or "auto" goes on. Anything else is a configuration
//      error, like an invalid SPEKTRALAB_RAW_THREADS.
//   3. The accepted list: a file whose camera and compression mode have been
//      accepted on real samples goes to PhotoCraft. Everything else stays on
//      LibRaw, the compatibility path.
//
// There is no fallback from one decoder to the other. A file sent to
// PhotoCraft that PhotoCraft does not develop fails with PhotoCraft's reason;
// it is never quietly re-decoded by LibRaw into a different picture.
//
// The accepted list starts empty: acceptance needs real camera files, on a
// real GPU, compared against the LibRaw path (engine/WINDOWS_PHOTOCRAFT_RAW.md
// says how, and records each one). An entry is added with its evidence.
#pragma once

#include <string>
#include <vector>

namespace spkhost {

enum class RawBackend { LibRaw, Photocraft };
const char* raw_backend_name(RawBackend backend);

// What identifies a RAW file for acceptance: the camera as LibRaw names it
// and LibRaw's sensor-data decoder, which is its compression mode.
struct RawIdentity {
    std::string make, model, libraw_decoder;
};

// One accepted camera and compression mode. `libraw_decoder` "*" matches any.
struct AcceptedRaw {
    std::string make, model, libraw_decoder;
    std::string evidence;   // where the acceptance is recorded
};
const std::vector<AcceptedRaw>& accepted_photocraft_raws();

struct RawBackendChoice {
    RawBackend backend = RawBackend::LibRaw;
    // "decode.raw_backend", "SPEKTRALAB_RAW_BACKEND", "accepted: <evidence>",
    // "not on the accepted PhotoCraft list".
    std::string reason;
};

// `requested` is decode.raw_backend; `environment` the value of
// SPEKTRALAB_RAW_BACKEND ("" when unset). False with `error` on an unknown
// name in either. `accepted` is the list to consult (the built-in one in the
// host; a test passes its own).
bool choose_raw_backend(const std::string& requested, const std::string& environment,
                        const RawIdentity& identity, const std::vector<AcceptedRaw>& accepted,
                        RawBackendChoice& out, std::string& error);

}  // namespace spkhost
