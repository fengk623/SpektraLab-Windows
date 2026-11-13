// raw_tiff_cli.hpp -- `spektralab-host raw-to-tiff`: one RAW file to a linear
// ProPhoto TIFF, through the host's own input boundary.
//
// The standalone acceptance entry for the RAW decoders. It decodes with
// decode_linear_prophoto -- the function `open`, a drop on the window, a
// folder step and a batch export all decode with -- so the TIFF holds exactly
// the pixels the engine would be handed, and writes them with the host's TIFF
// writer under the host's linear ProPhoto profile. By default it then reads
// the file back through the same reader the app uses for a TIFF and reports
// the difference. No engine, no GPU and no stdin protocol are involved.
#pragma once

#include <string>
#include <vector>

namespace spkhost {

// `args` is the whole UTF-8 command line; args[1] is "raw-to-tiff".
// 0 success, 2 usage, 3 the file is not developed by the chosen decoder,
// 4 decode failed, 5 the TIFF or the report could not be written,
// 6 the TIFF did not read back as written.
int raw_to_tiff_main(const std::vector<std::string>& args);

}  // namespace spkhost
