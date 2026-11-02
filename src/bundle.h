// UnityFS asset bundle: read (none / LZ4 / LZ4HC blocks) and write back uncompressed.
#pragma once
#include "bytes.h"

struct BundleNode {
    std::string path;  // e.g. CAB-0123abcd..., CAB-...resS
    uint32_t flags = 0;  // bit 2 (4) = SerializedFile
    Bytes data;
    bool is_serialized() const { return (flags & 4) != 0; }
};

struct Bundle {
    uint32_t version = 0;
    std::string unityVersion, unityRevision;
    uint32_t flags = 0;
    std::vector<BundleNode> nodes;

    static bool is_bundle(const Bytes& file);
    static Bundle load(const Bytes& file);  // throws ParseError (LZMA is reported as unsupported)
    Bytes save() const;  // uncompressed blocks; Unity loads these fine (CRC must be disabled in catalog)
};
