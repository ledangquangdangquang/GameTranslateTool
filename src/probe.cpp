#include "probe.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "bundle.h"
#include "serialized.h"

namespace {

// Heuristic: an ASCII run is "English-sentence-like" if it's long enough and has real word breaks,
// not just a run of identifier/class-name characters (e.g. "com.unity.modules.physics").
bool looks_like_sentence(const std::string& s) {
    if (s.size() < 20) return false;
    int letters = 0, spaces = 0;
    for (unsigned char c : s) {
        if (c == ' ') spaces++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) letters++;
    }
    return letters >= (int)(s.size() * 6 / 10) && spaces >= 2;
}

struct FileStat {
    int count = 0;
    long long chars = 0;
    std::vector<std::string> samples;
};

void scan_bytes_for_text(const Bytes& data, FileStat& st) {
    std::string run;
    for (size_t i = 0; i <= data.size(); i++) {
        uint8_t c = i < data.size() ? data[i] : 0;
        if (c >= 32 && c < 127) {
            run += (char)c;
            continue;
        }
        if (looks_like_sentence(run)) {
            st.count++;
            st.chars += (long long)run.size();
            if (st.samples.size() < 3) st.samples.push_back(run.substr(0, 150));
        }
        run.clear();
    }
}

const char* kSkipExt[] = {".png",  ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".ogg", ".mp3", ".wav",
                          ".wem",  ".bank", ".webm", ".mp4", ".ttf", ".otf", ".ico", ".exr", ".psd"};

std::string lower_ext(const fs::path& p) {
    std::string e = pathstr(p.extension());
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    return e;
}

}  // namespace

// Walks every file under `dir` (any extension, any depth) looking for runs of readable English-like
// text - inside plain files directly, and inside each object of every Unity .assets/bundle file (so
// text buried in an *embedded* TextAsset/MonoBehaviour that no known source `kind` recognizes still
// shows up, not just files `candidate_files()` would have scanned). For when every known detector
// came up empty and the real location is genuinely unknown.
void probe_dir(const fs::path& dir, int topN, const std::function<void(const std::string&)>& log) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        log("Khong thay thu muc: " + pathstr(dir));
        return;
    }
    std::map<std::string, FileStat> stats;
    int filesWalked = 0;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = pathstr(fs::relative(it->path(), dir, ec));
        // Engine/runtime noise that's the same in every Unity build and never holds this game's own
        // text: the Mono runtime tree, .NET/Unity-engine-module DLLs, and unitytl's own save files.
        if (rel.find("MonoBleedingEdge") != std::string::npos || rel.find("\\Managed\\") != std::string::npos ||
            rel.rfind("UnityPlayer.dll") != std::string::npos || rel.find("UnityCrashHandler") != std::string::npos ||
            rel.find("_unitytl") != std::string::npos || rel.find("unity default resources") != std::string::npos)
            continue;
        std::string ext = lower_ext(it->path());
        if (std::find(std::begin(kSkipExt), std::end(kSkipExt), ext) != std::end(kSkipExt)) continue;
        uint64_t sz = fs::file_size(it->path(), ec);
        if (ec || sz == 0 || sz > 300ull * 1024 * 1024) continue;
        filesWalked++;
        Bytes data;
        try {
            data = read_file(it->path());
        } catch (const std::exception&) {
            continue;
        }
        FileStat st;
        if (Bundle::is_bundle(data)) {
            try {
                Bundle b = Bundle::load(data);
                for (auto& n : b.nodes) {
                    if (!n.is_serialized() || !SerializedFile::looks_like(n.data)) continue;
                    try {
                        SerializedFile sf = SerializedFile::load(n.data);
                        for (auto& o : sf.objects) scan_bytes_for_text(sf.object_data(o), st);
                    } catch (const std::exception&) {
                    }
                }
            } catch (const std::exception&) {
                scan_bytes_for_text(data, st);  // fall back to a raw scan if it won't parse
            }
        } else if (SerializedFile::looks_like(data)) {
            try {
                SerializedFile sf = SerializedFile::load(data);
                for (auto& o : sf.objects) scan_bytes_for_text(sf.object_data(o), st);
            } catch (const std::exception&) {
                scan_bytes_for_text(data, st);
            }
        } else {
            scan_bytes_for_text(data, st);
        }
        if (st.count > 0) stats[rel] = std::move(st);
    }
    std::vector<std::pair<std::string, FileStat>> ranked(stats.begin(), stats.end());
    std::sort(ranked.begin(), ranked.end(),
              [](auto& a, auto& b) { return a.second.chars > b.second.chars; });
    log("Da quet " + std::to_string(filesWalked) + " file, " + std::to_string(ranked.size()) +
        " file co chu tieng Anh. Top " + std::to_string(std::min<size_t>(topN, ranked.size())) + ":");
    for (size_t i = 0; i < ranked.size() && (int)i < topN; i++) {
        auto& [path, st] = ranked[i];
        log("  " + path + "  (" + std::to_string(st.count) + " doan, " + std::to_string(st.chars) + " ky tu)");
        for (auto& s : st.samples) log("      " + s);
    }
}
