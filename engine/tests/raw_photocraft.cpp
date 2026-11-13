// The PhotoCraft RAW bridge (io/raw_photocraft.hpp) across its C boundary.
//
//   spk_raw_photocraft <fixtures dir> <engine/third_party/photocraft-raw/PIN> <scratch dir>
//
// The fixtures are synthetic DNGs whose expected linear ProPhoto values come
// from an independent implementation of the DNG specification's colour
// mapping (engine/tools/make_photocraft_fixtures.py, fixtures/photocraft/
// expected.h). A flat patch demosaics to itself away from its edges, so the
// interior of each patch is an exact target. What this does not cover: any
// real camera file, compressed sensor data, or X-Trans. Those need samples
// and are recorded by hand (engine/WINDOWS_PHOTOCRAFT_RAW.md).
#include "io/raw_photocraft.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fixtures/photocraft/expected.h"

namespace {
namespace fs = std::filesystem;
using spk::io::PhotocraftImage;
using spk::io::PhotocraftRequest;
using spk::io::PhotocraftStatus;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }
void require(bool ok, const std::string& what) { if (!ok) fail(what); }

std::vector<std::uint8_t> read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "cannot open " + path.string());
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write(const fs::path& path, const std::uint8_t* data, std::size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    require(bool(out), "cannot write " + path.string());
}

PhotocraftImage decode(const fs::path& path, const PhotocraftRequest& request = {}) {
    PhotocraftImage image;
    std::string error;
    const PhotocraftStatus status = spk::io::decode_raw_photocraft(path, request, image, error);
    require(status == PhotocraftStatus::Ok,
            path.filename().string() + ": " + spk::io::photocraft_status_name(status) + ": " + error);
    return image;
}

// Mean of the interior of patch `index` (8 pixels in from every edge), and the
// largest deviation from it, in an unrotated frame.
struct Patch { double mean[3]; double spread; };
Patch patch(const PhotocraftImage& image, int index) {
    using namespace spk_fixture;
    const int px = index % kPatchColumns * kPatch, py = index / kPatchColumns * kPatch;
    Patch p{};
    int count = 0;
    for (int y = py + 8; y < py + kPatch - 8; ++y)
        for (int x = px + 8; x < px + kPatch - 8; ++x, ++count)
            for (int c = 0; c < 3; ++c) p.mean[c] += image.rgb[(std::size_t(y) * image.width + x) * 3 + c];
    for (double& m : p.mean) m /= count;
    for (int y = py + 8; y < py + kPatch - 8; ++y)
        for (int x = px + 8; x < px + kPatch - 8; ++x)
            for (int c = 0; c < 3; ++c)
                p.spread = std::max(p.spread, std::abs(image.rgb[(std::size_t(y) * image.width + x) * 3 + c] - p.mean[c]));
    return p;
}

double check_patches(const PhotocraftImage& image, double gain, const std::string& what) {
    using namespace spk_fixture;
    require(image.width == std::uint32_t(kWidth) && image.height == std::uint32_t(kHeight), what + ": frame size");
    require(image.rgb.size() == std::size_t(kWidth) * kHeight * 3, what + ": sample count");
    double worst = 0;
    for (int i = 0; i < int(sizeof kPatches / sizeof kPatches[0]); ++i) {
        const Patch p = patch(image, i);
        require(p.spread < 1e-5, what + ": patch " + kPatches[i].name + " is not flat");
        for (int c = 0; c < 3; ++c) {
            const double want = kPatches[i].rgb[c] * gain, error = std::abs(p.mean[c] - want);
            worst = std::max(worst, error);
            require(error < 2e-5 + 2e-5 * std::abs(want),
                    what + ": patch " + kPatches[i].name + " channel " + std::to_string(c) + " is " +
                        std::to_string(p.mean[c]) + ", expected " + std::to_string(want));
        }
    }
    for (float v : image.rgb) require(std::isfinite(v), what + ": non-finite sample");
    return worst;
}

void expect_failure(PhotocraftStatus want, PhotocraftStatus got, const std::string& error,
                    const PhotocraftImage& sentinel, const std::string& what) {
    require(got == want, what + ": status " + spk::io::photocraft_status_name(got) + ", expected " +
                             spk::io::photocraft_status_name(want) + " (" + error + ")");
    require(!error.empty() && error != "stale", what + ": no error text");
    require(sentinel.width == 7 && sentinel.rgb == std::vector<float>({-0.25f, 0.5f, 4.0f}) &&
                sentinel.report.model == "previous",
            what + ": a failed decode changed the caller's image");
}

PhotocraftImage sentinel() {
    PhotocraftImage s;
    s.width = 7;
    s.height = 11;
    s.rgb = {-0.25f, 0.5f, 4.0f};
    s.report.model = "previous";
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4) fail("usage: spk_raw_photocraft <fixtures> <PIN> <scratch>");
        const fs::path fixtures = argv[1], scratch = argv[3];
        fs::create_directories(scratch);
        using namespace spk_fixture;

        // The library is the one this tree pins.
        require(spk::io::photocraft_available(), "this build has no PhotoCraft decoder");
        std::string pinned;
        {
            std::ifstream pin(argv[2]);
            for (std::string line; std::getline(pin, line);) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.rfind("commit=", 0) == 0) pinned = line.substr(7);
            }
        }
        require(pinned.size() == 40 && spk::io::photocraft_commit() == pinned,
                "the library says PhotoCraft " + spk::io::photocraft_commit() + ", PIN says " + pinned);

        // 1. The colour contract: every patch, to the reference's linear ProPhoto.
        const PhotocraftImage upright = decode(fixtures / "patches.dng");
        const double worst = check_patches(upright, 1.0, "as shot");
        const auto& r = upright.report;
        require(r.format == "DNG" && r.make == "SpektraLab" && r.model == "SpektraLab Fixture Patches", "identity");
        require(r.cfa == "RGGB" && !r.monochrome, "CFA");
        require(r.sensor_width == std::uint32_t(kWidth) && r.sensor_height == std::uint32_t(kHeight) &&
                    r.crop == std::array<std::uint32_t, 4>{0, 0, std::uint32_t(kWidth), std::uint32_t(kHeight)}, "sensor area");
        require(r.orientation == 1 && r.orientation_applied, "orientation");
        require(r.wb_source == spk::io::PhotocraftWbSource::FileAsShot, "white balance source");
        require(r.calibration == spk::io::PhotocraftCalibration::DngEmbedded, "calibration source");
        for (int c = 0; c < 3; ++c)
            require(std::abs(r.wb_multipliers[c] - kWbMultipliers[c]) < 1e-5, "white balance multipliers");
        require(r.baseline_exposure_ev == kBaselineExposureEv && !r.baseline_exposure_applied, "baseline exposure");
        require(r.black_level_min == kBlack && r.black_level_max == kBlack && r.white_level[1] == kWhite, "levels");
        require(r.warnings.empty(), "unexpected warning: " + (r.warnings.empty() ? std::string() : r.warnings[0]));
        require(r.decode_ms >= 0 && r.develop_ms > 0 && r.total_ms >= r.develop_ms, "timings");

        // 2. Nothing is clamped: a colour outside ProPhoto keeps its negative
        //    green, and with the baseline exposure applied the clipped patch is 2.
        int blue = -1, clip = -1;
        for (int i = 0; i < int(sizeof kPatches / sizeof kPatches[0]); ++i) {
            if (std::string(kPatches[i].name) == "blue") blue = i;
            if (std::string(kPatches[i].name) == "clip") clip = i;
        }
        require(blue >= 0 && clip >= 0 && kPatches[blue].rgb[1] < -0.1, "the fixture lost its out-of-gamut patch");
        require(patch(upright, blue).mean[1] < -0.17, "a negative component was clamped");
        PhotocraftRequest lifted_request;
        lifted_request.apply_baseline_exposure = true;
        const PhotocraftImage lifted = decode(fixtures / "patches.dng", lifted_request);
        check_patches(lifted, std::pow(2.0, kBaselineExposureEv), "baseline exposure applied");
        require(std::abs(patch(lifted, clip).mean[1] - 2.0) < 1e-3 && lifted.report.baseline_exposure_applied,
                "headroom above 1 was lost");
        // The reported matrix is the one the frame was made with.
        for (int row = 0; row < 3; ++row) {
            const double white = lifted.report.camera_to_prophoto[row * 3] + lifted.report.camera_to_prophoto[row * 3 + 1] +
                                 lifted.report.camera_to_prophoto[row * 3 + 2];
            require(std::abs(white - patch(lifted, clip).mean[row]) < 1e-4, "camera_to_prophoto does not map white to the clip patch");
        }

        // 3. Orientation 6: a quarter turn clockwise, applied once.
        const PhotocraftImage turned = decode(fixtures / "patches_rot6.dng");
        require(turned.width == std::uint32_t(kHeight) && turned.height == std::uint32_t(kWidth) &&
                    turned.report.orientation == 6 && turned.report.orientation_applied, "rotated frame size");
        for (std::uint32_t y = 0; y < turned.height; ++y)
            for (std::uint32_t x = 0; x < turned.width; ++x) {
                const std::uint32_t sx = y, sy = std::uint32_t(kHeight) - 1 - x;
                for (int c = 0; c < 3; ++c)
                    require(turned.rgb[(std::size_t(y) * turned.width + x) * 3 + c] ==
                                upright.rgb[(std::size_t(sy) * upright.width + sx) * 3 + c],
                            "orientation 6 moved a pixel to the wrong place");
            }
        PhotocraftRequest unturned_request;
        unturned_request.apply_orientation = false;
        const PhotocraftImage unturned = decode(fixtures / "patches_rot6.dng", unturned_request);
        require(unturned.rgb == upright.rgb && unturned.report.orientation == 6 && !unturned.report.orientation_applied,
                "an unapplied orientation changed the pixels");

        // 4. The caller's white balance, a bounded pool, another demosaic.
        PhotocraftRequest custom;
        custom.wb_multipliers = std::array<double, 3>{kWbMultipliers[0], kWbMultipliers[1], kWbMultipliers[2]};
        custom.threads = 2;
        custom.demosaic = PhotocraftRequest::Demosaic::Mhc;
        const PhotocraftImage same = decode(fixtures / "patches.dng", custom);
        check_patches(same, 1.0, "explicit multipliers, two threads, MHC");
        require(same.report.wb_source == spk::io::PhotocraftWbSource::CallerExplicit, "explicit white balance source");

        // 5. Identification.
        const auto bytes = read(fixtures / "patches.dng");
        require(spk::io::photocraft_identify(bytes.data(), bytes.size()) == "DNG", "identify");
        const std::uint8_t junk[] = "not a camera raw file at all";
        require(spk::io::photocraft_identify(junk, sizeof junk).empty(), "junk identified as raw");

        // 6. Failures: a status, a reason, and the caller's image untouched.
        const fs::path unicode = scratch / fs::path(u8"截断 RAW 文件.dng");
        write(unicode, bytes.data(), bytes.size() - std::size_t(kWidth) * kHeight);
        const fs::path garbage = scratch / "garbage.dng";
        write(garbage, junk, sizeof junk);
        const fs::path intact = scratch / fs::path(u8"完整 RAW 文件.dng");
        write(intact, bytes.data(), bytes.size());
        check_patches(decode(intact), 1.0, "a non-ASCII path");
        struct Case { const char* name; fs::path path; PhotocraftRequest request; std::uint64_t max_file; PhotocraftStatus want; };
        PhotocraftRequest tight_pixels, tight_memory, bad_wb;
        tight_pixels.max_pixels = std::uint64_t(kWidth) * kHeight - 1;
        tight_memory.max_alloc_bytes = 4096;
        bad_wb.wb_multipliers = std::array<double, 3>{0.0, 1.0, 1.0};
        const std::uint64_t roomy = std::uint64_t(2) << 30;
        const Case cases[] = {
            {"missing file", scratch / "absent.dng", {}, roomy, PhotocraftStatus::Io},
            {"not a raw file", garbage, {}, roomy, PhotocraftStatus::NotRaw},
            {"truncated strip", unicode, {}, roomy, PhotocraftStatus::Malformed},
            {"pixel limit", intact, tight_pixels, roomy, PhotocraftStatus::LimitExceeded},
            {"memory limit", intact, tight_memory, roomy, PhotocraftStatus::LimitExceeded},
            {"file size limit", intact, {}, bytes.size() - 1, PhotocraftStatus::LimitExceeded},
            {"invalid multipliers", intact, bad_wb, roomy, PhotocraftStatus::Malformed},
        };
        for (const Case& c : cases) {
            PhotocraftImage prior = sentinel();
            std::string error = "stale";
            const PhotocraftStatus got = spk::io::decode_raw_photocraft(c.path, c.request, prior, error, c.max_file);
            expect_failure(c.want, got, error, prior, c.name);
        }
        // After every failure an ordinary decode still works.
        check_patches(decode(intact), 1.0, "after the failures");
        std::error_code ignored;
        for (const fs::path& p : {unicode, garbage, intact}) fs::remove(p, ignored);

        std::cout << "PhotoCraft " << pinned << ": patches within " << worst << " of the DNG reference; decode "
                  << r.decode_ms << " ms, develop " << r.develop_ms << " ms\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "spk_raw_photocraft: " << e.what() << '\n';
        return 1;
    }
}
