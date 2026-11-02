// Downloader checks without re-downloading gigabytes:
//   dltest plan <empty dir>      -> GPU detection + plan for a machine with nothing installed
//   dltest resume <dir> <url> <size> -> resumes <dir>/models/<file>.part via a Range request
//   dltest verify <llm dir>      -> plan against an existing install (only SHA-256 checks run)
#include <cstdio>

#include "../src/downloader.h"

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::string cmd = argv[1];
    auto log = [](const std::string& s) { printf("%s\n", s.c_str()); };
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> done{0}, total{0};
    fs::path srv, model;
    if (cmd == "plan" || cmd == "verify") {
        DlPlan p = make_plan(u8path(argv[2]));
        printf("GPU: %s nvidia=%d vram=%llu MB driver=%.2f\n", p.gpu.name.c_str(), p.gpu.nvidia,
               (unsigned long long)p.gpu.vramMB, p.gpu.driver);
        printf("note: %s\nerror: %s\n", p.note.c_str(), p.error.c_str());
        for (auto& i : p.items)
            printf("  %s | %s | %llu | sha=%s\n", i.file.c_str(), i.url.c_str(), (unsigned long long)i.size,
                   i.sha256.empty() ? "-" : i.sha256.substr(0, 16).c_str());
        if (cmd == "verify") printf("run_plan -> %d\n", run_plan(p, stop, done, total, log, srv, model));
    } else if (cmd == "resume") {
        DlPlan p;
        p.root = u8path(argv[2]);
        DlItem it;
        it.url = argv[3];
        it.size = std::stoull(argv[4]);
        it.file = it.url.substr(it.url.rfind('/') + 1);
        it.label = it.file;
        p.items.push_back(it);
        bool ok = run_plan(p, stop, done, total, log, srv, model);
        printf("run_plan -> %d (llama-server missing is expected here), done=%llu\n", ok, (unsigned long long)done.load());
    }
}
