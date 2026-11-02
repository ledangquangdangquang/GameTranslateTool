// Generic object reader/writer driven by a typetree. Writing back is the exact mirror of
// reading, and callers verify read->write reproduces the original bytes before editing.
#pragma once
#include <utility>

#include "serialized.h"

struct Value {
    enum Kind { Int, UInt, Float, Bool, Str, Blob, Array, Object } kind = Object;
    int64_t i = 0;
    uint64_t u = 0;
    double f = 0;
    std::string s;  // Str (UTF-8 bytes) and Blob (raw byte arrays)
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    Value* get(const std::string& name) {
        for (auto& kv : obj) if (kv.first == name) return &kv.second;
        return nullptr;
    }
    const Value* get(const std::string& name) const {
        for (auto& kv : obj) if (kv.first == name) return &kv.second;
        return nullptr;
    }
    // path like "mSource/mTerms"
    const Value* path(const std::string& p) const;
    Value* path(const std::string& p) { return const_cast<Value*>(static_cast<const Value*>(this)->path(p)); }
};

Value tt_read(const TTNode& root, const Bytes& data, bool bigEndian);  // throws ParseError
Bytes tt_write(const TTNode& root, const Value& v, bool bigEndian);

// Read + verify that writing back gives identical bytes (refuses objects we can't round-trip).
bool tt_read_checked(const TTNode& root, const Bytes& data, bool bigEndian, Value& out);
