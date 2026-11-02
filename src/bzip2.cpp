#include "bzip2.h"

#include <array>

namespace {

// MSB-first bitstream reader (bzip2 packs bits big-endian within each byte).
struct BitReader {
    const uint8_t* d;
    size_t n;    // bytes
    size_t pos = 0;  // bit position
    bool bad = false;

    BitReader(const uint8_t* data, size_t size) : d(data), n(size) {}

    uint32_t bits(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; i++) {
            size_t byte = pos >> 3;
            uint32_t b;
            if (byte >= n) { bad = true; b = 0; }
            else b = (d[byte] >> (7 - (int)(pos & 7))) & 1;
            v = (v << 1) | b;
            pos++;
        }
        return v;
    }
    uint64_t bits64(int k) {
        uint64_t v = 0;
        for (int i = 0; i < k; i++) v = (v << 1) | bits(1);
        return v;
    }
};

constexpr int kMaxCodeLen = 23;  // bzip2 caps Huffman code length at 20-23 depending on version
constexpr int kGroupSize = 50;   // symbols per selector group
constexpr int kMaxAlpha = 258;   // 256 byte values + RUNA/RUNB + EOB

// bzip2's fixed pseudorandom table for the legacy "randomised block" mode (randtable.c). GameMaker's
// texture compressor still emits this (otherwise-deprecated, pre-0.9.5) mode.
const int kRNums[512] = {
    619, 720, 127, 481, 931, 816, 813, 233, 566, 247, 985, 724,
    205, 454, 863, 491, 741, 242, 949, 214, 733, 859, 335, 708,
    621, 574, 73, 654, 730, 472, 419, 436, 278, 496, 867, 210,
    399, 680, 480, 51, 878, 465, 811, 169, 869, 675, 611, 697,
    867, 561, 862, 687, 507, 283, 482, 129, 807, 591, 733, 623,
    150, 238, 59, 379, 684, 877, 625, 169, 643, 105, 170, 607,
    520, 932, 727, 476, 693, 425, 174, 647, 73, 122, 335, 530,
    442, 853, 695, 249, 445, 515, 909, 545, 703, 919, 874, 474,
    882, 500, 594, 612, 641, 801, 220, 162, 819, 984, 589, 513,
    495, 799, 161, 604, 958, 533, 221, 400, 386, 867, 600, 782,
    382, 596, 414, 171, 516, 375, 682, 485, 911, 276, 98, 553,
    163, 354, 666, 933, 424, 341, 533, 870, 227, 730, 475, 186,
    263, 647, 537, 686, 600, 224, 469, 68, 770, 919, 190, 373,
    294, 822, 808, 206, 184, 943, 795, 384, 383, 461, 404, 758,
    839, 887, 715, 67, 618, 276, 204, 918, 873, 777, 604, 560,
    951, 160, 578, 722, 79, 804, 96, 409, 713, 940, 652, 934,
    970, 447, 318, 353, 859, 672, 112, 785, 645, 863, 803, 350,
    139, 93, 354, 99, 820, 908, 609, 772, 154, 274, 580, 184,
    79, 626, 630, 742, 653, 282, 762, 623, 680, 81, 927, 626,
    789, 125, 411, 521, 938, 300, 821, 78, 343, 175, 128, 250,
    170, 774, 972, 275, 999, 639, 495, 78, 352, 126, 857, 956,
    358, 619, 580, 124, 737, 594, 701, 612, 669, 112, 134, 694,
    363, 992, 809, 743, 168, 974, 944, 375, 748, 52, 600, 747,
    642, 182, 862, 81, 344, 805, 988, 739, 511, 655, 814, 334,
    249, 515, 897, 955, 664, 981, 649, 113, 974, 459, 893, 228,
    433, 837, 553, 268, 926, 240, 102, 654, 459, 51, 686, 754,
    806, 760, 493, 403, 415, 394, 687, 700, 946, 670, 656, 610,
    738, 392, 760, 799, 887, 653, 978, 321, 576, 617, 626, 502,
    894, 679, 243, 440, 680, 879, 194, 572, 640, 724, 926, 56,
    204, 700, 707, 151, 457, 449, 797, 195, 791, 558, 945, 679,
    297, 59, 87, 824, 713, 663, 412, 693, 342, 606, 134, 108,
    571, 364, 631, 212, 174, 643, 304, 329, 343, 97, 430, 751,
    497, 314, 983, 374, 822, 928, 140, 206, 73, 263, 980, 736,
    876, 478, 430, 305, 170, 514, 364, 692, 829, 82, 855, 953,
    676, 246, 369, 970, 294, 750, 807, 827, 150, 790, 288, 923,
    804, 378, 215, 828, 592, 281, 565, 555, 710, 82, 896, 831,
    547, 261, 524, 462, 293, 465, 502, 56, 661, 821, 976, 991,
    658, 869, 905, 758, 745, 193, 768, 550, 608, 933, 378, 286,
    215, 979, 792, 961, 61, 688, 793, 644, 986, 403, 106, 366,
    905, 644, 372, 567, 466, 434, 645, 210, 389, 550, 919, 135,
    780, 773, 635, 389, 707, 100, 626, 958, 165, 504, 920, 176,
    193, 713, 857, 265, 203, 50, 668, 108, 645, 990, 626, 197,
    510, 357, 358, 850, 858, 364, 936, 638,
};

struct HuffTable {
    int minLen = 0, maxLen = 0;
    int limit[kMaxCodeLen + 2] = {};
    int base[kMaxCodeLen + 2] = {};
    int perm[kMaxAlpha] = {};
};

// Standard bzip2 canonical-Huffman decode table build (limit/base/perm), from the per-symbol code lengths.
void build_table(HuffTable& t, const uint8_t* len, int alphaSize) {
    t.minLen = 32;
    t.maxLen = 0;
    for (int i = 0; i < alphaSize; i++) {
        if (len[i] > t.maxLen) t.maxLen = len[i];
        if (len[i] < t.minLen) t.minLen = len[i];
    }
    int pp = 0;
    for (int length = t.minLen; length <= t.maxLen; length++)
        for (int sym = 0; sym < alphaSize; sym++)
            if (len[sym] == length) t.perm[pp++] = sym;

    for (int i = 0; i <= kMaxCodeLen + 1; i++) t.base[i] = 0;
    for (int sym = 0; sym < alphaSize; sym++) t.base[len[sym] + 1]++;
    for (int i = 1; i <= kMaxCodeLen + 1; i++) t.base[i] += t.base[i - 1];

    for (int i = 0; i <= kMaxCodeLen + 1; i++) t.limit[i] = 0;
    int vec = 0;
    for (int length = t.minLen; length <= t.maxLen; length++) {
        vec += t.base[length + 1] - t.base[length];
        t.limit[length] = vec - 1;
        vec <<= 1;
    }
    for (int length = t.minLen + 1; length <= t.maxLen; length++)
        t.base[length] = ((t.limit[length - 1] + 1) << 1) - t.base[length];
}

int decode_symbol(BitReader& br, const HuffTable& t) {
    int zn = t.minLen;
    int zvec = (int)br.bits(zn);
    while (zvec > t.limit[zn]) {
        zn++;
        if (zn > kMaxCodeLen) return -1;
        zvec = (zvec << 1) | (int)br.bits(1);
    }
    int idx = zvec - t.base[zn];
    if (idx < 0 || idx >= kMaxAlpha) return -1;
    return t.perm[idx];
}

}  // namespace

bool bzip2_decompress(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out) {
    if (srcLen < 10 || src[0] != 'B' || src[1] != 'Z' || src[2] != 'h') return false;
    int level = src[3] - '0';
    if (level < 1 || level > 9) return false;
    BitReader br(src, srcLen);
    br.pos = 32;  // "BZh" + level digit

    for (;;) {
        uint64_t magic = br.bits64(48);
        if (br.bad) return false;
        if (magic == 0x177245385090ULL) {  // end-of-stream
            br.bits(32);  // combined CRC, not verified
            break;
        }
        if (magic != 0x314159265359ULL) return false;

        br.bits(32);  // per-block CRC, not verified
        // legacy "randomised" block mode (deprecated since bzip2 0.9.5, but still emitted by
        // GameMaker's texture compressor): when set, output bytes get XORed per BZ2_rNums below.
        bool randomised = br.bits(1) != 0;
        uint32_t origPtr = br.bits(24);

        // symbol map: which of the 256 byte values occur in this block
        uint16_t used16 = (uint16_t)br.bits(16);
        uint8_t symMap[256];
        int nSymMap = 0;
        for (int i = 0; i < 16; i++) {
            if (!((used16 >> (15 - i)) & 1)) continue;
            uint16_t grp = (uint16_t)br.bits(16);
            for (int j = 0; j < 16; j++)
                if ((grp >> (15 - j)) & 1) symMap[nSymMap++] = (uint8_t)(i * 16 + j);
        }
        if (nSymMap == 0 || br.bad) return false;
        int alphaSize = nSymMap + 2;

        int nGroups = (int)br.bits(3);
        if (nGroups < 2 || nGroups > 6) return false;
        int nSelectors = (int)br.bits(15);
        if (nSelectors <= 0) return false;

        std::vector<uint8_t> selectorMtf(nSelectors);
        for (int i = 0; i < nSelectors; i++) {
            int j = 0;
            while (br.bits(1)) {
                if (++j >= nGroups) return false;
                if (br.bad) return false;
            }
            selectorMtf[i] = (uint8_t)j;
        }
        uint8_t groupMtf[6] = {0, 1, 2, 3, 4, 5};
        std::vector<uint8_t> selectors(nSelectors);
        for (int i = 0; i < nSelectors; i++) {
            int j = selectorMtf[i];
            uint8_t v = groupMtf[j];
            for (; j > 0; j--) groupMtf[j] = groupMtf[j - 1];
            groupMtf[0] = v;
            selectors[i] = v;
        }

        std::array<std::array<uint8_t, kMaxAlpha>, 6> lens{};
        HuffTable tables[6];
        for (int g = 0; g < nGroups; g++) {
            int curr = (int)br.bits(5);
            for (int sym = 0; sym < alphaSize; sym++) {
                for (;;) {
                    if (curr < 1 || curr > kMaxCodeLen) return false;
                    if (!br.bits(1)) break;
                    if (br.bits(1)) curr--;
                    else curr++;
                }
                lens[g][sym] = (uint8_t)curr;
            }
            build_table(tables[g], lens[g].data(), alphaSize);
        }
        if (br.bad) return false;

        // MTF + RLE2 + Huffman decode -> BWT buffer (the block, pre-inverse-BWT)
        std::vector<uint8_t> mtf(symMap, symMap + nSymMap);
        std::vector<uint8_t> bwt;
        bwt.reserve((size_t)level * 100000 + 64);
        int groupPos = 0, selIdx = 0, curGroup = -1;
        int64_t runLen = 0, runBit = 0;
        const int EOB = alphaSize - 1;
        for (;;) {
            if (groupPos == 0) {
                if (selIdx >= (int)selectors.size()) return false;
                curGroup = selectors[selIdx++];
                groupPos = kGroupSize;
            }
            groupPos--;
            int sym = decode_symbol(br, tables[curGroup]);
            if (sym < 0 || br.bad) return false;
            if (sym == 0 || sym == 1) {  // RUNA / RUNB: bijective base-2 run of the MTF front byte
                runLen += (int64_t)(sym + 1) << runBit;
                runBit++;
                if (runLen > 100 * 1000000) return false;  // sanity cap
                continue;
            }
            if (runLen > 0) {
                bwt.insert(bwt.end(), (size_t)runLen, mtf[0]);
                runLen = 0;
                runBit = 0;
            }
            if (sym == EOB) break;
            int idx = sym - 1;
            if (idx < 0 || idx >= (int)mtf.size()) return false;
            uint8_t b = mtf[idx];
            mtf.erase(mtf.begin() + idx);
            mtf.insert(mtf.begin(), b);
            bwt.push_back(b);
            if (bwt.size() > 9000100) return false;  // bzip2's hard per-block cap (level 9)
        }

        size_t nblock = bwt.size();
        if (nblock == 0 || origPtr >= nblock) return false;

        // inverse Burrows-Wheeler transform
        uint32_t count[256] = {};
        for (uint8_t b : bwt) count[b]++;
        uint32_t cum[256] = {};
        uint32_t sum = 0;
        for (int i = 0; i < 256; i++) { cum[i] = sum; sum += count[i]; }
        std::vector<uint32_t> next(nblock);
        for (size_t i = 0; i < nblock; i++) {
            uint8_t b = bwt[i];
            next[cum[b]] = (uint32_t)i;
            cum[b]++;
        }
        std::vector<uint8_t> block(nblock);
        uint32_t p = next[origPtr];
        if (randomised) {
            int rNToGo = 0, rTPos = 0;
            for (size_t i = 0; i < nblock; i++) {
                if (rNToGo == 0) {
                    rNToGo = kRNums[rTPos];
                    rTPos = (rTPos + 1) % 512;
                }
                rNToGo--;
                block[i] = bwt[p] ^ (rNToGo == 1 ? 1 : 0);
                p = next[p];
            }
        } else {
            for (size_t i = 0; i < nblock; i++) {
                block[i] = bwt[p];
                p = next[p];
            }
        }

        // undo the RLE1 pre-pass: 4 equal bytes are always followed by a repeat count byte (0-255)
        size_t i = 0;
        while (i < block.size()) {
            uint8_t b = block[i];
            size_t run = 1;
            while (run < 4 && i + run < block.size() && block[i + run] == b) run++;
            out.insert(out.end(), run, b);
            i += run;
            if (run == 4) {
                if (i >= block.size()) return false;
                out.insert(out.end(), block[i], b);
                i++;
            }
        }
    }
    return true;
}
