// Binary reader/writer with endianness + alignment, and UTF-8 aware file helpers.
#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size, bool bigEndian = false) : be(bigEndian), p_(data), n_(size) {}
    explicit Reader(const Bytes& b, bool bigEndian = false) : Reader(b.data(), b.size(), bigEndian) {}

    bool be;
    size_t pos = 0;

    size_t size() const { return n_; }
    size_t left() const { return pos <= n_ ? n_ - pos : 0; }
    const uint8_t* data() const { return p_; }

    void need(size_t k) const {
        if (k > left()) throw ParseError("doc vuot qua cuoi du lieu");
    }
    void skip(size_t k) { need(k); pos += k; }
    void align(size_t a) { pos = (pos + a - 1) / a * a; if (pos > n_) throw ParseError("align vuot qua cuoi du lieu"); }

    uint8_t u8() { need(1); return p_[pos++]; }
    uint16_t u16() { return (uint16_t)uint_n(2); }
    uint32_t u32() { return (uint32_t)uint_n(4); }
    uint64_t u64() { return uint_n(8); }
    int16_t i16() { return (int16_t)u16(); }
    int32_t i32() { return (int32_t)u32(); }
    int64_t i64() { return (int64_t)u64(); }
    float f32() { uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    double f64() { uint64_t v = u64(); double d; std::memcpy(&d, &v, 8); return d; }

    std::string bytes(size_t k) {
        need(k);
        std::string s((const char*)p_ + pos, k);
        pos += k;
        return s;
    }
    std::string cstr() {
        size_t e = pos;
        while (e < n_ && p_[e]) e++;
        if (e >= n_) throw ParseError("chuoi khong co ky tu ket thuc");
        std::string s((const char*)p_ + pos, e - pos);
        pos = e + 1;
        return s;
    }
    // Unity serialized string: int32 length + bytes + align 4
    std::string ustr() {
        int32_t k = i32();
        if (k < 0) throw ParseError("do dai chuoi am");
        std::string s = bytes((size_t)k);
        align(4);
        return s;
    }

private:
    uint64_t uint_n(int k) {
        need(k);
        uint64_t v = 0;
        if (be) for (int i = 0; i < k; i++) v = (v << 8) | p_[pos + i];
        else for (int i = k - 1; i >= 0; i--) v = (v << 8) | p_[pos + i];
        pos += k;
        return v;
    }
    const uint8_t* p_;
    size_t n_;
};

class Writer {
public:
    explicit Writer(bool bigEndian = false) : be(bigEndian) {}
    bool be;
    Bytes buf;

    size_t pos() const { return buf.size(); }
    void align(size_t a) { while (buf.size() % a) buf.push_back(0); }
    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) { uint_n(v, 2); }
    void u32(uint32_t v) { uint_n(v, 4); }
    void u64(uint64_t v) { uint_n(v, 8); }
    void i16(int16_t v) { u16((uint16_t)v); }
    void i32(int32_t v) { u32((uint32_t)v); }
    void i64(int64_t v) { u64((uint64_t)v); }
    void f32(float f) { uint32_t v; std::memcpy(&v, &f, 4); u32(v); }
    void f64(double d) { uint64_t v; std::memcpy(&v, &d, 8); u64(v); }
    void raw(const void* p, size_t k) { buf.insert(buf.end(), (const uint8_t*)p, (const uint8_t*)p + k); }
    void raw(const std::string& s) { raw(s.data(), s.size()); }
    void raw(const Bytes& b) { raw(b.data(), b.size()); }
    void cstr(const std::string& s) { raw(s); u8(0); }
    void ustr(const std::string& s) { i32((int32_t)s.size()); raw(s); align(4); }

    // overwrite an already written integer (for sizes patched after the fact)
    void patch_u32(size_t at, uint32_t v) { patch(at, v, 4); }
    void patch_u64(size_t at, uint64_t v) { patch(at, v, 8); }

private:
    void uint_n(uint64_t v, int k) {
        size_t at = buf.size();
        buf.resize(at + k);
        patch(at, v, k);
    }
    void patch(size_t at, uint64_t v, int k) {
        for (int i = 0; i < k; i++) {
            int shift = be ? (k - 1 - i) * 8 : i * 8;
            buf[at + i] = (uint8_t)(v >> shift);
        }
    }
};

// ---- files (UTF-8 paths, works with Vietnamese folder names on Windows) ----
inline fs::path u8path(const std::string& s) { return fs::u8path(s); }
inline std::string pathstr(const fs::path& p) { return p.u8string(); }

Bytes read_file(const fs::path& p);
void write_file(const fs::path& p, const Bytes& data);           // atomic: write .tmp then rename
inline void write_file(const fs::path& p, const std::string& s) { write_file(p, Bytes(s.begin(), s.end())); }
std::string read_text(const fs::path& p);
