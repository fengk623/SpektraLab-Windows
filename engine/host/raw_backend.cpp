#include "raw_backend.hpp"

#include <algorithm>
#include <cctype>

namespace spkhost {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// 0 auto, 1 libraw, 2 photocraft, -1 unknown.
int backend_word(const std::string& word) {
    const std::string w = lower(word);
    if (w.empty() || w == "auto") return 0;
    if (w == "libraw") return 1;
    if (w == "photocraft") return 2;
    return -1;
}

}  // namespace

const char* raw_backend_name(RawBackend backend) {
    return backend == RawBackend::Photocraft ? "photocraft" : "libraw";
}

const std::vector<AcceptedRaw>& accepted_photocraft_raws() {
    // Empty until a camera and compression mode pass acceptance on real
    // samples. Each entry names its record under desktop/docs/validation/.
    static const std::vector<AcceptedRaw> accepted = {};
    return accepted;
}

bool choose_raw_backend(const std::string& requested, const std::string& environment,
                        const RawIdentity& identity, const std::vector<AcceptedRaw>& accepted,
                        RawBackendChoice& out, std::string& error) {
    const int wire = backend_word(requested);
    if (wire < 0) { error = "decode.raw_backend must be auto, libraw or photocraft"; return false; }
    if (wire > 0) {
        out = {wire == 2 ? RawBackend::Photocraft : RawBackend::LibRaw, "decode.raw_backend"};
        return true;
    }
    const int env = backend_word(environment);
    if (env < 0) { error = "SPEKTRALAB_RAW_BACKEND must be auto, libraw or photocraft"; return false; }
    if (env > 0) {
        out = {env == 2 ? RawBackend::Photocraft : RawBackend::LibRaw, "SPEKTRALAB_RAW_BACKEND"};
        return true;
    }
    for (const AcceptedRaw& a : accepted) {
        if (lower(a.make) != lower(identity.make) || a.model != identity.model) continue;
        if (a.libraw_decoder != "*" && a.libraw_decoder != identity.libraw_decoder) continue;
        out = {RawBackend::Photocraft, "accepted: " + a.evidence};
        return true;
    }
    out = {RawBackend::LibRaw, "not on the accepted PhotoCraft list"};
    return true;
}

}  // namespace spkhost
