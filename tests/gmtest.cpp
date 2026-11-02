// GameMaker checks:
//   gmtest ini <translations.ini>                      -> sections + english count, apply(no-op) round trip
//   gmtest font <data.win> <out.win> [family] [text] [pixel family] -> patch the Latin fonts, write a copy
//   gmtest page <data.win> <index> <out.raw>           -> decode one texture page (u32 w, u32 h, BGRA)
#include <windows.h>

#include <cstdio>

#include "../src/gamemaker.h"

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::string cmd = argv[1];
    auto log = [](const std::string& s) { printf("%s\n", s.c_str()); };
    if (cmd == "ini") {
        std::string t = read_text(u8path(argv[2]));
        for (auto& s : gm_ini_sections(t)) printf("[%s] %zu\n", s.c_str(), gm_ini_read(t, s).size());
        int n = 0;
        std::map<std::string, std::string> same;
        for (auto& kv : gm_ini_read(t, "english")) same[kv.first] = kv.second;
        std::string back = gm_ini_apply(t, "english", same, n);
        printf("apply same: %d changed, identical=%d\n", n, back == t);
        return back == t ? 0 : 1;
    }
    if (cmd == "json") {  // json <localization.json>: sections, and re-writing every value unchanged
        std::string t = read_text(u8path(argv[2]));
        for (auto& s : gm_json_sections(t)) printf("[%s] %zu\n", s.c_str(), gm_json_read(t, s).size());
        auto secs = gm_json_sections(t);
        if (secs.empty()) return 1;
        std::map<std::string, std::string> same;
        for (auto& kv : gm_json_read(t, secs[0])) same[kv.first] = kv.second;
        int n = 0;
        std::string back = gm_json_apply(t, secs[0], same, n);
        size_t diff = 0;
        for (size_t i = 0; i < std::min(back.size(), t.size()); i++) diff += back[i] != t[i];
        printf("apply same: %d values, identical=%d (size %zu -> %zu)\n", n, back == t, t.size(), back.size());
        return back == t ? 0 : 1;
    }
    if (cmd == "page" && argc >= 5) {  // page <data.win> <index> <out.raw>: u32 w, u32 h, BGRA
        Bytes d = read_file(u8path(argv[2]));
        auto u32 = [&](size_t at) { return (uint32_t)(d[at] | d[at + 1] << 8 | d[at + 2] << 16 | (uint32_t)d[at + 3] << 24); };
        size_t pos = 8, txtr = 0;
        while (pos + 8 <= d.size()) {
            if (std::memcmp(&d[pos], "TXTR", 4) == 0) txtr = pos + 8;
            pos += 8 + u32(pos + 4);
        }
        uint32_t e = u32(txtr + 4 + 4 * std::stoul(argv[3]));
        int w = 0, h = 0;
        std::vector<uint8_t> px;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = gm_decode_texture(&d[u32(e + 24)], u32(e + 8), w, h, px);
        printf("decode=%d %dx%d\n", ok, w, h);
        Bytes out(8);
        for (int i = 0; i < 4; i++) out[i] = (uint8_t)(w >> 8 * i), out[4 + i] = (uint8_t)(h >> 8 * i);
        out.insert(out.end(), px.begin(), px.end());
        write_file(u8path(argv[4]), out);
        return ok ? 0 : 1;
    }
    if (cmd == "font" && argc >= 4) {
        Bytes d = read_file(u8path(argv[2]));
        std::string fam = argc > 4 ? argv[4] : "Constantia";
        std::string text = argc > 5 ? argv[5] : "";
        bool ok = gm_patch_fonts(d, utf8_to_u32(text), fam, argc > 6 ? argv[6] : "Tahoma", log);
        printf("patched=%d\n", ok);
        if (ok) write_file(u8path(argv[3]), d);
        return ok ? 0 : 1;
    }
    return 2;
}
