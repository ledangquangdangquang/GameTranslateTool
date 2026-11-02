// Godot .pck checks on a real game pack (nothing in the game folder is written):
//   godottest <game.pck> <scratch dir>
// 1) stored md5 == md5 of the data for every localization file (offsets/sizes parsed right)
// 2) json_flat_read/apply round trip is byte-exact with no changes
// 3) pck_write with one changed file: that file reads back changed, every other entry is unchanged
// 4) every .fontdata (<= 32 MB): font file out, back in, out again == same; Latin fonts lacking
//    Vietnamese get patched and the result written to <scratch>/<name>.fontdata / <name>_vi.ttf
// gcc -O2 -c -Ithird_party/zstd -o build/zstddeclib.o third_party/zstd/zstddeclib.c
// g++ -std=c++17 -O2 -Isrc -Ithird_party/zstd -o build/godottest.exe tests/godottest.cpp src/godot.cpp src/ttf.cpp src/gamemaker.cpp src/bytes.cpp src/csv.cpp src/bzip2.cpp build/zstddeclib.o -static -municode -lbcrypt -lwindowscodecs -lole32 -lgdi32 -luuid -loleaut32 -lz
#include <cstdio>

#include "../src/gamemaker.h"
#include "../src/godot.h"
#include "../src/ttf.h"

static int fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    fails += !ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("godottest <game.pck> <scratch dir>\n");
        return 2;
    }
    fs::path in = argv[1], out = fs::path(argv[2]) / "godottest_out.pck";
    Pck pck = Pck::load(in);
    std::printf("Godot %u.%u.%u, pack v%u, %zu files\n", pck.major, pck.minor, pck.patch, pck.version, pck.files.size());

    int hashed = 0, hashOk = 0, rt = 0, rtOk = 0;
    const PckFile* target = nullptr;
    for (auto& f : pck.files) {
        if (f.path.rfind("localization/", 0) != 0 || f.path.size() < 5 ||
            f.path.compare(f.path.size() - 5, 5, ".json") != 0)
            continue;
        Bytes b = pck.read(in, f);
        uint8_t md5[16];
        md5_digest(b.data(), b.size(), md5);
        hashed++;
        hashOk += std::memcmp(md5, f.md5, 16) == 0;
        std::string text(b.begin(), b.end());
        std::map<std::string, std::string> same;
        for (auto& kv : json_flat_read(text)) same[kv.first] = kv.second;
        int n = 0;
        rt++;
        rtOk += json_flat_apply(text, same, n) == text && n == (int)same.size();
        if (f.path == "localization/eng/card_reward_ui.json") target = &f;
    }
    check(hashed > 0 && hashOk == hashed, "md5 " + std::to_string(hashOk) + "/" + std::to_string(hashed));
    check(rt > 0 && rtOk == rt, "json round trip " + std::to_string(rtOk) + "/" + std::to_string(rt));
    if (!target) {
        check(false, "localization/eng/card_reward_ui.json");
        return 1;
    }

    Bytes b = pck.read(in, *target);
    std::string text(b.begin(), b.end());
    std::map<std::string, std::string> tr{{"OPTION_SKIP.name", "Bỏ qua \"thử\" [blue]{X}[/blue]"}};
    int n = 0;
    std::string changed = json_flat_apply(text, tr, n);
    pck_write(in, pck, {{target->path, Bytes(changed.begin(), changed.end())}}, out);

    Pck p2 = Pck::load(out);
    const PckFile* t2 = p2.find(target->path);
    Bytes b2 = p2.read(out, *t2);
    std::string back(b2.begin(), b2.end());
    std::string got;
    for (auto& kv : json_flat_read(back))
        if (kv.first == "OPTION_SKIP.name") got = kv.second;
    check(back == changed && got == tr.begin()->second, "changed file reads back");
    uint8_t md5[16];
    md5_digest(b2.data(), b2.size(), md5);
    check(std::memcmp(md5, t2->md5, 16) == 0, "new md5 stored");
    int same = 0;
    for (size_t i = 0; i < pck.files.size(); i++) {
        auto& a = pck.files[i];
        auto& c = p2.files[i];
        if (&a == target) continue;
        same += a.path == c.path && a.offset == c.offset && a.size == c.size && !std::memcmp(a.md5, c.md5, 16) &&
                a.flags == c.flags;
    }
    check(same == (int)pck.files.size() - 1, "other entries unchanged " + std::to_string(same));
    // original data region untouched: compare the bytes around the replaced entry's old slot
    Bytes oldAgain = p2.read(out, *target);  // old offset/size, read from the new file
    check(oldAgain == b, "old data left in place");
    check(p2.fileSize >= pck.fileSize + changed.size() && p2.fileSize <= pck.fileSize + changed.size() + 16,
          "file grew by the new data only");
    std::error_code ec;
    fs::remove(out, ec);

    int fonts = 0, fontsOk = 0;
    for (auto& f : pck.files) {
        if (f.path.size() < 9 || f.path.compare(f.path.size() - 9, 9, ".fontdata") != 0 || f.size > 32u << 20) continue;
        std::string name = pathstr(fs::u8path(f.path).filename());
        name = name.substr(0, name.rfind('-'));
        Bytes res = pck.read(in, f), ttf, again, back;
        fontdata_ttf(res, ttf);
        Bytes same = fontdata_with_ttf(res, ttf);
        fontdata_ttf(same, again);
        fonts++;
        fontsOk += again == ttf && fontdata_with_ttf(same, ttf) == same;
        int miss = ttf_missing_vietnamese(ttf);
        std::printf("     %s: font %zu byte, thieu %d chu Viet\n", name.c_str(), ttf.size(), miss);
        if (miss > 0 && ttf_add_vietnamese(ttf, [](const std::string& s) { std::printf("%s\n", s.c_str()); })) {
            Bytes patched = fontdata_with_ttf(res, ttf);
            fontdata_ttf(patched, back);
            check(back == ttf && ttf_missing_vietnamese(back) == 0, name + " da va, doc lai dung");
            write_file(fs::path(argv[2]) / (name + ".fontdata"), patched);
            write_file(fs::path(argv[2]) / (name + "_vi.ttf"), ttf);
        }
    }
    check(fonts > 0 && fontsOk == fonts, "fontdata tach/nhung lai " + std::to_string(fontsOk) + "/" + std::to_string(fonts));
    std::printf(fails ? "\n%d FAILED\n" : "\nALL OK\n", fails);
    return fails ? 1 : 0;
}
