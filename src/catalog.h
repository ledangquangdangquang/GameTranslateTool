// Addressables catalog.bin / catalog.json: after a bundle is modified its stored CRC no longer
// matches and Unity refuses to load it (black screen). Set CRC=0 (= "skip check").
// catalog.bin: the fields are located heuristically: bundle file name, then its old size
// (uint32 LE) within a short window after it; the CRC is the 4 bytes before the size.
// catalog.json: no catalog.bin exists at all (older/"legacy" Addressables JSON catalog format -
// ROADMAP item 2). m_ExtraDataString is base64 of a sequence of length-prefixed records, each
// one object-type byte (0x07 = JsonObject here, always, for AssetBundleRequestOptions) + a
// 1-byte-length-prefixed ASCII assembly name + a 1-byte-length-prefixed ASCII class name + a
// 4-byte LE byte-length + that many bytes of UTF-16LE JSON text (JsonUtility.ToJson output,
// confirmed by decompiling Unity.Addressables.dll's SerializationUtilities/ContentCatalogData
// with ilspycmd and IL-walking ReadObjectFromByteArray/CreateLocator - there is no catalog.bin
// equivalent of a readable bundle name to anchor on, so records are matched by their
// "m_BundleSize" field against `candidates` instead, same best-effort spirit as catalog.bin).
// m_Crc is overwritten **in place** with ASCII '0' left-padded with spaces (valid JSON
// whitespace before a value) so the record's byte length - and therefore every later record's
// offset, and every m_EntryDataString dataIndex that points at one - never changes. m_BundleSize
// is deliberately left untouched: it is only used by Addressables' download-size estimate
// (ComputeSize), never by the actual AssetBundle.LoadFromFile(path, crc) load path, so it isn't
// worth the much larger risk of having to resize+reindex the whole catalog when the translated,
// uncompressed bundle needs more digits than the original.
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

// Same idea as patch_catalog, for the JSON catalog format. newSize is unused (see comment above)
// but kept so call sites can pass the same arguments as patch_catalog.
CatalogPatch patch_catalog_json(const fs::path& catalogJson, const std::vector<uint64_t>& candidates,
                                 uint64_t newSize);
