#include "catalog.h"

#include <algorithm>
#include <optional>

CatalogPatch patch_catalog(const fs::path& catalogBin, const std::string& bundleName,
                           const std::vector<uint64_t>& candidates, uint64_t newSize) {
    CatalogPatch res;
    if (newSize > 0xFFFFFFFFull) {
        res.error = "bundle lon hon 4GB";
        return res;
    }
    fs::path bak = catalogBin;
    bak += ".bak";
    if (!fs::exists(bak)) fs::copy_file(catalogBin, bak);

    Bytes d = read_file(catalogBin);
    auto it = std::search(d.begin(), d.end(), bundleName.begin(), bundleName.end());
    if (it == d.end()) {
        res.error = "khong thay ten bundle trong catalog.bin";
        return res;
    }
    size_t name = (size_t)(it - d.begin());
    size_t end = std::min(d.size(), name + bundleName.size() + 600);
    size_t at = 0;
    for (uint64_t oldSize : candidates) {
        if (oldSize > 0xFFFFFFFFull) continue;
        uint8_t needle[4];
        for (int i = 0; i < 4; i++) needle[i] = (uint8_t)(oldSize >> (8 * i));
        auto sz = std::search(d.begin() + name, d.begin() + end, needle, needle + 4);
        if (sz != d.begin() + end && sz - d.begin() >= 4) {
            at = (size_t)(sz - d.begin());
            break;
        }
    }
    if (!at) {
        res.error = "khong tim thay truong kich thuoc gan ten bundle (dinh dang catalog khac?) - khong sua gi";
        return res;
    }
    res.oldCrc = d[at - 4] | d[at - 3] << 8 | d[at - 2] << 16 | (uint32_t)d[at - 1] << 24;
    for (int i = 0; i < 4; i++) {
        d[at - 4 + i] = 0;
        d[at + i] = (uint8_t)(newSize >> (8 * i));
    }
    write_file(catalogBin, d);
    res.ok = true;
    return res;
}

namespace {

const char* kB64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

Bytes base64_decode(const std::string& in) {
    int table[256];
    std::fill(table, table + 256, -1);
    for (int i = 0; i < 64; i++) table[(unsigned char)kB64Chars[i]] = i;
    Bytes out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (table[c] < 0) continue;  // '=' padding / whitespace
        val = (val << 6) + table[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back((uint8_t)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string base64_encode(const Bytes& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    int val = 0, bits = -6;
    for (uint8_t c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(kB64Chars[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(kB64Chars[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// One UnityEngine.AddressableAssets.Utility.SerializationUtilities ObjectType.JsonObject record:
// 1-byte-len ASCII assembly name, 1-byte-len ASCII class name, 4-byte LE byte length, that many
// bytes of UTF-16LE JSON text (JsonUtility.ToJson output). Confirmed by IL-walking
// SerializationUtilities.ReadObjectFromByteArray in Unity.Addressables.dll.
struct JsonRecord {
    size_t jsonStart, jsonEnd;  // byte range of the UTF-16LE JSON text within the decoded buffer
};

std::optional<JsonRecord> read_json_record(const Bytes& d, size_t& p) {
    if (p >= d.size()) return std::nullopt;
    uint8_t type = d[p++];
    if (type != 7) throw ParseError("extra data: kieu doi tuong khong ho tro (" + std::to_string(type) + ")");
    if (p >= d.size()) throw ParseError("extra data: thieu du lieu (ten assembly)");
    uint8_t asmLen = d[p++];
    p += asmLen;
    if (p >= d.size()) throw ParseError("extra data: thieu du lieu (ten class)");
    uint8_t clsLen = d[p++];
    p += clsLen;
    if (p + 4 > d.size()) throw ParseError("extra data: thieu do dai json");
    uint32_t jsonLen = d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24);
    p += 4;
    size_t jsonStart = p;
    if (jsonStart + (size_t)jsonLen > d.size()) throw ParseError("extra data: do dai json vuot qua cuoi du lieu");
    p = jsonStart + jsonLen;
    return JsonRecord{jsonStart, p};
}

// Finds an ASCII needle encoded as UTF-16LE (one 0x00 byte after every char) within [from, to).
size_t find_utf16(const Bytes& d, size_t from, size_t to, const char* needleAscii) {
    size_t nlen = std::strlen(needleAscii);
    if (nlen == 0 || from + nlen * 2 > to) return std::string::npos;
    for (size_t i = from; i + nlen * 2 <= to; i++) {
        bool match = true;
        for (size_t k = 0; k < nlen; k++) {
            if (d[i + k * 2] != (uint8_t)needleAscii[k] || d[i + k * 2 + 1] != 0) {
                match = false;
                break;
            }
        }
        if (match) return i;
    }
    return std::string::npos;
}

// Reads the ASCII-digit-as-UTF-16LE run starting at `at` (right after a ':' with no intervening
// characters - callers pass the offset right after the matched key text). Returns the numeric
// value and the half-open byte range [at, end) the digits occupy.
struct Utf16Digits {
    uint64_t value;
    size_t start, end;
};

Utf16Digits read_utf16_digits(const Bytes& d, size_t at, size_t to) {
    Utf16Digits r{0, at, at};
    while (r.end + 1 < to && d[r.end + 1] == 0 && d[r.end] >= '0' && d[r.end] <= '9') {
        r.value = r.value * 10 + (uint64_t)(d[r.end] - '0');
        r.end += 2;
    }
    return r;
}

}  // namespace

CatalogPatch patch_catalog_json(const fs::path& catalogJson, const std::vector<uint64_t>& candidates,
                                 uint64_t /*newSize*/) {
    CatalogPatch res;
    fs::path bak = catalogJson;
    bak += ".bak";
    if (!fs::exists(bak)) fs::copy_file(catalogJson, bak);

    Bytes fileBytes = read_file(catalogJson);
    std::string text((const char*)fileBytes.data(), fileBytes.size());

    static const std::string kKey = "\"m_ExtraDataString\":\"";
    size_t keyPos = text.find(kKey);
    if (keyPos == std::string::npos) {
        res.error = "khong thay truong m_ExtraDataString trong catalog.json";
        return res;
    }
    size_t valStart = keyPos + kKey.size();
    size_t valEnd = text.find('"', valStart);
    if (valEnd == std::string::npos) {
        res.error = "catalog.json hong (thieu dau ngoac kep sau m_ExtraDataString)";
        return res;
    }
    std::string oldB64 = text.substr(valStart, valEnd - valStart);

    Bytes raw = base64_decode(oldB64);
    size_t matchCount = 0;
    size_t p = 0;
    try {
        while (auto rec = read_json_record(raw, p)) {
            size_t bsPos = find_utf16(raw, rec->jsonStart, rec->jsonEnd, "\"m_BundleSize\":");
            if (bsPos == std::string::npos) continue;
            Utf16Digits bs = read_utf16_digits(raw, bsPos + std::strlen("\"m_BundleSize\":") * 2, rec->jsonEnd);
            bool known = std::find(candidates.begin(), candidates.end(), bs.value) != candidates.end();
            if (!known) continue;
            matchCount++;
            if (matchCount > 1) continue;  // ambiguous - keep scanning just to count, patch nothing

            size_t crcPos = find_utf16(raw, rec->jsonStart, rec->jsonEnd, "\"m_Crc\":");
            if (crcPos == std::string::npos) {
                res.error = "khong thay m_Crc trong ban ghi extra-data";
                return res;
            }
            Utf16Digits crc = read_utf16_digits(raw, crcPos + std::strlen("\"m_Crc\":") * 2, rec->jsonEnd);
            size_t nChars = (crc.end - crc.start) / 2;
            if (nChars == 0) {
                res.error = "truong m_Crc rong trong extra-data";
                return res;
            }
            res.oldCrc = (uint32_t)crc.value;
            // overwrite in place: spaces (valid JSON whitespace) then '0', same char count ->
            // same byte length -> no other record's offset moves.
            for (size_t i = 0; i + 1 < nChars; i++) {
                raw[crc.start + i * 2] = ' ';
                raw[crc.start + i * 2 + 1] = 0;
            }
            raw[crc.start + (nChars - 1) * 2] = '0';
            raw[crc.start + (nChars - 1) * 2 + 1] = 0;
        }
    } catch (const ParseError& e) {
        res.error = std::string("extra data cua catalog.json khong doc duoc: ") + e.what();
        return res;
    }
    if (matchCount == 0) {
        res.error = "khong tim thay ban ghi extra-data co kich thuoc cu khop (dinh dang catalog khac?) - khong sua gi";
        return res;
    }
    if (matchCount > 1) {
        res.error = "co " + std::to_string(matchCount) +
                     " ban ghi trung kich thuoc cu, khong chac ban ghi nao dung - khong sua gi";
        return res;
    }

    std::string newB64 = base64_encode(raw);
    if (newB64.size() != oldB64.size()) {
        res.error = "do dai base64 doi khac sau khi sua (khong nen xay ra) - khong sua gi";
        return res;
    }
    text.replace(valStart, oldB64.size(), newB64);
    write_file(catalogJson, Bytes(text.begin(), text.end()));
    res.ok = true;
    return res;
}
