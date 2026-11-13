//! `spk-photocraft-raw`: PhotoCraft's raw decoder and developer behind a C ABI.
//!
//! One call takes the bytes of a camera raw file and returns scene-linear
//! ProPhoto RGB (ROMM primaries, D50 white, no transfer function) as 32-bit
//! float, together with what was decoded and how. The host (`engine/src/io`)
//! links this as a static library; `engine/src/io/photocraft_raw_abi.h` is the
//! C side of the declarations at the bottom of this file and must change with
//! them.
//!
//! What is PhotoCraft's and what is ours:
//!
//! * PhotoCraft (`photocraft-raw`, pinned in `engine/third_party/photocraft-raw`)
//!   parses the container, decompresses the sensor data, applies linearization
//!   and black / white levels, white-balances, clips highlights, demosaics, and
//!   converts camera RGB to XYZ (D50) and on to linear ProPhoto by the DNG
//!   specification's method. Our overlay adds the output this crate reads: the
//!   frame before tone, encoding and quantisation.
//! * This crate decides the policy around it. PhotoCraft carries colour
//!   calibration only for DNG (the file's own matrices) and for one measured
//!   body; for everything else the caller supplies the camera's XYZ → camera
//!   matrix (the host takes it from LibRaw), and a file with no calibration from
//!   either side is refused instead of being developed as if its camera
//!   channels were sRGB primaries. A missing as-shot white balance is likewise
//!   an error, not a grey-world guess. The camera profile's tone curve and
//!   black point are dropped: the output is scene-linear.
//!
//! Boundary rules (see `engine/WINDOWS_PHOTOCRAFT_RAW.md`):
//!
//! * No path and no file handle crosses the boundary. The caller reads the
//!   file; this crate does no I/O.
//! * Every result is owned by this library and is released by
//!   [`spk_pcraw_free`]; the caller copies what it keeps. Pointers handed out
//!   by the accessors stay valid until then.
//! * No panic leaves the library: each entry point catches it and reports
//!   [`Status::Internal`].

use std::ffi::{CString, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::time::Instant;

use photocraft_raw::{Calibration, Demosaic, DevelopOptions, Limits, Mat3, RawError, Sensor, Tone, WhiteBalance};

/// The pinned PhotoCraft commit, once, for the Rust constant and the C string.
macro_rules! photocraft_commit {
    () => {
        "e052eabf4be74612725400955d184f85d208c0f0"
    };
}

/// Version of the C ABI below. A change to any `#[repr(C)]` layout or to the
/// meaning of a field raises it; the host refuses a library it was not built for.
pub const ABI_VERSION: u32 = 1;

/// The PhotoCraft commit this crate is written against and tested with
/// (`engine/third_party/photocraft-raw/PIN`; a CTest holds the two together).
pub const PHOTOCRAFT_COMMIT: &str = photocraft_commit!();

/// Default cap on one working buffer and on the peak working memory of a
/// development, when the caller passes 0: 8 GiB. A 102 MP frame needs about
/// 2.5 GiB (the f32 frame and its oriented copy); PhotoCraft's own default of
/// 2 GiB refuses that frame as soon as it has to be rotated.
pub const DEFAULT_MAX_ALLOC: u64 = 8 << 30;
/// Default cap on `width * height` when the caller passes 0: PhotoCraft's own.
pub const DEFAULT_MAX_PIXELS: u64 = 1 << 28;

/// Outcome of a call, as the C side sees it.
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    Ok = 0,
    /// Not a camera raw file PhotoCraft recognises.
    NotRaw = 1,
    /// Recognised, but this file (format, compression, sensor layout, or a
    /// missing calibration or white balance) is not developed by this path.
    Unsupported = 2,
    /// Damaged, truncated or inconsistent.
    Malformed = 3,
    /// The file's declared sizes exceed the caller's limits.
    LimitExceeded = 4,
    /// The call itself was wrong: null pointer, unknown ABI version, bad option.
    InvalidArgument = 5,
    /// A panic was caught inside the library. A bug; the message says where.
    Internal = 6,
}

/// Where the white balance came from.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WbSource {
    /// Monochrome data: nothing to balance.
    None = 0,
    /// The as-shot white balance PhotoCraft read from the file.
    FileAsShot = 1,
    /// The file gave PhotoCraft none; the caller's fallback multipliers were used.
    CallerAsShot = 2,
    /// The caller asked for these multipliers (Temperature / Tint in the host).
    CallerExplicit = 3,
}

/// Where the camera → XYZ conversion came from.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CalibrationSource {
    /// Monochrome data: no colour conversion.
    None = 0,
    /// The DNG's own ColorMatrix / ForwardMatrix tags.
    DngEmbedded = 1,
    /// The XYZ → camera matrix the caller supplied (LibRaw's, in the host).
    CallerMatrix = 2,
    /// A camera profile PhotoCraft measured itself (`cameras.rs`), used only
    /// when the caller supplied no matrix.
    PhotocraftProfile = 3,
}

/// The white balance the caller asks for.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum WbRequest {
    /// As shot: the file's own, else `Options::fallback_wb`, else an error.
    AsShot,
    /// These R, G, B camera multipliers.
    Multipliers([f64; 3]),
}

/// A request, in Rust terms. The C struct [`SpkPcrawOptions`] converts to this.
#[derive(Debug, Clone, PartialEq)]
pub struct Options {
    pub limits: Limits,
    /// Worker threads for the decode and the development; 0 leaves rayon's default.
    pub threads: usize,
    pub demosaic: Demosaic,
    pub white_balance: WbRequest,
    /// As-shot R, G, B multipliers from another reader, used only when the file
    /// gives PhotoCraft no as-shot white balance.
    pub fallback_wb: Option<[f64; 3]>,
    /// XYZ → camera (DNG ColorMatrix semantics) and its illuminant's
    /// temperature in kelvin, used for every non-DNG file.
    pub color_matrix: Option<(Mat3, f64)>,
    /// Rotate / flip the pixels to the file's orientation.
    pub apply_orientation: bool,
    /// Multiply by 2^BaselineExposure (DNG). Off keeps the exposure convention
    /// of the LibRaw path, which never applies it.
    pub apply_baseline_exposure: bool,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            limits: Limits { max_alloc: DEFAULT_MAX_ALLOC, max_pixels: DEFAULT_MAX_PIXELS, ..Limits::default() },
            threads: 0,
            demosaic: Demosaic::Ahd,
            white_balance: WbRequest::AsShot,
            fallback_wb: None,
            color_matrix: None,
            apply_orientation: true,
            apply_baseline_exposure: false,
        }
    }
}

/// What was developed and how.
#[derive(Debug, Clone, PartialEq)]
pub struct Report {
    pub width: u32,
    pub height: u32,
    pub format: &'static str,
    pub make: String,
    pub model: String,
    pub sensor_width: usize,
    pub sensor_height: usize,
    /// The output area inside the sensor data: x, y, width, height.
    pub crop: [usize; 4],
    /// CFA pattern at the crop origin (`"RGGB"`, or rows joined by `/`); empty
    /// for LinearRaw and monochrome data.
    pub cfa: String,
    /// TIFF orientation (1–8) the file records.
    pub orientation: u16,
    pub orientation_applied: bool,
    pub monochrome: bool,
    pub wb_source: WbSource,
    /// R, G, B multipliers used, the smallest 1.
    pub wb_multipliers: [f64; 3],
    pub calibration_source: CalibrationSource,
    /// The file's BaselineExposure in EV, whether or not it was applied.
    pub baseline_exposure_ev: f64,
    pub baseline_exposure_applied: bool,
    /// A camera profile tone curve or black point existed and was left out.
    pub profile_tone_dropped: bool,
    /// White-balanced camera RGB → linear ProPhoto, row-major, exposure gain
    /// included: the matrix the frame was made with. A host that needs to go
    /// back from a pixel to the camera's response (a neutral picker) inverts
    /// this and divides by `wb_multipliers`. Identity times the gain for
    /// monochrome data.
    pub camera_to_prophoto: [f64; 9],
    /// Smallest and largest black level over the repeat pattern, in sensor counts.
    pub black_level: [f64; 2],
    /// White (saturation) level per channel, in sensor counts.
    pub white_level: [f64; 3],
    /// Container parse and sensor decompression.
    pub decode_ms: f64,
    /// Levels, white balance, demosaic, colour conversion and orientation.
    pub develop_ms: f64,
}

/// A developed frame.
#[derive(Debug, Clone)]
pub struct Output {
    /// Interleaved RGB, `width * height * 3`, linear ProPhoto, finite, not clamped.
    pub rgb: Vec<f32>,
    pub report: Report,
    /// Anything approximated or not applied, PhotoCraft's notes and ours.
    pub warnings: Vec<String>,
}

fn status_of(e: &RawError) -> Status {
    match e {
        RawError::NotRaw => Status::NotRaw,
        RawError::Unsupported(_) => Status::Unsupported,
        RawError::Malformed(_) => Status::Malformed,
        RawError::LimitExceeded(_) => Status::LimitExceeded,
    }
}

/// The file's own as-shot neutral, by the rule PhotoCraft's `resolve_neutral`
/// applies before it would fall back to a grey-world estimate.
fn file_as_shot(s: &Sensor) -> bool {
    let c = &s.color;
    let neutral = c.as_shot_neutral.or_else(|| c.as_shot_white_xy.and_then(|xy| c.xy_to_neutral(xy))).or_else(|| s.camera_wb.map(|m| m.map(|v| 1.0 / v)));
    neutral.is_some_and(|n| {
        let max = n.iter().cloned().fold(f64::MIN, f64::max);
        max > 0.0 && n.iter().all(|v| v.is_finite() && *v > 1e-4)
    })
}

fn valid_multipliers(m: &[f64; 3]) -> bool {
    m.iter().all(|v| v.is_finite() && *v > 0.0)
}

fn valid_matrix(m: &Mat3) -> bool {
    let [[a, b, c], [d, e, f], [g, h, i]] = *m;
    let det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    m.iter().flatten().all(|v| v.is_finite()) && det.is_finite() && det.abs() > 1e-9
}

const IDENTITY: Mat3 = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]];

fn mat_mul(a: &Mat3, b: &Mat3) -> Mat3 {
    let mut o = [[0.0; 3]; 3];
    for (i, row) in o.iter_mut().enumerate() {
        for (j, v) in row.iter_mut().enumerate() {
            *v = (0..3).map(|k| a[i][k] * b[k][j]).sum();
        }
    }
    o
}

fn mat_invert(m: &Mat3) -> Option<Mat3> {
    let [[a, b, c], [d, e, f], [g, h, i]] = *m;
    let det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if !det.is_finite() || det.abs() < 1e-12 {
        return None;
    }
    let r = 1.0 / det;
    Some([
        [(e * i - f * h) * r, (c * h - b * i) * r, (b * f - c * e) * r],
        [(f * g - d * i) * r, (a * i - c * g) * r, (c * d - a * f) * r],
        [(d * h - e * g) * r, (b * g - a * h) * r, (a * e - b * d) * r],
    ])
}

/// XYZ (D50) → linear ProPhoto: the inverse of the ROMM primaries (ISO 22028-2)
/// scaled to a D50 white of chromaticity (0.3457, 0.3585). The same construction
/// as PhotoCraft's private `color::xyz_d50_to_prophoto`; a test holds the two
/// together through the developed pixels.
fn xyz_d50_to_prophoto() -> Option<Mat3> {
    const ROMM: [[f64; 2]; 3] = [[0.7347, 0.2653], [0.1596, 0.8404], [0.0366, 0.0001]];
    const D50_XY: [f64; 2] = [0.3457, 0.3585];
    let xyz = |xy: [f64; 2]| [xy[0] / xy[1], 1.0, (1.0 - xy[0] - xy[1]) / xy[1]];
    let cols = ROMM.map(xyz);
    let p: Mat3 = [[cols[0][0], cols[1][0], cols[2][0]], [cols[0][1], cols[1][1], cols[2][1]], [cols[0][2], cols[1][2], cols[2][2]]];
    let pi = mat_invert(&p)?;
    let w = xyz(D50_XY);
    let s = [0, 1, 2].map(|i| pi[i][0] * w[0] + pi[i][1] * w[1] + pi[i][2] * w[2]);
    mat_invert(&mat_mul(&p, &[[s[0], 0.0, 0.0], [0.0, s[1], 0.0], [0.0, 0.0, s[2]]]))
}

/// Decodes and develops `bytes` to scene-linear ProPhoto f32.
pub fn develop(bytes: &[u8], o: &Options) -> Result<Output, RawError> {
    let run = || develop_on_this_pool(bytes, o);
    if o.threads == 0 {
        return run();
    }
    match rayon::ThreadPoolBuilder::new().num_threads(o.threads).build() {
        Ok(pool) => pool.install(run),
        Err(e) => Err(RawError::LimitExceeded(format!("cannot start {} RAW worker threads: {e}", o.threads))),
    }
}

fn develop_on_this_pool(bytes: &[u8], o: &Options) -> Result<Output, RawError> {
    if let WbRequest::Multipliers(m) = &o.white_balance
        && !valid_multipliers(m)
    {
        return Err(RawError::Malformed("white-balance multipliers must be positive and finite".into()));
    }
    let started = Instant::now();
    let mut sensor = photocraft_raw::decode(bytes, &o.limits)?;
    let decode_ms = started.elapsed().as_secs_f64() * 1e3;
    let mut warnings = Vec::new();
    let monochrome = sensor.is_monochrome();
    let is_dng = sensor.format == photocraft_raw::RawFormat::Dng;

    // Colour calibration. A DNG is developed with its own matrices. Anything
    // else takes the caller's matrix when there is one, PhotoCraft's measured
    // profile for the body otherwise, and is refused when there is neither:
    // PhotoCraft's fallback would read the camera channels as sRGB primaries.
    let calibration_source = if monochrome {
        CalibrationSource::None
    } else if is_dng {
        if sensor.color.calibrations.is_empty() {
            return Err(RawError::Unsupported("this DNG carries no ColorMatrix, so its camera colours cannot be converted".into()));
        }
        CalibrationSource::DngEmbedded
    } else if let Some((matrix, temperature)) = &o.color_matrix {
        if !valid_matrix(matrix) || !temperature.is_finite() || *temperature < 1000.0 || *temperature > 50000.0 {
            return Err(RawError::Malformed("the supplied camera colour matrix is singular, not finite, or has no usable illuminant".into()));
        }
        sensor.color.calibrations = vec![Calibration { temperature: *temperature, color_matrix: *matrix, forward_matrix: None, camera_calibration: IDENTITY }];
        sensor.color.analog_balance = None;
        CalibrationSource::CallerMatrix
    } else if !sensor.color.calibrations.is_empty() {
        warnings.push("no camera colour matrix was supplied; PhotoCraft's own measured profile for this body is used".into());
        CalibrationSource::PhotocraftProfile
    } else {
        return Err(RawError::Unsupported("no colour calibration for this camera: the file carries none and none was supplied".into()));
    };

    // Scene-linear: the profile's tone curve and black point belong to a
    // display rendering. The overlay leaves them out of linear output anyway;
    // clearing them here keeps its warnings for cases we did not expect.
    let profile_tone_dropped = !sensor.tone_curve.is_empty() || sensor.black_point > 0.0;
    sensor.tone_curve.clear();
    sensor.black_point = 0.0;

    let baseline_exposure_ev = sensor.baseline_exposure;
    if !o.apply_baseline_exposure {
        sensor.baseline_exposure = 0.0;
    }

    let (white_balance, wb_source) = match (&o.white_balance, monochrome) {
        (_, true) => (WhiteBalance::AsShot, WbSource::None),
        (WbRequest::Multipliers(m), false) => (WhiteBalance::Multipliers(*m), WbSource::CallerExplicit),
        (WbRequest::AsShot, false) if file_as_shot(&sensor) => (WhiteBalance::AsShot, WbSource::FileAsShot),
        (WbRequest::AsShot, false) => match &o.fallback_wb {
            Some(m) if valid_multipliers(m) => {
                warnings.push("the file gives PhotoCraft no as-shot white balance; the caller's as-shot multipliers are used".into());
                (WhiteBalance::Multipliers(*m), WbSource::CallerAsShot)
            }
            _ => return Err(RawError::Unsupported("no usable as-shot white balance in the file and none was supplied".into())),
        },
    };

    let black = &sensor.black;
    let per_position = (black.rows.max(1) * black.cols.max(1) * sensor.samples.max(1)).min(black.values.len());
    let levels = black.values.get(..per_position).unwrap_or(&[]);
    let black_level = [levels.iter().cloned().fold(f32::INFINITY, f32::min), levels.iter().cloned().fold(f32::NEG_INFINITY, f32::max)].map(|v| if v.is_finite() { f64::from(v) } else { 0.0 });
    let white_level = sensor.white.map(f64::from);
    let crop = [sensor.crop.x, sensor.crop.y, sensor.crop.width, sensor.crop.height];
    let color = sensor.color.clone();
    let gain = 2f64.powf(sensor.baseline_exposure);

    let opts = DevelopOptions {
        demosaic: o.demosaic,
        white_balance,
        exposure: 0.0,
        orient: o.apply_orientation,
        tone: Tone::NEUTRAL,
        limits: o.limits,
        camera_look: None,
    };
    let started = Instant::now();
    let developed = photocraft_raw::develop_sensor_linear(&sensor, &opts)?;
    let develop_ms = started.elapsed().as_secs_f64() * 1e3;
    drop(sensor);

    let expected = (developed.width as usize).checked_mul(developed.height as usize).and_then(|n| n.checked_mul(3));
    if expected != Some(developed.rgb.len()) || developed.rgb.is_empty() {
        return Err(RawError::Malformed("the developed frame does not have width x height x 3 samples".into()));
    }
    let info = developed.info;
    // PhotoCraft says so when it had to guess; with the checks above it must not.
    if developed.warnings.iter().any(|w| w.contains("grey world") || w.contains("sRGB primaries")) {
        return Err(RawError::Unsupported(format!("PhotoCraft fell back to an uncalibrated rendering: {}", developed.warnings.join("; "))));
    }
    warnings.extend(developed.warnings);
    // The matrix PhotoCraft built from the white balance it resolved: its
    // multipliers are 1 / neutral, so the neutral is their reciprocal.
    let camera_to_prophoto = if monochrome {
        [[gain, 0.0, 0.0], [0.0, gain, 0.0], [0.0, 0.0, gain]]
    } else {
        let reciprocal = info.wb_multipliers.map(|m| 1.0 / m);
        let top = reciprocal.iter().cloned().fold(f64::MIN, f64::max);
        let to_xyz = color.balanced_to_xyz_d50(reciprocal.map(|v| v / top));
        match (xyz_d50_to_prophoto(), to_xyz) {
            (Some(p), Some(m)) => mat_mul(&p, &m).map(|row| row.map(|v| v * gain)),
            _ => return Err(RawError::Malformed("the camera colour calibration does not give a camera to ProPhoto matrix".into())),
        }
    };
    let camera_to_prophoto = [0, 1, 2, 3, 4, 5, 6, 7, 8].map(|i| camera_to_prophoto[i / 3][i % 3]);
    Ok(Output {
        rgb: developed.rgb,
        report: Report {
            width: developed.width,
            height: developed.height,
            format: info.format.name(),
            make: info.make.unwrap_or_default(),
            model: info.model.unwrap_or_default(),
            sensor_width: info.sensor_width,
            sensor_height: info.sensor_height,
            crop,
            cfa: info.cfa.unwrap_or_default(),
            orientation: info.orientation,
            orientation_applied: o.apply_orientation,
            monochrome,
            wb_source,
            wb_multipliers: info.wb_multipliers,
            calibration_source,
            baseline_exposure_ev,
            baseline_exposure_applied: o.apply_baseline_exposure,
            profile_tone_dropped,
            camera_to_prophoto,
            black_level,
            white_level,
            decode_ms,
            develop_ms,
        },
        warnings,
    })
}

/// The container family of `bytes` (`"DNG"`, `"ARW"`, …), or `None` when
/// PhotoCraft does not recognise them as a camera raw file. Header and IFDs only.
pub fn identify(bytes: &[u8]) -> Option<&'static str> {
    photocraft_raw::identify(bytes).map(|f| f.name())
}

// ---------------------------------------------------------------------------
// The C ABI. `engine/src/io/photocraft_raw_abi.h` declares the same layouts.
// ---------------------------------------------------------------------------

/// `spk_pcraw_options`. Zero-initialise, set `struct_size` and `abi_version`,
/// then the fields that matter.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SpkPcrawOptions {
    /// `sizeof(spk_pcraw_options)` as the caller compiled it.
    pub struct_size: u32,
    /// [`ABI_VERSION`] as the caller compiled it.
    pub abi_version: u32,
    /// Cap on one buffer and on peak working memory, bytes; 0: [`DEFAULT_MAX_ALLOC`].
    pub max_alloc_bytes: u64,
    /// Cap on `width * height`; 0: [`DEFAULT_MAX_PIXELS`].
    pub max_pixels: u64,
    /// Worker threads; 0: one per logical CPU.
    pub threads: u32,
    /// 0 AHD, 1 Malvar–He–Cutler, 2 bilinear. Non-Bayer sensors use PhotoCraft's
    /// one edge-directed method whatever this says.
    pub demosaic: u32,
    /// 0 as shot, 1 `wb_multipliers`.
    pub wb_mode: u32,
    /// Non-zero: rotate / flip the pixels to the file's orientation.
    pub apply_orientation: u32,
    /// Non-zero: multiply by 2^BaselineExposure.
    pub apply_baseline_exposure: u32,
    /// Non-zero: `fallback_wb_multipliers` holds another reader's as-shot multipliers.
    pub has_fallback_wb: u32,
    /// Non-zero: `color_matrix` and `color_matrix_temperature_k` are set.
    pub has_color_matrix: u32,
    pub reserved: u32,
    /// R, G, B camera multipliers (`wb_mode` 1).
    pub wb_multipliers: [f64; 3],
    pub fallback_wb_multipliers: [f64; 3],
    /// XYZ → camera, row-major 3 × 3 (DNG ColorMatrix semantics).
    pub color_matrix: [f64; 9],
    /// Temperature in kelvin of the illuminant `color_matrix` was made for.
    pub color_matrix_temperature_k: f64,
}

/// `spk_pcraw_info`: [`Report`] in fixed-size C terms. Strings are UTF-8,
/// NUL-terminated and truncated to fit.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SpkPcrawInfo {
    pub struct_size: u32,
    pub width: u32,
    pub height: u32,
    pub sensor_width: u32,
    pub sensor_height: u32,
    pub crop_x: u32,
    pub crop_y: u32,
    pub crop_width: u32,
    pub crop_height: u32,
    pub orientation: u32,
    pub orientation_applied: u32,
    pub monochrome: u32,
    pub wb_source: u32,
    pub calibration_source: u32,
    pub baseline_exposure_applied: u32,
    pub profile_tone_dropped: u32,
    pub wb_multipliers: [f64; 3],
    pub baseline_exposure_ev: f64,
    /// White-balanced camera RGB → linear ProPhoto, row-major 3 × 3, exposure
    /// gain included.
    pub camera_to_prophoto: [f64; 9],
    pub black_level_min: f64,
    pub black_level_max: f64,
    pub white_level: [f64; 3],
    pub decode_ms: f64,
    pub develop_ms: f64,
    pub format: [c_char; 16],
    pub make: [c_char; 64],
    pub model: [c_char; 64],
    pub cfa: [c_char; 160],
}

const _: () = assert!(size_of::<SpkPcrawOptions>() == 184);
const _: () = assert!(size_of::<SpkPcrawInfo>() == 528);

/// `spk_pcraw_result`: opaque to C.
pub struct SpkPcrawResult {
    status: Status,
    error: CString,
    warnings: CString,
    info: Option<SpkPcrawInfo>,
    rgb: Vec<f32>,
}

fn c_string(s: &str) -> CString {
    CString::new(s.replace('\0', " ")).unwrap_or_default()
}

fn fill<const N: usize>(text: &str) -> [c_char; N] {
    let mut out = [0 as c_char; N];
    // Whole UTF-8 characters only, leaving room for the terminator.
    let mut end = text.len().min(N.saturating_sub(1));
    while end > 0 && !text.is_char_boundary(end) {
        end -= 1;
    }
    for (o, b) in out.iter_mut().zip(text.as_bytes()[..end].iter()) {
        *o = *b as c_char;
    }
    out
}

fn clamp_u32(v: usize) -> u32 {
    u32::try_from(v).unwrap_or(u32::MAX)
}

fn info_of(r: &Report) -> SpkPcrawInfo {
    SpkPcrawInfo {
        struct_size: size_of::<SpkPcrawInfo>() as u32,
        width: r.width,
        height: r.height,
        sensor_width: clamp_u32(r.sensor_width),
        sensor_height: clamp_u32(r.sensor_height),
        crop_x: clamp_u32(r.crop[0]),
        crop_y: clamp_u32(r.crop[1]),
        crop_width: clamp_u32(r.crop[2]),
        crop_height: clamp_u32(r.crop[3]),
        orientation: u32::from(r.orientation),
        orientation_applied: u32::from(r.orientation_applied),
        monochrome: u32::from(r.monochrome),
        wb_source: r.wb_source as u32,
        calibration_source: r.calibration_source as u32,
        baseline_exposure_applied: u32::from(r.baseline_exposure_applied),
        profile_tone_dropped: u32::from(r.profile_tone_dropped),
        wb_multipliers: r.wb_multipliers,
        baseline_exposure_ev: r.baseline_exposure_ev,
        camera_to_prophoto: r.camera_to_prophoto,
        black_level_min: r.black_level[0],
        black_level_max: r.black_level[1],
        white_level: r.white_level,
        decode_ms: r.decode_ms,
        develop_ms: r.develop_ms,
        format: fill(r.format),
        make: fill(&r.make),
        model: fill(&r.model),
        cfa: fill(&r.cfa),
    }
}

fn failure(status: Status, message: String) -> SpkPcrawResult {
    SpkPcrawResult { status, error: c_string(&message), warnings: CString::default(), info: None, rgb: Vec::new() }
}

fn options_of(c: &SpkPcrawOptions) -> Result<Options, String> {
    if c.abi_version != ABI_VERSION {
        return Err(format!("the caller was built for PhotoCraft RAW ABI {}, this library is ABI {ABI_VERSION}", c.abi_version));
    }
    if c.struct_size as usize != size_of::<SpkPcrawOptions>() {
        return Err(format!("spk_pcraw_options is {} bytes for the caller and {} here", c.struct_size, size_of::<SpkPcrawOptions>()));
    }
    let demosaic = match c.demosaic {
        0 => Demosaic::Ahd,
        1 => Demosaic::Mhc,
        2 => Demosaic::Bilinear,
        n => return Err(format!("unknown demosaic {n}")),
    };
    let white_balance = match c.wb_mode {
        0 => WbRequest::AsShot,
        1 => WbRequest::Multipliers(c.wb_multipliers),
        n => return Err(format!("unknown white-balance mode {n}")),
    };
    let m = &c.color_matrix;
    let defaults = Limits::default();
    Ok(Options {
        limits: Limits {
            max_alloc: if c.max_alloc_bytes == 0 { DEFAULT_MAX_ALLOC } else { c.max_alloc_bytes },
            max_pixels: if c.max_pixels == 0 { DEFAULT_MAX_PIXELS } else { c.max_pixels },
            ..defaults
        },
        threads: c.threads as usize,
        demosaic,
        white_balance,
        fallback_wb: (c.has_fallback_wb != 0).then_some(c.fallback_wb_multipliers),
        color_matrix: (c.has_color_matrix != 0).then_some(([[m[0], m[1], m[2]], [m[3], m[4], m[5]], [m[6], m[7], m[8]]], c.color_matrix_temperature_k)),
        apply_orientation: c.apply_orientation != 0,
        apply_baseline_exposure: c.apply_baseline_exposure != 0,
    })
}

fn panic_message(payload: Box<dyn std::any::Any + Send>) -> String {
    let text = payload.downcast_ref::<&str>().map(|s| (*s).to_owned()).or_else(|| payload.downcast_ref::<String>().cloned());
    format!("internal error in the PhotoCraft RAW decoder: {}", text.unwrap_or_else(|| "panic".into()))
}

fn develop_c(bytes: &[u8], options: &SpkPcrawOptions) -> SpkPcrawResult {
    let options = match options_of(options) {
        Ok(o) => o,
        Err(message) => return failure(Status::InvalidArgument, message),
    };
    match catch_unwind(AssertUnwindSafe(|| develop(bytes, &options))) {
        Ok(Ok(out)) => SpkPcrawResult { status: Status::Ok, error: CString::default(), warnings: c_string(&out.warnings.join("\n")), info: Some(info_of(&out.report)), rgb: out.rgb },
        Ok(Err(e)) => failure(status_of(&e), e.to_string()),
        Err(payload) => failure(Status::Internal, panic_message(payload)),
    }
}

/// [`ABI_VERSION`] of this library.
#[unsafe(no_mangle)]
pub extern "C" fn spk_pcraw_abi_version() -> u32 {
    ABI_VERSION
}

/// The PhotoCraft commit this library was written against, NUL-terminated,
/// static.
#[unsafe(no_mangle)]
pub extern "C" fn spk_pcraw_photocraft_commit() -> *const c_char {
    concat!(photocraft_commit!(), "\0").as_ptr().cast()
}

/// Writes the container family of `bytes` (`"DNG"`, `"ARW"`, …) into `out`
/// (NUL-terminated, truncated to `out_len`) and returns 1; returns 0 when
/// PhotoCraft does not recognise the bytes as a camera raw file, or on a null
/// argument.
///
/// # Safety
/// `bytes` must be readable for `len` bytes and `out` writable for `out_len`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn spk_pcraw_identify(bytes: *const u8, len: usize, out: *mut c_char, out_len: usize) -> i32 {
    if bytes.is_null() || out.is_null() || out_len == 0 {
        return 0;
    }
    // SAFETY: the caller guarantees `bytes` is readable for `len` bytes.
    let data = unsafe { std::slice::from_raw_parts(bytes, len) };
    let Ok(Some(name)) = catch_unwind(|| identify(data)) else { return 0 };
    let n = name.len().min(out_len - 1);
    // SAFETY: `out` is writable for `out_len` bytes and `n < out_len`.
    unsafe {
        std::ptr::copy_nonoverlapping(name.as_ptr().cast::<c_char>(), out, n);
        *out.add(n) = 0;
    }
    1
}

/// Decodes and develops `len` bytes of a camera raw file. Always returns a
/// result to be released with [`spk_pcraw_free`]; [`spk_pcraw_status`] says
/// whether it holds a frame. Returns null only when `out` of memory for the
/// result record itself aborts the process, i.e. never to the caller.
///
/// # Safety
/// `bytes` must be readable for `len` bytes and `options` must point to a
/// `spk_pcraw_options` (either may be null: that is reported, not dereferenced).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn spk_pcraw_develop(bytes: *const u8, len: usize, options: *const SpkPcrawOptions) -> *mut SpkPcrawResult {
    let result = if bytes.is_null() || options.is_null() {
        failure(Status::InvalidArgument, "spk_pcraw_develop was given a null pointer".into())
    } else {
        // SAFETY: both pointers are non-null and the caller guarantees what they
        // point to; the options are read unaligned in case the caller packed them.
        let (data, options) = unsafe { (std::slice::from_raw_parts(bytes, len), std::ptr::read_unaligned(options)) };
        develop_c(data, &options)
    };
    Box::into_raw(Box::new(result))
}

/// The [`Status`] of a result; [`Status::InvalidArgument`] for null.
///
/// # Safety
/// `result` must be null or a live pointer from [`spk_pcraw_develop`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn spk_pcraw_status(result: *const SpkPcrawResult) -> i32 {
    // SAFETY: null or live, per the contract.
    unsafe { result.as_ref() }.map_or(Status::InvalidArgument, |r| r.status) as i32
}

/// The error message of a failed result (empty on success), NUL-terminated,
/// valid until [`spk_pcraw_free`].
///
/// # Safety
/// `result` must be null or a live pointer from [`spk_pcraw_develop`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn spk_pcraw_error(result: *const SpkPcrawResult) -> *const c_char {
    // SAFETY: null or live, per the contract.
    unsafe { result.as_ref() }.map_or(c"null result".as_ptr(), |r| r.error.as_ptr())
}

/// The warnings of a successful result, one per line (empty when there are
/// none), NUL-terminated, valid until [`spk_pcraw_free`].
///
/// # Safety
/// `result` must be null or a live pointer from [`spk_pcraw_develop`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn spk_pcraw_warnings(result: *const SpkPcrawResult) -> *const c_char {
    // SAFETY: null or live, per the contract.
    unsafe { result.as_ref() }.map_or(c"".as_ptr(), |r| r.warnings.as_ptr())
}

/// Copies the report of a successful result into `out` and returns 1; returns
/// 0 (and writes nothing) for a failed result, a null argument, or an `out`
/// whose `struct_size` is not this library's `sizeof(spk_pcraw_info)`.
///
/// # Safety
/// `result` must be null or live; `out` must be null or point to a
/// `spk_pcraw_info` whose `struct_size` the caller has set.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn spk_pcraw_get_info(result: *const SpkPcrawResult, out: *mut SpkPcrawInfo) -> i32 {
    // SAFETY: null or live, per the contract.
    let Some(info) = unsafe { result.as_ref() }.and_then(|r| r.info.as_ref()) else { return 0 };
    if out.is_null() {
        return 0;
    }
    // SAFETY: `out` is non-null and points to at least a `struct_size` field.
    let caller_size = unsafe { std::ptr::read_unaligned(out.cast::<u32>()) };
    if caller_size as usize != size_of::<SpkPcrawInfo>() {
        return 0;
    }
    // SAFETY: the caller's struct has this library's size.
    unsafe { std::ptr::write_unaligned(out, *info) };
    1
}

/// The pixels of a successful result: interleaved RGB f32, top row first,
/// `*samples` values (`width * height * 3`). Null with `*samples` 0 for a
/// failed result. Valid until [`spk_pcraw_free`]; the caller copies.
///
/// # Safety
/// `result` must be null or live; `samples` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn spk_pcraw_pixels(result: *const SpkPcrawResult, samples: *mut u64) -> *const f32 {
    // SAFETY: null or live, per the contract.
    let rgb = unsafe { result.as_ref() }.filter(|r| r.status == Status::Ok).map(|r| r.rgb.as_slice()).unwrap_or(&[]);
    if !samples.is_null() {
        // SAFETY: non-null and writable, per the contract.
        unsafe { *samples = rgb.len() as u64 };
    }
    if rgb.is_empty() { std::ptr::null() } else { rgb.as_ptr() }
}

/// Releases a result and everything its accessors handed out. Null is ignored.
///
/// # Safety
/// `result` must be null or a pointer from [`spk_pcraw_develop`] that has not
/// been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn spk_pcraw_free(result: *mut SpkPcrawResult) {
    if !result.is_null() {
        // SAFETY: a live pointer from `Box::into_raw` in `spk_pcraw_develop`.
        drop(unsafe { Box::from_raw(result) });
    }
}
