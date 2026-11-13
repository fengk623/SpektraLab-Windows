#pragma once

// linear_prophoto_icc.hpp -- the engine's working space as an ICC profile.
//
// What a linear TIFF of a decoded frame is tagged with: ROMM (ProPhoto)
// primaries, D50 white, a linear tone response. An ICC v4.3 matrix/TRC
// display profile whose colorants are the working-space matrix the host
// already uses for untagged deep TIFFs (`linear_prophoto_space()`), so a
// reader that honours the profile and one that assumes the working space see
// the same numbers.
//
// The bytes are fixed: a constant creation date, no clock, no build stamp.
// That makes the profile checkable from outside: its ICC Profile ID (the MD5
// the v4 header carries at bytes 84..99) is the same in every build, and the
// host recognises its own profile by comparing bytes, then takes the samples
// as they are instead of passing them through a matrix that is identity only
// to rounding.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spk::io {

inline constexpr const char* kLinearProphotoIccDescription = "SpektraLab Linear ProPhoto RGB (ROMM, D50, gamma 1.0)";

// Linear ProPhoto RGB -> PCS XYZ (D50), row-major. The same nine numbers as
// spkhost::linear_prophoto_space().to_xyz_d50; a host test holds them together.
inline constexpr std::array<double, 9> kLinearProphotoToXyzD50 = {
    0.7976749, 0.1351917, 0.0313534,
    0.2880402, 0.7118741, 0.0000857,
    0.0,       0.0,       0.8252100};

// The profile. Built once; the reference stays valid for the process.
const std::vector<std::uint8_t>& linear_prophoto_icc();
// Its ICC Profile ID as 32 lower-case hex digits.
std::string linear_prophoto_icc_id();
// Whether these bytes are exactly that profile.
bool is_linear_prophoto_icc(const std::uint8_t* data, std::size_t size);

// MD5 (RFC 1321), for the ICC Profile ID only. Not for security.
std::array<std::uint8_t, 16> md5(const std::uint8_t* data, std::size_t size);

}  // namespace spk::io
