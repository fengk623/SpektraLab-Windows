// decode_raw_photocraft_calibrated: PhotoCraft's pixels with LibRaw's metadata,
// and the two decoders side by side on the same synthetic DNG.
//
//   spk_raw_photocraft_calibrated <fixtures dir>
//
// The fixture is a DNG, so PhotoCraft develops it with the file's own
// ColorMatrix; what LibRaw contributes here is the metadata the host relies
// on (identity, sizes, the XYZ -> camera matrix for Kelvin/tint, the decoder
// name). The non-DNG case -- LibRaw's matrix as PhotoCraft's calibration --
// is exercised with a supplied matrix in engine/raw-photocraft/tests and with
// real camera files by hand; no camera file is in this repository.
//
// The side-by-side numbers are printed, and bounded loosely: the two paths
// are not expected to agree to rounding. LibRaw's compatible16 quantises to
// 16 bits, clamps to 0..1 and, for a DNG, applies its matrix without the
// chromatic adaptation the DNG specification prescribes.
#include "io/raw_decoder.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "fixtures/photocraft/expected.h"

namespace {
namespace fs = std::filesystem;
using spk::io::DecodedRaw;
using spk::io::PhotocraftStatus;

void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }

struct Mean { double rgb[3]; };
Mean patch(const DecodedRaw& image, int index) {
    using namespace spk_fixture;
    const int px = index % kPatchColumns * kPatch, py = index / kPatchColumns * kPatch;
    Mean m{};
    int count = 0;
    for (int y = py + 8; y < py + kPatch - 8; ++y)
        for (int x = px + 8; x < px + kPatch - 8; ++x, ++count)
            for (int c = 0; c < 3; ++c) m.rgb[c] += image.rgb[(std::size_t(y) * image.width + x) * 3 + c];
    for (double& v : m.rgb) v /= count;
    return m;
}

int index_of(const char* name) {
    using namespace spk_fixture;
    for (int i = 0; i < int(sizeof kPatches / sizeof kPatches[0]); ++i)
        if (std::string(kPatches[i].name) == name) return i;
    throw std::runtime_error(std::string("no patch ") + name);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: spk_raw_photocraft_calibrated <fixtures>");
        const fs::path fixtures = argv[1];
        using namespace spk_fixture;
        const int patches = int(sizeof kPatches / sizeof kPatches[0]);

        DecodedRaw photocraft;
        std::string error;
        PhotocraftStatus status = spk::io::decode_raw_photocraft_calibrated(fixtures / "patches.dng", nullptr, photocraft, error);
        require(status == PhotocraftStatus::Ok, "PhotoCraft: " + error);
        require(photocraft.width == std::uint32_t(kWidth) && photocraft.height == std::uint32_t(kHeight), "frame size");
        for (int i = 0; i < patches; ++i)
            for (int c = 0; c < 3; ++c) {
                const double want = kPatches[i].rgb[c], got = patch(photocraft, i).rgb[c];
                require(std::abs(got - want) < 2e-5 + 2e-5 * std::abs(want),
                        std::string("patch ") + kPatches[i].name + " is " + std::to_string(got) + ", expected " + std::to_string(want));
            }

        // The metadata contract host code reads.
        const auto& m = photocraft.metadata;
        require(m.backend == "photocraft" && m.photocraft.has_value(), "backend");
        require(m.decoder_name == "photocraft-raw DNG", "decoder name: " + m.decoder_name);
        require(!m.libraw_decoder_name.empty() && !m.libraw_version.empty(), "LibRaw identity of the file");
        require(m.make == "SpektraLab" && m.colour_channels == 3, "LibRaw make / colours: " + m.make);
        require(m.photocraft->calibration == spk::io::PhotocraftCalibration::DngEmbedded &&
                    m.photocraft->wb_source == spk::io::PhotocraftWbSource::FileAsShot, "sources");
        require(m.white_balance_exposure_restore == 1.0 && !m.headroom_enabled, "exposure convention");
        for (int c = 0; c < 3; ++c)
            require(std::abs(m.post_scale_pre_mul[c] - kWbMultipliers[c]) < 1e-4, "applied multipliers");
        // camera_to_output times the white-balanced camera value of a patch is the patch.
        const int clip = index_of("clip");
        for (int row = 0; row < 3; ++row) {
            const double white = m.camera_to_output[row * 4] + m.camera_to_output[row * 4 + 1] + m.camera_to_output[row * 4 + 2];
            require(std::abs(white - patch(photocraft, clip).rgb[row]) < 1e-4, "camera_to_output");
        }
        // LibRaw keeps a DNG's matrices in its DNG colour block and leaves
        // cam_xyz empty, with either decoder; the host's Kelvin/tint for a DNG
        // is unavailable on both paths for that reason. Reported, not required.
        double weight = 0;
        for (float v : m.camera_from_xyz) weight += std::abs(v);
        std::cout << "LibRaw XYZ -> camera matrix for this DNG: " << (weight > 0 ? "present" : "absent") << '\n';
        require(photocraft.timings.total_ms > 0 && photocraft.timings.process_ms > 0, "timings");
        for (const std::string& w : m.photocraft->warnings) std::cout << "warning: " << w << '\n';
        require(m.photocraft->warnings.empty(), "PhotoCraft and LibRaw read the fixture differently");

        // Rotated: both decoders apply orientation 6 and agree on the frame.
        DecodedRaw turned;
        status = spk::io::decode_raw_photocraft_calibrated(fixtures / "patches_rot6.dng", nullptr, turned, error);
        require(status == PhotocraftStatus::Ok, "rotated: " + error);
        require(turned.width == std::uint32_t(kHeight) && turned.height == std::uint32_t(kWidth), "rotated frame size");
        require(turned.metadata.photocraft->warnings.empty(), "rotated: the decoders disagree on orientation or area");

        // A caller's white balance: the file's own multipliers give the same patches.
        const std::array<float, 4> same = {float(kWbMultipliers[0]), float(kWbMultipliers[1]), float(kWbMultipliers[2]), float(kWbMultipliers[1])};
        DecodedRaw custom;
        status = spk::io::decode_raw_photocraft_calibrated(fixtures / "patches.dng", &same, custom, error);
        require(status == PhotocraftStatus::Ok, "custom white balance: " + error);
        require(custom.metadata.photocraft->wb_source == spk::io::PhotocraftWbSource::CallerExplicit, "custom source");
        for (int i = 0; i < patches; ++i)
            for (int c = 0; c < 3; ++c)
                require(std::abs(patch(custom, i).rgb[c] - kPatches[i].rgb[c]) < 1e-4, "custom white balance changed a patch");
        const std::array<float, 4> invalid = {0.0f, 1.0f, 1.0f, 1.0f};
        DecodedRaw prior;
        prior.width = 7;
        prior.rgb = {1.0f};
        status = spk::io::decode_raw_photocraft_calibrated(fixtures / "patches.dng", &invalid, prior, error);
        require(status == PhotocraftStatus::InvalidArgument && !error.empty() && prior.width == 7 && prior.rgb.size() == 1,
                "an invalid multiplier was accepted or changed the output");
        status = spk::io::decode_raw_photocraft_calibrated(fixtures / "absent.dng", nullptr, prior, error);
        require(status != PhotocraftStatus::Ok && !error.empty() && prior.width == 7, "a missing file was accepted");

        // The LibRaw path on the same file. Greys must agree closely (both map
        // the as-shot neutral to white); colours are reported.
        DecodedRaw libraw;
        require(spk::io::decode_raw_compatible(fixtures / "patches.dng", libraw, error), "LibRaw: " + error);
        require(libraw.metadata.backend == "libraw" && libraw.width == photocraft.width && libraw.height == photocraft.height,
                "LibRaw frame");
        std::cout << "patch       PhotoCraft (R G B)              LibRaw compatible16 (R G B)\n";
        double worst_grey = 0, worst_colour = 0;
        for (int i = 0; i < patches; ++i) {
            const Mean a = patch(photocraft, i), b = patch(libraw, i);
            std::cout << kPatches[i].name << "  " << a.rgb[0] << ' ' << a.rgb[1] << ' ' << a.rgb[2] << "    "
                      << b.rgb[0] << ' ' << b.rgb[1] << ' ' << b.rgb[2] << '\n';
            const bool grey = std::string(kPatches[i].name).find("grey") != std::string::npos || i == clip;
            for (int c = 0; c < 3; ++c) {
                // compatible16 cannot hold a negative; compare what it can.
                const double d = std::abs(std::max(a.rgb[c], 0.0) - b.rgb[c]);
                (grey ? worst_grey : worst_colour) = std::max(grey ? worst_grey : worst_colour, d);
            }
        }
        std::cout << "largest difference: neutral patches " << worst_grey << ", colour patches " << worst_colour << '\n';
        require(worst_grey < 5e-3, "the two decoders place a neutral differently");
        require(worst_colour < 0.1, "the two decoders disagree on colour beyond the adaptation difference");
        require(patch(libraw, index_of("blue")).rgb[1] >= 0 && patch(photocraft, index_of("blue")).rgb[1] < -0.17,
                "the out-of-gamut patch: compatible16 clamps it, PhotoCraft must not");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "spk_raw_photocraft_calibrated: " << e.what() << '\n';
        return 1;
    }
}
