#pragma once

// raw_photocraft.hpp -- RAW through PhotoCraft's decoder (engine/raw-photocraft).
//
// The alternative to raw_decoder.hpp's LibRaw paths: PhotoCraft parses the
// container, decompresses, applies levels and white balance, clips, demosaics
// and converts camera RGB to linear ProPhoto by the DNG specification's
// method. The frame comes back as 32-bit float before any tone curve,
// encoding or quantisation, so it keeps values below 0 (colours outside
// ProPhoto) and above 1 (exposure gain, matrix overshoot). Sensor samples are
// still clipped at the white-balanced white level before demosaicing, as in
// compatible16; clipped highlights are neutral and are not reconstructed.
//
// PhotoCraft has colour calibration only for DNG. For every other format the
// caller passes the camera's XYZ -> camera matrix (LibRaw's cam_xyz in the
// host), and a file with no calibration from either side is refused.
//
// A build without the Rust library (SPEKTRALAB_PHOTOCRAFT_RAW=OFF) still
// compiles this interface; every decode then fails with Unavailable.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace spk::io {

enum class PhotocraftStatus {
    Ok,
    NotRaw,           // not a camera raw file PhotoCraft recognises
    Unsupported,      // recognised, not developed (format, compression, missing calibration)
    Malformed,        // damaged, truncated or inconsistent
    LimitExceeded,    // larger than the request's limits
    InvalidArgument,  // a bug in the caller or an ABI mismatch
    Internal,         // a panic caught inside the library
    Unavailable,      // this build has no PhotoCraft decoder
    Io,               // the file could not be read
};
const char* photocraft_status_name(PhotocraftStatus status) noexcept;

struct PhotocraftRequest {
    // Caller-chosen R, G, B camera multipliers (Temperature / Tint). Unset: as shot.
    std::optional<std::array<double, 3>> wb_multipliers;
    // Another reader's as-shot multipliers, used only when the file gives
    // PhotoCraft no as-shot white balance of its own.
    std::optional<std::array<double, 3>> fallback_wb_multipliers;
    // XYZ -> camera, row-major (DNG ColorMatrix semantics), for a non-DNG file.
    std::optional<std::array<double, 9>> color_matrix;
    double color_matrix_temperature_k = 6504.0;
    bool apply_orientation = true;
    // Off keeps the LibRaw paths' exposure convention (BaselineExposure ignored).
    bool apply_baseline_exposure = false;
    unsigned threads = 0;            // 0: one per logical CPU
    std::uint64_t max_alloc_bytes = 0;   // 0: the library default (8 GiB)
    std::uint64_t max_pixels = 0;        // 0: the library default (2^28)
    enum class Demosaic { Ahd, Mhc, Bilinear } demosaic = Demosaic::Ahd;
};

enum class PhotocraftWbSource { None, FileAsShot, CallerAsShot, CallerExplicit };
enum class PhotocraftCalibration { None, DngEmbedded, CallerMatrix, PhotocraftProfile };
const char* photocraft_wb_source_name(PhotocraftWbSource source) noexcept;
const char* photocraft_calibration_name(PhotocraftCalibration source) noexcept;

struct PhotocraftReport {
    std::string format, make, model, cfa;
    std::uint32_t sensor_width = 0, sensor_height = 0;
    std::array<std::uint32_t, 4> crop{};   // x, y, width, height in the sensor data
    int orientation = 1;                   // TIFF 1..8 as recorded
    bool orientation_applied = false, monochrome = false;
    PhotocraftWbSource wb_source = PhotocraftWbSource::None;
    std::array<double, 3> wb_multipliers{};   // used; the smallest is 1
    PhotocraftCalibration calibration = PhotocraftCalibration::None;
    double baseline_exposure_ev = 0.0;
    bool baseline_exposure_applied = false, profile_tone_dropped = false;
    // White-balanced camera RGB -> linear ProPhoto, row-major, gain included.
    std::array<double, 9> camera_to_prophoto{};
    double black_level_min = 0.0, black_level_max = 0.0;
    std::array<double, 3> white_level{};
    // read: the file into memory; decode: container and decompression;
    // develop: levels to orientation; copy: into this process's buffer.
    double read_ms = 0.0, decode_ms = 0.0, develop_ms = 0.0, copy_ms = 0.0, total_ms = 0.0;
    std::vector<std::string> warnings;
};

struct PhotocraftImage {
    std::uint32_t width = 0, height = 0;
    // Packed top-down RGB, linear ProPhoto (ROMM, D50), finite, not clamped.
    std::vector<float> rgb;
    PhotocraftReport report;
};

// Whether this build links the PhotoCraft decoder, and the PhotoCraft commit
// it was written against (empty when it does not).
bool photocraft_available() noexcept;
std::string photocraft_commit();

// The container family PhotoCraft sees in these bytes ("DNG", "ARW", ...),
// empty when it is not a camera raw file it recognises. Header and IFDs only.
std::string photocraft_identify(const std::uint8_t* bytes, std::size_t size);

// On failure `out` is unchanged and `error` says why, prefixed by nothing:
// the status carries the kind.
PhotocraftStatus decode_raw_photocraft(const std::uint8_t* bytes, std::size_t size,
                                       const PhotocraftRequest& request, PhotocraftImage& out,
                                       std::string& error);
// The same from a file. Reads it whole (RAW files are sensor-sized); a file
// over `max_file_bytes` is refused as LimitExceeded before it is read.
PhotocraftStatus decode_raw_photocraft(const std::filesystem::path& source,
                                       const PhotocraftRequest& request, PhotocraftImage& out,
                                       std::string& error,
                                       std::uint64_t max_file_bytes = std::uint64_t(2) << 30);

}  // namespace spk::io
