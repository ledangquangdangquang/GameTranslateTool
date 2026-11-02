#include "serialized.h"

#include <algorithm>

#include "common_strings.h"

namespace {

std::string tt_string(uint32_t off, const std::string& local) {
    if (off & 0x80000000u) {
        off &= 0x7FFFFFFFu;
        if (off >= kCommonStringsSize) return "?";
        return std::string((const char*)kCommonStrings + off);
    }
    if (off >= local.size()) return "?";
    return std::string(local.c_str() + off);
}

TTNode read_tree_blob(Reader& r, uint32_t version) {
    int32_t count = r.i32();
    int32_t strSize = r.i32();
    if (count <= 0 || count > 1000000 || strSize < 0) throw ParseError("typetree hong");
    struct Flat { uint8_t level, flags; uint32_t typeOff, nameOff; int32_t size; uint32_t meta; };
    std::vector<Flat> flat(count);
    for (auto& f : flat) {
        r.u16();  // version
        f.level = r.u8();
        f.flags = r.u8();
        f.typeOff = r.u32();
        f.nameOff = r.u32();
        f.size = r.i32();
        r.i32();  // index
        f.meta = r.u32();
        if (version >= 19) r.u64();  // ref type hash
    }
    std::string strings = r.bytes((size_t)strSize);

    // rebuild hierarchy from levels
    TTNode root;
    std::vector<TTNode*> stack;
    for (int i = 0; i < count; i++) {
        TTNode n;
        n.type = tt_string(flat[i].typeOff, strings);
        n.name = tt_string(flat[i].nameOff, strings);
        n.byteSize = flat[i].size;
        n.metaFlag = flat[i].meta;
        n.typeFlags = flat[i].flags;
        if (i == 0) {
            root = std::move(n);
            stack.push_back(&root);
            continue;
        }
        size_t lvl = flat[i].level;
        if (lvl == 0 || lvl > stack.size()) throw ParseError("typetree sai cap");
        stack.resize(lvl);
        TTNode* parent = stack.back();
        parent->children.push_back(std::move(n));
        stack.push_back(&parent->children.back());
    }
    return root;
}

SerializedType read_type(Reader& r, uint32_t version, bool enableTree, bool isRef) {
    SerializedType t;
    t.classID = r.i32();
    if (version >= 16) r.u8();  // stripped
    if (version >= 17) t.scriptTypeIndex = r.i16();
    if (version >= 13) {
        if ((isRef && t.scriptTypeIndex >= 0) || (version < 16 && t.classID < 0) ||
            (version >= 16 && t.classID == kMonoBehaviour))
            r.skip(16);  // script id
        r.skip(16);      // old type hash
    }
    if (enableTree) {
        if (version >= 12 || version == 10) t.root = read_tree_blob(r, version);
        else throw ParseError("typetree dang cu (version " + std::to_string(version) + ") chua ho tro");
        t.hasTree = true;
        if (version >= 21) {
            if (isRef) {
                r.cstr();
                r.cstr();
                r.cstr();
            } else {
                int32_t n = r.i32();
                r.skip((size_t)std::max(0, n) * 4);
            }
        }
    }
    return t;
}

}  // namespace

bool SerializedFile::looks_like(const Bytes& b) {
    if (b.size() < 48) return false;
    Reader r(b, true);
    r.pos = 8;
    uint32_t version = r.u32();
    if (version < 9 || version > 60) return false;
    if (version >= 22) {
        r.pos = 24;
        return r.u64() == b.size();
    }
    r.pos = 4;
    return r.u32() == b.size();
}

SerializedFile SerializedFile::load(Bytes file) {
    SerializedFile sf;
    Reader r(file, true);
    r.u32();  // metadata size (old)
    uint32_t fileSize32 = r.u32();
    sf.version = r.u32();
    uint32_t dataOffset32 = r.u32();
    if (sf.version < 14) throw ParseError("SerializedFile qua cu (version " + std::to_string(sf.version) + ")");
    uint8_t endian = r.u8();
    r.skip(3);
    sf.fileSizePos_ = 4;
    sf.dataOffset_ = dataOffset32;
    (void)fileSize32;
    if (sf.version >= 22) {
        r.u32();  // metadata size
        sf.fileSizePos_ = r.pos;
        sf.fileSize64_ = true;
        r.u64();
        sf.dataOffset_ = r.u64();
        r.u64();
    }
    sf.bigEndian = endian != 0;
    r.be = sf.bigEndian;

    sf.unityVersion = r.cstr();
    r.i32();  // target platform
    sf.enableTypeTree = r.u8() != 0;

    int32_t typeCount = r.i32();
    if (typeCount < 0 || typeCount > 100000) throw ParseError("so type bat thuong");
    for (int i = 0; i < typeCount; i++) sf.types.push_back(read_type(r, sf.version, sf.enableTypeTree, false));

    int32_t objCount = r.i32();
    if (objCount < 0 || objCount > 10000000) throw ParseError("so object bat thuong");
    sf.objects.resize(objCount);
    for (auto& o : sf.objects) {
        r.align(4);
        o.pathId = r.i64();
        o.entryPos = r.pos;
        o.byteStart = sf.version >= 22 ? r.u64() : r.u32();
        o.byteSize = r.u32();
        o.typeIndex = r.i32();
        if (sf.version < 16) {
            o.classID = r.u16();
            r.i16();  // script type index
            if (sf.version == 15) r.u8();
        } else {
            o.classID = (o.typeIndex >= 0 && o.typeIndex < typeCount) ? sf.types[o.typeIndex].classID : -1;
        }
        if (sf.dataOffset_ + o.byteStart + o.byteSize > file.size()) throw ParseError("object nam ngoai file");
    }

    int32_t scriptCount = r.i32();
    for (int i = 0; i < scriptCount; i++) {
        r.i32();
        r.align(4);
        r.i64();
    }
    int32_t extCount = r.i32();
    for (int i = 0; i < extCount; i++) {
        r.cstr();
        r.skip(16);
        r.i32();
        sf.externals.push_back({r.cstr()});
    }
    // ref types + user information are not needed (metadata is copied verbatim on save)
    sf.file_ = std::move(file);
    return sf;
}

Bytes SerializedFile::object_data(const ObjectInfo& o) const {
    auto it = replaced_.find(o.pathId);
    if (it != replaced_.end()) return it->second;
    size_t st = (size_t)(dataOffset_ + o.byteStart);
    return Bytes(file_.begin() + st, file_.begin() + st + o.byteSize);
}

std::string SerializedFile::peek_name(const ObjectInfo& o) const {
    try {
        Bytes d = object_data(o);
        Reader r(d, bigEndian);
        if (o.classID == kMonoBehaviour) r.skip(12 + 4 + 12);  // m_GameObject, m_Enabled(+align), m_Script
        int32_t n = r.i32();
        if (n < 0 || n > 4096) return "";
        return r.bytes((size_t)n);
    } catch (const ParseError&) {
        return "";
    }
}

Bytes SerializedFile::save() const {
    std::vector<const ObjectInfo*> order;
    for (auto& o : objects) order.push_back(&o);
    std::sort(order.begin(), order.end(), [](auto a, auto b) { return a->byteStart < b->byteStart; });

    Writer w(bigEndian);
    w.raw(file_.data(), (size_t)dataOffset_);  // header + metadata, patched below
    struct Patch { size_t at; uint64_t start; uint32_t size; };
    std::vector<Patch> patches;
    // Unity 6 packs objects on 16-byte boundaries, older versions on 8: copy what the file uses
    size_t objAlign = 16;
    for (auto* o : order)
        if (o->byteStart % 16) { objAlign = 8; break; }
    uint64_t origEnd = dataOffset_;
    for (auto* o : order) {
        w.align(objAlign);
        Bytes d = object_data(*o);
        patches.push_back({o->entryPos, w.pos() - dataOffset_, (uint32_t)d.size()});
        w.raw(d);
        origEnd = std::max<uint64_t>(origEnd, dataOffset_ + o->byteStart + o->byteSize);
    }
    if (origEnd < file_.size()) w.raw(file_.data() + origEnd, file_.size() - (size_t)origEnd);  // trailing padding
    for (auto& p : patches) {
        if (version >= 22) {
            w.patch_u64(p.at, p.start);
            w.patch_u32(p.at + 8, p.size);
        } else {
            w.patch_u32(p.at, (uint32_t)p.start);
            w.patch_u32(p.at + 4, p.size);
        }
    }
    // header is always big endian
    Writer hdr(true);
    hdr.buf = std::move(w.buf);
    if (fileSize64_) hdr.patch_u64(fileSizePos_, hdr.buf.size());
    else hdr.patch_u32(fileSizePos_, (uint32_t)hdr.buf.size());
    return std::move(hdr.buf);
}
