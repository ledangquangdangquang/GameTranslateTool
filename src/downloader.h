// "Tai model": pick the right Hy-MT2 GGUF + llama.cpp build for this PC, show the plan for
// confirmation, then download (resumable), verify SHA-256 and unpack.
#pragma once
#include <atomic>
#include <functional>

#include "bytes.h"

struct GpuInfo {
    bool nvidia = false;
    std::string name;
    uint64_t vramMB = 0;
    double driver = 0;  // NVIDIA driver version, e.g. 581.95
};
GpuInfo detect_gpu();

struct DlItem {
    std::string label;   // shown to the user
    std::string url;
    std::string file;    // file name on disk
    uint64_t size = 0;
    std::string sha256;  // lowercase hex; empty = source publishes none
    bool extract = false;  // zip -> <root>/llama.cpp
};

struct DlPlan {
    GpuInfo gpu;
    fs::path root;  // <root>/models, <root>/llama.cpp, <root>/dl
    std::vector<DlItem> items;
    std::string note;   // why these variants were chosen
    std::string error;  // non-empty = plan could not be made (offline...)
    uint64_t total() const {
        uint64_t t = 0;
        for (auto& i : items) t += i.size;
        return t;
    }
};

// Network access (Hugging Face + GitHub APIs) to read exact sizes and hashes.
DlPlan make_plan(const fs::path& root);

using DlLog = std::function<void(const std::string&)>;
// Returns true when everything is downloaded, verified and unpacked; fills serverExe/model.
bool run_plan(const DlPlan& plan, std::atomic<bool>& stop, std::atomic<uint64_t>& done, std::atomic<uint64_t>& total,
              const DlLog& log, fs::path& serverExe, fs::path& model);
