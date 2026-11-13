//! The wrapper against synthetic DNG fixtures whose expected values come from
//! an independent implementation of the DNG colour mapping
//! (`engine/tools/make_photocraft_fixtures.py`), and against the C ABI.

use std::ffi::CStr;

use spk_photocraft_raw::*;

#[allow(dead_code)]
mod expected {
    include!("expected.rs");
}
use expected::{HEIGHT, PATCH, PATCHES, WIDTH};

/// A tracked fixture (`engine/tests/fixtures/photocraft`, or `SPK_PHOTOCRAFT_FIXTURES`).
fn fixture(name: &str) -> Vec<u8> {
    let dir = std::env::var("SPK_PHOTOCRAFT_FIXTURES").unwrap_or_else(|_| format!("{}/../tests/fixtures/photocraft", env!("CARGO_MANIFEST_DIR")));
    let path = format!("{dir}/{name}");
    std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"))
}

/// The mean and the largest deviation from it over the interior of patch `index` (8 pixels in
/// from every edge: farther than any demosaic reaches), in an unrotated `WIDTH x HEIGHT` frame.
fn patch(rgb: &[f32], index: usize) -> ([f64; 3], f64) {
    let (px, py) = (index % (WIDTH / PATCH) * PATCH, index / (WIDTH / PATCH) * PATCH);
    let mut sum = [0.0f64; 3];
    let mut pixels = Vec::new();
    for y in py + 8..py + PATCH - 8 {
        for x in px + 8..px + PATCH - 8 {
            let p = &rgb[(y * WIDTH + x) * 3..][..3];
            pixels.push([f64::from(p[0]), f64::from(p[1]), f64::from(p[2])]);
            for k in 0..3 {
                sum[k] += f64::from(p[k]);
            }
        }
    }
    let mean = sum.map(|v| v / pixels.len() as f64);
    let spread = pixels.iter().flat_map(|p| (0..3).map(|k| (p[k] - mean[k]).abs())).fold(0.0, f64::max);
    (mean, spread)
}

/// Every patch of an unrotated frame against the expected values times `gain`.
fn assert_patches(rgb: &[f32], gain: f64, what: &str) {
    for (i, (name, want)) in PATCHES.iter().enumerate() {
        let (mean, spread) = patch(rgb, i);
        assert!(spread < 1e-5, "{what}: patch {name} is not flat ({spread})");
        for k in 0..3 {
            let want = want[k] * gain;
            assert!((mean[k] - want).abs() < 2e-5 + 2e-5 * want.abs(), "{what}: patch {name} channel {k} is {} and should be {want}", mean[k]);
        }
    }
}

#[test]
fn a_dng_develops_to_the_dng_specifications_linear_prophoto() {
    let out = develop(&fixture("patches.dng"), &Options::default()).unwrap();
    let r = &out.report;
    assert_eq!((r.width as usize, r.height as usize), (WIDTH, HEIGHT));
    assert_eq!(out.rgb.len(), WIDTH * HEIGHT * 3);
    assert_patches(&out.rgb, 1.0, "default");
    assert!(out.rgb.iter().all(|v| v.is_finite()));

    assert_eq!(r.format, "DNG");
    assert_eq!((r.make.as_str(), r.model.as_str()), ("SpektraLab", "SpektraLab Fixture Patches"));
    assert_eq!((r.sensor_width, r.sensor_height, r.crop), (WIDTH, HEIGHT, [0, 0, WIDTH, HEIGHT]));
    assert_eq!(r.cfa, "RGGB");
    assert_eq!((r.orientation, r.orientation_applied, r.monochrome), (1, true, false));
    assert_eq!(r.wb_source, WbSource::FileAsShot);
    assert_eq!(r.calibration_source, CalibrationSource::DngEmbedded);
    for k in 0..3 {
        assert!((r.wb_multipliers[k] - expected::WB_MULTIPLIERS[k]).abs() < 1e-5, "{:?}", r.wb_multipliers);
    }
    assert_eq!((r.baseline_exposure_ev, r.baseline_exposure_applied), (expected::BASELINE_EXPOSURE_EV, false));
    assert_eq!((r.black_level, r.white_level), ([expected::BLACK; 2], [expected::WHITE; 3]));
    assert!(!r.profile_tone_dropped);
    assert!(out.warnings.is_empty(), "{:?}", out.warnings);
}

/// What an encode to 16 bits would clamp away is still there: the blue patch lies outside
/// ProPhoto and keeps its negative green, and with the baseline exposure applied the clipped
/// patch is 2.0.
#[test]
fn values_below_zero_and_above_one_survive() {
    let bytes = fixture("patches.dng");
    let out = develop(&bytes, &Options::default()).unwrap();
    let blue = PATCHES.iter().position(|(name, _)| *name == "blue").unwrap();
    let (mean, _) = patch(&out.rgb, blue);
    assert!(mean[1] < -0.17, "the out-of-gamut green was clamped: {mean:?}");

    let lifted = develop(&bytes, &Options { apply_baseline_exposure: true, ..Options::default() }).unwrap();
    assert!(lifted.report.baseline_exposure_applied);
    assert_patches(&lifted.rgb, 2f64.powf(expected::BASELINE_EXPOSURE_EV), "baseline exposure applied");
    let clip = PATCHES.iter().position(|(name, _)| *name == "clip").unwrap();
    let (mean, _) = patch(&lifted.rgb, clip);
    assert!(mean.iter().all(|v| (*v - 2.0).abs() < 1e-3), "headroom above 1 was lost: {mean:?}");
}

/// Orientation 6 turns the frame a quarter turn clockwise: output (x, y) is source
/// (y, height - 1 - x). Left unapplied, the pixels are the unrotated frame's and the report
/// still names the orientation.
#[test]
fn the_orientation_is_applied_once_or_left_to_the_caller() {
    let upright = develop(&fixture("patches.dng"), &Options::default()).unwrap();
    let turned = develop(&fixture("patches_rot6.dng"), &Options::default()).unwrap();
    assert_eq!((turned.report.width as usize, turned.report.height as usize), (HEIGHT, WIDTH));
    assert_eq!((turned.report.orientation, turned.report.orientation_applied), (6, true));
    for y in 0..WIDTH {
        for x in 0..HEIGHT {
            let (sx, sy) = (y, HEIGHT - 1 - x);
            assert_eq!(turned.rgb[(y * HEIGHT + x) * 3..][..3], upright.rgb[(sy * WIDTH + sx) * 3..][..3], "output ({x}, {y})");
        }
    }
    let left = develop(&fixture("patches_rot6.dng"), &Options { apply_orientation: false, ..Options::default() }).unwrap();
    assert_eq!((left.report.width as usize, left.report.height as usize), (WIDTH, HEIGHT));
    assert_eq!((left.report.orientation, left.report.orientation_applied), (6, false));
    assert!(left.rgb == upright.rgb);
}

/// The reported camera → ProPhoto matrix is the one the frame was made with: applied to each
/// patch's white-balanced camera value (its counts over the range above black, times the
/// reported multipliers, clipped at 1) it gives the patch's developed colour. That is what lets
/// a host go back from a pixel to the camera's response.
#[test]
fn the_reported_matrix_reproduces_the_frame() {
    for apply_baseline_exposure in [false, true] {
        let out = develop(&fixture("patches.dng"), &Options { apply_baseline_exposure, ..Options::default() }).unwrap();
        let (m, wb) = (out.report.camera_to_prophoto, out.report.wb_multipliers);
        // Camera fractions of the non-grey patches, as the generator states them.
        for (name, camera) in [("clip", [1.0, 1.0, 1.0]), ("red", [0.30, 0.12, 0.03]), ("green", [0.06, 0.45, 0.08]), ("blue", [0.0, 0.02, 0.50])] {
            let range = expected::WHITE - expected::BLACK;
            let balanced: Vec<f64> = (0..3).map(|k| (((camera[k] * range).round() / range) * wb[k]).min(1.0)).collect();
            let (mean, _) = patch(&out.rgb, PATCHES.iter().position(|(n, _)| *n == name).unwrap());
            for r in 0..3 {
                let want: f64 = (0..3).map(|c| m[r * 3 + c] * balanced[c]).sum();
                assert!((mean[r] - want).abs() < 3e-5, "{name} row {r}: {} from the frame, {want} from the matrix", mean[r]);
            }
        }
    }
}

#[test]
fn explicit_white_balance_replaces_the_files() {
    let bytes = fixture("patches.dng");
    // The file's own multipliers, passed explicitly: the same patches. (Only their interiors:
    // the multipliers differ in the last digits, and AHD may then pick the other direction on
    // a patch edge.)
    let same = develop(&bytes, &Options { white_balance: WbRequest::Multipliers(expected::WB_MULTIPLIERS), ..Options::default() }).unwrap();
    assert_eq!(same.report.wb_source, WbSource::CallerExplicit);
    assert_patches(&same.rgb, 1.0, "the file's own multipliers, passed explicitly");

    // A warmer balance: the grey patch is no longer neutral.
    let warm = develop(&bytes, &Options { white_balance: WbRequest::Multipliers([3.0, 1.0, 1.2]), ..Options::default() }).unwrap();
    let grey = PATCHES.iter().position(|(name, _)| *name == "mid_grey").unwrap();
    let (mean, _) = patch(&warm.rgb, grey);
    assert!(mean[0] > mean[2] * 1.2, "{mean:?}");

    for bad in [[0.0, 1.0, 1.0], [1.0, f64::NAN, 1.0], [1.0, 1.0, -2.0]] {
        let refused = develop(&bytes, &Options { white_balance: WbRequest::Multipliers(bad), ..Options::default() });
        assert!(matches!(refused, Err(photocraft_raw::RawError::Malformed(_))), "{bad:?}");
    }
}

/// A DNG is developed with its own matrices whatever the caller supplies.
#[test]
fn a_supplied_matrix_does_not_replace_a_dngs_own() {
    let wrong = ([[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]], 6504.0);
    let out = develop(&fixture("patches.dng"), &Options { color_matrix: Some(wrong), fallback_wb: Some([1.0; 3]), ..Options::default() }).unwrap();
    assert_eq!(out.report.calibration_source, CalibrationSource::DngEmbedded);
    assert_eq!(out.report.wb_source, WbSource::FileAsShot);
    assert_patches(&out.rgb, 1.0, "with an unrelated matrix supplied");
}

/// The same sensor data as a plain TIFF/EP raw (no DNGVersion): PhotoCraft has no colour
/// description for it. Without a supplied matrix it is refused, never developed as if the
/// camera channels were sRGB primaries; with the DNG's matrix and multipliers supplied by the
/// caller instead, it develops to the same patches, and the report says where both came from.
#[test]
fn a_raw_without_calibration_needs_the_callers_matrix_and_white_balance() {
    let mut bytes = fixture("patches.dng");
    // Rename the DNGVersion tag (50706) in IFD0 to an unassigned private tag.
    let entries = usize::from(u16::from_le_bytes([bytes[8], bytes[9]]));
    let at = (0..entries).map(|i| 10 + i * 12).find(|&at| u16::from_le_bytes([bytes[at], bytes[at + 1]]) == 50706).unwrap();
    bytes[at..at + 2].copy_from_slice(&65000u16.to_le_bytes());
    assert_ne!(identify(&bytes), Some("DNG"));
    assert!(identify(&bytes).is_some(), "the renamed file is still a raw PhotoCraft recognises");

    let m = expected::COLOR_MATRIX;
    let matrix = ([[m[0], m[1], m[2]], [m[3], m[4], m[5]], [m[6], m[7], m[8]]], 6504.0);
    let bare = develop(&bytes, &Options::default());
    assert!(matches!(&bare, Err(photocraft_raw::RawError::Unsupported(why)) if why.contains("colour calibration")), "{bare:?}");
    let no_wb = develop(&bytes, &Options { color_matrix: Some(matrix), ..Options::default() });
    assert!(matches!(&no_wb, Err(photocraft_raw::RawError::Unsupported(why)) if why.contains("white balance")), "{no_wb:?}");
    let singular = develop(&bytes, &Options { color_matrix: Some(([[1.0, 2.0, 3.0], [2.0, 4.0, 6.0], [0.0, 0.0, 1.0]], 6504.0)), ..Options::default() });
    assert!(matches!(singular, Err(photocraft_raw::RawError::Malformed(_))));

    let out = develop(&bytes, &Options { color_matrix: Some(matrix), fallback_wb: Some(expected::WB_MULTIPLIERS), ..Options::default() }).unwrap();
    assert_eq!(out.report.calibration_source, CalibrationSource::CallerMatrix);
    assert_eq!(out.report.wb_source, WbSource::CallerAsShot);
    assert!(out.warnings.iter().any(|w| w.contains("as-shot")), "{:?}", out.warnings);
    assert_patches(&out.rgb, 1.0, "the caller's matrix and white balance");
}

#[test]
fn damaged_and_oversized_files_are_refused_with_a_reason() {
    use photocraft_raw::RawError;
    let bytes = fixture("patches.dng");
    assert!(matches!(develop(&[], &Options::default()), Err(RawError::NotRaw)));
    assert!(matches!(develop(b"definitely not a camera raw file", &Options::default()), Err(RawError::NotRaw)));
    // The header and the IFD survive; the strip is cut short.
    let cut = &bytes[..bytes.len() - WIDTH * HEIGHT];
    let truncated = develop(cut, &Options::default());
    assert!(matches!(truncated, Err(RawError::Malformed(_))), "{truncated:?}");
    // Header only.
    let stub = develop(&bytes[..64], &Options::default());
    assert!(matches!(stub, Err(RawError::NotRaw | RawError::Malformed(_))), "{stub:?}");

    let mut tight = Options::default();
    tight.limits.max_pixels = (WIDTH * HEIGHT - 1) as u64;
    assert!(matches!(develop(&bytes, &tight), Err(RawError::LimitExceeded(_))));
    let mut tight = Options::default();
    tight.limits.max_alloc = 4096;
    assert!(matches!(develop(&bytes, &tight), Err(RawError::LimitExceeded(_))));
    // A frame that fits upright does not fit with its oriented copy beside it.
    let mut tight = Options::default();
    tight.limits.max_alloc = (WIDTH * HEIGHT * 24 - 1) as u64;
    assert!(matches!(develop(&fixture("patches_rot6.dng"), &tight), Err(RawError::LimitExceeded(_))));
}

#[test]
fn a_bounded_thread_pool_gives_the_same_frame() {
    let bytes = fixture("patches.dng");
    let free = develop(&bytes, &Options::default()).unwrap();
    for threads in [1, 3] {
        let bounded = develop(&bytes, &Options { threads, ..Options::default() }).unwrap();
        assert!(bounded.rgb == free.rgb, "{threads} threads");
    }
}

fn c_options() -> SpkPcrawOptions {
    // SAFETY: plain data; all-zero is the documented starting point.
    let mut o: SpkPcrawOptions = unsafe { std::mem::zeroed() };
    o.struct_size = size_of::<SpkPcrawOptions>() as u32;
    o.abi_version = ABI_VERSION;
    o.apply_orientation = 1;
    o
}

/// The C boundary end to end: a frame, its report, its warnings, ownership, and every way the
/// call itself can be wrong.
#[test]
fn the_c_abi_reports_frames_errors_and_bad_calls() {
    let bytes = fixture("patches.dng");
    assert_eq!(spk_pcraw_abi_version(), ABI_VERSION);
    // SAFETY: the static, NUL-terminated commit string.
    assert_eq!(unsafe { CStr::from_ptr(spk_pcraw_photocraft_commit()) }.to_str().unwrap(), PHOTOCRAFT_COMMIT);

    // SAFETY (the whole block): every pointer passed is valid for the stated length, results
    // are read while live and freed exactly once.
    unsafe {
        let mut name = [0 as std::ffi::c_char; 8];
        assert_eq!(spk_pcraw_identify(bytes.as_ptr(), bytes.len(), name.as_mut_ptr(), name.len()), 1);
        assert_eq!(CStr::from_ptr(name.as_ptr()).to_str().unwrap(), "DNG");
        assert_eq!(spk_pcraw_identify(b"nope".as_ptr(), 4, name.as_mut_ptr(), name.len()), 0);
        assert_eq!(spk_pcraw_identify(std::ptr::null(), 0, name.as_mut_ptr(), name.len()), 0);

        let options = c_options();
        let result = spk_pcraw_develop(bytes.as_ptr(), bytes.len(), &options);
        assert_eq!(spk_pcraw_status(result), Status::Ok as i32);
        assert_eq!(CStr::from_ptr(spk_pcraw_error(result)).to_bytes(), b"");
        assert_eq!(CStr::from_ptr(spk_pcraw_warnings(result)).to_bytes(), b"");
        let mut info: SpkPcrawInfo = std::mem::zeroed();
        info.struct_size = size_of::<SpkPcrawInfo>() as u32;
        assert_eq!(spk_pcraw_get_info(result, &mut info), 1);
        assert_eq!((info.width as usize, info.height as usize), (WIDTH, HEIGHT));
        assert_eq!((info.wb_source, info.calibration_source), (WbSource::FileAsShot as u32, CalibrationSource::DngEmbedded as u32));
        assert_eq!(CStr::from_ptr(info.format.as_ptr()).to_str().unwrap(), "DNG");
        assert_eq!(CStr::from_ptr(info.model.as_ptr()).to_str().unwrap(), "SpektraLab Fixture Patches");
        assert_eq!(CStr::from_ptr(info.cfa.as_ptr()).to_str().unwrap(), "RGGB");
        assert_eq!((info.black_level_min, info.white_level), (expected::BLACK, [expected::WHITE; 3]));
        let mut samples = 0u64;
        let pixels = spk_pcraw_pixels(result, &mut samples);
        assert_eq!(samples as usize, WIDTH * HEIGHT * 3);
        assert_patches(std::slice::from_raw_parts(pixels, samples as usize), 1.0, "through the C ABI");
        // A report struct of another size is refused, not half-written.
        let mut wrong = info;
        wrong.struct_size -= 8;
        assert_eq!(spk_pcraw_get_info(result, &mut wrong), 0);
        spk_pcraw_free(result);

        // A file error: a result with a status and a message, no frame.
        let failed = spk_pcraw_develop(b"not raw".as_ptr(), 7, &options);
        assert_eq!(spk_pcraw_status(failed), Status::NotRaw as i32);
        assert!(!CStr::from_ptr(spk_pcraw_error(failed)).to_bytes().is_empty());
        assert_eq!(spk_pcraw_get_info(failed, &mut info), 0);
        let mut samples = 99u64;
        assert!(spk_pcraw_pixels(failed, &mut samples).is_null());
        assert_eq!(samples, 0);
        spk_pcraw_free(failed);

        // Bad calls.
        for (bytes, options) in [(std::ptr::null(), &options as *const SpkPcrawOptions), (bytes.as_ptr(), std::ptr::null())] {
            let r = spk_pcraw_develop(bytes, 0, options);
            assert_eq!(spk_pcraw_status(r), Status::InvalidArgument as i32);
            spk_pcraw_free(r);
        }
        let mut old = c_options();
        old.abi_version = ABI_VERSION + 1;
        let mut short = c_options();
        short.struct_size -= 8;
        let mut demosaic = c_options();
        demosaic.demosaic = 9;
        let mut mode = c_options();
        mode.wb_mode = 9;
        for bad in [old, short, demosaic, mode] {
            let r = spk_pcraw_develop(bytes.as_ptr(), bytes.len(), &bad);
            assert_eq!(spk_pcraw_status(r), Status::InvalidArgument as i32);
            assert!(!CStr::from_ptr(spk_pcraw_error(r)).to_bytes().is_empty());
            spk_pcraw_free(r);
        }
        assert_eq!(spk_pcraw_status(std::ptr::null()), Status::InvalidArgument as i32);
        spk_pcraw_free(std::ptr::null_mut());
    }
}
