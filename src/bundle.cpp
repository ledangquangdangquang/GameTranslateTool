#include "bundle.h"

#include "lz4.h"

namespace {
enum : uint32_t {
    kCompressionMask = 0x3F,
    kBlocksInfoAtEnd = 0x80,
    kBlockInfoNeedPaddingAtStart = 0x200,
};

Bytes decompress(const uint8_t* src, size_t srcLen, size_t dstLen, uint32_t method) {
    Bytes out(dstLen);
    switch (method) {
        case 0:
            if (srcLen != dstLen) throw ParseError("block khong nen nhung sai kich thuoc");
            std::memcpy(out.data(), src, srcLen);
            return out;
        case 2:
        case 3:
            if (!lz4_decompress(src, srcLen, out.data(), dstLen)) throw ParseError("giai nen LZ4 that bai");
            return out;
        case 1:
            throw ParseError("bundle nen LZMA - chua ho tro");
        default:
            throw ParseError("kieu nen bundle khong biet: " + std::to_string(method));
    }
}
}  // namespace

bool Bundle::is_bundle(const Bytes& f) { return f.size() > 8 && std::memcmp(f.data(), "UnityFS\0", 8) == 0; }

Bundle Bundle::load(const Bytes& file) {
    if (!is_bundle(file)) throw ParseError("khong phai UnityFS bundle");
    Reader r(file, true);
    r.cstr();  // signature
    Bundle b;
    b.version = r.u32();
    b.unityVersion = r.cstr();
    b.unityRevision = r.cstr();
    r.i64();  // total size
    uint32_t ciSize = r.u32(), uiSize = r.u32();
    b.flags = r.u32();
    if (b.version >= 7) r.align(16);

    size_t infoPos = (b.flags & kBlocksInfoAtEnd) ? file.size() - ciSize : r.pos;
    if (infoPos + ciSize > file.size()) throw ParseError("blocks info ngoai file");
    Bytes info = decompress(file.data() + infoPos, ciSize, uiSize, b.flags & kCompressionMask);
    if (!(b.flags & kBlocksInfoAtEnd)) {
        r.pos += ciSize;
        if (b.flags & kBlockInfoNeedPaddingAtStart) r.align(16);
    }
    size_t dataPos = r.pos;

    Reader ir(info, true);
    ir.skip(16);  // uncompressed data hash
    int32_t blockCount = ir.i32();
    struct Block { uint32_t u, c; uint16_t f; };
    std::vector<Block> blocks(blockCount);
    uint64_t total = 0;
    for (auto& bl : blocks) {
        bl.u = ir.u32();
        bl.c = ir.u32();
        bl.f = ir.u16();
        total += bl.u;
    }
    Bytes data;
    data.reserve((size_t)total);
    size_t at = dataPos;
    for (auto& bl : blocks) {
        if (at + bl.c > file.size()) throw ParseError("block du lieu ngoai file");
        Bytes chunk = decompress(file.data() + at, bl.c, bl.u, bl.f & kCompressionMask);
        data.insert(data.end(), chunk.begin(), chunk.end());
        at += bl.c;
    }

    int32_t nodeCount = ir.i32();
    for (int i = 0; i < nodeCount; i++) {
        int64_t off = ir.i64(), size = ir.i64();
        BundleNode n;
        n.flags = ir.u32();
        n.path = ir.cstr();
        if (off < 0 || size < 0 || (uint64_t)(off + size) > data.size()) throw ParseError("node ngoai du lieu");
        n.data.assign(data.begin() + off, data.begin() + off + size);
        b.nodes.push_back(std::move(n));
    }
    return b;
}

Bytes Bundle::save() const {
    const uint32_t kChunk = 0x20000;
    Bytes data;
    std::vector<int64_t> offsets;
    for (auto& n : nodes) {
        offsets.push_back((int64_t)data.size());
        data.insert(data.end(), n.data.begin(), n.data.end());
    }

    Writer info(true);
    for (int i = 0; i < 16; i++) info.u8(0);
    uint32_t blockCount = (uint32_t)((data.size() + kChunk - 1) / kChunk);
    info.i32((int32_t)blockCount);
    for (uint32_t i = 0; i < blockCount; i++) {
        uint32_t sz = (uint32_t)std::min<size_t>(kChunk, data.size() - (size_t)i * kChunk);
        info.u32(sz);
        info.u32(sz);
        info.u16(0);  // uncompressed
    }
    info.i32((int32_t)nodes.size());
    for (size_t i = 0; i < nodes.size(); i++) {
        info.i64(offsets[i]);
        info.i64((int64_t)nodes[i].data.size());
        info.u32(nodes[i].flags);
        info.cstr(nodes[i].path);
    }

    uint32_t outFlags = flags & ~(kCompressionMask | kBlocksInfoAtEnd);
    Writer w(true);
    w.cstr("UnityFS");
    w.u32(version);
    w.cstr(unityVersion);
    w.cstr(unityRevision);
    size_t sizeAt = w.pos();
    w.i64(0);
    w.u32((uint32_t)info.buf.size());
    w.u32((uint32_t)info.buf.size());
    w.u32(outFlags);
    if (version >= 7) w.align(16);
    w.raw(info.buf);
    if (outFlags & kBlockInfoNeedPaddingAtStart) w.align(16);
    w.raw(data);
    w.patch_u64(sizeAt, w.buf.size());
    return w.buf;
}
