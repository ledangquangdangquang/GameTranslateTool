// Godot 4 games: everything ships in one <name>.pck resource pack ("GDPC": header, file data,
// directory of path/offset/size/md5/flags). Slay the Spire 2 keeps its text as flat
// {"KEY": "text"} JSON files under localization/<lang>/ inside the pack.
#pragma once
#include <map>

#include "bytes.h"

struct PckFile {
    std::string path;      // "localization/eng/cards.json" (no res://, no NUL padding)
    uint64_t offset = 0;   // absolute position of the data in the .pck
    uint64_t size = 0;
    uint8_t md5[16] = {};
    uint32_t flags = 0;    // 1 encrypted, 2 removed, 4 delta
    uint64_t entryAt = 0;  // absolute position of this entry's offset field in the directory
};

struct Pck {
    uint32_t version = 0, major = 0, minor = 0, patch = 0, flags = 0;
    uint64_t fileBase = 0;  // absolute; stored offsets are relative to it
    uint64_t fileSize = 0;
    std::vector<PckFile> files;

    static bool is_pck(const fs::path& p);  // standalone .pck starting with "GDPC"
    static Pck load(const fs::path& p);      // header + directory only (packs are gigabytes)
    const PckFile* find(const std::string& path) const;
    Bytes read(const fs::path& p, const PckFile& f) const;
};

// <dir>/<name>.pck next to <name>.exe (or any standalone .pck in dir)
bool godot_find_pck(const fs::path& dir, fs::path& pck);

// Writes `out` as a copy of `in` (the pack described by `pck`) with the files in `replace` (path ->
// new content) changed. Their new data is appended at the end and only their directory entries
// (offset, size, md5) are rewritten in place, so every other byte stays identical.
void pck_write(const fs::path& in, const Pck& pck, const std::map<std::string, Bytes>& replace,
               const fs::path& out);

void md5_digest(const uint8_t* p, size_t n, uint8_t out[16]);

// .fontdata = imported FontFile resource (binary "RSRC", or "RSCC" = the same zstd/deflate-compressed
// in blocks). The font file itself is the "data" byte array, first property of the main resource.
bool fontdata_ttf(const Bytes& res, Bytes& ttf);
// The same resource with `ttf` as its font file, written uncompressed (Godot reads both forms). The
// pre-rendered glyph cache stays: glyph indices of the old font must still mean the same glyphs.
Bytes fontdata_with_ttf(const Bytes& res, const Bytes& ttf);
