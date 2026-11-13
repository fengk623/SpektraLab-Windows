// The host's RAW input boundary with two decoders behind it, the linear
// ProPhoto TIFF, and `spektralab-host raw-to-tiff`.
//
//   spk_host_raw_photocraft <fixtures dir> <scratch dir>
//
// Synthetic DNG fixtures only (engine/tools/make_photocraft_fixtures.py): no
// camera file, no compressed sensor data, no GPU. What is established here is
// the plumbing and the numbers: the same rule picks the decoder however a
// file arrives, PhotoCraft's frame reaches the host unclamped and matches the
// DNG reference, the float TIFF reads back bit for bit through the reader the
// app opens TIFFs with, and a failure leaves the next decode working.
#include "image_io.hpp"
#include "io/linear_prophoto_icc.hpp"
#include "json.hpp"
#include "output.hpp"
#include "platform.hpp"
#include "raw_backend.hpp"
#include "raw_tiff_cli.hpp"
#include "tiff_ifd.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fixtures/photocraft/expected.h"

namespace {
namespace fs = std::filesystem;
using namespace spkhost;

void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }

FloatImage decode(const fs::path& path, const std::string& backend, Probe* probe_out = nullptr) {
    DecodeOptions options;
    options.raw_backend = backend;
    FloatImage image;
    Probe probe;
    std::string error;
    require(decode_linear_prophoto(path, options, image, probe, error), path_utf8(path.filename()) + " (" + backend + "): " + error);
    if (probe_out) *probe_out = probe;
    return image;
}

double patch_mean(const FloatImage& image, int index, int channel) {
    using namespace spk_fixture;
    const int px = index % kPatchColumns * kPatch, py = index / kPatchColumns * kPatch;
    double sum = 0;
    int count = 0;
    for (int y = py + 8; y < py + kPatch - 8; ++y)
        for (int x = px + 8; x < px + kPatch - 8; ++x, ++count)
            sum += image.rgb[(size_t(y) * image.width + x) * 3 + channel];
    return sum / count;
}

void check_patches(const FloatImage& image, const std::string& what) {
    using namespace spk_fixture;
    require(image.width == uint32_t(kWidth) && image.height == uint32_t(kHeight), what + ": frame size");
    for (int i = 0; i < int(sizeof kPatches / sizeof kPatches[0]); ++i)
        for (int c = 0; c < 3; ++c) {
            const double want = kPatches[i].rgb[c], got = patch_mean(image, i, c);
            require(std::abs(got - want) < 2e-5 + 2e-5 * std::abs(want),
                    what + ": patch " + kPatches[i].name + " is " + std::to_string(got) + ", expected " + std::to_string(want));
        }
}

void backend_rule() {
    const std::vector<AcceptedRaw> list = {
        {"Sony", "ILCE-7RM5", "sony_arw2_load_raw", "record A"},
        {"FUJIFILM", "GFX100RF", "*", "record B"},
    };
    const RawIdentity sony{"SONY", "ILCE-7RM5", "sony_arw2_load_raw"};
    const RawIdentity sony_lossless{"SONY", "ILCE-7RM5", "sony_ljpeg_load_raw"};
    const RawIdentity fuji{"Fujifilm", "GFX100RF", "fuji_compressed_load_raw"};
    const RawIdentity other{"Canon", "EOS R5", "crxLoadRaw"};
    struct Case { const char* wire; const char* env; RawIdentity id; RawBackend want; const char* reason; };
    const Case cases[] = {
        // The wire wins over the environment, the environment over the list.
        {"libraw", "photocraft", sony, RawBackend::LibRaw, "decode.raw_backend"},
        {"photocraft", "libraw", other, RawBackend::Photocraft, "decode.raw_backend"},
        {"auto", "photocraft", other, RawBackend::Photocraft, "SPEKTRALAB_RAW_BACKEND"},
        {"auto", "LibRaw", sony, RawBackend::LibRaw, "SPEKTRALAB_RAW_BACKEND"},
        {"", "", sony, RawBackend::Photocraft, "accepted: record A"},
        {"auto", "auto", sony, RawBackend::Photocraft, "accepted: record A"},
        // Accepted by camera *and* compression mode: another mode is not.
        {"auto", "", sony_lossless, RawBackend::LibRaw, "not on the accepted PhotoCraft list"},
        {"auto", "", fuji, RawBackend::Photocraft, "accepted: record B"},
        {"auto", "", other, RawBackend::LibRaw, "not on the accepted PhotoCraft list"},
    };
    for (const Case& c : cases) {
        RawBackendChoice choice;
        std::string error;
        require(choose_raw_backend(c.wire, c.env, c.id, list, choice, error), std::string("rule: ") + error);
        require(choice.backend == c.want && choice.reason == c.reason,
                std::string("rule: ") + c.wire + "/" + c.env + "/" + c.id.model + " gave " +
                    raw_backend_name(choice.backend) + " (" + choice.reason + ")");
    }
    RawBackendChoice choice;
    std::string error;
    require(!choose_raw_backend("rawspeed", "", sony, list, choice, error) && error.find("decode.raw_backend") != std::string::npos,
            "an unknown decode.raw_backend was accepted");
    require(!choose_raw_backend("auto", "fastest", sony, list, choice, error) && error.find("SPEKTRALAB_RAW_BACKEND") != std::string::npos,
            "an unknown SPEKTRALAB_RAW_BACKEND was accepted");
    // The shipped list. Nothing is accepted until a camera and compression
    // mode pass on real samples; adding an entry means changing this line
    // together with the record that justifies it.
    require(accepted_photocraft_raws().empty(), "the accepted PhotoCraft list changed without this test");
}

spk::Json run_cli(std::vector<std::string> args, int want_status, const fs::path& report) {
    std::vector<std::string> all = {"spektralab-host", "raw-to-tiff"};
    all.insert(all.end(), args.begin(), args.end());
    if (!report.empty()) { all.push_back("--report"); all.push_back(path_utf8(report)); }
    const int status = raw_to_tiff_main(all);
    require(status == want_status, "raw-to-tiff exited " + std::to_string(status) + ", expected " + std::to_string(want_status));
    spk::Json parsed;
    if (want_status == 0 && !report.empty()) {
        std::vector<uint8_t> bytes;
        std::string error;
        require(read_file(report, bytes, error), "report: " + error);
        require(spk::Json::parse(std::string(bytes.begin(), bytes.end()), parsed, error), "report JSON: " + error);
    }
    return parsed;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: spk_host_raw_photocraft <fixtures> <scratch>");
        const fs::path fixtures = utf8_path(argv[1]), scratch = utf8_path(argv[2]);
        std::error_code ec;
        fs::remove_all(scratch, ec);
        fs::create_directories(scratch);
        using namespace spk_fixture;
        set_env("SPEKTRALAB_RAW_BACKEND", "");

        backend_rule();

        // --- 1. One boundary, two decoders ---------------------------------
        Probe probe;
        const FloatImage photocraft = decode(fixtures / "patches.dng", "photocraft", &probe);
        check_patches(photocraft, "photocraft");
        require(photocraft.raw_metadata && photocraft.raw_metadata->backend == "photocraft" &&
                    photocraft.raw_backend_reason == "decode.raw_backend", "photocraft: backend record");
        require(probe.kind == FileKind::Raw && probe.width == photocraft.width && probe.height == photocraft.height,
                "photocraft: probe size");
        require(!probe.raw_decoder.empty(), "the probe names no LibRaw decoder for the file");
        int blue = -1;
        for (int i = 0; i < int(sizeof kPatches / sizeof kPatches[0]); ++i)
            if (std::string(kPatches[i].name) == "blue") blue = i;
        require(patch_mean(photocraft, blue, 1) < -0.17, "the host clamped PhotoCraft's frame");

        const FloatImage libraw = decode(fixtures / "patches.dng", "libraw");
        require(libraw.raw_metadata && libraw.raw_metadata->backend == "libraw" && libraw.raw_backend_reason == "decode.raw_backend",
                "libraw: backend record");
        require(patch_mean(libraw, blue, 1) >= 0, "compatible16 is expected to clamp");

        // "auto" with nothing accepted: the compatibility path, and it says why.
        const FloatImage automatic = decode(fixtures / "patches.dng", "auto");
        require(automatic.raw_metadata->backend == "libraw" && automatic.raw_backend_reason == "not on the accepted PhotoCraft list",
                "auto: " + automatic.raw_metadata->backend + " (" + automatic.raw_backend_reason + ")");
        require(automatic.rgb == libraw.rgb, "auto and libraw decoded differently");
        // The tester's override reaches the same boundary without the wire.
        set_env("SPEKTRALAB_RAW_BACKEND", "photocraft");
        const FloatImage overridden = decode(fixtures / "patches.dng", "auto");
        require(overridden.raw_metadata->backend == "photocraft" && overridden.raw_backend_reason == "SPEKTRALAB_RAW_BACKEND" &&
                    overridden.rgb == photocraft.rgb, "SPEKTRALAB_RAW_BACKEND=photocraft was not obeyed");
        set_env("SPEKTRALAB_RAW_BACKEND", "nonsense");
        {
            FloatImage image;
            Probe p;
            std::string error;
            require(!decode_linear_prophoto(fixtures / "patches.dng", DecodeOptions{}, image, p, error) &&
                        error.find("SPEKTRALAB_RAW_BACKEND") != std::string::npos, "an invalid override was ignored");
        }
        set_env("SPEKTRALAB_RAW_BACKEND", "");

        // Orientation: applied once, by either decoder, to the same frame size.
        for (const char* backend : {"photocraft", "libraw"}) {
            Probe turned_probe;
            const FloatImage turned = decode(fixtures / "patches_rot6.dng", backend, &turned_probe);
            require(turned.width == uint32_t(kHeight) && turned.height == uint32_t(kWidth) &&
                        turned_probe.width == turned.width && turned_probe.height == turned.height,
                    std::string(backend) + ": rotated frame size");
            // Output (x, y) is source (y, height - 1 - x): the top-left patch moves to the top right.
            const size_t at = (size_t(16) * turned.width + (turned.width - 16)) * 3;
            require(std::abs(turned.rgb[at + 1] - kPatches[0].rgb[1]) < 2e-3, std::string(backend) + ": rotated the wrong way");
        }

        // --- 2. The linear TIFF ---------------------------------------------
        const auto working = linear_prophoto_space().to_xyz_d50;
        for (int i = 0; i < 9; ++i)
            require(working[i] == spk::io::kLinearProphotoToXyzD50[i], "the ICC colorants are not the working-space matrix");
        const std::vector<uint8_t>& icc = spk::io::linear_prophoto_icc();
        require(is_working_linear_icc(icc) && spk::io::linear_prophoto_icc_id().size() == 32, "linear profile identity");
        RgbSpace parsed;
        std::string error;
        require(parse_icc(icc, parsed, error), "the host cannot parse its own linear profile: " + error);
        require(parsed.description == spk::io::kLinearProphotoIccDescription && parsed.trc[0].decode(0.25) == 0.25 &&
                    parsed.trc[2].decode(2.5) == 2.5 && parsed.trc[1].decode(-0.5) == -0.5, "linear profile curve");
        for (int i = 0; i < 9; ++i)
            require(std::abs(parsed.to_xyz_d50[i] - working[i]) < 1.0 / 65536, "linear profile colorants");
        std::vector<uint8_t> other = icc;
        other[90] ^= 1;
        require(!is_working_linear_icc(other) && !is_working_linear_icc({}), "another profile was taken for the working space");

        Exif exif;
        exif.make = "SpektraLab"; exif.model = "Fixture Patches"; exif.lens = "Synthetic 50mm";
        exif.datetime_original = "2026:11:12 12:00:00";
        exif.iso = 200; exif.shutter_s = 1.0 / 250; exif.aperture = 5.6; exif.focal_mm = 50;
        const fs::path tiff = scratch / utf8_path("线性 ProPhoto.tif");
        require(write_file_atomic(tiff, [&](std::FILE* file, std::string& why) {
            return encode_tiff_file(file, photocraft.rgb.data(), photocraft.width, photocraft.height, 32, icc, why, &exif);
        }, false, error), "float TIFF write: " + error);
        {
            // The tags a reader goes by.
            std::vector<uint8_t> bytes;
            require(read_file(tiff, bytes, error), error);
            TiffBytes t(bytes.data(), bytes.size());
            uint32_t ifd0 = 0;
            TiffBytes::Ifd ifd;
            require(t.header(ifd0) && t.read_ifd(ifd0, ifd), "float TIFF structure");
            std::vector<double> values;
            require(t.numbers(ifd, 258, values) && values == std::vector<double>({32, 32, 32}), "BitsPerSample");
            require(t.numbers(ifd, 339, values) && values == std::vector<double>({3, 3, 3}), "SampleFormat");
            double v = 0;
            require(t.number(ifd, 274, v) && v == 1, "Orientation");
            require(t.number(ifd, 259, v) && v == 1 && t.number(ifd, 277, v) && v == 3 && t.number(ifd, 262, v) && v == 2,
                    "Compression / SamplesPerPixel / Photometric");
            require(t.number(ifd, 256, v) && v == kWidth && t.number(ifd, 257, v) && v == kHeight, "dimensions");
            std::vector<uint8_t> embedded;
            require(t.bytes(ifd, 34675, embedded) && embedded == icc, "embedded ICC profile");
        }
        Probe tiff_probe;
        const FloatImage reread = decode(tiff, "auto", &tiff_probe);
        require(tiff_probe.kind == FileKind::Tiff && !reread.raw_metadata && reread.raw_backend_reason.empty(), "TIFF kind");
        require(reread.width == photocraft.width && reread.height == photocraft.height &&
                    reread.rgb.size() == photocraft.rgb.size() &&
                    std::memcmp(reread.rgb.data(), photocraft.rgb.data(), reread.rgb.size() * sizeof(float)) == 0,
                "the float TIFF did not read back bit for bit");
        require(tiff_probe.metadata.make == "SpektraLab" && tiff_probe.metadata.model == "Fixture Patches" &&
                    tiff_probe.metadata.lens == "Synthetic 50mm" && tiff_probe.metadata.datetime_original == "2026:11:12 12:00:00" &&
                    tiff_probe.metadata.iso && *tiff_probe.metadata.iso == 200 && tiff_probe.metadata.orientation == 1,
                "the float TIFF lost its shooting data");
        // Options that would add a second page or compress are refused for float.
        require(!write_file_atomic(scratch / "refused.tif", [&](std::FILE* file, std::string& why) {
            return encode_tiff_file(file, photocraft.rgb.data(), photocraft.width, photocraft.height, 32, icc, TiffOptions{true, false}, why, &exif);
        }, false, error), "a compressed float TIFF was written");

        // --- 3. raw-to-tiff ---------------------------------------------------
        const std::string input = path_utf8(fixtures / "patches.dng");
        const fs::path f32 = scratch / "cli-f32.tif", f32_report = scratch / "cli-f32.json";
        spk::Json report = run_cli({"--input", input, "--output", path_utf8(f32), "--backend", "photocraft"}, 0, f32_report);
        require(report.at("decode").at("backend").as_string() == "photocraft" &&
                    report.at("decode").at("photocraft").at("calibration").as_string() == "dng_embedded" &&
                    report.at("decode").at("photocraft").at("white_balance_source").as_string() == "file_as_shot", "report: decode");
        require(report.at("tiff").at("bits_per_sample").as_double() == 32 && report.at("tiff").at("orientation").as_double() == 1 &&
                    report.at("tiff").at("icc_profile_id").as_string() == spk::io::linear_prophoto_icc_id(), "report: tiff");
        require(report.at("verify").at("passed").as_bool() && report.at("verify").at("max_abs_error").as_double() == 0 &&
                    report.at("verify").at("samples_different").as_double() == 0, "report: the float TIFF was not verified exact");
        require(report.at("pixels").at("samples_below_zero").as_double() > 0 && report.at("pixels").at("samples_non_finite").as_double() == 0,
                "report: pixel statistics");
        require(report.at("memory").at("peak_after_decode_bytes").as_double() > 0, "report: memory");
        require(decode(f32, "auto").rgb == photocraft.rgb, "raw-to-tiff's TIFF is not the frame the host decodes");
        // The same file through LibRaw, for the side-by-side a tester runs.
        report = run_cli({"--input", input, "--output", path_utf8(scratch / "cli-libraw.tif"), "--backend", "libraw"}, 0,
                         scratch / "cli-libraw.json");
        require(report.at("decode").at("backend").as_string() == "libraw" && report.at("decode").at("raw_mode").as_string() == "compatible16" &&
                    report.at("pixels").at("samples_below_zero").as_double() == 0 && report.at("verify").at("passed").as_bool(),
                "report: libraw");
        // 16-bit: clamped and rounded, and said so.
        report = run_cli({"--input", input, "--output", path_utf8(scratch / "cli-u16.tif"), "--backend", "photocraft", "--format", "u16"}, 0,
                         scratch / "cli-u16.json");
        require(report.at("tiff").at("bits_per_sample").as_double() == 16 &&
                    report.at("tiff").at("samples_clipped_below_zero").as_double() > 0 && report.at("verify").at("passed").as_bool() &&
                    report.at("verify").at("max_abs_error").as_double() <= report.at("verify").at("max_abs_error_allowed").as_double(),
                "report: u16");
        // Existing outputs are never replaced unasked; a bad call is a usage error.
        run_cli({"--input", input, "--output", path_utf8(f32), "--backend", "photocraft"}, 5, {});
        run_cli({"--input", input, "--output", path_utf8(f32), "--backend", "photocraft", "--overwrite", "--no-verify"}, 0, {});
        run_cli({"--input", input}, 2, {});
        run_cli({"--input", input, "--output", path_utf8(scratch / "x.tif"), "--backend", "dcraw"}, 2, {});
        run_cli({"--input", input, "--output", path_utf8(scratch / "x.tif"), "--tint", "10"}, 2, {});

        // --- 4. Failure, then an ordinary photo ---------------------------------
        const fs::path garbage = scratch / "garbage.dng", cut = scratch / "cut.dng";
        std::vector<uint8_t> bytes;
        require(read_file(fixtures / "patches.dng", bytes, error), error);
        require(write_file_atomic(garbage, std::vector<uint8_t>(64, 0x5a), false, error), error);
        require(write_file_atomic(cut, std::vector<uint8_t>(bytes.begin(), bytes.end() - kWidth * kHeight), false, error), error);
        for (const fs::path& bad : {garbage, cut}) {
            for (const char* backend : {"photocraft", "libraw"}) {
                FloatImage image;
                image.width = 7;
                image.rgb = {1.0f};
                Probe p;
                DecodeOptions options;
                options.raw_backend = backend;
                std::string why;
                require(!decode_linear_prophoto(bad, options, image, p, why) && !why.empty(),
                        path_utf8(bad.filename()) + " was accepted by " + backend);
                require(image.width == 7 && image.rgb.size() == 1, "a failed decode changed the caller's frame");
            }
            run_cli({"--input", path_utf8(bad), "--output", path_utf8(scratch / "never.tif"), "--backend", "photocraft"}, 4, {});
            require(!fs::exists(scratch / "never.tif"), "a failed decode left a TIFF behind");
        }
        for (const auto& entry : fs::directory_iterator(scratch))
            require(path_utf8(entry.path().filename()).find(".partial-") == std::string::npos,
                    "an intermediate file was left behind: " + path_utf8(entry.path().filename()));
        check_patches(decode(fixtures / "patches.dng", "photocraft"), "after the failures");
        require(decode(tiff, "auto").rgb == photocraft.rgb, "an ordinary TIFF no longer opens after a failed RAW");

        std::cout << "host RAW boundary: PhotoCraft and LibRaw behind one rule; float TIFF exact; raw-to-tiff verified\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "spk_host_raw_photocraft: " << e.what() << '\n';
        return 1;
    }
}
