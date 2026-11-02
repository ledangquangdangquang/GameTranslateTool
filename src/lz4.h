// LZ4 block decompression (Unity bundles use raw LZ4/LZ4HC blocks, no frame header).
#pragma once
#include <cstddef>
#include <cstdint>

// Returns false on corrupt input or if the output size does not match exactly.
bool lz4_decompress(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen);
