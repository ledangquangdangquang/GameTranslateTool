#include "unity_font.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <set>

#include "ttf.h"
#include "typetree.h"

namespace {

// ------------------------------------------------------------------ GDI glyph rasterisation
const MAT2 kIdent = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};

HFONT make_font(const wchar_t* family, int em) {
    LOGFONTW lf{};
    lf.lfHeight = -em;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcsncpy(lf.lfFaceName, family, LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

// Black-box height (cap/x-height reference) of `ch` in `family` at a nominal em, via GGO_METRICS.
int ref_height(HDC dc, const wchar_t* family, int em, wchar_t ch) {
    HFONT f = make_font(family, em);
    HGDIOBJ old = SelectObject(dc, f);
    GLYPHMETRICS gm{};
    DWORD r = GetGlyphOutlineW(dc, ch, GGO_METRICS, &gm, 0, nullptr, &kIdent);
    SelectObject(dc, old);
    DeleteObject(f);
    return r == GDI_ERROR ? 0 : (int)gm.gmptGlyphOrigin.y;
}

struct RasterGlyph {
    int bw = 0, bh = 0;         // black-box size, GDI pixels (top-down rows)
    std::vector<uint8_t> cov;   // bw*bh, 0..255 coverage, row 0 = top
    double originX = 0, originY = 0;  // baseline -> top-left of black box, GDI pixels
    double advance = 0;
};

bool rasterize(HDC dc, const wchar_t* family, int em, char32_t cp, RasterGlyph& out) {
    if (cp > 0xFFFF) return false;  // every Vietnamese letter is in the BMP
    HFONT f = make_font(family, em);
    HGDIOBJ old = SelectObject(dc, f);
    WCHAR wc = (WCHAR)cp;
    WORD gi = 0;
    bool has = GetGlyphIndicesW(dc, &wc, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && gi != 0xFFFF;
    if (!has) {
        SelectObject(dc, old);
        DeleteObject(f);
        return false;
    }
    GLYPHMETRICS gm{};
    DWORD sz = GetGlyphOutlineW(dc, wc, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &kIdent);
    if (sz == GDI_ERROR) {
        SelectObject(dc, old);
        DeleteObject(f);
        return false;
    }
    out.advance = gm.gmCellIncX;
    out.originX = gm.gmptGlyphOrigin.x;
    out.originY = gm.gmptGlyphOrigin.y;
    out.bw = (int)gm.gmBlackBoxX;
    out.bh = (int)gm.gmBlackBoxY;
    if (sz > 0 && out.bw > 0 && out.bh > 0) {
        std::vector<uint8_t> buf(sz);
        GetGlyphOutlineW(dc, wc, GGO_GRAY8_BITMAP, &gm, sz, buf.data(), &kIdent);
        int pitch = (out.bw + 3) & ~3;
        out.cov.resize((size_t)out.bw * out.bh);
        for (int y = 0; y < out.bh; y++)
            for (int x = 0; x < out.bw; x++)
                out.cov[(size_t)y * out.bw + x] = (uint8_t)std::min(255, buf[(size_t)y * pitch + x] * 255 / 64);
    } else {
        out.bw = out.bh = 0;
    }
    SelectObject(dc, old);
    DeleteObject(f);
    return true;
}

// ------------------------------------------------------------------ exact 2D Euclidean distance transform
// Felzenszwalb & Huttenlocher's lower-envelope-of-parabolas algorithm: squared distance from every
// index to the nearest zero in `f` (others must be a large sentinel), one dimension at a time.
void edt1d(const std::vector<float>& f, std::vector<float>& d) {
    int n = (int)f.size();
    const float INF = 1e20f;
    std::vector<int> v(n);
    std::vector<float> z(n + 1);
    int k = 0;
    v[0] = 0;
    z[0] = -INF;
    z[1] = INF;
    for (int q = 1; q < n; q++) {
        float s;
        while (true) {
            s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.f * q - 2.f * v[k]);
            if (k > 0 && s <= z[k]) {
                k--;
                continue;
            }
            break;
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = INF;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < q) k++;
        float dx = (float)(q - v[k]);
        d[q] = dx * dx + f[v[k]];
    }
}

// Distance (not squared) from every pixel to the nearest pixel where `seed` is true.
std::vector<float> edt2d(const std::vector<uint8_t>& seed, int w, int h) {
    const float INF = 1e20f;
    std::vector<float> g((size_t)w * h);
    std::vector<float> col(h), cold(h);
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) col[y] = seed[(size_t)y * w + x] ? 0.f : INF;
        edt1d(col, cold);
        for (int y = 0; y < h; y++) g[(size_t)y * w + x] = cold[y];
    }
    std::vector<float> row(w), rowd(w), out((size_t)w * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) row[x] = g[(size_t)y * w + x];
        edt1d(row, rowd);
        for (int x = 0; x < w; x++) out[(size_t)y * w + x] = std::sqrt(rowd[x]);
    }
    return out;
}

// ------------------------------------------------------------------ bake one glyph's SDF cell
struct BakedGlyph {
    char32_t cp = 0;
    int w = 0, h = 0;              // final cell size, atlas pixels
    std::vector<uint8_t> sdf;      // w*h, row 0 = BOTTOM (matches the atlas's own row order)
    double bearingX = 0, bearingY = 0, advance = 0, width = 0, height = 0;  // metrics-space (== atlas px)
};

// `em` is the GDI render size for the supersampled raster; `ss` the supersample factor (em is already
// ss times the final target size). `margin` and `gradientScale` are in final atlas pixels.
bool bake_glyph(HDC dc, const wchar_t* family, char32_t cp, int em, int ss, int margin, double gradientScale,
                BakedGlyph& out) {
    RasterGlyph r;
    if (!rasterize(dc, family, em, cp, r)) return false;
    out.cp = cp;
    out.advance = r.advance / ss;
    if (r.bw == 0 || r.bh == 0) {  // space or combining mark with no ink: metrics only, no atlas cell
        out.w = out.h = 0;
        return true;
    }
    int pad = margin * ss;
    int gw = r.bw + 2 * pad, gh = r.bh + 2 * pad;
    std::vector<uint8_t> mask((size_t)gw * gh, 0);  // 1 = ink
    for (int y = 0; y < r.bh; y++)
        for (int x = 0; x < r.bw; x++)
            if (r.cov[(size_t)y * r.bw + x] >= 128) mask[(size_t)(y + pad) * gw + (x + pad)] = 1;
    std::vector<uint8_t> inv(mask.size());
    for (size_t i = 0; i < mask.size(); i++) inv[i] = mask[i] ? 0 : 1;
    std::vector<float> distToInk = edt2d(mask, gw, gh);     // valid magnitude where mask==0 (outside)
    std::vector<float> distToOut = edt2d(inv, gw, gh);      // valid magnitude where mask==1 (inside)

    // final size = ink + margin both sides, in atlas px; computed from the padded supersampled raster
    // (not r.bw/ss + 2*margin directly) since r.bw may not be an exact multiple of ss.
    out.w = gw / ss;
    out.h = gh / ss;
    out.sdf.assign((size_t)out.w * out.h, 0);
    for (int fy = 0; fy < out.h; fy++) {
        for (int fx = 0; fx < out.w; fx++) {
            double sum = 0;
            int n = 0;
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++) {
                    int gx = fx * ss + sx, gy = fy * ss + sy;
                    if (gx >= gw || gy >= gh) continue;
                    size_t idx = (size_t)gy * gw + gx;
                    double signedPx = mask[idx] ? distToOut[idx] : -distToInk[idx];
                    sum += signedPx / ss;  // GDI-pixel distance -> atlas-pixel distance
                    n++;
                }
            double signedAtlasPx = n ? sum / n : 0;
            double norm = 0.5 + 0.5 * signedAtlasPx / gradientScale;
            norm = std::clamp(norm, 0.0, 1.0);
            // fy counts down from the top of the padded raster; the atlas stores rows bottom-up.
            int rowFromBottom = out.h - 1 - fy;
            out.sdf[(size_t)rowFromBottom * out.w + fx] = (uint8_t)std::lround(norm * 255);
        }
    }
    // metrics: top-left of the black box, offset outward by `margin` to match the grown cell.
    out.bearingX = r.originX / ss - margin;
    out.bearingY = r.originY / ss + margin;
    out.width = out.w;
    out.height = out.h;
    return true;
}

// ------------------------------------------------------------------ shelf packer for the new atlas rows
struct Shelf {
    int y, h;
    int x = 0;
};

// ------------------------------------------------------------------ typetree helpers
std::set<uint32_t> covered_codepoints(const Value& v) {
    std::set<uint32_t> cps;
    auto ct = v.get("m_CharacterTable");
    if (ct)
        for (auto& c : ct->arr) {
            auto u = c.get("m_Unicode");
            if (u) cps.insert((uint32_t)u->u);
        }
    return cps;
}

double find_gradient_scale(SerializedFile& sf, const Value& fontVal) {
    auto mat = fontVal.get("m_Material");
    if (!mat) return 9.0;
    int64_t matPathId = (int64_t)mat->get("m_PathID")->i;
    for (auto& o : sf.objects) {
        if (o.pathId != matPathId) continue;
        auto tree = sf.tree_of(o);
        if (!tree) break;
        try {
            Value mv = tt_read(*tree, sf.object_data(o), sf.bigEndian);
            auto floats = mv.get("m_SavedProperties") ? mv.get("m_SavedProperties")->get("m_Floats") : nullptr;
            if (floats)
                for (auto& kv : floats->arr) {
                    auto first = kv.get("first");
                    if (first && first->s == "_GradientScale") return kv.get("second")->f;
                }
        } catch (const std::exception&) {
        }
        break;
    }
    auto pad = fontVal.get("m_AtlasPadding");
    return pad ? (double)pad->i + 1 : 9.0;
}

BundleNode* find_resS(Bundle& bundle, const std::string& streamPath) {
    size_t sl = streamPath.find_last_of('/');
    std::string resName = sl == std::string::npos ? streamPath : streamPath.substr(sl + 1);
    for (auto& n : bundle.nodes)
        if (n.path == resName) return &n;
    return nullptr;
}

// Pick a Windows-installed, Vietnamese-capable donor by rough style match to the target family name.
const wchar_t* pick_donor(const std::string& family) {
    std::string l = family;
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    if (l.find("mono") != std::string::npos || l.find("code") != std::string::npos) return L"Consolas";
    if (l.find("serif") != std::string::npos || l.find("georgia") != std::string::npos ||
        l.find("times") != std::string::npos || l.find("garamond") != std::string::npos ||
        l.find("bookerly") != std::string::npos || l.find("merriweather") != std::string::npos ||
        l.find("bitter") != std::string::npos)
        return L"Cambria";
    return L"Segoe UI";
}

}  // namespace

int tmp_patch_vietnamese(SerializedFile& sf, Bundle& bundle, const std::function<void(const std::string&)>& log) {
    int patched = 0;
    HDC dc = CreateCompatibleDC(nullptr);
    for (auto& o : sf.objects) {
        if (o.classID != 114) continue;
        auto tree = sf.tree_of(o);
        if (!tree || !tree->child("m_CharacterTable") || !tree->child("m_GlyphTable") ||
            !tree->child("m_AtlasPopulationMode"))
            continue;
        Value v;
        try {
            v = tt_read(*tree, sf.object_data(o), sf.bigEndian);
        } catch (const std::exception&) {
            continue;
        }
        if (v.get("m_AtlasPopulationMode")->i != 0) continue;  // only the "baked once" static mode
        std::string fontName = v.get("m_Name") ? v.get("m_Name")->s : "";
        std::set<uint32_t> have = covered_codepoints(v);
        std::vector<char32_t> need;
        for (const char32_t* p = kVietnamese; *p; p++)
            if (!have.count((uint32_t)*p)) need.push_back(*p);
        if (need.empty()) continue;

        auto atlasTextures = v.get("m_AtlasTextures");
        if (!atlasTextures || atlasTextures->arr.empty()) continue;
        int64_t texPathId = (int64_t)atlasTextures->arr[0].get("m_PathID")->i;
        ObjectInfo* texObj = nullptr;
        for (auto& o2 : sf.objects)
            if (o2.pathId == texPathId) texObj = &o2;
        if (!texObj) continue;
        auto texTree = sf.tree_of(*texObj);
        if (!texTree) continue;
        Value tex;
        try {
            tex = tt_read(*texTree, sf.object_data(*texObj), sf.bigEndian);
        } catch (const std::exception&) {
            continue;
        }
        auto sd = tex.get("m_StreamData");
        if (!sd) continue;
        BundleNode* res = find_resS(bundle, sd->get("path")->s);
        if (!res) continue;
        uint64_t oldOff = sd->get("offset")->u, oldSize = sd->get("size")->u;
        if (oldOff + oldSize > res->data.size()) continue;
        int64_t atlasW = v.get("m_AtlasWidth")->i, atlasHOld = v.get("m_AtlasHeight")->i;
        if ((uint64_t)atlasW * atlasHOld != oldSize) continue;  // not plain 1 byte/px Alpha8 as expected
        auto padNode = v.get("m_AtlasPadding");
        int margin = padNode ? std::max(2, (int)padNode->i) : 8;
        double gradientScale = find_gradient_scale(sf, v);
        double capLine = v.path("m_FaceInfo/m_CapLine") ? v.path("m_FaceInfo/m_CapLine")->f : 0;
        double meanLine = v.path("m_FaceInfo/m_MeanLine") ? v.path("m_FaceInfo/m_MeanLine")->f : 0;
        if (capLine <= 0 || meanLine <= 0) continue;

        const wchar_t* donor = pick_donor(fontName);
        const int ss = 4;
        const int nominalEm = 200;
        int donorCap = ref_height(dc, donor, nominalEm, L'H');
        int donorMean = ref_height(dc, donor, nominalEm, L'x');
        if (donorCap <= 0 || donorMean <= 0) continue;
        int emUpper = (int)std::lround((double)nominalEm * capLine / donorCap * ss);
        int emLower = (int)std::lround((double)nominalEm * meanLine / donorMean * ss);

        std::vector<BakedGlyph> baked;
        for (char32_t cp : need) {
            int em = iswupper((wint_t)cp) ? emUpper : emLower;
            BakedGlyph g;
            if (bake_glyph(dc, donor, cp, em, ss, margin, gradientScale, g)) baked.push_back(std::move(g));
        }
        if (baked.empty()) continue;

        // shelf-pack the new glyphs into rows appended below the existing atlas
        std::sort(baked.begin(), baked.end(), [](auto& a, auto& b) { return a.h > b.h; });
        std::vector<Shelf> shelves;
        int extraH = 0;
        const int gap = 1;
        std::vector<std::pair<int, int>> place(baked.size());  // x, y (y relative to extra region)
        for (size_t i = 0; i < baked.size(); i++) {
            auto& g = baked[i];
            if (g.w == 0 || g.h == 0) {
                place[i] = {0, 0};
                continue;
            }
            Shelf* use = nullptr;
            for (auto& s : shelves)
                if (s.h >= g.h && s.x + g.w + gap <= atlasW) {
                    use = &s;
                    break;
                }
            if (!use) {
                shelves.push_back({extraH + gap, g.h});
                extraH += g.h + gap;
                use = &shelves.back();
            }
            place[i] = {use->x, use->y};
            use->x += g.w + gap;
        }
        extraH += gap;
        int atlasHNew = (int)atlasHOld + extraH;

        // grow the resS node: append a full fresh copy of the texture (old pixels + new rows) at the
        // end of the node, and repoint the Texture2D at it - nothing already in the node moves.
        uint64_t newOff = res->data.size();
        uint64_t newSize = (uint64_t)atlasW * atlasHNew;
        res->data.resize(res->data.size() + newSize, 0);
        std::copy(res->data.begin() + oldOff, res->data.begin() + oldOff + oldSize, res->data.begin() + newOff);

        auto gt = v.get("m_GlyphTable");
        auto ct = v.get("m_CharacterTable");
        auto ur = v.get("m_UsedGlyphRects");
        Value glyphTemplate = gt->arr[0], charTemplate = ct->arr[0], rectTemplate = ur && !ur->arr.empty() ? ur->arr[0] : Value();
        uint64_t maxIdx = 0;
        for (auto& g : gt->arr) maxIdx = std::max(maxIdx, g.get("m_Index")->u);
        int n = 0;
        for (size_t i = 0; i < baked.size(); i++) {
            auto& g = baked[i];
            uint64_t idx = ++maxIdx;
            Value gv = glyphTemplate;
            gv.get("m_Index")->u = idx;
            gv.get("m_Scale")->f = 1;
            gv.get("m_AtlasIndex")->i = 0;
            gv.get("m_ClassDefinitionType")->i = 0;
            auto metrics = gv.get("m_Metrics");
            metrics->get("m_Width")->f = g.width;
            metrics->get("m_Height")->f = g.height;
            metrics->get("m_HorizontalBearingX")->f = g.bearingX;
            metrics->get("m_HorizontalBearingY")->f = g.bearingY;
            metrics->get("m_HorizontalAdvance")->f = g.advance;
            auto rect = gv.get("m_GlyphRect");
            int rx = place[i].first, ry = (int)atlasHOld + place[i].second;
            if (g.w == 0 || g.h == 0) { rx = 0; ry = 0; }
            rect->get("m_X")->i = rx;
            rect->get("m_Y")->i = ry;
            rect->get("m_Width")->i = g.w;
            rect->get("m_Height")->i = g.h;
            gt->arr.push_back(gv);

            Value cv = charTemplate;
            cv.get("m_ElementType")->u = 0;
            cv.get("m_Unicode")->u = (uint32_t)g.cp;
            cv.get("m_GlyphIndex")->u = idx;
            cv.get("m_Scale")->f = 1;
            ct->arr.push_back(cv);

            if (ur && !rectTemplate.obj.empty() && g.w > 0 && g.h > 0) {
                Value rv = rectTemplate;
                rv.get("m_X")->i = rx;
                rv.get("m_Y")->i = ry;
                rv.get("m_Width")->i = g.w;
                rv.get("m_Height")->i = g.h;
                ur->arr.push_back(rv);
            }

            if (g.w > 0 && g.h > 0) {
                for (int yy = 0; yy < g.h; yy++)
                    std::copy(g.sdf.begin() + (size_t)yy * g.w, g.sdf.begin() + (size_t)(yy + 1) * g.w,
                              res->data.begin() + newOff + (size_t)(ry + yy) * atlasW + rx);
            }
            n++;
        }
        v.get("m_AtlasHeight")->i = atlasHNew;
        sd->get("offset")->u = newOff;
        sd->get("size")->u = newSize;
        tex.get("m_Height")->i = atlasHNew;
        tex.get("m_CompleteImageSize")->u = newSize;

        try {
            sf.set_object_data(o.pathId, tt_write(*tree, v, sf.bigEndian));
            sf.set_object_data(texObj->pathId, tt_write(*texTree, tex, sf.bigEndian));
        } catch (const std::exception& ex) {
            log("  ! " + fontName + ": khong ghi lai duoc (" + std::string(ex.what()) + ")");
            continue;
        }
        log("  " + fontName + ": them " + std::to_string(n) + " chu tieng Viet (" + std::to_string(atlasW) + "x" +
            std::to_string(atlasHOld) + " -> " + std::to_string(atlasW) + "x" + std::to_string(atlasHNew) + ")");
        patched++;
    }
    DeleteDC(dc);
    return patched;
}
