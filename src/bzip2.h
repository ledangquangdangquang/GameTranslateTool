// bzip2 stream decompression (decode only - GameMaker's compressed texture pages, "2zoq" magic +
// w/h/decompressed-size header + a plain "BZh..." stream wrapping the original QOI/PNG blob).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Decompresses one complete bzip2 stream starting at src[0] == 'B','Z','h'. Appends to out.
// False on corrupt or unsupported input (the legacy pre-0.9.5 "randomized" block mode isn't handled -
// no known encoder still emits it).
bool bzip2_decompress(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out);
