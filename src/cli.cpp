// unitytl-cli: same engine as the GUI, for scripting and testing.
//   unitytl-cli scan <game folder>
//   unitytl-cli translate <game folder> [--all] [--limit N] [--local [--port 8080]]
//   unitytl-cli inject <game folder> [--font "Constantia"] [--fold3]   (GameMaker only)
//   unitytl-cli restore <game folder>
//   unitytl-cli log <game folder>
//   unitytl-cli probe <any folder> [--top N]   (diagnostic: no known kind found text - where IS it?)
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <thread>

#include "probe.h"
#include "project.h"

static void say(const std::string& s) {
    std::string line = s + "\n";
    DWORD w;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetFileType(h) == FILE_TYPE_CHAR) {  // console: write UTF-16 so Vietnamese shows correctly
        int n = MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0);
        std::wstring ws(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), ws.data(), n);
        WriteConsoleW(h, ws.data(), (DWORD)ws.size(), &w, nullptr);
    } else {
        WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
    }
}

int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, 0);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
        args.push_back(s);
    }
    if (args.size() < 3) {
        say("unitytl-cli scan|translate|inject|restore|log|probe <thu muc game> [--all] [--limit N] [--top N]");
        return 2;
    }
    const std::string& cmd = args[1];
    if (cmd == "probe") {
        int topN = 30;
        for (size_t i = 3; i + 1 < args.size(); i++)
            if (args[i] == "--top") topN = std::stoi(args[i + 1]);
        try {
            probe_dir(u8path(args[2]), topN, say);
        } catch (const std::exception& e) {
            say(std::string("LOI: ") + e.what());
            return 1;
        }
        return 0;
    }
    Project p;
    std::string err;
    if (!p.open(u8path(args[2]), err)) {
        say(err);
        return 1;
    }
    say("Game: " + p.name() + "  (" + pathstr(p.dataDir) + ")");
    try {
        if (cmd == "scan") p.scan(say);
        else if (cmd == "translate") {
            if (p.entries.empty()) p.scan(say);
            bool all = false;
            int limit = 0;
            EngineConfig engine;
            for (size_t i = 3; i < args.size(); i++) {
                if (args[i] == "--all") all = true;
                if (args[i] == "--limit" && i + 1 < args.size()) limit = std::stoi(args[++i]);
                if (args[i] == "--local") engine.kind = EngineConfig::Local;  // llama-server must be running
                if (args[i] == "--port" && i + 1 < args.size()) engine.port = std::stoi(args[++i]);
            }
            std::atomic<bool> stop{false};
            std::atomic<int> done{0}, total{0};
            std::thread watcher;
            if (limit > 0)
                watcher = std::thread([&] {
                    while (!stop && (total == 0 || done < total)) {
                        if (done >= limit) stop = true;
                        Sleep(50);
                    }
                });
            p.translate(all, stop, done, total, say, engine);
            stop = true;
            if (watcher.joinable()) watcher.join();
        } else if (cmd == "inject") {
            for (size_t i = 3; i + 1 < args.size(); i++)
                if (args[i] == "--font") p.gmFont = args[i + 1];  // GameMaker: Windows font for Vietnamese letters
            for (size_t i = 3; i < args.size(); i++)
                if (args[i] == "--fold3") p.gmFold3 = true;  // GameMaker: game crashes on 3-byte UTF-8
            p.inject(say);
        }
        else if (cmd == "restore") p.restore(say);
        else if (cmd == "log") p.check_log(say);
        else {
            say("Lenh khong biet: " + cmd);
            return 2;
        }
    } catch (const std::exception& e) {
        say(std::string("LOI: ") + e.what());
        return 1;
    }
    return 0;
}
