// Translates the lines of a flat {"key": "text"} JSON file that contain `filter` with the local model
// and checks every placeholder/tag survived (translation_ok):
//   tltest <file.json> <filter> [max] [port]
// g++ -std=c++17 -O2 -Isrc -o build/tltest.exe tests/tltest.cpp src/translate.cpp src/json.cpp src/bytes.cpp -static -municode -lwinhttp
#include <windows.h>

#include <cstdio>

#include "../src/bytes.h"
#include "../src/json.h"
#include "../src/translate.h"

static void say(const std::string& s) {
    std::string line = s + "\n";
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), (DWORD)line.size(), &w, nullptr);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        say("tltest <file.json> <filter> [max] [port]");
        return 2;
    }
    std::wstring wf = argv[2];
    std::string filter(wf.begin(), wf.end());
    int max = argc > 3 ? _wtoi(argv[3]) : 20;
    EngineConfig eng;
    eng.kind = EngineConfig::Local;
    if (argc > 4) eng.port = _wtoi(argv[4]);
    Json j = Json::parse(read_text(argv[1]));
    Translator tr(Glossary{}, eng);
    int n = 0, ok = 0, fail = 0;
    for (auto& [k, v] : j.members()) {
        const std::string& en = v.str();
        if (en.find(filter) == std::string::npos) continue;
        if (n++ >= max) break;
        auto vi = tr.translate(en);
        if (!vi) {
            fail++;
            say("[" + k + "] KHONG DICH DUOC (" + tr.last_error() + ")\n  en: " + en);
            continue;
        }
        bool same = translation_ok(en, *vi);
        ok += same;
        say("[" + k + "] " + (same ? "ok" : "SAI TAG") + "\n  en: " + en + "\n  vi: " + *vi);
    }
    say("\n" + std::to_string(ok) + " ok, " + std::to_string(fail) + " khong dich duoc, " +
        std::to_string(n > max ? max : n) + " dong");
    return 0;
}
