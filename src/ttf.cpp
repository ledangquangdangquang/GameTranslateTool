#include "ttf.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>

const char32_t kVietnamese[] =
    U"aàáảãạăằắẳẵặâầấẩẫậeèéẻẽẹêềếểễệiìíỉĩịoòóỏõọôồốổỗộơờớởỡợuùúủũụưừứửữựyỳýỷỹỵđ"
    U"AÀÁẢÃẠĂẰẮẲẴẶÂẦẤẨẪẬEÈÉẺẼẸÊỀẾỂỄỆIÌÍỈĨỊOÒÓỎÕỌÔỒỐỔỖỘƠỜỚỞỠỢUÙÚỦŨỤƯỪỨỬỮỰYỲÝỶỸỴĐ";

namespace {

// ------------------------------------------------------------------ big-endian helpers
uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
int16_t i16(const uint8_t* p) { return (int16_t)u16(p); }
uint32_t u32(const uint8_t* p) { return (uint32_t)u16(p) << 16 | u16(p + 2); }
void put16(Bytes& b, uint32_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
void put32(Bytes& b, uint32_t v) { put16(b, v >> 16); put16(b, v & 0xFFFF); }
void set16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void set32(uint8_t* p, uint32_t v) { set16(p, v >> 16); set16(p + 2, v & 0xFFFF); }
constexpr uint32_t tag(const char* s) { return (uint32_t)s[0] << 24 | s[1] << 16 | s[2] << 8 | s[3]; }

struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty = true;
    void add(int a, int b, int c, int d) {
        if (empty) { x0 = a; y0 = b; x1 = c; y1 = d; empty = false; return; }
        x0 = std::min(x0, a); y0 = std::min(y0, b); x1 = std::max(x1, c); y1 = std::max(y1, d);
    }
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
};

struct Pt {
    double x = 0, y = 0;
};

// bounds-checked view for parsing tables of an untrusted file
struct View {
    const Bytes* b;
    size_t base, size;
    const uint8_t* at(size_t off, size_t n) const {
        if (off + n > size) throw ParseError("bang font hong");
        return b->data() + base + off;
    }
    uint16_t r16(size_t off) const { return u16(at(off, 2)); }
    int16_t s16(size_t off) const { return i16(at(off, 2)); }
    uint32_t r32(size_t off) const { return u32(at(off, 4)); }
    View sub(size_t off) const {
        if (off > size) throw ParseError("bang font hong");
        return View{b, base + off, size - off};
    }
};

// ------------------------------------------------------------------ the font
struct Comp {
    int gid;
    int dx, dy;
    double scale = 1.0;
};

struct Font {
    Bytes src;
    uint32_t sfnt = 0;
    std::map<uint32_t, Bytes> tables;
    int upm = 1000, numGlyphs = 0;
    std::vector<Bytes> glyphs;
    std::vector<int> adv, lsb;
    std::map<uint32_t, int> cmap;

    bool load(const Bytes& b) {
        if (b.size() < 12) return false;
        sfnt = u32(b.data());
        if (sfnt != 0x00010000 && sfnt != tag("true")) return false;  // CFF ("OTTO") has no composite glyphs
        int n = u16(b.data() + 4);
        if (12 + 16 * (size_t)n > b.size()) return false;
        for (int i = 0; i < n; i++) {
            const uint8_t* r = b.data() + 12 + 16 * i;
            uint32_t off = u32(r + 8), len = u32(r + 12);
            if ((uint64_t)off + len > b.size()) return false;
            tables[u32(r)] = Bytes(b.begin() + off, b.begin() + off + len);
        }
        for (const char* t : {"head", "maxp", "loca", "glyf", "hhea", "hmtx", "cmap"})
            if (!tables.count(tag(t))) return false;
        const Bytes& head = tables[tag("head")];
        const Bytes& maxp = tables[tag("maxp")];
        const Bytes& hhea = tables[tag("hhea")];
        if (head.size() < 54 || maxp.size() < 6 || hhea.size() < 36) return false;
        upm = u16(head.data() + 18);
        bool longLoca = i16(head.data() + 50) != 0;
        numGlyphs = u16(maxp.data() + 4);
        const Bytes& loca = tables[tag("loca")];
        const Bytes& glyf = tables[tag("glyf")];
        if (loca.size() < (size_t)(numGlyphs + 1) * (longLoca ? 4 : 2)) return false;
        for (int i = 0; i < numGlyphs; i++) {
            size_t a = longLoca ? u32(loca.data() + 4 * i) : 2u * u16(loca.data() + 2 * i);
            size_t e = longLoca ? u32(loca.data() + 4 * i + 4) : 2u * u16(loca.data() + 2 * i + 2);
            if (e < a || e > glyf.size()) return false;
            glyphs.emplace_back(glyf.begin() + a, glyf.begin() + e);
        }
        int nh = u16(hhea.data() + 34);
        const Bytes& hmtx = tables[tag("hmtx")];
        if (nh < 1 || nh > numGlyphs || hmtx.size() < 4u * nh + 2u * (numGlyphs - nh)) return false;
        for (int i = 0; i < numGlyphs; i++) {
            if (i < nh) {
                adv.push_back(u16(hmtx.data() + 4 * i));
                lsb.push_back(i16(hmtx.data() + 4 * i + 2));
            } else {
                adv.push_back(adv[nh - 1]);
                lsb.push_back(i16(hmtx.data() + 4 * nh + 2 * (i - nh)));
            }
        }
        load_cmap();
        return !cmap.empty();
    }

    void load_cmap() {
        const Bytes& t = tables[tag("cmap")];
        View v{&t, 0, t.size()};
        int n = v.r16(2);
        for (int i = 0; i < n; i++) {
            int plat = v.r16(4 + 8 * i), enc = v.r16(6 + 8 * i);
            if (!(plat == 0 || (plat == 3 && (enc == 1 || enc == 10)))) continue;
            View s = v.sub(v.r32(8 + 8 * i));
            int fmt = s.r16(0);
            if (fmt == 4) {
                int segs = s.r16(6) / 2;
                size_t ends = 14, starts = 16 + 2 * segs, deltas = starts + 2 * segs, ranges = deltas + 2 * segs;
                for (int k = 0; k < segs; k++) {
                    uint32_t st = s.r16(starts + 2 * k), en = s.r16(ends + 2 * k);
                    int delta = s.r16(deltas + 2 * k), ro = s.r16(ranges + 2 * k);
                    for (uint32_t c = st; c <= en && c != 0xFFFF; c++) {
                        int g = ro ? s.r16(ranges + 2 * k + ro + 2 * (c - st)) : (int)c;
                        if (ro && !g) continue;
                        g = (g + delta) & 0xFFFF;
                        if (g && g < numGlyphs) cmap.emplace(c, g);
                    }
                }
            } else if (fmt == 12) {
                uint32_t groups = s.r32(12);
                for (uint32_t k = 0; k < groups; k++) {
                    uint32_t st = s.r32(16 + 12 * k), en = s.r32(20 + 12 * k), g = s.r32(24 + 12 * k);
                    for (uint32_t c = st; c <= en && c - st < 0x10000; c++)
                        if (g + (c - st) < (uint32_t)numGlyphs) cmap.emplace(c, (int)(g + (c - st)));
                }
            }
        }
    }

    int gid(uint32_t cp) const {
        auto it = cmap.find(cp);
        return it == cmap.end() ? -1 : it->second;
    }

    Box box(int g) const {
        Box b;
        if (g < 0 || g >= (int)glyphs.size() || glyphs[g].size() < 10) return b;
        const uint8_t* p = glyphs[g].data();
        b.add(i16(p + 2), i16(p + 4), i16(p + 6), i16(p + 8));
        return b;
    }

    // points / contours of a glyph, components flattened (for maxp)
    void stats(int g, int depth, int& points, int& contours, int& maxDepth) const {
        maxDepth = std::max(maxDepth, depth);
        if (g < 0 || g >= (int)glyphs.size() || glyphs[g].size() < 10 || depth > 16) return;
        const Bytes& d = glyphs[g];
        int nc = i16(d.data());
        if (nc >= 0) {
            if (nc > 0 && d.size() >= 10u + 2 * nc) points += u16(d.data() + 10 + 2 * (nc - 1)) + 1;
            contours += nc;
            return;
        }
        size_t p = 10;
        while (p + 4 <= d.size()) {
            int flags = u16(d.data() + p), sub = u16(d.data() + p + 2);
            stats(sub, depth + 1, points, contours, maxDepth);
            p += 4 + ((flags & 1) ? 4 : 2) + ((flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0);
            if (!(flags & 0x20)) break;
        }
    }

    int add_glyph(Bytes data, int advance, int leftBearing) {
        glyphs.push_back(std::move(data));
        adv.push_back(advance);
        lsb.push_back(leftBearing);
        return (int)glyphs.size() - 1;
    }

    // composite glyph; returns its bounding box through `bb`
    Bytes composite(const std::vector<Comp>& cs, bool useMetrics, Box& bb) const {
        Bytes out;
        for (auto& c : cs) {
            Box b = box(c.gid);
            if (!b.empty)
                bb.add((int)std::floor(b.x0 * c.scale) + c.dx, (int)std::floor(b.y0 * c.scale) + c.dy,
                       (int)std::ceil(b.x1 * c.scale) + c.dx, (int)std::ceil(b.y1 * c.scale) + c.dy);
        }
        put16(out, 0xFFFF);
        put16(out, (uint16_t)bb.x0);
        put16(out, (uint16_t)bb.y0);
        put16(out, (uint16_t)bb.x1);
        put16(out, (uint16_t)bb.y1);
        for (size_t i = 0; i < cs.size(); i++) {
            const Comp& c = cs[i];
            bool scaled = std::fabs(c.scale - 1.0) > 1e-6;
            uint16_t flags = 0x1 | 0x2 | 0x4;  // word args, x/y offsets, round to grid
            if (scaled) flags |= 0x8 | 0x1000;  // scale; offsets are not scaled
            if (i + 1 < cs.size()) flags |= 0x20;
            if (i == 0 && useMetrics) flags |= 0x200;
            put16(out, flags);
            put16(out, (uint16_t)c.gid);
            put16(out, (uint16_t)(int16_t)c.dx);
            put16(out, (uint16_t)(int16_t)c.dy);
            if (scaled) put16(out, (uint16_t)(int16_t)std::lround(c.scale * 16384));
        }
        return out;
    }

    Bytes save() const;
};

// simple glyph from integer contours (on-curve flag per point), no instructions
Bytes simple_glyph(const std::vector<std::vector<std::pair<Pt, bool>>>& contours, Box& bb) {
    std::vector<int> xs, ys, ends;
    std::vector<uint8_t> on;
    for (auto& c : contours) {
        for (auto& [p, o] : c) {
            xs.push_back((int)std::lround(p.x));
            ys.push_back((int)std::lround(p.y));
            on.push_back(o);
            bb.add(xs.back(), ys.back(), xs.back(), ys.back());
        }
        ends.push_back((int)xs.size() - 1);
    }
    Bytes out;
    put16(out, (uint16_t)ends.size());
    put16(out, (uint16_t)bb.x0);
    put16(out, (uint16_t)bb.y0);
    put16(out, (uint16_t)bb.x1);
    put16(out, (uint16_t)bb.y1);
    for (int e : ends) put16(out, (uint16_t)e);
    put16(out, 0);  // instructions
    Bytes flags, xb, yb;
    int px = 0, py = 0;
    for (size_t i = 0; i < xs.size(); i++) {
        uint8_t f = on[i] ? 1 : 0;
        int dx = xs[i] - px, dy = ys[i] - py;
        if (dx == 0) f |= 0x10;
        else if (std::abs(dx) < 256) { f |= 0x02 | (dx > 0 ? 0x10 : 0); xb.push_back((uint8_t)std::abs(dx)); }
        else put16(xb, (uint16_t)(int16_t)dx);
        if (dy == 0) f |= 0x20;
        else if (std::abs(dy) < 256) { f |= 0x04 | (dy > 0 ? 0x20 : 0); yb.push_back((uint8_t)std::abs(dy)); }
        else put16(yb, (uint16_t)(int16_t)dy);
        flags.push_back(f);
        px = xs[i];
        py = ys[i];
    }
    out.insert(out.end(), flags.begin(), flags.end());
    out.insert(out.end(), xb.begin(), xb.end());
    out.insert(out.end(), yb.begin(), yb.end());
    return out;
}

uint32_t checksum(const Bytes& b) {
    uint32_t s = 0;
    for (size_t i = 0; i < b.size(); i += 4) {
        uint32_t v = 0;
        for (size_t k = 0; k < 4; k++) v = v << 8 | (i + k < b.size() ? b[i + k] : 0);
        s += v;
    }
    return s;
}

Bytes cmap_table(const std::map<uint32_t, int>& map) {
    // format 4 (BMP): runs of consecutive code points with consecutive glyphs, idDelta only
    std::vector<std::array<uint32_t, 3>> segs;  // start, end, gid of start
    std::vector<std::array<uint32_t, 3>> groups;
    for (auto& [cp, g] : map) {
        auto& v = cp <= 0xFFFF ? segs : groups;
        if (cp == 0xFFFF) continue;
        if (!v.empty() && v.back()[1] + 1 == cp && v.back()[2] + (cp - v.back()[0]) == (uint32_t)g) v.back()[1] = cp;
        else v.push_back({cp, cp, (uint32_t)g});
    }
    Bytes f4;
    size_t segCount = segs.size() + 1;
    int pow2 = 1, sel = 0;
    while (pow2 * 2 <= (int)segCount) { pow2 *= 2; sel++; }
    put16(f4, 4);
    put16(f4, (uint16_t)(16 + 8 * segCount));
    put16(f4, 0);
    put16(f4, (uint16_t)(2 * segCount));
    put16(f4, (uint16_t)(2 * pow2));
    put16(f4, (uint16_t)sel);
    put16(f4, (uint16_t)(2 * segCount - 2 * pow2));
    for (auto& s : segs) put16(f4, s[1]);
    put16(f4, 0xFFFF);
    put16(f4, 0);
    for (auto& s : segs) put16(f4, s[0]);
    put16(f4, 0xFFFF);
    for (auto& s : segs) put16(f4, (s[2] - s[0]) & 0xFFFF);
    put16(f4, 1);
    for (size_t i = 0; i < segCount; i++) put16(f4, 0);
    Bytes f12;
    if (!segs.empty() || !groups.empty()) {
        std::vector<std::array<uint32_t, 3>> all = segs;
        all.insert(all.end(), groups.begin(), groups.end());
        put16(f12, 12);
        put16(f12, 0);
        put32(f12, (uint32_t)(16 + 12 * all.size()));
        put32(f12, 0);
        put32(f12, (uint32_t)all.size());
        for (auto& gr : all) { put32(f12, gr[0]); put32(f12, gr[1]); put32(f12, gr[2]); }
    }
    bool wide = !groups.empty();
    Bytes out;
    int n = wide ? 4 : 2;
    put16(out, 0);
    put16(out, (uint16_t)n);
    uint32_t off4 = 4 + 8 * n, off12 = off4 + (uint32_t)f4.size();
    auto rec = [&](int p, int e, uint32_t off) { put16(out, p); put16(out, e); put32(out, off); };
    rec(0, 3, off4);
    if (wide) rec(0, 4, off12);
    rec(3, 1, off4);
    if (wide) rec(3, 10, off12);
    out.insert(out.end(), f4.begin(), f4.end());
    if (wide) out.insert(out.end(), f12.begin(), f12.end());
    return out;
}

Bytes Font::save() const {
    std::map<uint32_t, Bytes> t = tables;
    for (const char* drop : {"hdmx", "LTSH", "VDMX", "DSIG", "vhea", "vmtx"}) t.erase(tag(drop));
    int n = (int)glyphs.size();
    // glyf + loca (short offsets when they fit)
    Bytes glyf;
    std::vector<uint32_t> offs;
    for (auto& g : glyphs) {
        offs.push_back((uint32_t)glyf.size());
        glyf.insert(glyf.end(), g.begin(), g.end());
        while (glyf.size() % 4) glyf.push_back(0);
    }
    offs.push_back((uint32_t)glyf.size());
    bool longLoca = glyf.size() > 0x1FFFC;
    Bytes loca;
    for (uint32_t o : offs) longLoca ? put32(loca, o) : put16(loca, o / 2);
    t[tag("glyf")] = glyf;
    t[tag("loca")] = loca;
    Bytes& head = t[tag("head")];
    set16(head.data() + 50, longLoca ? 1 : 0);
    Box all;
    for (int i = 0; i < n; i++) {
        Box b = box(i);
        if (!b.empty) all.add(b.x0, b.y0, b.x1, b.y1);
    }
    set16(head.data() + 36, (uint16_t)all.x0);
    set16(head.data() + 38, (uint16_t)all.y0);
    set16(head.data() + 40, (uint16_t)all.x1);
    set16(head.data() + 42, (uint16_t)all.y1);
    // hmtx: full metrics for every glyph
    Bytes hmtx;
    int maxAdv = 0;
    for (int i = 0; i < n; i++) {
        put16(hmtx, (uint16_t)adv[i]);
        put16(hmtx, (uint16_t)(int16_t)lsb[i]);
        maxAdv = std::max(maxAdv, adv[i]);
    }
    t[tag("hmtx")] = hmtx;
    Bytes& hhea = t[tag("hhea")];
    set16(hhea.data() + 10, (uint16_t)maxAdv);
    set16(hhea.data() + 34, (uint16_t)n);
    set16(t[tag("maxp")].data() + 4, (uint16_t)n);
    t[tag("cmap")] = cmap_table(cmap);
    if (t.count(tag("post")) && t[tag("post")].size() >= 32) {  // glyph names would need one per glyph
        Bytes& post = t[tag("post")];
        post.resize(32);
        set32(post.data(), 0x00030000);
    }
    // assemble
    set32(head.data() + 8, 0);
    int nt = (int)t.size(), pow2 = 1, sel = 0;
    while (pow2 * 2 <= nt) { pow2 *= 2; sel++; }
    Bytes out;
    put32(out, sfnt);
    put16(out, (uint16_t)nt);
    put16(out, (uint16_t)(16 * pow2));
    put16(out, (uint16_t)sel);
    put16(out, (uint16_t)(16 * nt - 16 * pow2));
    uint32_t off = 12 + 16 * nt;
    for (auto& [tg, data] : t) {
        put32(out, tg);
        put32(out, checksum(data));
        put32(out, off);
        put32(out, (uint32_t)data.size());
        off += (uint32_t)((data.size() + 3) / 4 * 4);
    }
    size_t headAt = 0;
    for (auto& [tg, data] : t) {
        if (tg == tag("head")) headAt = out.size();
        out.insert(out.end(), data.begin(), data.end());
        while (out.size() % 4) out.push_back(0);
    }
    set32(out.data() + headAt + 8, 0xB1B0AFBA - checksum(out));
    return out;
}

// ------------------------------------------------------------------ GPOS mark-to-base anchors
struct MarkBase {
    std::map<int, std::pair<int, Pt>> marks;      // mark glyph -> class, anchor
    std::map<int, std::vector<std::optional<Pt>>> bases;  // base glyph -> anchor per class
};

std::vector<int> coverage(const View& v) {
    std::vector<int> out;
    int fmt = v.r16(0);
    if (fmt == 1) {
        int n = v.r16(2);
        for (int i = 0; i < n; i++) out.push_back(v.r16(4 + 2 * i));
    } else if (fmt == 2) {
        int n = v.r16(2);
        for (int i = 0; i < n; i++) {
            int st = v.r16(4 + 6 * i), en = v.r16(6 + 6 * i), idx = v.r16(8 + 6 * i);
            for (int g = st; g <= en; g++) {
                if (idx + (g - st) >= (int)out.size()) out.resize(idx + (g - st) + 1, -1);
                out[idx + (g - st)] = g;
            }
        }
    }
    return out;
}

Pt anchor(const View& v) { return Pt{(double)v.s16(2), (double)v.s16(4)}; }

std::vector<MarkBase> read_gpos(const Font& f) {
    std::vector<MarkBase> out;
    auto it = f.tables.find(tag("GPOS"));
    if (it == f.tables.end()) return out;
    try {
        View g{&it->second, 0, it->second.size()};
        View ll = g.sub(g.r16(8));
        int nl = ll.r16(0);
        for (int i = 0; i < nl; i++) {
            View lk = ll.sub(ll.r16(2 + 2 * i));
            int type = lk.r16(0), ns = lk.r16(4);
            for (int s = 0; s < ns; s++) {
                View st = lk.sub(lk.r16(6 + 2 * s));
                int t = type;
                if (type == 9) {
                    t = st.r16(2);
                    st = st.sub(st.r32(4));
                }
                if (t != 4 || st.r16(0) != 1) continue;
                MarkBase mb;
                auto mc = coverage(st.sub(st.r16(2)));
                auto bc = coverage(st.sub(st.r16(4)));
                int classes = st.r16(6);
                View ma = st.sub(st.r16(8)), ba = st.sub(st.r16(10));
                for (size_t k = 0; k < mc.size() && k < ma.r16(0); k++)
                    if (mc[k] >= 0)
                        mb.marks[mc[k]] = {ma.r16(2 + 4 * k), anchor(ma.sub(ma.r16(4 + 4 * k)))};
                for (size_t k = 0; k < bc.size() && k < ba.r16(0); k++) {
                    if (bc[k] < 0) continue;
                    std::vector<std::optional<Pt>> a(classes);
                    for (int c = 0; c < classes; c++) {
                        int o = ba.r16(2 + 2 * (k * classes + c));
                        if (o) a[c] = anchor(ba.sub(o));
                    }
                    mb.bases[bc[k]] = a;
                }
                out.push_back(std::move(mb));
            }
        }
    } catch (const std::exception&) {
        out.clear();
    }
    return out;
}

// ------------------------------------------------------------------ marks traced from a Windows font
using Contours = std::vector<std::vector<std::pair<Pt, bool>>>;

struct Donor {
    HDC dc = nullptr;
    HFONT font = nullptr, old = nullptr;
    std::wstring family;

    ~Donor() { close(); }
    void close() {
        if (dc && old) SelectObject(dc, old);
        if (font) DeleteObject(font);
        if (dc) DeleteDC(dc);
        dc = nullptr;
        font = old = nullptr;
    }
    bool open(const std::wstring& fam, bool bold) {
        close();
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        auto make = [&](int h) {
            return CreateFontW(-h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                               CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, fam.c_str());
        };
        font = make(1000);
        old = (HFONT)SelectObject(dc, font);
        wchar_t face[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, face);
        if (_wcsicmp(face, fam.c_str()) != 0) return false;  // the font mapper picked something else
        OUTLINETEXTMETRICW otm{};
        otm.otmSize = sizeof otm;
        if (!GetOutlineTextMetricsW(dc, sizeof otm, &otm)) return false;
        SelectObject(dc, old);
        DeleteObject(font);
        font = make((int)otm.otmEMSquare);  // 1 unit = 1 font design unit
        old = (HFONT)SelectObject(dc, font);
        family = fam;
        return true;
    }
    bool has(wchar_t c) {
        WORD idx = 0xFFFF;
        return GetGlyphIndicesW(dc, &c, 1, &idx, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && idx != 0xFFFF;
    }
    bool outline(wchar_t c, Contours& out, Box& bb) {
        out.clear();
        if (!has(c)) return false;
        GLYPHMETRICS gm{};
        MAT2 m{{0, 1}, {0, 0}, {0, 0}, {0, 1}};
        DWORD n = GetGlyphOutlineW(dc, c, GGO_NATIVE | GGO_UNHINTED, &gm, 0, nullptr, &m);
        if (n == GDI_ERROR || n == 0) return false;
        std::vector<uint8_t> buf(n);
        if (GetGlyphOutlineW(dc, c, GGO_NATIVE | GGO_UNHINTED, &gm, n, buf.data(), &m) == GDI_ERROR) return false;
        auto fx = [](const POINTFX& p) { return Pt{p.x.value + p.x.fract / 65536.0, p.y.value + p.y.fract / 65536.0}; };
        size_t at = 0;
        while (at + sizeof(TTPOLYGONHEADER) <= n) {
            auto* h = (TTPOLYGONHEADER*)(buf.data() + at);
            if (h->cb < sizeof(TTPOLYGONHEADER) || at + h->cb > n) break;
            std::vector<std::pair<Pt, bool>> c1{{fx(h->pfxStart), true}};
            size_t q = at + sizeof(TTPOLYGONHEADER);
            while (q < at + h->cb) {
                auto* cv = (TTPOLYCURVE*)(buf.data() + q);
                if (cv->wType == TT_PRIM_LINE)
                    for (int i = 0; i < cv->cpfx; i++) c1.push_back({fx(cv->apfx[i]), true});
                else if (cv->wType == TT_PRIM_QSPLINE)
                    for (int i = 0; i < cv->cpfx; i++) c1.push_back({fx(cv->apfx[i]), i == cv->cpfx - 1});
                else  // cubic (CFF font): flattened
                    for (int i = 0; i + 2 < cv->cpfx; i += 3) {
                        Pt p0 = c1.back().first, p1 = fx(cv->apfx[i]), p2 = fx(cv->apfx[i + 1]), p3 = fx(cv->apfx[i + 2]);
                        for (int k = 1; k <= 8; k++) {
                            double t = k / 8.0, u = 1 - t;
                            c1.push_back({{u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
                                           u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y},
                                          true});
                        }
                    }
                q += sizeof(WORD) * 2 + sizeof(POINTFX) * cv->cpfx;
            }
            if (c1.size() > 1 && c1.back().second && std::fabs(c1.back().first.x - c1[0].first.x) < 1e-3 &&
                std::fabs(c1.back().first.y - c1[0].first.y) < 1e-3)
                c1.pop_back();
            out.push_back(std::move(c1));
            at += h->cb;
        }
        for (auto& c2 : out)
            for (auto& [p, on] : c2) bb.add((int)std::floor(p.x), (int)std::floor(p.y), (int)std::ceil(p.x), (int)std::ceil(p.y));
        return !out.empty();
    }
};

// contours of a simple glyph (false for a composite / empty one)
bool decode_simple(const Bytes& d, Contours& out) {
    out.clear();
    if (d.size() < 12 || i16(d.data()) <= 0) return false;
    int nc = i16(d.data());
    size_t p = 10;
    std::vector<int> ends;
    for (int i = 0; i < nc; i++, p += 2) {
        if (p + 2 > d.size()) return false;
        ends.push_back(u16(d.data() + p));
    }
    if (p + 2 > d.size()) return false;
    p += 2 + u16(d.data() + p);  // instructions
    size_t n = (size_t)ends.back() + 1;
    std::vector<uint8_t> flags;
    while (flags.size() < n) {
        if (p >= d.size()) return false;
        uint8_t f = d[p++];
        flags.push_back(f);
        if (f & 8) {
            if (p >= d.size()) return false;
            for (int r = d[p++]; r > 0 && flags.size() < n; r--) flags.push_back(f);
        }
    }
    std::vector<int> xs(n), ys(n);
    for (int axis = 0; axis < 2; axis++) {
        uint8_t shortBit = axis ? 4 : 2, sameBit = axis ? 0x20 : 0x10;
        int v = 0;
        for (size_t i = 0; i < n; i++) {
            if (flags[i] & shortBit) {
                if (p >= d.size()) return false;
                v += (flags[i] & sameBit) ? d[p] : -d[p];
                p++;
            } else if (!(flags[i] & sameBit)) {
                if (p + 2 > d.size()) return false;
                v += i16(d.data() + p);
                p += 2;
            }
            (axis ? ys : xs)[i] = v;
        }
    }
    size_t st = 0;
    for (int e : ends) {
        std::vector<std::pair<Pt, bool>> c;
        for (size_t i = st; i <= (size_t)e && i < n; i++) c.push_back({Pt{(double)xs[i], (double)ys[i]}, (flags[i] & 1) != 0});
        out.push_back(std::move(c));
        st = (size_t)e + 1;
    }
    return true;
}

Contours transform(const Contours& c, double s, double dx, double dy) {
    Contours out = c;
    for (auto& c2 : out)
        for (auto& [p, on] : c2) p = Pt{p.x * s + dx, p.y * s + dy};
    return out;
}

// ------------------------------------------------------------------ the Vietnamese letters
// base letter, modifier (^ circumflex, ( breve, + horn, - stroke of đ), tone (` grave, ' acute, ~ tilde, ? hook, . dot below)
struct Letter {
    uint32_t cp;
    char base, mod, tone;
};

std::vector<Letter> letters() {
    static const char* block[45] = {"A.", "A?", "A^'", "A^`", "A^?", "A^~", "A^.", "A('", "A(`", "A(?", "A(~", "A(.",
                                    "E.", "E?", "E~", "E^'", "E^`", "E^?", "E^~", "E^.", "I?", "I.", "O.", "O?",
                                    "O^'", "O^`", "O^?", "O^~", "O^.", "O+'", "O+`", "O+?", "O+~", "O+.", "U.", "U?",
                                    "U+'", "U+`", "U+?", "U+~", "U+.", "Y`", "Y.", "Y?", "Y~"};
    std::vector<std::pair<uint32_t, const char*>> spec;
    for (int i = 0; i < 45; i++) spec.push_back({0x1EA0u + 2 * i, block[i]});
    for (auto& e : std::vector<std::pair<uint32_t, const char*>>{
             {0xC0, "A`"}, {0xC1, "A'"}, {0xC2, "A^"}, {0xC3, "A~"}, {0xC8, "E`"}, {0xC9, "E'"}, {0xCA, "E^"},
             {0xCC, "I`"}, {0xCD, "I'"}, {0xD2, "O`"}, {0xD3, "O'"}, {0xD4, "O^"}, {0xD5, "O~"}, {0xD9, "U`"},
             {0xDA, "U'"}, {0xDD, "Y'"}, {0x102, "A("}, {0x128, "I~"}, {0x168, "U~"}, {0x1A0, "O+"}, {0x1AF, "U+"},
             {0x110, "D-"}})
        spec.push_back(e);
    std::vector<Letter> out;
    for (auto& [cp, s] : spec) {
        Letter l{cp, s[0], 0, 0};
        for (const char* p = s + 1; *p; p++) (std::strchr("^(+-", *p) ? l.mod : l.tone) = *p;
        out.push_back(l);
        Letter lo = l;
        lo.cp = cp < 0x100 ? cp + 0x20 : cp + 1;
        lo.base = (char)(l.base + 32);
        out.push_back(lo);
    }
    return out;
}

const uint32_t kMarkCp[] = {0x300, 0x301, 0x302, 0x303, 0x306, 0x309, 0x31B, 0x323};
uint32_t mark_cp(char c) {
    switch (c) {
        case '`': return 0x300;
        case '\'': return 0x301;
        case '^': return 0x302;
        case '~': return 0x303;
        case '(': return 0x306;
        case '?': return 0x309;
        case '+': return 0x31B;
        default: return 0x323;
    }
}

// a z A Z: also ASCII-only fonts (Interact With Me's Wobbly has 97 characters, no accent at all - every
// mark then comes from the Windows donor font)
bool latin_text_font(const Font& f) {
    if (f.cmap.size() > 3000) return false;  // CJK
    for (uint32_t c : {0x61u, 0x7Au, 0x41u, 0x5Au})
        if (f.gid(c) < 0) return false;
    return true;
}

}  // namespace

int ttf_missing_vietnamese(const Bytes& ttf) {
    Font f;
    if (!f.load(ttf) || !latin_text_font(f)) return -1;
    int n = 0;
    for (auto& l : letters()) n += f.gid(l.cp) < 0;
    return n;
}

bool ttf_add_vietnamese(Bytes& ttf, const std::function<void(const std::string&)>& log) {
    Font f;
    if (!f.load(ttf) || !latin_text_font(f)) return false;
    std::vector<Letter> todo;
    for (auto& l : letters())
        if (f.gid(l.cp) < 0) todo.push_back(l);
    if (todo.empty()) return false;
    const int upm = f.upm;
    const int origGlyphs = (int)f.glyphs.size();

    auto heightOf = [&](uint32_t cp) { return f.box(f.gid(cp)).y1; };
    int xH = heightOf('x'), capH = heightOf('H');
    // OS/2 heights (no overshoot) only when they agree with the glyphs: Wobbly claims an x-height of 792
    // for an 'x' 440 units tall, which put every mark a line too high
    auto agrees = [](int v, int glyph) { return v > 0 && (glyph <= 0 || std::abs(v - glyph) * 100 <= glyph * 15); };
    if (auto os2 = f.tables.find(tag("OS/2")); os2 != f.tables.end() && os2->second.size() >= 90 && u16(os2->second.data()) >= 2) {
        if (int v = i16(os2->second.data() + 86); agrees(v, xH)) xH = v;
        if (int v = i16(os2->second.data() + 88); agrees(v, capH)) capH = v;
    }
    // the donor's Regular marks look thin next to a Regular text face (Kreon): Bold from weight 400 up
    bool bold = true;
    if (auto os2 = f.tables.find(tag("OS/2")); os2 != f.tables.end() && os2->second.size() >= 6)
        bold = u16(os2->second.data() + 4) >= 400;

    // ---- anchors
    auto gpos = read_gpos(f);
    int topRef = f.gid(0x301) >= 0 ? f.gid(0x301) : f.gid(0x300), botRef = f.gid(0x323);
    auto gposAnchor = [&](int base, int refMark) -> std::optional<Pt> {
        for (auto& mb : gpos) {
            auto m = mb.marks.find(refMark);
            auto b = mb.bases.find(base);
            if (m == mb.marks.end() || b == mb.bases.end()) continue;
            if (m->second.first < (int)b->second.size() && b->second[m->second.first]) return b->second[m->second.first];
        }
        return std::nullopt;
    };
    auto topAnchor = [&](int base, bool upper) {
        if (auto a = gposAnchor(base, topRef)) return *a;
        Box b = f.box(base);
        return Pt{(b.x0 + b.x1) / 2.0, (double)(upper ? capH : xH)};
    };
    auto bottomAnchor = [&](int base) {
        if (auto a = gposAnchor(base, botRef)) return *a;
        Box b = f.box(base);
        if (b.y0 < -upm / 10) return Pt{b.x1 - b.w() / 4.0, 0};  // descender (y): beside the tail
        return Pt{(b.x0 + b.x1) / 2.0, 0};
    };

    // ---- marks: placed at an anchor point P as component (gid, P + offset, scale)
    struct Mark {
        int gid = -1;
        Pt off;
        double scale = 1;
        Box box;  // as placed at anchor (0,0)
    };
    std::map<uint32_t, Mark> marks;
    auto sane = [&](const Box& b) { return !b.empty && b.w() >= upm / 30 && b.h() >= upm / 30; };
    int aboveGap = upm / 20;  // anchor -> bottom of an above mark
    int acuteH = 0;
    for (uint32_t cp : kMarkCp) {
        int g = f.gid(cp);
        Box b = f.box(g);
        if (g < 0 || !sane(b)) continue;
        Mark m;
        m.gid = g;
        std::optional<Pt> a;
        for (auto& mb : gpos)
            if (auto it = mb.marks.find(g); it != mb.marks.end()) { a = it->second.second; break; }
        bool below = cp == 0x323;
        Pt anc = a ? *a : Pt{(b.x0 + b.x1) / 2.0, below ? 0.0 : (double)xH};
        m.off = Pt{-anc.x, -anc.y};
        m.box = Box{b.x0 - (int)anc.x, b.y0 - (int)anc.y, b.x1 - (int)anc.x, b.y1 - (int)anc.y, false};
        marks[cp] = m;
        if (cp == 0x301) {
            aboveGap = m.box.y0;
            acuteH = m.box.h();
        }
    }
    std::vector<std::string> made;
    // dot below: the font's own period, scaled down
    if (!marks.count(0x323) && f.gid('.') >= 0) {
        Box p = f.box(f.gid('.'));
        Mark m;
        m.gid = f.gid('.');
        m.scale = 0.8;
        double top = -upm * 0.07;
        m.off = Pt{-(p.x0 + p.x1) / 2.0 * m.scale, top - p.y1 * m.scale};
        m.box = Box{(int)(p.x0 * m.scale + m.off.x), (int)(p.y0 * m.scale + m.off.y), (int)(p.x1 * m.scale + m.off.x),
                    (int)top, false};
        marks[0x323] = m;
        made.push_back("cham duoi = dau cham cau");
    }
    // the rest from a Windows font with Vietnamese
    std::vector<uint32_t> need;
    for (uint32_t cp : kMarkCp)
        if (!marks.count(cp)) need.push_back(cp);
    std::map<char, std::pair<double, double>> hornAt;  // base letter -> horn (left, bottom) - (base right, base top)
    double hornScale = 1;
    if (!need.empty()) {
        Donor d;
        bool ok = false;
        for (const wchar_t* fam : {L"Cambria", L"Constantia", L"Times New Roman", L"Arial"})
            if (d.open(fam, bold) && d.has(0x309) && d.has(0x31B)) {
                ok = true;
                break;
            }
        if (!ok) {
            log("    ! khong tim thay font Windows co dau mu/rau tieng Viet (Cambria, Times New Roman...)");
            return false;
        }
        Contours cx;
        Box bx;
        d.outline(L'x', cx, bx);
        hornScale = bx.y1 > 0 ? (double)xH / bx.y1 : (double)upm / 2048;
        double s = hornScale;
        for (uint32_t cp : need) {
            Contours c;
            Box b;
            if (!d.outline((wchar_t)cp, c, b)) continue;
            Mark m;
            Box gb;
            if (cp == 0x31B) {  // horn: left-bottom corner at the origin, placed by hand below
                m.gid = f.add_glyph(simple_glyph(transform(c, s, -b.x0 * s, -b.y0 * s), gb), 0, 0);
            } else if (cp == 0x323) {  // below: centred, top a little under the baseline
                m.gid = f.add_glyph(simple_glyph(transform(c, s, -(b.x0 + b.x1) / 2.0 * s, -upm * 0.07 - b.y1 * s), gb), 0, 0);
            } else {  // above: centred on the anchor, bottom where the font's acute starts, as tall as it
                double sa = acuteH > 0 && b.h() > 0 ? acuteH * 0.95 / b.h() : s;
                m.gid = f.add_glyph(simple_glyph(transform(c, sa, -(b.x0 + b.x1) / 2.0 * sa, aboveGap - b.y0 * sa), gb), 0, 0);
            }
            f.lsb.back() = gb.x0;
            m.box = gb;
            marks[cp] = m;
        }
        // where the donor puts the horn on o/u/O/U, relative to the letter's right edge and top
        Contours hc;
        Box hb;
        d.outline(0x31B, hc, hb);
        for (auto [base, horned] : std::vector<std::pair<char, wchar_t>>{{'o', 0x1A1}, {'O', 0x1A0}, {'u', 0x1B0}, {'U', 0x1AF}}) {
            Contours c;
            Box b0, b1;
            if (d.outline((wchar_t)base, c, b0) && d.outline(horned, c, b1) && !hb.empty)
                hornAt[base] = {(b1.x1 - hb.w() - b0.x1) * s, (b1.y1 - hb.h() - b0.y1) * s};
        }
        std::string fam(d.family.begin(), d.family.end());
        made.push_back("dau mu/rau lay tu " + fam + (bold ? " Bold" : ""));
    }

    // dotless ı (base of í ì ỉ ĩ - an above tone would sit on the dot): 'i' without the contours above x-height
    if (f.gid(0x131) < 0 && f.gid('i') >= 0) {
        Contours ci, keep;
        if (decode_simple(f.glyphs[f.gid('i')], ci)) {
            for (auto& c : ci) {
                double lo = 1e9;
                for (auto& pt : c) lo = std::min(lo, pt.first.y);
                if (lo < xH * 0.8) keep.push_back(c);
            }
            if (!keep.empty() && keep.size() < ci.size()) {
                Box gb;
                int ig = f.gid('i');
                Bytes g = simple_glyph(keep, gb);
                f.cmap[0x131] = f.add_glyph(std::move(g), f.adv[ig], gb.x0);
                made.push_back("i khong cham tu chu i");
            }
        }
    }
    // stroke of đ / Đ: a bar across the stem, as thick as about half of it ('l' gives the stem width)
    int stemW = std::clamp(f.gid('l') >= 0 ? f.box(f.gid('l')).w() : upm / 12, upm / 25, upm / 7);
    int barT = std::clamp(stemW * 11 / 20, upm * 35 / 1000, upm * 8 / 100);
    auto bar = [&](int x0, int x1, int yMid) {
        int y0 = yMid - barT / 2, y1 = y0 + barT;
        Contours c{{{Pt{(double)x0, (double)y0}, true}, {Pt{(double)x0, (double)y1}, true},
                    {Pt{(double)x1, (double)y1}, true}, {Pt{(double)x1, (double)y0}, true}}};
        Box gb;
        Bytes g = simple_glyph(c, gb);
        return f.add_glyph(std::move(g), 0, gb.x0);
    };

    // ---- the letters
    const std::map<std::string, uint32_t> precomposed = {{"a^", 0xE2}, {"A^", 0xC2}, {"e^", 0xEA}, {"E^", 0xCA},
                                                         {"o^", 0xF4}, {"O^", 0xD4}, {"a(", 0x103}, {"A(", 0x102}};
    const int stackGap = upm * 3 / 100;
    int added = 0, maxComps = 0;
    std::vector<std::string> skipped;
    for (auto& l : todo) {
        bool upper = l.base >= 'A' && l.base <= 'Z';
        bool aboveTone = l.tone && l.tone != '.';
        int base = f.gid((uint32_t)(l.base == 'i' && aboveTone ? 0x131 : (uint32_t)l.base));
        if (base < 0) base = f.gid((uint32_t)l.base);
        auto mk = [&](char c) -> const Mark* {
            auto it = marks.find(mark_cp(c));
            return it == marks.end() ? nullptr : &it->second;
        };
        if (base < 0 || (l.mod && l.mod != '-' && !mk(l.mod)) || (l.tone && !mk(l.tone))) {
            skipped.push_back(std::string(1, l.base) + l.mod + l.tone);
            continue;
        }
        Pt top = topAnchor(f.gid((uint32_t)l.base), upper), bot = bottomAnchor(f.gid((uint32_t)l.base));
        if (l.base == 'i') top = topAnchor(base, false);
        auto place = [&](const Mark& m, Pt at) {
            return Comp{m.gid, (int)std::lround(at.x + m.off.x), (int)std::lround(at.y + m.off.y), m.scale};
        };
        std::vector<Comp> cs;
        int advance = f.adv[base];
        Box baseBox = f.box(base);
        if (l.mod == '^' || l.mod == '(') {
            auto pre = precomposed.find(std::string(1, l.base) + l.mod);
            int pg = pre == precomposed.end() ? -1 : f.gid(pre->second);
            if (pg >= 0 && pg < origGlyphs && l.tone) cs.push_back({pg, 0, 0});
            else {
                cs.push_back({base, 0, 0});
                cs.push_back(place(*mk(l.mod), top));
            }
        } else if (l.mod == '+') {
            const Mark& h = *mk('+');
            auto rel = hornAt.count(l.base) ? hornAt[l.base] : std::make_pair(-0.03 * upm, -0.6 * h.box.h());
            int hx = (int)std::lround(baseBox.x1 + rel.first), hy = (int)std::lround(baseBox.y1 + rel.second);
            cs.push_back({base, 0, 0});
            cs.push_back({h.gid, hx, hy});
            advance = std::max(advance, hx + h.box.x1 + (f.adv[base] - baseBox.x1));
        } else if (l.mod == '-') {
            // Đ: across the left stem at half the cap height; đ: across the ascender, between x-height and its top
            int ext = stemW * 9 / 10;
            cs.push_back({base, 0, 0});
            cs.push_back({upper ? bar(baseBox.x0 - ext * 6 / 10, baseBox.x0 + stemW + ext, baseBox.y1 * 48 / 100)
                                : bar(baseBox.x1 - stemW - ext, baseBox.x1 + ext * 4 / 10, (xH + baseBox.y1) / 2),
                          0, 0});
        } else {
            cs.push_back({base, 0, 0});
        }
        if (l.tone) {
            const Mark& m = *mk(l.tone);
            Comp c = place(m, l.tone == '.' ? bot : top);
            if (l.mod == '^' || l.mod == '(') {  // stack the tone above the circumflex / breve
                Box core;
                std::vector<Comp> cur = cs;
                f.composite(cur, false, core);
                int bottom = c.dy + (int)std::floor(f.box(m.gid).y0 * m.scale);
                if (l.tone != '.') c.dy += core.y1 + stackGap - bottom;
            }
            cs.push_back(c);
        }
        Box bb;
        bool useMetrics = advance == f.adv[cs[0].gid];
        Bytes g = f.composite(cs, useMetrics, bb);
        int ng = f.add_glyph(std::move(g), advance, bb.x0);
        f.cmap[l.cp] = ng;
        maxComps = std::max(maxComps, (int)cs.size());
        added++;
    }
    if (!added) return false;

    // maxp (version 1.0): composite limits must cover the new glyphs
    Bytes& maxp = f.tables[tag("maxp")];
    if (maxp.size() >= 32 && u32(maxp.data()) == 0x00010000) {
        int mp = u16(maxp.data() + 6), mc = u16(maxp.data() + 8), mcp = u16(maxp.data() + 10), mcc = u16(maxp.data() + 12);
        int me = u16(maxp.data() + 28), md = u16(maxp.data() + 30);
        for (int g = origGlyphs; g < (int)f.glyphs.size(); g++) {
            int pts = 0, cts = 0, depth = 0;
            f.stats(g, 0, pts, cts, depth);
            if (i16(f.glyphs[g].data()) >= 0) { mp = std::max(mp, pts); mc = std::max(mc, cts); }
            else { mcp = std::max(mcp, pts); mcc = std::max(mcc, cts); md = std::max(md, depth); }
        }
        me = std::max(me, maxComps);
        set16(maxp.data() + 6, mp);
        set16(maxp.data() + 8, mc);
        set16(maxp.data() + 10, mcp);
        set16(maxp.data() + 12, mcc);
        set16(maxp.data() + 28, me);
        set16(maxp.data() + 30, md);
    }
    if (auto os2 = f.tables.find(tag("OS/2")); os2 != f.tables.end() && os2->second.size() >= 82 && u16(os2->second.data()) >= 1)
        set32(os2->second.data() + 78, u32(os2->second.data() + 78) | 1u << 8);  // code page 1258 (Vietnamese)

    ttf = f.save();
    std::string note;
    for (auto& m : made) note += (note.empty() ? "" : ", ") + m;
    log("    + " + std::to_string(added) + " chu tieng Viet" + (note.empty() ? "" : " (" + note + ")") +
        (skipped.empty() ? "" : ", bo qua " + std::to_string(skipped.size())));
    return true;
}
