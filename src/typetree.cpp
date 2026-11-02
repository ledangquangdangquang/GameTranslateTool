#include "typetree.h"

namespace {

enum Prim { kNotPrim, kBool, kI8, kU8, kI16, kU16, kI32, kU32, kI64, kU64, kF32, kF64 };

Prim prim_of(const std::string& t) {
    if (t == "bool") return kBool;
    if (t == "SInt8") return kI8;
    if (t == "UInt8" || t == "char") return kU8;
    if (t == "SInt16" || t == "short") return kI16;
    if (t == "UInt16" || t == "unsigned short") return kU16;
    if (t == "int" || t == "SInt32") return kI32;
    if (t == "UInt32" || t == "unsigned int" || t == "Type*") return kU32;
    if (t == "SInt64" || t == "long long") return kI64;
    if (t == "UInt64" || t == "unsigned long long" || t == "FileSize") return kU64;
    if (t == "float") return kF32;
    if (t == "double") return kF64;
    return kNotPrim;
}

bool is_array(const TTNode& n) { return !n.children.empty() && n.children[0].type == "Array"; }

void read_node(const TTNode& n, Reader& r, Value& v);

void read_prim(Prim p, Reader& r, Value& v) {
    switch (p) {
        case kBool: v.kind = Value::Bool; v.u = r.u8(); break;
        case kI8: v.kind = Value::Int; v.i = (int8_t)r.u8(); break;
        case kU8: v.kind = Value::UInt; v.u = r.u8(); break;
        case kI16: v.kind = Value::Int; v.i = r.i16(); break;
        case kU16: v.kind = Value::UInt; v.u = r.u16(); break;
        case kI32: v.kind = Value::Int; v.i = r.i32(); break;
        case kU32: v.kind = Value::UInt; v.u = r.u32(); break;
        case kI64: v.kind = Value::Int; v.i = r.i64(); break;
        case kU64: v.kind = Value::UInt; v.u = r.u64(); break;
        case kF32: v.kind = Value::Float; v.f = r.f32(); break;
        case kF64: v.kind = Value::Float; v.f = r.f64(); break;
        default: break;
    }
}

void read_node(const TTNode& n, Reader& r, Value& v) {
    if (n.type == "ManagedReferencesRegistry" || n.type == "ReferencedObject")
        throw ParseError("SerializeReference chua ho tro");
    Prim p = prim_of(n.type);
    bool align = n.aligned();
    if (p != kNotPrim && n.children.empty()) {
        read_prim(p, r, v);
    } else if (n.type == "string" && is_array(n)) {
        v.kind = Value::Str;
        int32_t len = r.i32();
        if (len < 0) throw ParseError("chuoi am");
        v.s = r.bytes((size_t)len);
        if (n.children[0].aligned()) align = true;
    } else if (n.type == "TypelessData") {
        v.kind = Value::Blob;
        int32_t len = r.i32();
        if (len < 0) throw ParseError("TypelessData am");
        v.s = r.bytes((size_t)len);
    } else if (is_array(n)) {
        const TTNode& arrNode = n.children[0];
        if (arrNode.children.size() < 2) throw ParseError("Array thieu node");
        const TTNode& elem = arrNode.children[1];
        int32_t count = r.i32();
        if (count < 0 || (size_t)count > r.left() + 1) throw ParseError("so phan tu mang bat thuong");
        Prim ep = prim_of(elem.type);
        if ((ep == kU8 || ep == kI8 || ep == kBool) && elem.children.empty() && !elem.aligned()) {
            v.kind = Value::Blob;
            v.s = r.bytes((size_t)count);
        } else {
            v.kind = Value::Array;
            v.arr.resize(count);
            for (auto& e : v.arr) read_node(elem, r, e);
        }
        if (arrNode.aligned()) align = true;
    } else {
        v.kind = Value::Object;
        v.obj.reserve(n.children.size());
        for (size_t k = 0; k < n.children.size(); k++) {
            const TTNode& c = n.children[k];
            v.obj.emplace_back(c.name, Value());
            if (c.type == "ManagedReferencesRegistry" && k + 1 == n.children.size()) {
                // SerializeReference data: always the last field; kept as opaque bytes
                v.obj.back().second.kind = Value::Blob;
                v.obj.back().second.s = r.bytes(r.left());
                continue;
            }
            read_node(c, r, v.obj.back().second);
        }
    }
    if (align) r.align(4);
}

void write_prim(Prim p, Writer& w, const Value& v) {
    switch (p) {
        case kBool: case kU8: w.u8((uint8_t)v.u); break;
        case kI8: w.u8((uint8_t)(int8_t)v.i); break;
        case kI16: w.i16((int16_t)v.i); break;
        case kU16: w.u16((uint16_t)v.u); break;
        case kI32: w.i32((int32_t)v.i); break;
        case kU32: w.u32((uint32_t)v.u); break;
        case kI64: w.i64(v.i); break;
        case kU64: w.u64(v.u); break;
        case kF32: w.f32((float)v.f); break;
        case kF64: w.f64(v.f); break;
        default: break;
    }
}

void write_node(const TTNode& n, Writer& w, const Value& v) {
    Prim p = prim_of(n.type);
    bool align = n.aligned();
    if (p != kNotPrim && n.children.empty()) {
        write_prim(p, w, v);
    } else if (n.type == "string" && is_array(n)) {
        w.i32((int32_t)v.s.size());
        w.raw(v.s);
        if (n.children[0].aligned()) align = true;
    } else if (n.type == "TypelessData") {
        w.i32((int32_t)v.s.size());
        w.raw(v.s);
    } else if (is_array(n)) {
        const TTNode& arrNode = n.children[0];
        const TTNode& elem = arrNode.children[1];
        if (v.kind == Value::Blob) {
            w.i32((int32_t)v.s.size());
            w.raw(v.s);
        } else {
            w.i32((int32_t)v.arr.size());
            for (auto& e : v.arr) write_node(elem, w, e);
        }
        if (arrNode.aligned()) align = true;
    } else {
        for (size_t i = 0; i < n.children.size(); i++) {
            if (n.children[i].type == "ManagedReferencesRegistry" && v.obj[i].second.kind == Value::Blob) {
                w.raw(v.obj[i].second.s);
                continue;
            }
            write_node(n.children[i], w, v.obj[i].second);
        }
    }
    if (align) w.align(4);
}

}  // namespace

const Value* Value::path(const std::string& p) const {
    const Value* cur = this;
    size_t st = 0;
    while (cur && st <= p.size()) {
        size_t e = p.find('/', st);
        if (e == std::string::npos) e = p.size();
        cur = cur->get(p.substr(st, e - st));
        st = e + 1;
    }
    return cur;
}

Value tt_read(const TTNode& root, const Bytes& data, bool bigEndian) {
    Reader r(data, bigEndian);
    Value v;
    read_node(root, r, v);
    return v;
}

Bytes tt_write(const TTNode& root, const Value& v, bool bigEndian) {
    Writer w(bigEndian);
    write_node(root, w, v);
    return w.buf;
}

bool tt_read_checked(const TTNode& root, const Bytes& data, bool bigEndian, Value& out) {
    try {
        Reader r(data, bigEndian);
        read_node(root, r, out);
        if (r.pos != data.size()) return false;
        return tt_write(root, out, bigEndian) == data;
    } catch (const ParseError&) {
        return false;
    }
}
