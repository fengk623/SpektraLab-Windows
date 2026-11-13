#include "raw_tiff_cli.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "json.hpp"
#include "image_io.hpp"
#include "io/linear_prophoto_icc.hpp"
#include "io/raw_photocraft.hpp"
#include "output.hpp"
#include "platform.hpp"

#ifndef SPEKTRALAB_HOST_VERSION
#define SPEKTRALAB_HOST_VERSION "0.1.0"
#endif

namespace spkhost {
namespace {
using spk::Json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
Json num(double v) { return Json(v); }
Json text(const std::string& s) { return Json(s); }
template <typename Values> Json list(const Values& values) {
    Json a = Json::array();
    for (double v : values) a.push(num(v));
    return a;
}

void usage() {
    std::fprintf(stderr,
        "usage: spektralab-host raw-to-tiff --input <raw> --output <tif> [--report <json>]\n"
        "           [--backend auto|libraw|photocraft] [--raw-mode compatible16|headroom]\n"
        "           [--temperature <kelvin> [--tint <tint>]] [--format f32|u16]\n"
        "           [--no-verify] [--overwrite]\n"
        "Decodes one RAW file as the app does and writes linear ProPhoto RGB (ROMM, D50):\n"
        "  f32 (default)  32-bit IEEE float, not clamped: negative and above-one samples kept\n"
        "  u16            16-bit integer, clamped to 0..1 and rounded to the nearest of 65535 steps\n"
        "The TIFF is uncompressed, Orientation 1 (the pixels are already upright), and carries\n"
        "the host's linear ProPhoto ICC profile. Outputs must not exist unless --overwrite.\n");
}

struct Stats {
    double min[3] = {0, 0, 0}, max[3] = {0, 0, 0}, mean[3] = {0, 0, 0};
    uint64_t below_zero = 0, above_one = 0, non_finite = 0;
};

Stats stats_of(const std::vector<float>& rgb) {
    Stats s;
    for (int c = 0; c < 3; ++c) { s.min[c] = std::numeric_limits<double>::infinity(); s.max[c] = -s.min[c]; }
    double sum[3] = {0, 0, 0};
    const size_t pixels = rgb.size() / 3;
    for (size_t p = 0; p < pixels; ++p)
        for (int c = 0; c < 3; ++c) {
            const float v = rgb[p * 3 + c];
            if (!std::isfinite(v)) { ++s.non_finite; continue; }
            s.min[c] = std::min<double>(s.min[c], v);
            s.max[c] = std::max<double>(s.max[c], v);
            sum[c] += v;
            if (v < 0) ++s.below_zero;
            if (v > 1) ++s.above_one;
        }
    for (int c = 0; c < 3; ++c) s.mean[c] = pixels ? sum[c] / double(pixels) : 0;
    return s;
}

Json stats_json(const Stats& s) {
    Json j = Json::object();
    j.set("min", list(s.min)); j.set("max", list(s.max)); j.set("mean", list(s.mean));
    j.set("samples_below_zero", num(double(s.below_zero)));
    j.set("samples_above_one", num(double(s.above_one)));
    j.set("samples_non_finite", num(double(s.non_finite)));
    return j;
}

// The shooting data the TIFF carries, and what the source did not have.
Json metadata_json(const Metadata& m, Exif& exif) {
    exif.make = m.make; exif.model = m.model; exif.lens = m.lens; exif.datetime_original = m.datetime_original;
    exif.iso = m.iso.value_or(0); exif.shutter_s = m.shutter_s.value_or(0);
    exif.aperture = m.aperture.value_or(0); exif.focal_mm = m.focal_mm.value_or(0);
    exif.exposure_program = m.exposure_program; exif.exposure_bias_ev = m.exposure_bias_ev; exif.focal_35mm = m.focal_35mm;
    exif.extra = m.extra_exif;
    Json j = Json::object(), missing = Json::array();
    const auto field = [&](const char* name, const std::string& value) {
        if (value.empty()) missing.push(text(name)); else j.set(name, text(value));
    };
    const auto number = [&](const char* name, const std::optional<double>& value) {
        if (!value) missing.push(text(name)); else j.set(name, num(*value));
    };
    field("make", m.make); field("model", m.model); field("lens", m.lens);
    field("datetime_original", m.datetime_original);
    number("iso", m.iso); number("shutter_s", m.shutter_s); number("aperture", m.aperture); number("focal_mm", m.focal_mm);
    j.set("source_orientation", num(m.orientation));
    j.set("missing", std::move(missing));
    return j;
}

Json photocraft_json(const spk::io::PhotocraftReport& r) {
    Json j = Json::object();
    j.set("format", text(r.format)); j.set("make", text(r.make)); j.set("model", text(r.model));
    j.set("cfa", text(r.cfa));
    j.set("sensor_width", num(r.sensor_width)); j.set("sensor_height", num(r.sensor_height));
    j.set("crop", list(r.crop));
    j.set("orientation", num(r.orientation)); j.set("orientation_applied", Json(r.orientation_applied));
    j.set("monochrome", Json(r.monochrome));
    j.set("white_balance_source", text(spk::io::photocraft_wb_source_name(r.wb_source)));
    j.set("white_balance_multipliers", list(r.wb_multipliers));
    j.set("calibration", text(spk::io::photocraft_calibration_name(r.calibration)));
    j.set("baseline_exposure_ev", num(r.baseline_exposure_ev));
    j.set("baseline_exposure_applied", Json(r.baseline_exposure_applied));
    j.set("profile_tone_dropped", Json(r.profile_tone_dropped));
    j.set("camera_to_prophoto", list(r.camera_to_prophoto));
    j.set("black_level", list(std::array<double, 2>{r.black_level_min, r.black_level_max}));
    j.set("white_level", list(r.white_level));
    Json t = Json::object();
    t.set("read", num(r.read_ms)); t.set("decode", num(r.decode_ms)); t.set("develop", num(r.develop_ms));
    t.set("copy", num(r.copy_ms)); t.set("total", num(r.total_ms));
    j.set("timings_ms", std::move(t));
    Json w = Json::array();
    for (const std::string& line : r.warnings) w.push(text(line));
    j.set("warnings", std::move(w));
    return j;
}

bool write_text(const fs::path& path, const std::string& body, bool overwrite, std::string& error) {
    return write_file_atomic(path, std::vector<uint8_t>(body.begin(), body.end()), overwrite, error);
}

}  // namespace

int raw_to_tiff_main(const std::vector<std::string>& args) {
    std::string input, output, report_path, format = "f32";
    DecodeOptions options;
    bool verify = true, overwrite = false, have_tint = false;
    double tint = 0;
    for (size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool has_value = i + 1 < args.size();
        if (a == "--input" && has_value) input = args[++i];
        else if (a == "--output" && has_value) output = args[++i];
        else if (a == "--report" && has_value) report_path = args[++i];
        else if (a == "--backend" && has_value) options.raw_backend = args[++i];
        else if (a == "--raw-mode" && has_value) options.raw_mode = args[++i];
        else if (a == "--format" && has_value) format = args[++i];
        else if (a == "--temperature" && has_value) options.temperature_k = std::strtod(args[++i].c_str(), nullptr);
        else if (a == "--tint" && has_value) { tint = std::strtod(args[++i].c_str(), nullptr); have_tint = true; }
        else if (a == "--no-verify") verify = false;
        else if (a == "--overwrite") overwrite = true;
        else { usage(); return 2; }
    }
    const bool backend_ok = options.raw_backend == "auto" || options.raw_backend == "libraw" || options.raw_backend == "photocraft";
    const bool mode_ok = options.raw_mode == "compatible16" || options.raw_mode == "headroom";
    const bool wb_ok = !options.temperature_k ? !have_tint
                                              : (*options.temperature_k >= 2000 && *options.temperature_k <= 50000 &&
                                                 tint >= -150 && tint <= 150);
    if (input.empty() || output.empty() || !backend_ok || !mode_ok || !wb_ok || (format != "f32" && format != "u16")) {
        usage();
        return 2;
    }
    if (options.temperature_k) options.tint = tint;
    const fs::path source = utf8_path(input), target = utf8_path(output);
    std::error_code ec;
    if (!overwrite && (fs::exists(target, ec) || (!report_path.empty() && fs::exists(utf8_path(report_path), ec)))) {
        std::fprintf(stderr, "raw-to-tiff: the output or the report already exists (use new names, or --overwrite)\n");
        return 5;
    }

    FloatImage image;
    Probe probe;
    std::string error;
    const auto decode_started = Clock::now();
    if (!decode_linear_prophoto(source, options, image, probe, error)) {
        const bool unsupported = error.rfind(kUnsupportedPrefix, 0) == 0;
        std::fprintf(stderr, "raw-to-tiff: %s\n", error.c_str());
        return unsupported ? 3 : 4;
    }
    const double decode_ms = ms_since(decode_started);
    const uint64_t peak_after_decode = peak_memory_bytes();
    if (probe.kind != FileKind::Raw || !image.raw_metadata) {
        std::fprintf(stderr, "raw-to-tiff: %s is not a RAW file\n", input.c_str());
        return 3;
    }
    const spk::io::RawMetadata& raw = *image.raw_metadata;
    const Stats stats = stats_of(image.rgb);
    if (stats.non_finite) {
        std::fprintf(stderr, "raw-to-tiff: the decoder produced %llu non-finite samples\n",
                     static_cast<unsigned long long>(stats.non_finite));
        return 4;
    }

    Exif exif;
    Json metadata = metadata_json(probe.metadata, exif);
    const std::vector<uint8_t>& icc = spk::io::linear_prophoto_icc();
    std::vector<uint16_t> codes;
    const void* pixels = image.rgb.data();
    uint32_t bits = 32;
    if (format == "u16") {
        // 0..1 maps to 0..65535, rounded to nearest; what lies outside is clipped.
        codes.resize(image.rgb.size());
        for (size_t i = 0; i < codes.size(); ++i)
            codes[i] = uint16_t(std::lround(std::clamp(image.rgb[i], 0.0f, 1.0f) * 65535.0f));
        pixels = codes.data();
        bits = 16;
    }
    const auto write_started = Clock::now();
    const bool written = write_file_atomic(target, [&](std::FILE* file, std::string& why) {
        return encode_tiff_file(file, pixels, image.width, image.height, bits, icc, why, &exif);
    }, overwrite, error);
    if (!written) {
        std::fprintf(stderr, "raw-to-tiff: %s\n", error.c_str());
        return 5;
    }
    const double write_ms = ms_since(write_started);
    const uint64_t peak_after_write = peak_memory_bytes();
    codes = {};

    Json report = Json::object();
    report.set("tool", text("spektralab-host raw-to-tiff"));
    report.set("host_version", text(SPEKTRALAB_HOST_VERSION));
    report.set("photocraft_commit", text(spk::io::photocraft_commit()));
    report.set("libraw_version", text(raw.libraw_version));
    {
        Json in = Json::object();
        in.set("path", text(input));
        in.set("bytes", num(double(fs::file_size(source, ec))));
        report.set("input", std::move(in));
    }
    {
        Json d = Json::object();
        d.set("backend", text(raw.backend));
        d.set("backend_reason", text(image.raw_backend_reason));
        d.set("decoder", text(raw.decoder_name));
        // LibRaw's decoder for the file is its compression mode, whoever decoded it.
        d.set("libraw_decoder", text(raw.libraw_decoder_name));
        if (raw.backend == "libraw") d.set("raw_mode", text(options.raw_mode));
        d.set("white_balance", text(options.temperature_k ? "custom" : "as_shot"));
        if (options.temperature_k) { d.set("temperature_k", num(*options.temperature_k)); d.set("tint", num(tint)); }
        d.set("width", num(image.width)); d.set("height", num(image.height));
        d.set("ms", num(decode_ms));
        Json camera = Json::object();
        camera.set("make", text(raw.make)); camera.set("model", text(raw.model));
        d.set("camera", std::move(camera));
        if (raw.photocraft) d.set("photocraft", photocraft_json(*raw.photocraft));
        if (image.raw_timings) {
            const auto& t = *image.raw_timings;
            Json timings = Json::object();
            timings.set("open", num(t.open_ms)); timings.set("unpack", num(t.unpack_ms));
            timings.set("process", num(t.process_ms)); timings.set("convert", num(t.convert_ms));
            timings.set("total", num(t.total_ms)); timings.set("workers", num(t.workers));
            d.set("decoder_timings_ms", std::move(timings));
        }
        report.set("decode", std::move(d));
    }
    report.set("metadata", std::move(metadata));
    report.set("pixels", stats_json(stats));
    {
        Json t = Json::object();
        t.set("path", text(output));
        t.set("bytes", num(double(fs::file_size(target, ec))));
        t.set("width", num(image.width)); t.set("height", num(image.height));
        t.set("samples_per_pixel", num(3)); t.set("bits_per_sample", num(bits));
        t.set("sample_format", text(bits == 32 ? "ieee_float" : "unsigned_integer"));
        if (bits == 16) {
            t.set("quantisation", text("clamp to 0..1, times 65535, round to nearest"));
            t.set("samples_clipped_below_zero", num(double(stats.below_zero)));
            t.set("samples_clipped_above_one", num(double(stats.above_one)));
        }
        t.set("compression", text("none")); t.set("orientation", num(1));
        t.set("color_space", text("linear ProPhoto RGB (ROMM primaries, D50, gamma 1.0)"));
        t.set("icc_description", text(spk::io::kLinearProphotoIccDescription));
        t.set("icc_profile_id", text(spk::io::linear_prophoto_icc_id()));
        t.set("icc_bytes", num(double(icc.size())));
        t.set("write_ms", num(write_ms));
        report.set("tiff", std::move(t));
    }

    int status = 0;
    uint64_t peak_after_verify = peak_after_write;
    if (verify) {
        // The file as the app would open it: the TIFF reader, its ICC handling,
        // its orientation step. For f32 every sample must come back as written.
        FloatImage again;
        Probe again_probe;
        Json v = Json::object();
        const auto verify_started = Clock::now();
        if (!decode_linear_prophoto(target, DecodeOptions{}, again, again_probe, error)) {
            v.set("error", text(error));
            status = 6;
        } else if (again.width != image.width || again.height != image.height || again.rgb.size() != image.rgb.size()) {
            v.set("error", text("the TIFF reads back with a different size"));
            status = 6;
        } else {
            double worst = 0;
            uint64_t different = 0;
            for (size_t i = 0; i < image.rgb.size(); ++i) {
                const double want = bits == 32 ? double(image.rgb[i]) : double(std::clamp(image.rgb[i], 0.0f, 1.0f));
                const double d = std::abs(double(again.rgb[i]) - want);
                if (d > 0) ++different;
                worst = std::max(worst, d);
            }
            v.set("max_abs_error", num(worst));
            v.set("samples_different", num(double(different)));
            // f32: bit for bit. u16: half a code, plus the reader's matrix
            // (identity to rounding) on integer samples.
            const double allowed = bits == 32 ? 0.0 : 0.5 / 65535.0 + 1e-5;
            v.set("max_abs_error_allowed", num(allowed));
            if (worst > allowed) status = 6;
            const Metadata& a = probe.metadata;
            const Metadata& b = again_probe.metadata;
            Json kept = Json::object();
            kept.set("make", Json(a.make == b.make)); kept.set("model", Json(a.model == b.model));
            kept.set("lens", Json(a.lens == b.lens));
            kept.set("datetime_original", Json(a.datetime_original == b.datetime_original));
            const auto close = [](const std::optional<double>& x, const std::optional<double>& y) {
                if (!x) return true;   // nothing to keep
                return y && std::abs(*x - *y) <= 0.01 * std::max(1.0, std::abs(*x));
            };
            kept.set("iso", Json(close(a.iso, b.iso))); kept.set("shutter_s", Json(close(a.shutter_s, b.shutter_s)));
            kept.set("aperture", Json(close(a.aperture, b.aperture))); kept.set("focal_mm", Json(close(a.focal_mm, b.focal_mm)));
            v.set("metadata_preserved", std::move(kept));
            v.set("orientation_read_back", num(b.orientation));
        }
        v.set("ms", num(ms_since(verify_started)));
        v.set("passed", Json(status == 0));
        report.set("verify", std::move(v));
        peak_after_verify = peak_memory_bytes();
    }
    {
        // The process's peak working set at three points. Each is a running
        // maximum, so a later figure is never below an earlier one.
        Json m = Json::object();
        m.set("peak_after_decode_bytes", num(double(peak_after_decode)));
        m.set("peak_after_tiff_write_bytes", num(double(peak_after_write)));
        m.set("peak_after_verify_bytes", num(double(peak_after_verify)));
        m.set("frame_bytes", num(double(image.rgb.size() * sizeof(float))));
        report.set("memory", std::move(m));
    }

    const std::string body = report.dump() + "\n";
    if (!report_path.empty() && !write_text(utf8_path(report_path), body, overwrite, error)) {
        std::fprintf(stderr, "raw-to-tiff: report: %s\n", error.c_str());
        return 5;
    }
    std::fwrite(body.data(), 1, body.size(), stdout);
    if (status == 6) std::fprintf(stderr, "raw-to-tiff: the TIFF did not read back as written\n");
    return status;
}

}  // namespace spkhost
