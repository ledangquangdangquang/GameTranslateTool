// Addressables catalog.bin: after a bundle is modified its stored CRC no longer matches and
// Unity refuses to load it (black screen). Set CRC=0 (= "skip check") and update the size.
// The fields are located heuristically: bundle file name, then its old size (uint32 LE)
// within a short window after it; the CRC is the 4 bytes before the size.
#pragma once
#include "bytes.h"

struct CatalogPatch {
    bool ok = false;
    uint32_t oldCrc = 0;
    std::string error;
};

// candidates: sizes the catalog may currently hold for this bundle (last written, live file, original)
CatalogPatch patch_catalog(const fs::path& catalogBin, const std::string& bundleName,
                           const std::vector<uint64_t>& candidates, uint64_t newSize);
