// Ren'Py checks on a real game (only <game>/_unitytl_renpy_tmp is created, then removed):
//   renpytest <game folder> <scratch dir>
// 1) every .rpa index parses (safe pickle reader)
// 2) renpy_generate writes the English template into <scratch>/tl (game's own Ren'Py, plus zz_unitytl_extra.rpy)
// 3) every template file: renpy_tl_apply with each line "translated" to itself gives back the same bytes,
//    and with a marker added, every line reads back with the marker
// g++ -std=c++17 -O2 -Isrc -o build/renpytest.exe tests/renpytest.cpp src/renpy.cpp src/bytes.cpp -static -municode -lz
#include <cstdio>

#include "../src/renpy.h"

static int fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    fails += !ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("renpytest <game folder> <scratch dir>\n");
        return 2;
    }
    fs::path root;
    std::string name;
    if (!renpy_find(argv[1], root, name)) {
        check(false, "khong thay game Ren'Py");
        return 1;
    }
    std::printf("Ren'Py game %s\n", name.c_str());
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root / "game", ec))
        if (e.path().extension() == ".rpa") {
            auto idx = rpa_index(e.path());
            check(!idx.empty(), pathstr(e.path().filename()) + ": " + std::to_string(idx.size()) + " file");
        }
    fs::path out = fs::path(argv[2]) / "tl";
    bool gen = renpy_generate(root, name, "vietnamese", out, [](const std::string& s) { std::printf("%s\n", s.c_str()); });
    check(gen, "renpy_generate");
    if (!gen) return 1;
    check(!fs::exists(root / "_unitytl_renpy_tmp", ec), "ban sao tam da xoa");
    int files = 0, same = 0, lines = 0, marked = 0;
    for (auto& e : fs::directory_iterator(out, ec)) {
        std::string text = read_text(e.path());
        auto ls = renpy_tl_read(text);
        std::map<std::string, std::string> id, mark;
        for (auto& l : ls) {
            id[l.key] = l.text;
            mark[l.key] = "VI \"" + l.text + "\"";
        }
        int n = 0;
        files++;
        same += renpy_tl_apply(text, id, n) == text;
        auto back = renpy_tl_read(renpy_tl_apply(text, mark, n));
        lines += (int)ls.size();
        for (size_t i = 0; i < back.size() && i < ls.size(); i++) marked += back[i].text == mark[ls[i].key];
    }
    check(files > 0 && same == files, "tl doc/ghi giu nguyen " + std::to_string(same) + "/" + std::to_string(files));
    check(lines > 0 && marked == lines, "dong ghi roi doc lai " + std::to_string(marked) + "/" + std::to_string(lines));
    // only games shipping .rpy sources get on-screen literals (Between Humanity has .rpyc only)
    std::printf("     zz_unitytl_extra.rpy: %s\n", fs::exists(out / "zz_unitytl_extra.rpy", ec) ? "co" : "khong (game khong co .rpy)");
    std::printf(fails ? "\n%d FAILED\n" : "\nALL OK\n", fails);
    return fails ? 1 : 0;
}
