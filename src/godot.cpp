#include "godot.h"

#include <windows.h>
#include <bcrypt.h>
#include <zlib.h>

#include <cstdio>

#include "zstd.h"

namespace {

const uint32_t kMagic = 0x43504447;  // "GDPC"
const uint32_t kDirEncrypted = 1, kSparseBundle = 4;
const uint32_t kFileEncrypted = 1;

struct File {
    FILE* f = nullptr;
    fs::path p;
    File(const fs::path& path, const wchar_t* mode) : p(path) {
        f = _wfopen(path.wstring().c_str(), mode);
        if (!f) throw std::runtime_error("khong mo duoc file: " + pathstr(path));
    }
    ~File() { if (f) fclose(f); }
    void seek(uint64_t at) { _fseeki64(f, (long long)at, SEEK_SET); }
    uint64_t tell() { return (uint64_t)_ftelli64(f); }
    uint64_t size() {
        uint64_t at = tell();
        _fseeki64(f, 0, SEEK_END);
        uint64_t n = tell();
        seek(at);
        return n;
    }
    void get(void* dst, size_t n) {
        if (fread(dst, 1, n, f) != n) throw ParseError("doc vuot qua cuoi file .pck");
    }
    void put(const void* src, size_t n) {
        if (n && fwrite(src, 1, n, f) != n) throw std::runtime_error("ghi file loi (het dung luong?): " + pathstr(p));
    }
    uint32_t u32() { uint8_t b[4]; get(b, 4); return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
    uint64_t u64() { uint64_t lo = u32(); return lo | (uint64_t)u32() << 32; }
    void put_u64(uint64_t v) {
        uint8_t b[8];
        for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
        put(b, 8);
    }
};

}  // namespace

void md5_digest(const uint8_t* p, size_t n, uint8_t out[16]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::memset(out, 0, 16);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0) return;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0);
        BCryptFinishHash(h, out, 16, 0);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
}

bool Pck::is_pck(const fs::path& p) {
    FILE* f = _wfopen(p.wstring().c_str(), L"rb");
    if (!f) return false;
    uint8_t b[4] = {};
    bool ok = fread(b, 1, 4, f) == 4 && (b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24) == kMagic;
    fclose(f);
    return ok;
}

Pck Pck::load(const fs::path& p) {
    File f(p, L"rb");
    Pck k;
    k.fileSize = f.size();
    if (f.u32() != kMagic) throw ParseError("khong phai file .pck cua Godot (thieu GDPC)");
    k.version = f.u32();
    k.major = f.u32();
    k.minor = f.u32();
    k.patch = f.u32();
    if (k.version < 2 || k.version > 4)
        throw ParseError("phien ban .pck chua ho tro: " + std::to_string(k.version));
    k.flags = f.u32();
    if (k.flags & (kDirEncrypted | kSparseBundle)) throw ParseError(".pck ma hoa / sparse bundle: chua ho tro");
    k.fileBase = f.u64();
    if (k.version >= 3) f.seek(f.u64());  // directory offset (data comes first, directory at the end)
    else for (int i = 0; i < 16; i++) f.u32();  // V2: reserved, directory right after the header
    uint32_t count = f.u32();
    k.files.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        PckFile e;
        uint32_t sl = f.u32();
        if (sl > 4096) throw ParseError("duong dan trong .pck qua dai - file hong?");
        std::string path(sl, '\0');
        f.get(path.data(), sl);
        path.resize(strnlen(path.data(), sl));  // paths are NUL-padded to a multiple of 4
        if (path.rfind("res://", 0) == 0) path.erase(0, 6);
        e.path = path;
        e.entryAt = f.tell();
        e.offset = k.fileBase + f.u64();
        e.size = f.u64();
        f.get(e.md5, 16);
        e.flags = f.u32();
        if (e.offset + e.size > k.fileSize && !(e.flags & 2)) throw ParseError("file trong .pck vuot qua cuoi: " + path);
        k.files.push_back(std::move(e));
    }
    return k;
}

const PckFile* Pck::find(const std::string& path) const {
    for (auto& f : files)
        if (f.path == path) return &f;
    return nullptr;
}

Bytes Pck::read(const fs::path& p, const PckFile& e) const {
    if (e.flags & kFileEncrypted) throw ParseError("file ma hoa trong .pck: " + e.path);
    File f(p, L"rb");
    f.seek(e.offset);
    Bytes b((size_t)e.size);
    f.get(b.data(), b.size());
    return b;
}

bool godot_find_pck(const fs::path& dir, fs::path& pck) {
    std::error_code ec;
    fs::path any;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        if (!it->is_regular_file(ec) || p.extension() != ".pck" || !Pck::is_pck(p)) continue;
        fs::path exe = p;
        exe.replace_extension(".exe");
        if (fs::exists(exe, ec)) {
            pck = p;
            return true;
        }
        if (any.empty()) any = p;
    }
    if (any.empty()) return false;
    pck = any;
    return true;
}

// ------------------------------------------------------------------ .fontdata (FontFile resource)
namespace {

uint32_t le32(const Bytes& b, size_t at) {
    if (at + 4 > b.size()) throw ParseError("resource Godot hong");
    return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | (uint32_t)b[at + 3] << 24;
}
uint64_t le64(const Bytes& b, size_t at) { return le32(b, at) | (uint64_t)le32(b, at + 4) << 32; }
void set_le32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(v >> (8 * i));
}
void set_le64(Bytes& b, size_t at, uint64_t v) {
    set_le32(b, at, (uint32_t)v);
    set_le32(b, at + 4, (uint32_t)(v >> 32));
}

// "RSCC" + mode + block size + total + (total / block + 1) compressed sizes + blocks
Bytes rscc_decompress(const Bytes& res) {
    uint32_t mode = le32(res, 4), bs = le32(res, 8), total = le32(res, 12);
    if (bs == 0) throw ParseError("RSCC hong");
    uint32_t bc = total / bs + 1;
    size_t at = 16 + 4ull * bc;
    Bytes out(total);
    for (uint32_t i = 0; i < bc; i++) {
        uint32_t cs = le32(res, 16 + 4 * i);
        size_t want = i == bc - 1 ? total % bs : bs;
        if (at + cs > res.size()) throw ParseError("RSCC hong");
        if (want) {
            size_t got = 0;
            if (mode == 2) {  // zstd
                got = ZSTD_decompress(out.data() + (size_t)i * bs, want, res.data() + at, cs);
                if (ZSTD_isError(got)) throw ParseError("RSCC: loi giai nen zstd");
            } else if (mode == 1 || mode == 3) {  // deflate (zlib) / gzip
                z_stream z{};
                inflateInit2(&z, 15 + 32);
                z.next_in = (Bytef*)(res.data() + at);
                z.avail_in = cs;
                z.next_out = out.data() + (size_t)i * bs;
                z.avail_out = (uInt)want;
                inflate(&z, Z_FINISH);
                got = want - z.avail_out;
                inflateEnd(&z);
            } else {
                throw ParseError("RSCC: kieu nen " + std::to_string(mode) + " chua ho tro");
            }
            if (got != want) throw ParseError("RSCC: giai nen thieu du lieu");
        }
        at += cs;
    }
    return out;
}

struct FontRes {
    Bytes plain;                      // "RSRC" + body; every stored offset is a position in it
    size_t lenAt = 0, dataAt = 0;     // "data" byte array: its u32 length, its first byte
    uint32_t len = 0;
    std::vector<size_t> offsetFields;  // u64 offsets that point into the body (internal resources, import metadata)
};

FontRes parse_fontres(const Bytes& res) {
    if (res.size() < 8) throw ParseError("resource Godot hong");
    FontRes r;
    bool compressed = std::memcmp(res.data(), "RSCC", 4) == 0;
    if (!compressed && std::memcmp(res.data(), "RSRC", 4) != 0) throw ParseError("khong phai resource Godot");
    if (compressed) {
        // the compressed stream has no magic: its offsets are 4 bytes short of the plain file's
        Bytes body = rscc_decompress(res);
        r.plain = Bytes{'R', 'S', 'R', 'C'};
        r.plain.insert(r.plain.end(), body.begin(), body.end());
    } else {
        r.plain = res;
    }
    const Bytes& b = r.plain;
    size_t p = 4;
    if (le32(b, p) != 0) throw ParseError("resource big-endian chua ho tro");
    p += 20;
    auto str = [&] { uint32_t n = le32(b, p); p += 4 + n; };
    str();  // type
    size_t importAt = p;
    p += 8;
    uint32_t flags = le32(b, p);
    p += 4 + 8;  // flags, uid
    if (flags & 8) str();  // script class
    p += 11 * 4;
    uint32_t nstr = le32(b, p);
    p += 4;
    std::vector<std::string> strings;
    for (uint32_t i = 0; i < nstr; i++) {
        uint32_t n = le32(b, p);
        if (p + 4 + n > b.size()) throw ParseError("resource Godot hong");
        strings.emplace_back((const char*)b.data() + p + 4, strnlen((const char*)b.data() + p + 4, n));
        p += 4 + n;
    }
    uint32_t next = le32(b, p);
    p += 4;
    for (uint32_t i = 0; i < next; i++) {
        str();
        str();
        if (flags & 2) p += 8;
    }
    uint32_t nint = le32(b, p);
    p += 4;
    uint64_t main = 0;
    for (uint32_t i = 0; i < nint; i++) {
        str();
        r.offsetFields.push_back(p);
        main = le64(b, p);
        p += 8;
    }
    if (le64(b, importAt)) r.offsetFields.push_back(importAt);
    if (compressed) {
        for (size_t f : r.offsetFields) set_le64(r.plain, f, le64(r.plain, f) + 4);
        main += 4;
    }
    // main resource: type, property count, then "data" = PackedByteArray (31)
    p = (size_t)main;
    str();
    p += 4;
    uint32_t name = le32(b, p);
    std::string pname = name & 0x80000000 ? std::string((const char*)b.data() + p + 4, name & 0x7FFFFFFF)
                        : name < strings.size() ? strings[name] : "";
    if (pname != "data" || le32(b, p + 4) != 31) throw ParseError("FontFile khong co du lieu font (data)");
    r.lenAt = p + 8;
    r.len = le32(b, r.lenAt);
    r.dataAt = r.lenAt + 4;
    if (r.dataAt + r.len > b.size()) throw ParseError("resource Godot hong");
    return r;
}

}  // namespace

bool fontdata_ttf(const Bytes& res, Bytes& ttf) {
    FontRes r = parse_fontres(res);
    ttf.assign(r.plain.begin() + r.dataAt, r.plain.begin() + r.dataAt + r.len);
    return true;
}

Bytes fontdata_with_ttf(const Bytes& res, const Bytes& ttf) {
    FontRes r = parse_fontres(res);
    size_t oldEnd = r.dataAt + (r.len + 3) / 4 * 4;
    Bytes out(r.plain.begin(), r.plain.begin() + r.lenAt);
    out.resize(out.size() + 4);
    set_le32(out, r.lenAt, (uint32_t)ttf.size());
    out.insert(out.end(), ttf.begin(), ttf.end());
    while ((out.size() - r.dataAt) % 4) out.push_back(0);
    int64_t delta = (int64_t)out.size() - (int64_t)oldEnd;
    out.insert(out.end(), r.plain.begin() + oldEnd, r.plain.end());
    for (size_t f : r.offsetFields) {
        uint64_t v = le64(out, f);
        if (v > r.dataAt) set_le64(out, f, v + delta);
    }
    return out;
}

void pck_write(const fs::path& in, const Pck& pck, const std::map<std::string, Bytes>& replace,
               const fs::path& out) {
    std::vector<std::pair<const PckFile*, const Bytes*>> todo;
    for (auto& [path, data] : replace) {
        const PckFile* e = pck.find(path);
        if (!e) throw ParseError("khong thay trong .pck: " + path);
        if (e->flags & kFileEncrypted) throw ParseError("file ma hoa trong .pck: " + path);
        todo.push_back({e, &data});
    }
    fs::path tmp = out;
    tmp += ".tmp";
    fs::copy_file(in, tmp, fs::copy_options::overwrite_existing);
    try {
        File f(tmp, L"r+b");
        uint64_t end = f.size();
        if (end != pck.fileSize) throw ParseError("file .pck goc da thay doi kich thuoc");
        for (auto& [e, data] : todo) {
            uint64_t at = (end + 15) / 16 * 16;  // Godot keeps file data 16-byte aligned
            static const uint8_t zero[16] = {};
            f.seek(end);
            f.put(zero, (size_t)(at - end));
            f.put(data->data(), data->size());
            end = at + data->size();
            uint8_t md5[16];
            md5_digest(data->data(), data->size(), md5);
            f.seek(e->entryAt);
            f.put_u64(at - pck.fileBase);
            f.put_u64(data->size());
            f.put(md5, 16);
        }
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (ec) {
        fs::remove(out, ec);
        fs::rename(tmp, out);
    }
}
