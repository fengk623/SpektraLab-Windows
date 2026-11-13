/* photocraft_raw_abi.h -- the C side of engine/raw-photocraft/src/lib.rs.
 *
 * spk-photocraft-raw is a Rust static library around PhotoCraft's raw decoder
 * (pinned in engine/third_party/photocraft-raw). One call takes the bytes of
 * a camera raw file and returns scene-linear ProPhoto RGB (ROMM primaries,
 * D50, no transfer function) as 32-bit float. raw_photocraft.cpp is the only
 * caller; everything else goes through spk::io::decode_raw_photocraft.
 *
 * The layouts and constants here mirror the #[repr(C)] declarations in
 * lib.rs. Change both together and raise SPK_PCRAW_ABI_VERSION: the library
 * refuses a caller built for another version, and the static_asserts below
 * refuse a compiler that lays the structs out differently.
 *
 * Ownership: a spk_pcraw_result and everything its accessors return belong
 * to the library until spk_pcraw_free. The caller copies what it keeps. No
 * path or file handle crosses this boundary, and no panic does either (it
 * becomes SPK_PCRAW_INTERNAL).
 */
#ifndef SPK_PHOTOCRAFT_RAW_ABI_H
#define SPK_PHOTOCRAFT_RAW_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPK_PCRAW_ABI_VERSION 1u

enum {
    SPK_PCRAW_OK = 0,
    SPK_PCRAW_NOT_RAW = 1,          /* not a camera raw file PhotoCraft recognises */
    SPK_PCRAW_UNSUPPORTED = 2,      /* recognised, but not developed by this path */
    SPK_PCRAW_MALFORMED = 3,        /* damaged, truncated or inconsistent */
    SPK_PCRAW_LIMIT_EXCEEDED = 4,   /* declared sizes exceed the caller's limits */
    SPK_PCRAW_INVALID_ARGUMENT = 5, /* the call itself was wrong */
    SPK_PCRAW_INTERNAL = 6          /* a panic was caught inside the library */
};

enum { SPK_PCRAW_DEMOSAIC_AHD = 0, SPK_PCRAW_DEMOSAIC_MHC = 1, SPK_PCRAW_DEMOSAIC_BILINEAR = 2 };
enum { SPK_PCRAW_WB_AS_SHOT = 0, SPK_PCRAW_WB_MULTIPLIERS = 1 };
/* spk_pcraw_info.wb_source */
enum {
    SPK_PCRAW_WB_NONE = 0,            /* monochrome data */
    SPK_PCRAW_WB_FILE_AS_SHOT = 1,    /* the as-shot balance PhotoCraft read from the file */
    SPK_PCRAW_WB_CALLER_AS_SHOT = 2,  /* fallback_wb_multipliers: the file gave PhotoCraft none */
    SPK_PCRAW_WB_CALLER_EXPLICIT = 3  /* wb_multipliers */
};
/* spk_pcraw_info.calibration_source */
enum {
    SPK_PCRAW_CAL_NONE = 0,              /* monochrome data */
    SPK_PCRAW_CAL_DNG_EMBEDDED = 1,      /* the DNG's own ColorMatrix / ForwardMatrix */
    SPK_PCRAW_CAL_CALLER_MATRIX = 2,     /* color_matrix */
    SPK_PCRAW_CAL_PHOTOCRAFT_PROFILE = 3 /* a body PhotoCraft measured; no matrix was supplied */
};

/* Zero-initialise, set struct_size and abi_version, then what matters. */
typedef struct spk_pcraw_options {
    uint32_t struct_size;             /* sizeof(spk_pcraw_options) */
    uint32_t abi_version;             /* SPK_PCRAW_ABI_VERSION */
    uint64_t max_alloc_bytes;         /* one buffer / peak working memory; 0: 8 GiB */
    uint64_t max_pixels;              /* width * height; 0: 2^28 */
    uint32_t threads;                 /* 0: one per logical CPU */
    uint32_t demosaic;                /* SPK_PCRAW_DEMOSAIC_*; Bayer sensors only */
    uint32_t wb_mode;                 /* SPK_PCRAW_WB_AS_SHOT or _MULTIPLIERS */
    uint32_t apply_orientation;       /* non-zero: rotate / flip to the file's orientation */
    uint32_t apply_baseline_exposure; /* non-zero: multiply by 2^BaselineExposure (DNG) */
    uint32_t has_fallback_wb;         /* non-zero: fallback_wb_multipliers is set */
    uint32_t has_color_matrix;        /* non-zero: color_matrix and its temperature are set */
    uint32_t reserved;
    double wb_multipliers[3];          /* R G B camera multipliers (wb_mode MULTIPLIERS) */
    double fallback_wb_multipliers[3]; /* another reader's as-shot R G B multipliers */
    double color_matrix[9];            /* XYZ -> camera, row-major (DNG ColorMatrix semantics) */
    double color_matrix_temperature_k; /* its illuminant, kelvin (6504 for D65) */
} spk_pcraw_options;

/* What was developed and how. Strings are UTF-8, NUL-terminated, truncated to fit. */
typedef struct spk_pcraw_info {
    uint32_t struct_size;             /* set by the caller: sizeof(spk_pcraw_info) */
    uint32_t width, height;           /* of the returned frame */
    uint32_t sensor_width, sensor_height;
    uint32_t crop_x, crop_y, crop_width, crop_height; /* the output area in the sensor data */
    uint32_t orientation;             /* TIFF 1..8, as the file records it */
    uint32_t orientation_applied;
    uint32_t monochrome;
    uint32_t wb_source;               /* SPK_PCRAW_WB_* */
    uint32_t calibration_source;      /* SPK_PCRAW_CAL_* */
    uint32_t baseline_exposure_applied;
    uint32_t profile_tone_dropped;    /* a profile tone curve / black point was left out */
    double wb_multipliers[3];         /* R G B used, the smallest 1 */
    double baseline_exposure_ev;      /* the file's, applied or not */
    double camera_to_prophoto[9];     /* balanced camera RGB -> linear ProPhoto, row-major, gain included */
    double black_level_min, black_level_max; /* sensor counts */
    double white_level[3];            /* sensor counts */
    double decode_ms;                 /* container parse and decompression */
    double develop_ms;                /* levels, balance, demosaic, colour, orientation */
    char format[16];                  /* "DNG", "ARW", ... */
    char make[64];
    char model[64];
    char cfa[160];                    /* "RGGB", or rows joined by '/'; empty: not CFA data */
} spk_pcraw_info;

#ifdef __cplusplus
static_assert(sizeof(spk_pcraw_options) == 184, "spk_pcraw_options layout differs from lib.rs");
static_assert(sizeof(spk_pcraw_info) == 528, "spk_pcraw_info layout differs from lib.rs");
#endif

typedef struct spk_pcraw_result spk_pcraw_result;

uint32_t spk_pcraw_abi_version(void);
/* The PhotoCraft commit the library was written against. Static. */
const char* spk_pcraw_photocraft_commit(void);
/* 1 and the container family ("DNG", "ARW", ...) in out; 0 when not a raw file. */
int32_t spk_pcraw_identify(const uint8_t* bytes, size_t len, char* out, size_t out_len);
/* Always returns a result; release it with spk_pcraw_free. */
spk_pcraw_result* spk_pcraw_develop(const uint8_t* bytes, size_t len, const spk_pcraw_options* options);
int32_t spk_pcraw_status(const spk_pcraw_result* result);
const char* spk_pcraw_error(const spk_pcraw_result* result);    /* empty on success */
const char* spk_pcraw_warnings(const spk_pcraw_result* result); /* one per line */
int32_t spk_pcraw_get_info(const spk_pcraw_result* result, spk_pcraw_info* out); /* 1 when filled */
/* Interleaved RGB f32, top row first; *samples = width * height * 3. NULL on failure. */
const float* spk_pcraw_pixels(const spk_pcraw_result* result, uint64_t* samples);
void spk_pcraw_free(spk_pcraw_result* result);

#ifdef __cplusplus
}
#endif
#endif
