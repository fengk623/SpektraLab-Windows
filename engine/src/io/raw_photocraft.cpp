#include "io/raw_photocraft.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>

#if SPK_PHOTOCRAFT_RAW
#include "io/photocraft_raw_abi.h"
#endif

namespace spk::io {
namespace {

double elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace

const char* photocraft_status_name(PhotocraftStatus status) noexcept {
    switch (status) {
        case PhotocraftStatus::Ok: return "ok";
        case PhotocraftStatus::NotRaw: return "not_raw";
        case PhotocraftStatus::Unsupported: return "unsupported";
        case PhotocraftStatus::Malformed: return "malformed";
        case PhotocraftStatus::LimitExceeded: return "limit_exceeded";
        case PhotocraftStatus::InvalidArgument: return "invalid_argument";
        case PhotocraftStatus::Internal: return "internal";
        case PhotocraftStatus::Unavailable: return "unavailable";
        case PhotocraftStatus::Io: return "io";
    }
    return "unknown";
}

const char* photocraft_wb_source_name(PhotocraftWbSource source) noexcept {
    switch (source) {
        case PhotocraftWbSource::None: return "none";
        case PhotocraftWbSource::FileAsShot: return "file_as_shot";
        case PhotocraftWbSource::CallerAsShot: return "libraw_as_shot";
        case PhotocraftWbSource::CallerExplicit: return "custom";
    }
    return "unknown";
}

const char* photocraft_calibration_name(PhotocraftCalibration source) noexcept {
    switch (source) {
        case PhotocraftCalibration::None: return "none";
        case PhotocraftCalibration::DngEmbedded: return "dng_embedded";
        case PhotocraftCalibration::CallerMatrix: return "libraw_matrix";
        case PhotocraftCalibration::PhotocraftProfile: return "photocraft_profile";
    }
    return "unknown";
}

#if SPK_PHOTOCRAFT_RAW

bool photocraft_available() noexcept { return true; }
std::string photocraft_commit() { return spk_pcraw_photocraft_commit(); }

std::string photocraft_identify(const std::uint8_t* bytes, std::size_t size) {
    char name[32] = {};
    if (!bytes || !spk_pcraw_identify(bytes, size, name, sizeof name)) return {};
    return name;
}

PhotocraftStatus decode_raw_photocraft(const std::uint8_t* bytes, std::size_t size,
                                       const PhotocraftRequest& request, PhotocraftImage& out,
                                       std::string& error) {
    const auto started = std::chrono::steady_clock::now();
    if (spk_pcraw_abi_version() != SPK_PCRAW_ABI_VERSION) {
        error = "the PhotoCraft RAW library is ABI " + std::to_string(spk_pcraw_abi_version()) +
                ", this build expects " + std::to_string(SPK_PCRAW_ABI_VERSION);
        return PhotocraftStatus::InvalidArgument;
    }
    if (!bytes || !size) { error = "empty RAW file"; return PhotocraftStatus::NotRaw; }

    spk_pcraw_options options{};
    options.struct_size = sizeof options;
    options.abi_version = SPK_PCRAW_ABI_VERSION;
    options.max_alloc_bytes = request.max_alloc_bytes;
    options.max_pixels = request.max_pixels;
    options.threads = request.threads;
    switch (request.demosaic) {
        case PhotocraftRequest::Demosaic::Ahd: options.demosaic = SPK_PCRAW_DEMOSAIC_AHD; break;
        case PhotocraftRequest::Demosaic::Mhc: options.demosaic = SPK_PCRAW_DEMOSAIC_MHC; break;
        case PhotocraftRequest::Demosaic::Bilinear: options.demosaic = SPK_PCRAW_DEMOSAIC_BILINEAR; break;
    }
    options.apply_orientation = request.apply_orientation;
    options.apply_baseline_exposure = request.apply_baseline_exposure;
    if (request.wb_multipliers) {
        options.wb_mode = SPK_PCRAW_WB_MULTIPLIERS;
        for (int i = 0; i < 3; ++i) options.wb_multipliers[i] = (*request.wb_multipliers)[i];
    }
    if (request.fallback_wb_multipliers) {
        options.has_fallback_wb = 1;
        for (int i = 0; i < 3; ++i) options.fallback_wb_multipliers[i] = (*request.fallback_wb_multipliers)[i];
    }
    if (request.color_matrix) {
        options.has_color_matrix = 1;
        for (int i = 0; i < 9; ++i) options.color_matrix[i] = (*request.color_matrix)[i];
        options.color_matrix_temperature_k = request.color_matrix_temperature_k;
    }

    // The result is the library's; this handle frees it on every path out.
    const std::unique_ptr<spk_pcraw_result, void (*)(spk_pcraw_result*)> result(
        spk_pcraw_develop(bytes, size, &options), spk_pcraw_free);
    if (!result) { error = "the PhotoCraft RAW library returned no result"; return PhotocraftStatus::Internal; }
    switch (spk_pcraw_status(result.get())) {
        case SPK_PCRAW_OK: break;
        case SPK_PCRAW_NOT_RAW: error = spk_pcraw_error(result.get()); return PhotocraftStatus::NotRaw;
        case SPK_PCRAW_UNSUPPORTED: error = spk_pcraw_error(result.get()); return PhotocraftStatus::Unsupported;
        case SPK_PCRAW_MALFORMED: error = spk_pcraw_error(result.get()); return PhotocraftStatus::Malformed;
        case SPK_PCRAW_LIMIT_EXCEEDED: error = spk_pcraw_error(result.get()); return PhotocraftStatus::LimitExceeded;
        case SPK_PCRAW_INVALID_ARGUMENT: error = spk_pcraw_error(result.get()); return PhotocraftStatus::InvalidArgument;
        default: error = spk_pcraw_error(result.get()); return PhotocraftStatus::Internal;
    }

    spk_pcraw_info info{};
    info.struct_size = sizeof info;
    std::uint64_t samples = 0;
    const float* pixels = spk_pcraw_pixels(result.get(), &samples);
    if (!spk_pcraw_get_info(result.get(), &info) || !pixels || !info.width || !info.height ||
        samples != std::uint64_t(info.width) * info.height * 3) {
        error = "the PhotoCraft RAW library returned an inconsistent frame";
        return PhotocraftStatus::Internal;
    }

    PhotocraftImage next;
    const auto copy_started = std::chrono::steady_clock::now();
    try {
        next.rgb.assign(pixels, pixels + samples);
    } catch (const std::bad_alloc&) {
        error = "not enough memory for the decoded RAW frame (" + std::to_string(samples * sizeof(float)) + " bytes)";
        return PhotocraftStatus::LimitExceeded;
    }
    next.width = info.width;
    next.height = info.height;
    PhotocraftReport& r = next.report;
    r.copy_ms = elapsed_ms(copy_started);
    // The fixed arrays are NUL-terminated by the library; bound the read anyway.
    const auto text = [](const char* s, std::size_t n) { return std::string(s, strnlen(s, n)); };
    r.format = text(info.format, sizeof info.format);
    r.make = text(info.make, sizeof info.make);
    r.model = text(info.model, sizeof info.model);
    r.cfa = text(info.cfa, sizeof info.cfa);
    r.sensor_width = info.sensor_width;
    r.sensor_height = info.sensor_height;
    r.crop = {info.crop_x, info.crop_y, info.crop_width, info.crop_height};
    r.orientation = int(info.orientation);
    r.orientation_applied = info.orientation_applied != 0;
    r.monochrome = info.monochrome != 0;
    switch (info.wb_source) {
        case SPK_PCRAW_WB_FILE_AS_SHOT: r.wb_source = PhotocraftWbSource::FileAsShot; break;
        case SPK_PCRAW_WB_CALLER_AS_SHOT: r.wb_source = PhotocraftWbSource::CallerAsShot; break;
        case SPK_PCRAW_WB_CALLER_EXPLICIT: r.wb_source = PhotocraftWbSource::CallerExplicit; break;
        default: r.wb_source = PhotocraftWbSource::None; break;
    }
    switch (info.calibration_source) {
        case SPK_PCRAW_CAL_DNG_EMBEDDED: r.calibration = PhotocraftCalibration::DngEmbedded; break;
        case SPK_PCRAW_CAL_CALLER_MATRIX: r.calibration = PhotocraftCalibration::CallerMatrix; break;
        case SPK_PCRAW_CAL_PHOTOCRAFT_PROFILE: r.calibration = PhotocraftCalibration::PhotocraftProfile; break;
        default: r.calibration = PhotocraftCalibration::None; break;
    }
    for (int i = 0; i < 3; ++i) r.wb_multipliers[i] = info.wb_multipliers[i];
    r.baseline_exposure_ev = info.baseline_exposure_ev;
    r.baseline_exposure_applied = info.baseline_exposure_applied != 0;
    r.profile_tone_dropped = info.profile_tone_dropped != 0;
    for (int i = 0; i < 9; ++i) r.camera_to_prophoto[i] = info.camera_to_prophoto[i];
    r.black_level_min = info.black_level_min;
    r.black_level_max = info.black_level_max;
    for (int i = 0; i < 3; ++i) r.white_level[i] = info.white_level[i];
    r.decode_ms = info.decode_ms;
    r.develop_ms = info.develop_ms;
    std::istringstream lines(spk_pcraw_warnings(result.get()));
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) r.warnings.push_back(line);
    r.total_ms = elapsed_ms(started);
    out = std::move(next);
    return PhotocraftStatus::Ok;
}

#else  // SPK_PHOTOCRAFT_RAW

bool photocraft_available() noexcept { return false; }
std::string photocraft_commit() { return {}; }
std::string photocraft_identify(const std::uint8_t*, std::size_t) { return {}; }

PhotocraftStatus decode_raw_photocraft(const std::uint8_t*, std::size_t, const PhotocraftRequest&,
                                       PhotocraftImage&, std::string& error) {
    error = "this build does not include the PhotoCraft RAW decoder (SPEKTRALAB_PHOTOCRAFT_RAW=OFF)";
    return PhotocraftStatus::Unavailable;
}

#endif  // SPK_PHOTOCRAFT_RAW

PhotocraftStatus decode_raw_photocraft(const std::filesystem::path& source, const PhotocraftRequest& request,
                                       PhotocraftImage& out, std::string& error,
                                       std::uint64_t max_file_bytes) {
    if (!photocraft_available()) return decode_raw_photocraft(nullptr, 0, request, out, error);
    const auto started = std::chrono::steady_clock::now();
    std::error_code ec;
    const auto size = std::filesystem::file_size(source, ec);
    if (ec) { error = "RAW read: " + ec.message(); return PhotocraftStatus::Io; }
    if (size > max_file_bytes) {
        error = "RAW file is " + std::to_string(size) + " bytes, over the " + std::to_string(max_file_bytes) +
                " byte limit for an in-memory decode";
        return PhotocraftStatus::LimitExceeded;
    }
    std::vector<std::uint8_t> file;
    try {
        file.resize(std::size_t(size));
    } catch (const std::bad_alloc&) {
        error = "not enough memory to read the RAW file (" + std::to_string(size) + " bytes)";
        return PhotocraftStatus::LimitExceeded;
    }
    // A filesystem::path opens with the wide-character API on Windows, so
    // non-ASCII names work; the source is opened for reading only.
    std::ifstream stream(source, std::ios::binary);
    if (!stream || (size && !stream.read(reinterpret_cast<char*>(file.data()), std::streamsize(size))) ||
        std::uint64_t(stream.gcount()) != size) {
        error = "RAW read: could not read the whole file";
        return PhotocraftStatus::Io;
    }
    stream.close();
    const double read_ms = elapsed_ms(started);
    PhotocraftImage next;
    const PhotocraftStatus status = decode_raw_photocraft(file.data(), file.size(), request, next, error);
    if (status != PhotocraftStatus::Ok) return status;
    next.report.read_ms = read_ms;
    next.report.total_ms = elapsed_ms(started);
    out = std::move(next);
    return status;
}

}  // namespace spk::io
