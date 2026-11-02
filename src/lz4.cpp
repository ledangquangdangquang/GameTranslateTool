#include "lz4.h"

#include <cstring>

static bool read_len(const uint8_t* src, size_t srcLen, size_t& ip, size_t& len) {
    uint8_t b;
    do {
        if (ip >= srcLen) return false;
        b = src[ip++];
        len += b;
    } while (b == 255);
    return true;
}

bool lz4_decompress(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    size_t ip = 0, op = 0;
    while (ip < srcLen) {
        uint8_t token = src[ip++];
        size_t lit = token >> 4;
        if (lit == 15 && !read_len(src, srcLen, ip, lit)) return false;
        if (ip + lit > srcLen || op + lit > dstLen) return false;
        std::memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;
        if (ip >= srcLen) break;  // last sequence has literals only
        if (ip + 2 > srcLen) return false;
        size_t off = src[ip] | (src[ip + 1] << 8);
        ip += 2;
        if (off == 0 || off > op) return false;
        size_t match = token & 15;
        if (match == 15 && !read_len(src, srcLen, ip, match)) return false;
        match += 4;
        if (op + match > dstLen) return false;
        for (size_t k = 0; k < match; k++, op++) dst[op] = dst[op - off];  // may overlap
    }
    return op == dstLen;
}
