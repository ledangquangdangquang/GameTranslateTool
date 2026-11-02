#include "gamemaker.h"

#include <windows.h>
#include <wincodec.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "bzip2.h"
#include "csv.h"
#include "ttf.h"

// ------------------------------------------------------------------ misc
bool gm_is_game_dir(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "data.win", ec);
}

std::u32string utf8_to_u32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        char32_t v = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
        for (int k = 1; k < n && i + k < s.size(); k++) v = (v << 6) | (s[i + k] & 0x3F);
        out += v;
        i += n;
    }
    return out;
}

// ------------------------------------------------------------------ .ini
namespace {

struct IniLine {
    size_t start, end;  // [start, end) without the line break
};

std::vector<IniLine> split_lines(const std::string& t) {
    std::vector<IniLine> out;
    size_t st = 0;
    while (st < t.size()) {
        size_t e = t.find('\n', st);
        if (e == std::string::npos) e = t.size();
        size_t le = e;
        if (le > st && t[le - 1] == '\r') le--;
        out.push_back({st, le});
        st = e + 1;
    }
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// "[english]" -> "english", else ""
std::string section_of(const std::string& line) {
    size_t a = line.find_first_not_of(" \t"), b = line.find_last_not_of(" \t");
    if (a == std::string::npos || line[a] != '[' || line[b] != ']' || b <= a + 1) return "";
    return line.substr(a + 1, b - a - 1);
}

}  // namespace

std::vector<std::string> gm_ini_sections(const std::string& text) {
    std::vector<std::string> out;
    for (auto& l : split_lines(text)) {
        std::string s = section_of(text.substr(l.start, l.end - l.start));
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

TextMap gm_ini_read(const std::string& text, const std::string& section) {
    TextMap out;
    std::set<std::string> seen;
    bool in = false;
    for (auto& l : split_lines(text)) {
        std::string line = text.substr(l.start, l.end - l.start);
        std::string s = section_of(line);
        if (!s.empty()) { in = lower(s) == lower(section); continue; }
        size_t eq = line.find('=');
        if (!in || eq == std::string::npos || eq == 0) continue;
        std::string key = line.substr(0, eq);
        if (seen.insert(key).second) out.emplace_back(key, line.substr(eq + 1));
    }
    return out;
}

std::string gm_ini_apply(const std::string& text, const std::string& section,
                         const std::map<std::string, std::string>& tr, int& changed) {
    std::string out;
    out.reserve(text.size() + text.size() / 4);
    changed = 0;
    bool in = false;
    size_t prev = 0;
    for (auto& l : split_lines(text)) {
        std::string line = text.substr(l.start, l.end - l.start);
        std::string s = section_of(line);
        if (!s.empty()) in = lower(s) == lower(section);
        size_t eq = line.find('=');
        if (in && s.empty() && eq != std::string::npos && eq > 0) {
            auto it = tr.find(line.substr(0, eq));
            if (it != tr.end()) {
                std::string v = it->second;
                // one entry per line: a translation must never introduce a line break
                for (auto& c : v) if (c == '\r' || c == '\n') c = ' ';
                out += text.substr(prev, l.start + eq + 1 - prev);
                out += v;
                prev = l.end;
                changed++;
            }
        }
    }
    out += text.substr(prev);
    return out;
}

// ------------------------------------------------------------------ localization .json
// {"eng": {"key": "text", ...}, "jpn": {...}} (Fields of Mistria). Scanned on the raw text so a rewrite
// only replaces the string values it translates; everything else stays byte-identical.
namespace {

size_t skip_ws(const std::string& t, size_t i) {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) i++;
    return i;
}

void put_utf8(std::string& o, uint32_t c) {
    if (c < 0x80) o += (char)c;
    else if (c < 0x800) o += (char)(0xC0 | c >> 6), o += (char)(0x80 | (c & 0x3F));
    else if (c < 0x10000) o += (char)(0xE0 | c >> 12), o += (char)(0x80 | (c >> 6 & 0x3F)), o += (char)(0x80 | (c & 0x3F));
    else
        o += (char)(0xF0 | c >> 18), o += (char)(0x80 | (c >> 12 & 0x3F)), o += (char)(0x80 | (c >> 6 & 0x3F)),
            o += (char)(0x80 | (c & 0x3F));
}

// t[i] == '"': decodes the string, end = index after the closing quote
bool read_jstring(const std::string& t, size_t i, size_t& end, std::string& out) {
    out.clear();
    for (size_t k = i + 1; k < t.size(); k++) {
        char c = t[k];
        if (c == '"') { end = k + 1; return true; }
        if (c != '\\') { out += c; continue; }
        if (++k >= t.size()) return false;
        switch (t[k]) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (k + 4 >= t.size()) return false;
                uint32_t c1 = std::stoul(t.substr(k + 1, 4), nullptr, 16);
                k += 4;
                if (c1 >= 0xD800 && c1 < 0xDC00 && k + 6 < t.size() && t[k + 1] == '\\' && t[k + 2] == 'u') {
                    uint32_t c2 = std::stoul(t.substr(k + 3, 4), nullptr, 16);
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                    k += 6;
                }
                put_utf8(out, c1);
                break;
            }
            default: out += t[k];  // \" \\ \/
        }
    }
    return false;
}

std::string write_jstring(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else o += (char)c;
    }
    return o + "\"";
}

bool skip_jvalue(const std::string& t, size_t i, size_t& end) {
    i = skip_ws(t, i);
    if (i >= t.size()) return false;
    std::string tmp;
    if (t[i] == '"') return read_jstring(t, i, end, tmp);
    if (t[i] == '{' || t[i] == '[') {
        char close = t[i] == '{' ? '}' : ']';
        i = skip_ws(t, i + 1);
        while (i < t.size() && t[i] != close) {
            if (close == '}') {
                if (!read_jstring(t, i, i, tmp)) return false;
                i = skip_ws(t, i);
                if (i >= t.size() || t[i] != ':') return false;
                i++;
            }
            if (!skip_jvalue(t, i, i)) return false;
            i = skip_ws(t, i);
            if (i < t.size() && t[i] == ',') i = skip_ws(t, i + 1);
        }
        if (i >= t.size()) return false;
        end = i + 1;
        return true;
    }
    while (i < t.size() && !std::strchr(",}] \t\r\n", t[i])) i++;  // number / true / false / null
    end = i;
    return true;
}

// Members of the object starting at t[i] == '{': key -> [start, end) of the raw value.
bool json_members(const std::string& t, size_t i, std::vector<std::pair<std::string, std::pair<size_t, size_t>>>& out) {
    i = skip_ws(t, i);
    if (i >= t.size() || t[i] != '{') return false;
    i = skip_ws(t, i + 1);
    while (i < t.size() && t[i] != '}') {
        std::string key;
        if (t[i] != '"' || !read_jstring(t, i, i, key)) return false;
        i = skip_ws(t, i);
        if (i >= t.size() || t[i] != ':') return false;
        size_t vs = skip_ws(t, i + 1), ve;
        if (!skip_jvalue(t, vs, ve)) return false;
        out.push_back({key, {vs, ve}});
        i = skip_ws(t, ve);
        if (i < t.size() && t[i] == ',') i = skip_ws(t, i + 1);
    }
    return i < t.size();
}

}  // namespace

std::vector<std::string> gm_json_sections(const std::string& text) {
    std::vector<std::string> out;
    std::vector<std::pair<std::string, std::pair<size_t, size_t>>> top;
    size_t st = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    if (!json_members(text, st, top)) return out;
    for (auto& [k, span] : top) {
        std::vector<std::pair<std::string, std::pair<size_t, size_t>>> m;
        if (text[span.first] == '{' && json_members(text, span.first, m) && !m.empty() &&
            std::all_of(m.begin(), m.end(), [&](auto& x) { return text[x.second.first] == '"'; }))
            out.push_back(k);
    }
    return out;
}

TextMap gm_json_read(const std::string& text, const std::string& section) {
    TextMap out;
    std::vector<std::pair<std::string, std::pair<size_t, size_t>>> top, m;
    size_t st = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    if (!json_members(text, st, top)) return out;
    for (auto& [k, span] : top) {
        if (k != section || !json_members(text, span.first, m)) continue;
        for (auto& [key, v] : m) {
            std::string s;
            size_t e;
            if (text[v.first] == '"' && read_jstring(text, v.first, e, s)) out.emplace_back(key, s);
        }
    }
    return out;
}

std::string gm_json_apply(const std::string& text, const std::string& section,
                          const std::map<std::string, std::string>& tr, int& changed) {
    changed = 0;
    std::vector<std::pair<std::string, std::pair<size_t, size_t>>> top, m;
    size_t st = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    if (!json_members(text, st, top)) throw ParseError("JSON khong hop le");
    std::string out;
    size_t prev = 0;
    for (auto& [k, span] : top) {
        if (k != section || !json_members(text, span.first, m)) continue;
        for (auto& [key, v] : m) {
            auto it = tr.find(key);
            if (it == tr.end() || text[v.first] != '"') continue;
            out += text.substr(prev, v.first - prev);
            out += write_jstring(it->second);
            prev = v.second;
            changed++;
        }
    }
    return out + text.substr(prev);
}

TextMap json_flat_read(const std::string& text) {
    TextMap out;
    std::vector<std::pair<std::string, std::pair<size_t, size_t>>> m;
    size_t st = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    if (!json_members(text, st, m)) return out;
    for (auto& [key, v] : m) {
        std::string s;
        size_t e;
        if (text[v.first] == '"' && read_jstring(text, v.first, e, s)) out.emplace_back(key, s);
    }
    return out;
}

std::string json_flat_apply(const std::string& text, const std::map<std::string, std::string>& tr, int& changed) {
    changed = 0;
    std::vector<std::pair<std::string, std::pair<size_t, size_t>>> m;
    size_t st = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    if (!json_members(text, st, m)) throw ParseError("JSON khong hop le");
    std::string out;
    size_t prev = 0;
    for (auto& [key, v] : m) {
        auto it = tr.find(key);
        if (it == tr.end() || text[v.first] != '"') continue;
        out += text.substr(prev, v.first - prev);
        out += write_jstring(it->second);
        prev = v.second;
        changed++;
    }
    return out + text.substr(prev);
}

// ------------------------------------------------------------------ multi-language .csv
// key,English,French,... - one row per key, one column per language (Starvester). Parsing/encoding
// itself is the shared delimiter-agnostic table reader in csv.h; this just fixes delim=',' and
// column 0 as the key.
std::vector<std::string> gm_csv_sections(const std::string& text) {
    std::vector<std::string> out;
    auto rows = csv_parse(text.substr(csv_bom(text)));
    if (rows.empty() || rows[0].size() < 2) return out;
    for (size_t c = 1; c < rows[0].size(); c++) out.push_back(rows[0][c].value);
    return out;
}

TextMap gm_csv_read(const std::string& text, const std::string& column) {
    TextMap out;
    auto rows = csv_parse(text.substr(csv_bom(text)));
    int col = csv_find_column(rows, column);
    if (col < 0) return out;
    std::set<std::string> seen;
    for (size_t r = 1; r < rows.size(); r++) {
        if ((size_t)col >= rows[r].size() || rows[r][0].value.empty()) continue;
        const std::string& key = rows[r][0].value;
        if (seen.insert(key).second) out.emplace_back(key, rows[r][col].value);
    }
    return out;
}

std::string gm_csv_apply(const std::string& text, const std::string& column,
                         const std::map<std::string, std::string>& tr, int& changed) {
    changed = 0;
    size_t bom = csv_bom(text);
    std::string body = text.substr(bom);
    auto rows = csv_parse(body);
    int col = csv_find_column(rows, column);
    if (col < 0) throw ParseError("CSV: khong thay cot " + column);
    std::string out = text.substr(0, bom);
    size_t prev = 0;
    for (size_t r = 1; r < rows.size(); r++) {
        if ((size_t)col >= rows[r].size()) continue;
        auto it = tr.find(rows[r][0].value);
        if (it == tr.end()) continue;
        const CsvField& f = rows[r][col];
        out += body.substr(prev, f.start - prev);
        out += csv_encode(it->second);
        prev = f.end;
        changed++;
    }
    out += body.substr(prev);
    return out;
}

// ------------------------------------------------------------------ data.win fonts
namespace {

struct Win {
    Bytes& d;
    std::map<std::string, std::pair<uint32_t, uint32_t>> chunks;  // name -> (content offset, size)

    uint32_t u32(size_t at) const {
        if (at + 4 > d.size()) throw ParseError("data.win: doc vuot cuoi file");
        return d[at] | d[at + 1] << 8 | d[at + 2] << 16 | (uint32_t)d[at + 3] << 24;
    }
    uint16_t u16(size_t at) const { return (uint16_t)(d[at] | d[at + 1] << 8); }
    void w32(size_t at, uint32_t v) { for (int i = 0; i < 4; i++) d[at + i] = (uint8_t)(v >> (8 * i)); }
    void w16(size_t at, uint16_t v) { d[at] = (uint8_t)v; d[at + 1] = (uint8_t)(v >> 8); }
    std::string str(uint32_t p) const {
        uint32_t n = u32(p - 4);
        return std::string((const char*)&d[p], std::min<size_t>(n, d.size() - p));
    }
    bool in(const char* name, uint32_t p) const {
        auto it = chunks.find(name);
        return it != chunks.end() && p >= it->second.first && p < it->second.first + it->second.second;
    }

    explicit Win(Bytes& data) : d(data) {
        if (d.size() < 16 || std::memcmp(d.data(), "FORM", 4) != 0) throw ParseError("khong phai data.win (FORM)");
        size_t pos = 8;
        while (pos + 8 <= d.size()) {
            std::string n((const char*)&d[pos], 4);
            uint32_t sz = u32(pos + 4);
            chunks[n] = {(uint32_t)pos + 8, sz};
            pos += 8 + (size_t)sz;
        }
        for (const char* c : {"FONT", "TPAG", "TXTR"})
            if (!chunks.count(c)) throw ParseError(std::string("data.win thieu chunk ") + c);
    }
};

struct Font {
    int index;
    uint32_t rec;         // record offset
    uint32_t hdr;         // header size (bytes before glyph count)
    uint32_t tpagField;   // header offset of the texture (TPAG) pointer
    uint32_t tpag;
    uint32_t count, end;  // glyph count, end of last glyph record
    std::string name, display;
    std::set<char32_t> chars;
    int cellH = 0;
};

struct Page {
    uint32_t entry, blob, blobSize, w, h;
    bool png = false;  // else QOI / bzip2+QOI ("2zoq"): still replaceable by a PNG, not measurable
};

struct RenderedGlyph {
    char32_t ch;
    int w, h, shift, offset, inkY;  // inkY: top of the bitmap inside the cell
    int bw = 0, bh = 0;
    std::vector<uint8_t> alpha;  // bw*bh, 0..255
    int x = 0, y = 0;            // atlas position
};

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// GameMaker's QOI texture ("fioq" + u16 width + u16 height + u32 data length): an early draft of the
// QOI format (8/16-bit runs, 8/16/24-bit diffs, colour hash r^g^b^a). Output is BGRA.
bool decode_qoi(const uint8_t* p, size_t size, int& w, int& h, std::vector<uint8_t>& bgra) {
    if (size < 12 || std::memcmp(p, "fioq", 4) != 0) return false;
    w = p[4] | p[5] << 8;
    h = p[6] | p[7] << 8;
    size_t len = p[8] | p[9] << 8 | p[10] << 16 | (size_t)p[11] << 24;
    if (12 + len > size || !w || !h) return false;
    const uint8_t* s = p + 12;
    const uint8_t* end = s + len;
    struct Px { int r, g, b, a; } px{0, 0, 0, 255}, index[64] = {};
    bgra.assign((size_t)w * h * 4, 0);
    int run = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        if (run > 0) run--;
        else if (s < end) {
            int b1 = *s++;
            if ((b1 & 0xC0) == 0x00) px = index[b1 & 0x3F];
            else if ((b1 & 0xE0) == 0x40) run = b1 & 0x1F;
            else if ((b1 & 0xE0) == 0x60) {
                if (s >= end) return false;
                run = (((b1 & 0x1F) << 8) | *s++) + 32;
            } else if ((b1 & 0xC0) == 0x80) {
                px.r += ((b1 >> 4) & 3) - 2;
                px.g += ((b1 >> 2) & 3) - 2;
                px.b += (b1 & 3) - 2;
            } else if ((b1 & 0xE0) == 0xC0) {
                if (s >= end) return false;
                int b2 = *s++;
                px.r += (b1 & 0x1F) - 16;
                px.g += (b2 >> 4) - 8;
                px.b += (b2 & 0x0F) - 8;
            } else if ((b1 & 0xF0) == 0xE0) {
                if (s + 2 > end) return false;
                int b2 = *s++, b3 = *s++;
                px.r += (((b1 & 0x0F) << 1) | (b2 >> 7)) - 16;
                px.g += ((b2 & 0x7C) >> 2) - 16;
                px.b += (((b2 & 0x03) << 3) | ((b3 & 0xE0) >> 5)) - 16;
                px.a += (b3 & 0x1F) - 16;
            } else {  // 1111rgba: the flagged channels follow as full bytes
                if (b1 & 8) px.r = s < end ? *s++ : 0;
                if (b1 & 4) px.g = s < end ? *s++ : 0;
                if (b1 & 2) px.b = s < end ? *s++ : 0;
                if (b1 & 1) px.a = s < end ? *s++ : 0;
            }
            px.r &= 255, px.g &= 255, px.b &= 255, px.a &= 255;
            index[(px.r ^ px.g ^ px.b ^ px.a) & 63] = px;
        }
        uint8_t* o = &bgra[i * 4];
        o[0] = (uint8_t)px.b, o[1] = (uint8_t)px.g, o[2] = (uint8_t)px.r, o[3] = (uint8_t)px.a;
    }
    return true;
}

bool decode_png(const uint8_t* png, size_t size, int& w, int& h, std::vector<uint8_t>& bgra);

}  // namespace

bool gm_decode_texture(const uint8_t* p, size_t size, int& w, int& h, std::vector<uint8_t>& bgra) {
    if (size >= 4 && std::memcmp(p, "\x89PNG", 4) == 0) return decode_png(p, size, w, h, bgra);
    // "2zoq": "2zoq" + u16 width + u16 height + u32 decompressed size + a bzip2 stream wrapping a
    // plain "fioq" QOI blob (GameMaker's compressed texture pages - every page in some games).
    if (size >= 12 && std::memcmp(p, "2zoq", 4) == 0) {
        uint32_t decompSize = p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24;
        std::vector<uint8_t> inner;
        inner.reserve(decompSize);
        if (!bzip2_decompress(p + 12, size - 12, inner) || inner.size() != decompSize) return false;
        return gm_decode_texture(inner.data(), inner.size(), w, h, bgra);
    }
    return decode_qoi(p, size, w, h, bgra);
}

namespace {

uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

void put_chunk(Bytes& out, const char* type, const uint8_t* data, uint32_t len) {
    out.push_back((uint8_t)(len >> 24)), out.push_back((uint8_t)(len >> 16));
    out.push_back((uint8_t)(len >> 8)), out.push_back((uint8_t)len);
    size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data, data + len);
    uint32_t crc = crc32(&out[crcStart], out.size() - crcStart);
    out.push_back((uint8_t)(crc >> 24)), out.push_back((uint8_t)(crc >> 16));
    out.push_back((uint8_t)(crc >> 8)), out.push_back((uint8_t)crc);
}

// WIC's PNG zlib stream isn't always at the best compression level. Inflates it and re-deflates at
// Z_BEST_COMPRESSION, keeping the result only if it's smaller - the filtered scanline bytes (and so the
// decoded pixels) are unchanged either way.
void recompress_zlib(Bytes& z) {
    uLongf cap = (uLongf)z.size() * 32 + 4096;
    std::vector<uint8_t> raw;
    for (;;) {
        if (cap > 64u * 1024 * 1024) return;
        raw.resize(cap);
        uLongf outLen = cap;
        int r = uncompress(raw.data(), &outLen, z.data(), (uLong)z.size());
        if (r == Z_OK) { raw.resize(outLen); break; }
        if (r == Z_BUF_ERROR) { cap *= 2; continue; }
        return;
    }
    uLongf outCap = compressBound((uLong)raw.size());
    Bytes out(outCap);
    uLongf outLen = outCap;
    if (compress2(out.data(), &outLen, raw.data(), (uLong)raw.size(), Z_BEST_COMPRESSION) != Z_OK) return;
    out.resize(outLen);
    if (out.size() < z.size()) z = std::move(out);
}

// Keeps only the chunks GameMaker's own PNG/QOI loader needs (IHDR, PLTE, tRNS, IDAT, IEND), dropping
// whatever ancillary chunks WIC adds (gAMA, cHRM, sRGB, iCCP, pHYs...), and coalesces the IDAT chunks WIC
// splits the pixel stream into back into one. Every byte counts: a replacement page must fit the
// original blob's size, which for GameMaker's "2zoq" pages is a bzip2-compressed size.
void strip_png_chunks(Bytes& png) {
    if (png.size() < 8 || std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) != 0) return;
    Bytes out(png.begin(), png.begin() + 8);
    Bytes idat;
    size_t pos = 8;
    while (pos + 8 <= png.size()) {
        uint32_t len = (uint32_t)png[pos] << 24 | png[pos + 1] << 16 | png[pos + 2] << 8 | png[pos + 3];
        if (pos + 12 + len > png.size()) break;
        const char* type = (const char*)&png[pos + 4];
        if (!std::memcmp(type, "IDAT", 4)) idat.insert(idat.end(), &png[pos + 8], &png[pos + 8] + len);
        else if (!std::memcmp(type, "IHDR", 4) || !std::memcmp(type, "PLTE", 4) || !std::memcmp(type, "tRNS", 4))
            out.insert(out.end(), png.begin() + pos, png.begin() + pos + 12 + len);
        pos += 12 + len;
    }
    recompress_zlib(idat);
    put_chunk(out, "IDAT", idat.data(), (uint32_t)idat.size());
    put_chunk(out, "IEND", nullptr, 0);
    png = std::move(out);
}

bool encode_png(const std::vector<uint8_t>& bgra, int w, int h, Bytes& out) {
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                (void**)&fac)))
        return false;
    IStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &st)) &&
              SUCCEEDED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fr, &props));
    if (ok && props) {  // per-scanline adaptive filtering: smaller than WIC's default, every byte counts here
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"FilterOption");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI1;
        v.bVal = WICPngFilterAdaptive;
        props->Write(1, &opt, &v);
    }
    ok = ok && SUCCEEDED(fr->Initialize(props)) && SUCCEEDED(fr->SetSize(w, h));
    if (ok) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(fr->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
             SUCCEEDED(fr->WritePixels(h, w * 4, (UINT)bgra.size(), (BYTE*)bgra.data())) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (ok) {
        STATSTG stat{};
        LARGE_INTEGER zero{};
        ok = SUCCEEDED(st->Stat(&stat, STATFLAG_NONAME)) && SUCCEEDED(st->Seek(zero, STREAM_SEEK_SET, nullptr));
        if (ok) {
            out.resize((size_t)stat.cbSize.QuadPart);
            ULONG got = 0;
            ok = SUCCEEDED(st->Read(out.data(), (ULONG)out.size(), &got)) && got == out.size();
        }
    }
    if (ok) strip_png_chunks(out);
    if (props) props->Release();
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    fac->Release();
    return ok;
}

bool decode_png(const uint8_t* png, size_t size, int& w, int& h, std::vector<uint8_t>& bgra) {
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                (void**)&fac)))
        return false;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    UINT uw = 0, uh = 0;
    bool ok = SUCCEEDED(fac->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)png, (DWORD)size)) &&
              SUCCEEDED(fac->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&uw, &uh)) &&
              SUCCEEDED(fac->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom));
    if (ok) {
        w = (int)uw;
        h = (int)uh;
        bgra.resize((size_t)w * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)bgra.size(), bgra.data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    fac->Release();
    return ok;
}

// Rows [top, bottom) holding ink inside the cell (x, y, w, h) of a decoded page; false when empty.
bool ink_rows(const std::vector<uint8_t>& bgra, int pageW, int x, int y, int w, int h, int& top, int& bottom) {
    top = -1;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            if (bgra[((size_t)(y + r) * pageW + x + c) * 4 + 3] > 96) {
                if (top < 0) top = r;
                bottom = r + 1;
                break;
            }
    return top >= 0;
}

HFONT make_font(const std::wstring& family, int weight, bool italic, int em, bool pixel = false) {
    LOGFONTW lf{};
    lf.lfHeight = -em;
    lf.lfWeight = weight;
    lf.lfItalic = italic;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = pixel ? NONANTIALIASED_QUALITY : ANTIALIASED_QUALITY;
    wcsncpy(lf.lfFaceName, family.c_str(), LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

const MAT2 kIdent = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};

// Is `family` installed (not substituted by GDI) and does it have every Vietnamese letter?
bool family_has_vietnamese(const std::wstring& family) {
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT f = make_font(family, FW_NORMAL, false, 32);
    HGDIOBJ old = SelectObject(dc, f);
    wchar_t face[LF_FACESIZE] = {};
    GetTextFaceW(dc, LF_FACESIZE, face);
    bool ok = _wcsicmp(face, family.c_str()) == 0;
    for (const char32_t* p = kVietnamese; ok && *p; p++) {
        WCHAR wc = (WCHAR)*p;
        WORD gi = 0;
        ok = GetGlyphIndicesW(dc, &wc, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && gi != 0xFFFF;
    }
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    return ok;
}

// Renders `chars` with GDI into GameMaker glyph cells of height cellH with the baseline at `baseline`.
// Starts at the size whose capital H is capPx tall (the original font's). Stacked Vietnamese capitals
// (Ẫ, Ể...) must fit the cell: smooth fonts shrink until they do; pixel fonts (pixel = no antialiasing,
// 1-bit glyphs) keep their size and the cell grows instead (cellH / baseline are updated).
std::vector<RenderedGlyph> render(const std::wstring& family, int weight, bool italic, int emPx, int capPx,
                                  int& cellH, int& baseline, bool pixel, const std::set<char32_t>& chars,
                                  std::string& info, std::u32string& missing, bool fold3) {
    HDC dc = CreateCompatibleDC(nullptr);
    std::vector<RenderedGlyph> out;
    const MAT2 ident = kIdent;
    if (capPx > 0) {
        HFONT f = make_font(family, weight, italic, emPx, pixel);
        HGDIOBJ old = SelectObject(dc, f);
        GLYPHMETRICS gm{};
        if (GetGlyphOutlineW(dc, L'H', GGO_METRICS, &gm, 0, nullptr, &ident) != GDI_ERROR && gm.gmptGlyphOrigin.y > 0)
            emPx = (int)std::lround((double)emPx * capPx / gm.gmptGlyphOrigin.y);
        SelectObject(dc, old);
        DeleteObject(f);
    }
    if (pixel) emPx = std::max(emPx, 9);  // below 9px 1-bit Vietnamese marks turn to mush
    int minEm = pixel ? emPx : std::max(6, emPx * 6 / 10);
    for (int em = emPx; em >= minEm; em--) {
        HFONT f = make_font(family, weight, italic, em, pixel);
        HGDIOBJ old = SelectObject(dc, f);
        out.clear();
        missing.clear();
        int top = 0, bottom = 0;  // worst overflow above the cell / below it
        for (char32_t c : chars) {
            // fold3: U+0500..U+0559 hold the Vietnamese letters U+1EA0..U+1EF9 (see gm_fold_3byte)
            char32_t src = fold3 && c >= 0x500 && c <= 0x559 ? 0x1EA0 + (c - 0x500) : c;
            if (src > 0xFFFF) { missing += c; continue; }
            WCHAR wc = (WCHAR)src;
            WORD gi = 0;
            if (GetGlyphIndicesW(dc, &wc, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR || gi == 0xFFFF) {
                missing += c;
                continue;
            }
            GLYPHMETRICS gm{};
            UINT fmt = pixel ? GGO_BITMAP : GGO_GRAY8_BITMAP;
            DWORD sz = GetGlyphOutlineW(dc, wc, fmt, &gm, 0, nullptr, &ident);
            if (sz == GDI_ERROR) { missing += c; continue; }
            RenderedGlyph g;
            g.ch = c;
            g.shift = gm.gmCellIncX;
            g.h = cellH;
            if (sz > 0) {
                std::vector<uint8_t> buf(sz);
                GetGlyphOutlineW(dc, wc, fmt, &gm, sz, buf.data(), &ident);
                g.bw = gm.gmBlackBoxX;
                g.bh = gm.gmBlackBoxY;
                g.alpha.resize((size_t)g.bw * g.bh);
                if (pixel) {  // 1 bit per pixel, MSB first, rows padded to 4 bytes
                    int pitch = ((g.bw + 31) / 32) * 4;
                    for (int y = 0; y < g.bh; y++)
                        for (int x = 0; x < g.bw; x++)
                            g.alpha[(size_t)y * g.bw + x] = (buf[(size_t)y * pitch + x / 8] >> (7 - x % 8)) & 1 ? 255 : 0;
                } else {
                    int pitch = (g.bw + 3) & ~3;
                    for (int y = 0; y < g.bh; y++)
                        for (int x = 0; x < g.bw; x++)
                            g.alpha[(size_t)y * g.bw + x] = (uint8_t)std::min(255, buf[(size_t)y * pitch + x] * 255 / 64);
                }
                g.w = g.bw;
                g.offset = gm.gmptGlyphOrigin.x;
                g.inkY = baseline - gm.gmptGlyphOrigin.y;
                top = std::max(top, -g.inkY);
                bottom = std::max(bottom, g.inkY + g.bh - cellH);
            } else {
                g.w = std::max(1, (int)gm.gmCellIncX);  // space: empty cell as wide as the advance
                g.offset = 0;
                g.inkY = 0;
            }
            out.push_back(std::move(g));
        }
        SelectObject(dc, old);
        DeleteObject(f);
        if (pixel && (top > 0 || bottom > 0)) {
            int grow = std::max(top, 0), extra = grow + std::max(bottom, 0);
            for (auto& g : out) {
                if (!g.alpha.empty()) g.inkY += grow;
                g.h += extra;
            }
            info = std::to_string(em) + "px pixel, o chu cao " + std::to_string(cellH) + " -> " +
                   std::to_string(cellH + extra) + "px";
            baseline += grow;
            cellH += extra;
            break;
        }
        if ((top <= 0 && bottom <= 1) || em == minEm) {
            info = std::to_string(em) + "px" + (pixel ? " pixel" : "") +
                   (top > 0 ? " (dau tren chu hoa bi cat " + std::to_string(top) + "px)" : "");
            break;
        }
    }
    DeleteDC(dc);
    return out;
}

bool pack(std::vector<RenderedGlyph*>& gs, int W, int H) {
    int x = 2, y = 2, rowH = 0;
    for (auto* g : gs) {
        if (x + g->w + 2 > W || (rowH && g->h != rowH)) { x = 2; y += rowH + 2; rowH = 0; }
        if (g->w + 4 > W) return false;
        g->x = x;
        g->y = y;
        x += g->w + 2;
        rowH = std::max(rowH, g->h);
    }
    return y + rowH + 2 <= H;
}

}  // namespace

bool gm_patch_fonts(Bytes& data, const std::u32string& wanted, const std::string& familyUtf8,
                    const std::string& pixelFamilyUtf8, const std::function<void(const std::string&)>& log,
                    bool fold3) {
    struct Com {  // WIC needs COM on this (worker) thread
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~Com() { if (SUCCEEDED(hr)) CoUninitialize(); }
    } com;
    Win w(data);
    auto [fo, fsz] = w.chunks["FONT"];
    auto [to, tsz] = w.chunks["TPAG"];
    auto [xo, xsz] = w.chunks["TXTR"];
    (void)fsz;
    (void)tsz;
    (void)xsz;

    // texture pages: entry = {scaled, mips, blobSize, width, height, index, blobPtr}
    std::vector<Page> pages;
    uint32_t pc = w.u32(xo);
    for (uint32_t i = 0; i < pc; i++) {
        uint32_t e = w.u32(xo + 4 + 4 * i);
        Page p{e, w.u32(e + 24), w.u32(e + 8), w.u32(e + 12), w.u32(e + 16)};
        bool known = w.in("TXTR", p.blob) && p.blob + 4 <= data.size();
        p.png = known && std::memcmp(&data[p.blob], "\x89PNG", 4) == 0;
        known = known && (p.png || std::memcmp(&data[p.blob], "2zoq", 4) == 0 || std::memcmp(&data[p.blob], "fioq", 4) == 0);
        pages.push_back(known ? p : Page{e, 0, 0, 0, 0});
    }
    // how many TPAG items sit on each page
    std::map<uint16_t, int> perPage;
    uint32_t tc = w.u32(to);
    for (uint32_t i = 0; i < tc; i++) perPage[w.u16(w.u32(to + 4 + 4 * i) + 20)]++;

    // fonts
    std::vector<Font> fonts;
    uint32_t G = 0;  // glyph record size without kerning pairs
    uint32_t fc = w.u32(fo);
    for (uint32_t i = 0; i < fc; i++) {
        Font f;
        f.index = (int)i;
        f.rec = w.u32(fo + 4 + 4 * i);
        f.name = w.str(w.u32(f.rec));
        f.display = w.str(w.u32(f.rec + 4));
        f.hdr = 0;
        for (uint32_t off = 8; off < 200; off += 4) {
            uint32_t n = w.u32(f.rec + off);
            if (n > 0 && n < 70000 && w.u32(f.rec + off + 4) == f.rec + off + 4 + 4 * n) { f.hdr = off; break; }
        }
        if (!f.hdr) throw ParseError("data.win: khong doc duoc font " + f.name);
        f.tpagField = 0;
        for (uint32_t off = 8; off < f.hdr; off += 4)
            if (w.in("TPAG", w.u32(f.rec + off))) { f.tpagField = off; break; }
        if (!f.tpagField) throw ParseError("data.win: font " + f.name + " khong co texture");
        f.tpag = w.u32(f.rec + f.tpagField);
        f.count = w.u32(f.rec + f.hdr);
        uint32_t list = f.rec + f.hdr + 4;
        // glyph = char, x, y, w, h, shift, offset, [one more i16 since GameMaker 2024], kerning count + pairs
        if (!G && f.count >= 2) {
            uint32_t q0 = w.u32(list), q1 = w.u32(list + 4);
            for (uint32_t size : {16u, 18u})
                if (q1 == q0 + size + 4 * w.u16(q0 + size - 2)) G = size;
        }
        for (uint32_t g = 0; g < f.count; g++) {
            uint32_t q = w.u32(list + 4 * g);
            uint32_t next = g + 1 < f.count ? w.u32(list + 4 * (g + 1)) : 0;
            uint32_t gs = G ? G : 16;
            uint32_t kern = w.u16(q + gs - 2);
            if (next && next != q + gs + 4 * kern) throw ParseError("data.win: cau truc glyph la (" + f.name + ")");
            f.chars.insert(w.u16(q));
            f.cellH = std::max<int>(f.cellH, w.u16(q + 8));
            f.end = q + gs + 4 * kern;
        }
        fonts.push_back(f);
    }

    auto letters = [](const Font& f) {
        int n = 0;
        for (char32_t c : f.chars) n += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        return n;
    };
    std::set<char32_t> need(wanted.begin(), wanted.end());
    for (const char32_t* p = kVietnamese; *p; p++) need.insert(*p);
    for (char32_t c = 0x20; c < 0x7F; c++) need.insert(c);
    if (fold3) {  // the game never sees 3-byte characters: their 2-byte stand-ins get the glyphs instead
        std::set<char32_t> folded;
        for (char32_t c : need)
            if (c < 0x800) folded.insert(c);
            else if (c >= 0x1EA0 && c <= 0x1EF9) folded.insert(0x500 + (c - 0x1EA0));
        need = folded;
    }

    std::vector<int> recipients, donors;
    for (auto& f : fonts) {
        if (f.count >= 500) donors.push_back(f.index);  // CJK-sized
        else if (letters(f) >= 40 && !std::includes(f.chars.begin(), f.chars.end(), need.begin(), need.end()))
            recipients.push_back(f.index);
    }
    // Donors are scarce: fonts for the [english] text first, language-specific Latin fonts (Font_ES,
    // fnt_ru...) and debug fonts (fnt_bugger...) last
    auto rank = [&](int r) {
        std::string n = lower(fonts[r].name);
        for (const char* k : {"debug", "bugger", "console", "fps"})
            if (n.find(k) != std::string::npos) return 3;
        // a language code as a name part: Font_ES, fnt_ru_title
        std::string part;
        for (size_t i = 0; i <= n.size(); i++) {
            if (i < n.size() && std::isalnum((unsigned char)n[i])) { part += n[i]; continue; }
            for (const char* code : {"es", "fr", "de", "ru", "pt", "br", "it", "pl", "tr", "jp", "cn", "kr", "ko", "zh"})
                if (part == code) return 2;
            part.clear();
        }
        // fonts with accented Latin letters were made for European text; ASCII-only ones may be debug/HUD
        int latin1 = 0;
        for (char32_t c : fonts[r].chars) latin1 += c >= 0xC0 && c <= 0xFF;
        return latin1 >= 10 ? 0 : 1;
    };
    std::stable_sort(recipients.begin(), recipients.end(), [&](int a, int b) { return rank(a) < rank(b); });

    // Decoded original pages (PNG or QOI), on demand
    std::map<uint16_t, std::vector<uint8_t>> pix;
    std::map<uint16_t, int> pixW;
    auto decoded = [&](uint16_t pg) -> const std::vector<uint8_t>* {
        auto it = pix.find(pg);
        if (it == pix.end()) {
            std::vector<uint8_t> px;
            int W = 0, H = 0;
            const Page& P = pages[pg];
            if (!P.blob || !gm_decode_texture(&data[P.blob], P.blobSize, W, H, px) || W != (int)P.w || H != (int)P.h)
                px.clear();
            pixW[pg] = W;
            it = pix.emplace(pg, std::move(px)).first;
        }
        return it->second.empty() ? nullptr : &it->second;
    };

    // Where a new atlas may go:
    //  - a whole page: the recipient's own page when nothing else is on it, or a page holding nothing but CJK
    //    fonts (Well Dweller; Crypt Custodian keeps Font_JP and Font_JP_Large together) - drawn from scratch;
    //  - else the rectangle of a CJK font on a page shared with sprites (Fields of Mistria): the page is
    //    decoded, only that rectangle is redrawn, everything else on the page stays.
    struct Slot {
        uint32_t tpag;
        bool whole;
    };
    std::set<uint32_t> cjkTpags;
    std::map<uint16_t, int> cjkItems;
    for (int d : donors)
        if (cjkTpags.insert(fonts[d].tpag).second) cjkItems[w.u16(fonts[d].tpag + 20)]++;
    std::vector<Slot> pool;
    for (bool whole : {true, false})
        for (uint32_t t : cjkTpags) {
            uint16_t pg = w.u16(t + 20);
            if (pages[pg].blob && (perPage[pg] == cjkItems[pg]) == whole) pool.push_back({t, whole});
        }

    struct Job {
        int r, d;
        Slot slot;
        std::vector<RenderedGlyph> gs;
        int cellH;
        std::string face, info, miss;
    };
    std::vector<Job> jobs;
    std::wstring family = widen(familyUtf8), pixelFamily = widen(pixelFamilyUtf8);
    std::set<int> usedDonor;
    std::set<uint32_t> usedTpag;
    for (int r : recipients) {
        Font& R = fonts[r];
        std::set<char32_t> chars = R.chars;
        chars.insert(need.begin(), need.end());
        std::vector<char32_t> control;  // e.g. a CR glyph: kept as an empty cell
        for (auto it = chars.begin(); it != chars.end();)
            if (*it < 0x20) { control.push_back(*it); it = chars.erase(it); }
            else ++it;

        // donor record big enough for the new glyph table
        uint32_t needBytes = R.hdr + 4 + (uint32_t)(chars.size() + control.size()) * (G + 4);
        int D = -1;
        for (int d : donors)
            if (!usedDonor.count(d) && fonts[d].end - fonts[d].rec >= needBytes) { D = d; break; }
        Slot slot{0, false};
        uint16_t ownPage = w.u16(R.tpag + 20);
        if (perPage[ownPage] == 1 && pages[ownPage].blob) slot = {R.tpag, true};
        else
            for (const Slot& s : pool)
                if (!usedTpag.count(s.tpag) && (s.whole || decoded(w.u16(s.tpag + 20)))) { slot = s; break; }
        if (D < 0 || !slot.tpag) {
            log("  ! font " + R.name + ": het cho trong (can them font CJK de muon) - bo qua");
            continue;
        }

        // size + baseline from the original header (GameMaker 2023+: ..., ascenderOffset, ascender, sdf, lineHeight)
        uint32_t em = w.u32(R.rec + 8);
        float pt;
        if (em & 0x80000000u) { std::memcpy(&pt, &em, 4); pt = std::fabs(pt); }
        else pt = (float)em;
        int emPx = (int)std::lround(pt * 96.0 / 72.0);
        int cellH = R.cellH;
        int baseline = R.hdr >= 56 && w.u32(R.rec + 44) ? (int)w.u32(R.rec + 40) + (int)w.u32(R.rec + 44) : 0;
        // the capital H of the original glyphs, measured on the original page: cap height (and the
        // baseline when the header has none, as in older fonts)
        int capPx = 0;
        if (R.chars.count('H'))
            if (const std::vector<uint8_t>* px = decoded(ownPage)) {
                uint32_t list = R.rec + R.hdr + 4;
                for (uint32_t g = 0; g < R.count; g++) {
                    uint32_t q = w.u32(list + 4 * g);
                    if (w.u16(q) != 'H') continue;
                    int top, bottom;
                    if (ink_rows(*px, pixW[ownPage], w.u16(R.tpag) + w.u16(q + 2), w.u16(R.tpag + 2) + w.u16(q + 4),
                                 w.u16(q + 6), w.u16(q + 8), top, bottom)) {
                        if (baseline <= 0 || baseline > cellH) baseline = bottom;
                        capPx = bottom - top;
                    }
                    break;
                }
            }
        if (baseline <= 0 || baseline > cellH) baseline = cellH * 3 / 4;
        int weight = FW_NORMAL;
        std::string dl = lower(R.display);
        if (dl.find("black") != std::string::npos || dl.find("heavy") != std::string::npos) weight = FW_HEAVY;
        else if (dl.find("extrabold") != std::string::npos) weight = FW_EXTRABOLD;
        else if (dl.find("bold") != std::string::npos || w.u32(R.rec + 12)) weight = FW_BOLD;
        // antialias flag off (header byte +23) = pixel-art font: crisp 1-bit glyphs from the pixel font choice;
        // otherwise the original typeface when Windows has it with Vietnamese (Arial...), else the chosen one
        bool pixel = data[R.rec + 23] == 0;
        std::wstring face = pixel ? pixelFamily : family_has_vietnamese(widen(R.display)) ? widen(R.display) : family;
        Job j{r, D, slot, {}, 0, std::string(face.begin(), face.end()), "", ""};
        std::u32string missing;
        j.gs = render(face, weight, w.u32(R.rec + 16) != 0, emPx, capPx, cellH, baseline, pixel, chars, j.info,
                      missing, fold3);
        j.cellH = cellH;
        for (auto it = control.rbegin(); it != control.rend(); ++it) {
            RenderedGlyph g;
            g.ch = *it;
            g.w = 1;
            g.h = cellH;
            g.shift = g.offset = g.inkY = 0;
            j.gs.insert(j.gs.begin(), std::move(g));
        }
        // Characters the font originally had but our rendering family can't draw, and that aren't part
        // of `need` (so not plain Latin/Vietnamese text) are inline icons (a fuel-drop glyph at a PUA
        // code point, etc.): copied pixel-for-pixel from the original page instead of being dropped.
        for (char32_t c : missing) {
            if (need.count(c)) { j.miss += c < 0x80 ? std::string(1, (char)c) : "U+" + std::to_string((unsigned)c) + " "; continue; }
            bool copied = false;
            if (const std::vector<uint8_t>* px = decoded(ownPage)) {
                uint32_t list = R.rec + R.hdr + 4;
                for (uint32_t g = 0; g < R.count && !copied; g++) {
                    uint32_t q = w.u32(list + 4 * g);
                    if (w.u16(q) != c) continue;
                    int gw = w.u16(q + 6), gh = w.u16(q + 8);
                    int pageH = pixW[ownPage] ? (int)(px->size() / 4 / pixW[ownPage]) : 0;
                    int px0 = w.u16(R.tpag) + w.u16(q + 2), py0 = w.u16(R.tpag + 2) + w.u16(q + 4);
                    RenderedGlyph rg;
                    rg.ch = c;
                    rg.shift = (int16_t)w.u16(q + 10);
                    rg.offset = (int16_t)w.u16(q + 12);
                    rg.inkY = 0;
                    if (gw > 0 && gh > 0 && px0 + gw <= pixW[ownPage] && py0 + gh <= pageH) {
                        rg.w = gw;
                        rg.h = gh;
                        rg.bw = gw;
                        rg.bh = gh;
                        rg.alpha.resize((size_t)gw * gh);
                        for (int yy = 0; yy < gh; yy++)
                            for (int xx = 0; xx < gw; xx++)
                                rg.alpha[(size_t)yy * gw + xx] = (*px)[((size_t)(py0 + yy) * pixW[ownPage] + px0 + xx) * 4 + 3];
                    } else {
                        rg.w = std::max(1, (int)rg.shift);  // e.g. a space variant: empty cell as wide as the advance
                        rg.h = cellH;
                    }
                    j.gs.push_back(std::move(rg));
                    copied = true;
                }
            }
            if (!copied) j.miss += c < 0x80 ? std::string(1, (char)c) : "U+" + std::to_string((unsigned)c) + " ";
        }
        usedDonor.insert(D);
        usedTpag.insert(slot.tpag);
        jobs.push_back(std::move(j));
    }

    // one image per texture page, shared by every font placed on it
    std::map<uint16_t, std::vector<Job*>> byPage;
    for (auto& j : jobs) byPage[w.u16(j.slot.tpag + 20)].push_back(&j);
    int patched = 0;
    for (auto& [pg, js] : byPage) {
        Page& P = pages[pg];
        std::string names;
        for (Job* j : js) names += (names.empty() ? "" : ", ") + fonts[j->r].name;
        bool whole = js[0]->slot.whole;
        int W = (int)P.w, H = (int)P.h;
        std::vector<uint8_t> img;
        if (whole) {
            std::vector<RenderedGlyph*> all;
            for (Job* j : js)
                for (auto& g : j->gs) all.push_back(&g);
            while (!pack(all, W, H) && W <= 8192) {
                if (W <= H) W *= 2;
                else H *= 2;
            }
            if (W > 8192) { log("  ! " + names + ": chu qua to, khong xep vua 8192px"); continue; }
            img.assign((size_t)W * H * 4, 0);
            for (size_t i = 0; i < img.size(); i += 4) img[i] = img[i + 1] = img[i + 2] = 255;  // white, alpha 0
        } else {
            const std::vector<uint8_t>* px = decoded(pg);
            if (!px) { log("  ! " + names + ": khong doc duoc anh trang " + std::to_string(pg)); continue; }
            img = *px;
            bool fits = true;
            for (Job* j : js) {
                uint32_t t = j->slot.tpag;
                int rx = w.u16(t), ry = w.u16(t + 2), rw = w.u16(t + 4), rh = w.u16(t + 6);
                std::vector<RenderedGlyph*> mine;
                for (auto& g : j->gs) mine.push_back(&g);
                if (!pack(mine, rw, rh)) { fits = false; break; }
                for (int y = ry; y < ry + rh; y++)  // clear the CJK font's rectangle
                    for (int x = rx; x < rx + rw; x++) {
                        uint8_t* o = &img[((size_t)y * W + x) * 4];
                        o[0] = o[1] = o[2] = 255;
                        o[3] = 0;
                    }
                for (auto& g : j->gs) g.x += rx, g.y += ry;  // drawn in page coordinates below
            }
            if (!fits) { log("  ! " + names + ": khong xep vua vung font cu"); continue; }
        }
        for (Job* j : js)
            for (auto& g : j->gs)
                for (int y = 0; y < g.bh; y++) {
                    int py = g.y + g.inkY + y;
                    if (py < g.y || py >= g.y + g.h) continue;  // clipped to the cell
                    for (int x = 0; x < g.bw; x++)
                        img[((size_t)py * W + g.x + x) * 4 + 3] = g.alpha[(size_t)y * g.bw + x];
                }
        Bytes png;
        if (!encode_png(img, W, H, png)) { log("  ! " + names + ": khong ma hoa duoc PNG"); continue; }
        if (png.size() > P.blobSize) {
            log("  ! " + names + ": anh moi (" + std::to_string(png.size()) + " B) lon hon cho cu (" +
                std::to_string(P.blobSize) + " B) - bo qua");
            continue;
        }

        // --- commit: texture page (a PNG also replaces a QOI page: the runner checks each page's magic)
        std::memcpy(&data[P.blob], png.data(), png.size());
        std::fill(data.begin() + P.blob + png.size(), data.begin() + P.blob + P.blobSize, 0);
        w.w32(P.entry + 8, (uint32_t)png.size());
        w.w32(P.entry + 12, W);
        w.w32(P.entry + 16, H);
        for (Job* j : js) {
            Font& R = fonts[j->r];
            Font& Dn = fonts[j->d];
            uint32_t tpag = j->slot.tpag;
            if (whole) {  // TPAG item: the whole page; glyph coordinates are page coordinates
                uint16_t item[10] = {0, 0, (uint16_t)W, (uint16_t)H, 0, 0, (uint16_t)W, (uint16_t)H, (uint16_t)W, (uint16_t)H};
                for (int i = 0; i < 10; i++) w.w16(tpag + 2 * i, item[i]);
            } else {  // TPAG item unchanged; glyph coordinates relative to its rectangle
                for (auto& g : j->gs) g.x -= w.u16(tpag), g.y -= w.u16(tpag + 2);
            }
            // glyph table into the donor record: header copied from the recipient (name, size, ascender...)
            uint32_t at = Dn.rec;
            uint32_t dnName = w.u32(Dn.rec), dnDisp = w.u32(Dn.rec + 4);
            std::vector<uint8_t> hdr(data.begin() + R.rec, data.begin() + R.rec + R.hdr);
            std::memcpy(&data[at], hdr.data(), hdr.size());
            w.w32(at + R.tpagField, tpag);
            if (R.hdr >= 56 && j->cellH != R.cellH && w.u32(at + 52)) w.w32(at + 52, j->cellH);  // lineHeight
            uint32_t n = (uint32_t)j->gs.size();
            w.w32(at + R.hdr, n);
            uint32_t glyphs = at + R.hdr + 4 + 4 * n;
            for (uint32_t i = 0; i < n; i++) {
                const auto& g = j->gs[i];
                uint32_t q = glyphs + G * i;
                w.w32(at + R.hdr + 4 + 4 * i, q);
                w.w16(q, (uint16_t)g.ch);
                w.w16(q + 2, (uint16_t)g.x);
                w.w16(q + 4, (uint16_t)g.y);
                w.w16(q + 6, (uint16_t)g.w);
                w.w16(q + 8, (uint16_t)g.h);
                w.w16(q + 10, (uint16_t)(int16_t)g.shift);
                w.w16(q + 12, (uint16_t)(int16_t)g.offset);
                if (G == 18) w.w16(q + 14, 0);
                w.w16(q + G - 2, 0);  // no kerning pairs
            }
            // swap the FONT table entries; the old record takes the donor's name so names stay unique
            w.w32(fo + 4 + 4 * R.index, Dn.rec);
            w.w32(fo + 4 + 4 * Dn.index, R.rec);
            w.w32(R.rec, dnName);
            w.w32(R.rec + 4, dnDisp);
            patched++;
            log("  font " + R.name + " (" + R.display + ") -> " + j->face + " " + j->info + ", " + std::to_string(n) +
                " chu, trang " + std::to_string(pg) + (whole ? " " + std::to_string(W) + "x" + std::to_string(H) : " (vung cu)") +
                ", PNG " + std::to_string(png.size() / 1024) + "/" + std::to_string(P.blobSize / 1024) +
                " KB (muon cho cua " + Dn.name + ")" + (j->miss.empty() ? "" : ". Font thieu: " + j->miss));
        }
        P.blobSize = (uint32_t)png.size();
    }
    return patched > 0;
}

std::string gm_fold_3byte(const std::string& utf8) {
    std::string out;
    for (char32_t c : utf8_to_u32(utf8)) {
        if (c < 0x800) put_utf8(out, c);
        else if (c >= 0x1EA0 && c <= 0x1EF9) put_utf8(out, 0x500 + (c - 0x1EA0));
        else if (c == 0x2026) out += "...";
        else if (c == 0x2013 || c == 0x2014 || c == 0x2212) out += "-";
        else if (c == 0x2018 || c == 0x2019 || c == 0x201B) out += "'";
        else if (c == 0x201C || c == 0x201D) out += "\"";
        else if (c == 0x2002 || c == 0x2003 || c == 0x2009) out += " ";
        else out += "?";
    }
    return out;
}
