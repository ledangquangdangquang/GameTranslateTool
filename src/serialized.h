// Unity SerializedFile (.assets, CAB-* inside bundles): parse header/metadata, read object
// bytes, and rebuild the file with replaced objects. The metadata block is kept byte-for-byte;
// only each object's offset/size fields and the header file size are patched.
#pragma once
#include <map>
#include <memory>

#include "bytes.h"

enum ClassID : int32_t {
    kTextAsset = 49,
    kMonoBehaviour = 114,
    kMonoScript = 115,
    kFont = 128,
    kAssetBundle = 142,
};

struct TTNode {
    std::string type, name;
    int32_t byteSize = 0;
    uint32_t metaFlag = 0;
    uint8_t typeFlags = 0;
    std::vector<TTNode> children;
    bool aligned() const { return (metaFlag & 0x4000) != 0; }
    const TTNode* child(const std::string& n) const {
        for (auto& c : children) if (c.name == n) return &c;
        return nullptr;
    }
};

struct SerializedType {
    int32_t classID = 0;
    int16_t scriptTypeIndex = -1;
    bool hasTree = false;
    TTNode root;
};

struct ObjectInfo {
    int64_t pathId = 0;
    uint64_t byteStart = 0;
    uint32_t byteSize = 0;
    int32_t typeIndex = 0;
    int32_t classID = 0;
    size_t entryPos = 0;  // file offset of the byteStart field (for patching)
};

struct External {
    std::string path;
};

class SerializedFile {
public:
    static bool looks_like(const Bytes& b);
    static SerializedFile load(Bytes file);  // throws ParseError

    uint32_t version = 0;
    bool bigEndian = false;
    std::string unityVersion;
    bool enableTypeTree = false;
    std::vector<SerializedType> types;
    std::vector<ObjectInfo> objects;
    std::vector<External> externals;

    const SerializedType* type_of(const ObjectInfo& o) const {
        return o.typeIndex >= 0 && (size_t)o.typeIndex < types.size() ? &types[o.typeIndex] : nullptr;
    }
    const TTNode* tree_of(const ObjectInfo& o) const {
        auto t = type_of(o);
        return t && t->hasTree ? &t->root : nullptr;
    }
    Bytes object_data(const ObjectInfo& o) const;
    void set_object_data(int64_t pathId, Bytes data) { replaced_[pathId] = std::move(data); }
    bool modified() const { return !replaced_.empty(); }
    Bytes save() const;

    // Name of an object (m_Name) when it is the first field (TextAsset, MonoBehaviour, Font...).
    std::string peek_name(const ObjectInfo& o) const;

private:
    Bytes file_;
    uint64_t dataOffset_ = 0;
    size_t fileSizePos_ = 0;
    bool fileSize64_ = false;
    std::map<int64_t, Bytes> replaced_;
};
