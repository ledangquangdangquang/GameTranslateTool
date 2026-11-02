#include "catalog.h"

#include <algorithm>

CatalogPatch patch_catalog(const fs::path& catalogBin, const std::string& bundleName,
                           const std::vector<uint64_t>& candidates, uint64_t newSize) {
    CatalogPatch res;
    if (newSize > 0xFFFFFFFFull) {
        res.error = "bundle lon hon 4GB";
        return res;
    }
    fs::path bak = catalogBin;
    bak += ".bak";
    if (!fs::exists(bak)) fs::copy_file(catalogBin, bak);

    Bytes d = read_file(catalogBin);
    auto it = std::search(d.begin(), d.end(), bundleName.begin(), bundleName.end());
    if (it == d.end()) {
        res.error = "khong thay ten bundle trong catalog.bin";
        return res;
    }
    size_t name = (size_t)(it - d.begin());
    size_t end = std::min(d.size(), name + bundleName.size() + 600);
    size_t at = 0;
    for (uint64_t oldSize : candidates) {
        if (oldSize > 0xFFFFFFFFull) continue;
        uint8_t needle[4];
        for (int i = 0; i < 4; i++) needle[i] = (uint8_t)(oldSize >> (8 * i));
        auto sz = std::search(d.begin() + name, d.begin() + end, needle, needle + 4);
        if (sz != d.begin() + end && sz - d.begin() >= 4) {
            at = (size_t)(sz - d.begin());
            break;
        }
    }
    if (!at) {
        res.error = "khong tim thay truong kich thuoc gan ten bundle (dinh dang catalog khac?) - khong sua gi";
        return res;
    }
    res.oldCrc = d[at - 4] | d[at - 3] << 8 | d[at - 2] << 16 | (uint32_t)d[at - 1] << 24;
    for (int i = 0; i < 4; i++) {
        d[at - 4 + i] = 0;
        d[at + i] = (uint8_t)(newSize >> (8 * i));
    }
    write_file(catalogBin, d);
    res.ok = true;
    return res;
}
