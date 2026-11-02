#include "project.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "bundle.h"
#include "catalog.h"
#include "csv.h"
#include "gamemaker.h"
#include "godot.h"
#include "renpy.h"
#include "json.h"
#include "ttf.h"
#include "unity_font.h"
#include "unreal.h"

namespace {

const char* kSkipWords[] = {"mesh", "texture", "image", "sprite", "audio", "sound", "music", "shader", "anim",
                            "material", "vfx", "prefab", "scene", "monoscripts", "unitybuiltin", "video"};
const uintmax_t kMaxFile = 300ull * 1024 * 1024;

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

fs::path bak_of(const fs::path& p) {
    fs::path b = p;
    b += ".bak";
    return b;
}

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// StreamingAssets/Localization/<lang>/*.csv: one "key;text" pair per line, no quoting (the game's own
// text never contains a literal ';'; pre-wrapped multi-line text uses a literal "\n", not a real line
// break). Every language folder mirrors the same files/keys/order as the English one.
const char* kUnityLocMarker = "_unitytl_marker.txt";

// StreamingAssets/Translations/<Language>/*.json (ShoSakyu): same idea, different game's own convention
// - capitalised language names, one flat {guid: text} JSON object per "scene" file instead of a CSV.
const char* kUnityTrMarker = "_unitytl_marker.txt";

std::string strip_bom_json(const std::string& s) {
    return s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF ? s.substr(3) : s;
}

// StreamingAssets/Localization/<Language>.csv (Escape from Duckov): some "value" cells spell every
// space as a literal "\ " (seen only on longer entries, not short UI labels) - unescaped before
// translating, re-escaped on write only when that row's English cell used it too.
std::string unescape_bslash_space(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == ' ') {
            out += ' ';
            i++;
        } else out += s[i];
    }
    return out;
}
std::string escape_bslash_space(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == ' ') out += "\\ ";
        else out += c;
    }
    return out;
}

struct LocLine {
    std::string key, text;
};

std::vector<LocLine> loc_csv_read(const std::string& content) {
    std::vector<LocLine> out;
    size_t i = 0, n = content.size();
    while (i < n) {
        size_t eol = content.find('\n', i);
        size_t end = eol == std::string::npos ? n : eol;
        std::string line = content.substr(i, end - i);
        i = eol == std::string::npos ? n : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        size_t sep = line.find(';');
        if (sep == std::string::npos) continue;
        out.push_back({line.substr(0, sep), line.substr(sep + 1)});
    }
    return out;
}

std::string loc_csv_write(const std::vector<LocLine>& lines) {
    std::string out;
    for (auto& l : lines) {
        out += l.key;
        out += ';';
        out += l.text;
        out += "\r\n";
    }
    return out;
}

// One loaded file: a plain SerializedFile or a bundle with one or more SerializedFile nodes.
struct Container {
    bool isBundle = false;
    Bundle bundle;
    std::vector<std::pair<std::string, SerializedFile>> files;  // node name ("" for .assets)

    static Container load(const fs::path& p) {
        Container c;
        Bytes data = read_file(p);
        if (Bundle::is_bundle(data)) {
            c.isBundle = true;
            c.bundle = Bundle::load(data);
            for (auto& n : c.bundle.nodes)
                if (n.is_serialized() && SerializedFile::looks_like(n.data))
                    c.files.emplace_back(n.path, SerializedFile::load(n.data));
        } else if (SerializedFile::looks_like(data)) {
            c.files.emplace_back("", SerializedFile::load(std::move(data)));
        }
        return c;
    }
    SerializedFile* get(const std::string& node) {
        for (auto& f : files)
            if (f.first == node) return &f.second;
        return nullptr;
    }
    Bytes save() {
        if (!isBundle) return files.at(0).second.save();
        for (auto& f : files) {
            if (!f.second.modified()) continue;
            for (auto& n : bundle.nodes)
                if (n.path == f.first) n.data = f.second.save();
        }
        return bundle.save();
    }
};

std::string source_key(const TextSource& s) { return s.file + "|" + s.node + "|" + std::to_string(s.pathId); }

Json source_to_json(const TextSource& s) {
    Json j = Json::object();
    j["file"] = Json(s.file);
    j["node"] = Json(s.node);
    j["pathId"] = Json((int64_t)s.pathId);
    j["kind"] = Json(s.kind);
    j["asset"] = Json(s.asset);
    Json o = Json::array(), c = Json::array();
    for (auto& x : s.options) o.push(Json(x));
    for (auto& x : s.chosen) c.push(Json(x));
    j["options"] = o;
    j["chosen"] = c;
    j["variant"] = Json(s.variant);
    j["count"] = Json(s.count);
    j["enabled"] = Json(s.enabled);
    return j;
}

TextSource source_from_json(const Json& j) {
    TextSource s;
    s.file = j.get_str("file");
    s.node = j.get_str("node");
    s.pathId = j.at("pathId").int64();
    s.kind = j.get_str("kind");
    s.asset = j.get_str("asset");
    for (auto& x : j.at("options").items()) s.options.push_back(x.str());
    for (auto& x : j.at("chosen").items()) s.chosen.push_back(x.str());
    s.variant = (int)j.at("variant").int64();
    s.count = (int)j.at("count").int64();
    if (auto e = j.find("enabled")) s.enabled = e->boolean();
    return s;
}

}  // namespace

// ------------------------------------------------------------------ locate / open
bool Project::locate(const fs::path& any, fs::path& root, fs::path& dataDir) {
    std::error_code ec;
    fs::path start = fs::is_directory(any, ec) ? any : any.parent_path();
    auto isData = [&](const fs::path& d) {
        std::string n = pathstr(d.filename());
        return n.size() > 5 && n.substr(n.size() - 5) == "_Data" &&
               (fs::exists(d / "globalgamemanagers", ec) || fs::exists(d / "data.unity3d", ec));
    };
    if (isData(start)) {
        dataDir = start;
        root = start.parent_path();
        return true;
    }
    // search the folder and up to 3 levels below it (e.g. Rubinite/Rubinite/game/Rubinite_Data)
    std::vector<std::pair<fs::path, int>> stack{{start, 0}};
    while (!stack.empty()) {
        auto [dir, depth] = stack.back();
        stack.pop_back();
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            if (isData(it->path())) {
                dataDir = it->path();
                root = dir;
                return true;
            }
            if (depth < 3) stack.push_back({it->path(), depth + 1});
        }
    }
    return false;
}

// GameMaker: the folder (or one up to 3 levels below) holding data.win
static bool locate_gm(const fs::path& any, fs::path& root) {
    std::error_code ec;
    fs::path start = fs::is_directory(any, ec) ? any : any.parent_path();
    std::vector<std::pair<fs::path, int>> stack{{start, 0}};
    while (!stack.empty()) {
        auto [dir, depth] = stack.back();
        stack.pop_back();
        if (gm_is_game_dir(dir)) {
            root = dir;
            return true;
        }
        if (depth < 3)
            for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                if (it->is_directory(ec)) stack.push_back({it->path(), depth + 1});
    }
    return false;
}

// Godot: the folder (or one up to 3 levels below) holding <name>.pck
static bool locate_godot(const fs::path& any, fs::path& root, fs::path& pck) {
    std::error_code ec;
    fs::path start = fs::is_directory(any, ec) ? any : any.parent_path();
    std::vector<std::pair<fs::path, int>> stack{{start, 0}};
    while (!stack.empty()) {
        auto [dir, depth] = stack.back();
        stack.pop_back();
        if (godot_find_pck(dir, pck)) {
            root = dir;
            return true;
        }
        if (depth < 3)
            for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                if (it->is_directory(ec)) stack.push_back({it->path(), depth + 1});
    }
    return false;
}

bool Project::open(const fs::path& any_, std::string& err) {
    gm = false;
    godot = false;
    renpy = false;
    unreal = false;
    // Always absolute: CreateProcessW's lpCurrentDirectory (used to run the game's own Ren'Py for
    // template generation) must be fully qualified per its own docs - a relative one silently breaks
    // the child process's view of its own argv paths instead of erroring.
    std::error_code absEc;
    fs::path any = fs::absolute(any_, absEc);
    if (absEc) any = any_;
    fs::path pck;
    if (!locate(any, root, dataDir)) {
        if (locate_gm(any, root)) gm = true;
        else if (locate_godot(any, root, pck)) godot = true;
        else if (renpy_find(any, root, renpyName)) renpy = true;
        else if (locate_unreal(any, root, unrealPak)) unreal = true;
        else {
            err = "Khong tim thay game Unity (thu muc <Ten>_Data), GameMaker (data.win), Godot (.pck), Ren'Py "
                  "(renpy/ + game/) hay Unreal (Content/Paks/*.pak) trong: " + pathstr(any);
            return false;
        }
        dataDir = root;
    }
    company.clear();
    product.clear();
    if (gm) product = pathstr(root.filename());
    if (godot) product = pathstr(pck.stem());
    if (renpy) product = renpyName;
    if (unreal) {
        unrealProjectName = pathstr(unrealPak.parent_path().parent_path().parent_path().filename());
        product = unrealProjectName;
    }
    std::error_code ec;
    if (fs::exists(dataDir / "app.info", ec)) {
        std::string info = read_text(dataDir / "app.info");
        size_t nl = info.find('\n');
        company = info.substr(0, nl);
        if (nl != std::string::npos) product = info.substr(nl + 1);
        while (!product.empty() && (product.back() == '\r' || product.back() == '\n')) product.pop_back();
    }
    reload_glossary();
    sources.clear();
    entries.clear();
    catalogSizes.clear();
    load();
    return true;
}

void Project::reload_glossary() { glossary = Glossary::load_or_default(pathstr(glossary_file())); }

std::string Project::rel(const fs::path& p) const { return pathstr(fs::relative(p, dataDir)); }

fs::path Project::unity_loc_live() const { return dataDir / "StreamingAssets" / "Localization" / "vietnamese"; }

fs::path Project::unity_tr_live() const { return dataDir / "StreamingAssets" / "Translations" / "Vietnamese"; }

std::vector<fs::path> Project::candidate_files() const {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dataDir, ec))
        if (e.is_regular_file() && (e.path().extension() == ".assets" || e.path().filename() == "data.unity3d"))
            out.push_back(e.path());

    fs::path sa = dataDir / "StreamingAssets";
    Bytes catalog;
    if (fs::exists(sa / "aa" / "catalog.bin", ec)) {
        fs::path cb = sa / "aa" / "catalog.bin";
        catalog = read_file(fs::exists(bak_of(cb), ec) ? bak_of(cb) : cb);
    }
    if (fs::exists(sa, ec)) {
        for (auto it = fs::recursive_directory_iterator(sa, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (!it->is_regular_file(ec) || it->path().extension() != ".bundle") continue;
            std::string name = pathstr(it->path().filename());
            std::string ln = lower(name);
            if (ln.find("localization-string-tables") == std::string::npos &&
                std::any_of(std::begin(kSkipWords), std::end(kSkipWords),
                            [&](const char* w) { return ln.find(w) != std::string::npos; }))
                continue;
            // only bundles the game actually loads (a broken patch can leave stray ones behind)
            if (!catalog.empty() && std::search(catalog.begin(), catalog.end(), name.begin(), name.end()) == catalog.end())
                continue;
            out.push_back(it->path());
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(), [&](const fs::path& p) { return fs::file_size(p, ec) > kMaxFile; }),
              out.end());
    return out;
}

// ------------------------------------------------------------------ scan
void Project::scan(const LogFn& log) {
    std::map<std::string, TextSource> prevSources;
    for (auto& s : sources) prevSources[source_key(s)] = s;
    std::unordered_map<std::string, std::string> prevVi, prevByEn;
    for (auto& e : entries)
        if (!e.vi.empty()) {
            prevVi[e.id] = e.vi;
            prevByEn.emplace(e.en, e.vi);
        }

    std::vector<TextSource> found;
    std::vector<Entry> newEntries;
    if (gm || godot || renpy || unreal) {
        if (gm) scan_gm(found, newEntries, log);
        else if (godot) scan_godot(found, newEntries, log);
        else if (renpy) scan_renpy(found, newEntries, log);
        else scan_unreal(found, newEntries, log);
        for (auto& e : newEntries) {
            auto pv = prevVi.find(e.id);
            if (pv != prevVi.end()) e.vi = pv->second;
        }
    }
    if (!gm && !godot && !renpy && !unreal) {
        scan_unity_loc(found, newEntries, log);
        scan_unity_tr(found, newEntries, log);
        scan_unity_csv(found, newEntries, log);
    }
    auto files = (gm || godot || renpy || unreal) ? std::vector<fs::path>{} : candidate_files();
    for (size_t i = 0; i < files.size(); i++) {
        const fs::path& live = files[i];
        log("[" + std::to_string(i + 1) + "/" + std::to_string(files.size()) + "] " + rel(live));
        std::error_code ec;
        bool hasBak = fs::exists(bak_of(live), ec);
        try {
            Container orig = Container::load(hasBak ? bak_of(live) : live);
            std::vector<TextSource> here;
            for (auto& f : orig.files) detect_sources(f.second, rel(live), f.first, here);
            here.erase(std::remove_if(here.begin(), here.end(), [](const TextSource& s) { return s.count == 0; }),
                       here.end());
            if (here.empty()) continue;
            Container cur;
            if (hasBak) cur = Container::load(live);
            for (auto& s : here) {
                auto prev = prevSources.find(source_key(s));
                if (prev != prevSources.end()) {
                    s.enabled = prev->second.enabled;
                    if (!prev->second.chosen.empty()) s.chosen = prev->second.chosen;
                }
                int idx = (int)found.size();
                found.push_back(s);
                if (!s.enabled) {
                    log("    - " + s.label() + " (tat - bat trong danh sach nguon neu can)");
                    continue;
                }
                TextMap en = read_source(*orig.get(s.node), s);
                std::unordered_map<std::string, std::string> liveText;
                if (hasBak)
                    if (SerializedFile* sf = cur.get(s.node))
                        try {
                            for (auto& kv : read_source(*sf, s)) liveText[kv.first] = kv.second;
                        } catch (const std::exception&) {
                        }
                for (auto& [key, text] : en) {
                    Entry e;
                    e.id = source_key(s) + "|" + key;
                    e.src = idx;
                    e.key = key;
                    e.en = text;
                    auto pv = prevVi.find(e.id);
                    if (pv != prevVi.end()) e.vi = pv->second;
                    else {
                        auto lv = liveText.find(key);
                        if (lv != liveText.end() && lv->second != text) e.vi = lv->second;  // already translated in game
                    }
                    newEntries.push_back(std::move(e));
                }
                log("    + " + s.label() + " - " + std::to_string(en.size()) + " dong");
            }
        } catch (const std::exception& ex) {
            log("    ! bo qua: " + std::string(ex.what()));
        }
    }
    // same English line in several sources: share an existing translation. A line whose id changed
    // (a regenerated Ren'Py template moves a string to another tl file) keeps its old translation too.
    std::unordered_map<std::string, std::string> known;
    for (auto& e : newEntries)
        if (!e.vi.empty()) known.emplace(e.en, e.vi);
    for (auto& kv : prevByEn) known.emplace(kv);
    for (auto& e : newEntries)
        if (e.vi.empty()) {
            auto k = known.find(e.en);
            if (k != known.end()) e.vi = k->second;
        }
    {
        std::lock_guard<std::mutex> lk(mu);
        sources = std::move(found);
        entries = std::move(newEntries);
    }
    save();
    log("Quet xong: " + std::to_string(sources.size()) + " nguon, " + std::to_string(entries.size()) + " dong, da dich " +
        std::to_string(translated_count()) + ".");
    if (sources.empty())
        log(gm      ? "Khong tim thay file .ini / .json / .csv co muc tieng Anh canh data.win."
            : godot ? "Khong tim thay file .json tieng Anh (vd. localization/eng/*.json) trong file .pck."
            : renpy ? "Ren'Py khong sinh duoc file dich (xem log o tren)."
            : unreal ? "Khong tim thay .locres tieng Anh nao trong pak (hoac repak/UnrealLocres bi thieu trong tools/)."
                    : "Khong tim thay dinh dang chu nao duoc ho tro (I2 / Unity Localization / JSON / Dialogue System).");
}

// GameMaker text files next to data.win, all with an English section/column:
//   .ini  [english] key=value          (Crypt Custodian, Well Dweller)
//   .json {"eng": {"key": "text"}}     (Fields of Mistria localization.json)
//   .csv  key,English,French,...       (Starvester)
static bool gm_is_json(const std::string& file) { return lower(pathstr(u8path(file).extension())) == ".json"; }
static bool gm_is_csv(const std::string& file) { return lower(pathstr(u8path(file).extension())) == ".csv"; }
static std::vector<std::string> gm_sections(const std::string& file, const std::string& text) {
    return gm_is_json(file) ? gm_json_sections(text) : gm_is_csv(file) ? gm_csv_sections(text) : gm_ini_sections(text);
}
static TextMap gm_read(const std::string& file, const std::string& text, const std::string& section) {
    return gm_is_json(file) ? gm_json_read(text, section)
         : gm_is_csv(file)  ? gm_csv_read(text, section)
                            : gm_ini_read(text, section);
}

void Project::scan_gm(std::vector<TextSource>& found, std::vector<Entry>& out, const LogFn& log) {
    std::map<std::string, TextSource> prev;
    for (auto& s : sources) prev[source_key(s)] = s;
    std::error_code ec;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_regular_file()) continue;
        std::string ext = lower(pathstr(e.path().extension()));
        if (ext == ".ini") files.push_back(e.path());
        else if (ext == ".json") {
            // only localization tables: an English section right at the start (skips big data .json files)
            std::ifstream f(e.path(), std::ios::binary);
            char buf[64] = {};
            f.read(buf, sizeof buf);
            std::string h = lower(std::string(buf, (size_t)f.gcount()));
            if (h.find("\"eng\"") != std::string::npos || h.find("\"english\"") != std::string::npos ||
                h.find("\"en\"") != std::string::npos)
                files.push_back(e.path());
        } else if (ext == ".csv") {
            // only localization tables: an English column in the header row (skips unrelated data .csv files)
            std::ifstream f(e.path(), std::ios::binary);
            char buf[256] = {};
            f.read(buf, sizeof buf);
            std::string h = lower(std::string(buf, (size_t)f.gcount()));
            if (h.find("english") != std::string::npos || h.find(",eng,") != std::string::npos)
                files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (auto& live : files) {
        bool hasBak = fs::exists(bak_of(live), ec);
        std::string orig = read_text(hasBak ? bak_of(live) : live);
        std::string file = rel(live);
        auto secs = gm_sections(file, orig);
        auto en = std::find_if(secs.begin(), secs.end(), [](const std::string& x) {
            std::string l = lower(x);
            return l == "english" || l == "eng" || l == "en";
        });
        if (en == secs.end()) continue;
        TextSource s;
        s.file = file;
        s.kind = gm_is_json(file) ? "gmjson" : gm_is_csv(file) ? "gmcsv" : "gmini";
        s.asset = *en;
        s.options = secs;
        s.chosen = {*en};
        // leftovers like "translations - Copy.ini" are not read by the game
        s.enabled = lower(s.file).find("copy") == std::string::npos;
        auto p = prev.find(source_key(s));
        if (p != prev.end()) {
            s.enabled = p->second.enabled;
            if (!p->second.chosen.empty()) s.chosen = p->second.chosen;
        }
        TextMap text = gm_read(file, orig, s.chosen[0]);
        s.count = (int)text.size();
        int idx = (int)found.size();
        found.push_back(s);
        if (!s.enabled) {
            log("    - " + s.label() + " (tat - bat trong danh sach nguon neu can)");
            continue;
        }
        std::unordered_map<std::string, std::string> liveText;
        if (hasBak)
            for (auto& kv : gm_read(file, read_text(live), s.chosen[0])) liveText[kv.first] = kv.second;
        for (auto& [key, t] : text) {
            Entry e;
            e.id = source_key(s) + "|" + key;
            e.src = idx;
            e.key = key;
            e.en = t;
            auto lv = liveText.find(key);
            if (lv != liveText.end() && lv->second != t) e.vi = lv->second;  // already translated in game
            out.push_back(std::move(e));
        }
        log("    + " + s.label() + " - " + std::to_string(text.size()) + " dong");
    }
}

// Godot: English .json files inside <name>.pck - a .json with an English folder or file name on its
// path (Slay the Spire 2: localization/eng/cards.json, flat {"KEY": "text"}).
static bool godot_english_json(const std::string& path) {
    fs::path p = u8path(path);
    if (lower(pathstr(p.extension())) != ".json" || path.rfind(".godot/", 0) == 0) return false;
    if (is_english(pathstr(p.stem()))) return true;
    for (auto& part : p.parent_path())
        if (is_english(pathstr(part))) return true;
    return false;
}

void Project::scan_godot(std::vector<TextSource>& found, std::vector<Entry>& out, const LogFn& log) {
    std::map<std::string, TextSource> prev;
    for (auto& s : sources) prev[source_key(s)] = s;
    fs::path live;
    if (!godot_find_pck(root, live)) return;
    std::error_code ec;
    bool hasBak = fs::exists(bak_of(live), ec);
    fs::path origPath = hasBak ? bak_of(live) : live;
    Pck orig = Pck::load(origPath);
    Pck cur;
    if (hasBak) cur = Pck::load(live);
    std::string file = rel(live);
    log(file + ": Godot " + std::to_string(orig.major) + "." + std::to_string(orig.minor) + "." +
        std::to_string(orig.patch) + ", " + std::to_string(orig.files.size()) + " file");
    for (auto& pf : orig.files) {
        if ((pf.flags & 3) || !godot_english_json(pf.path)) continue;  // encrypted / removed
        Bytes b = orig.read(origPath, pf);
        TextMap text = json_flat_read(std::string(b.begin(), b.end()));
        if (text.empty()) continue;
        fs::path pp = u8path(pf.path);
        TextSource s;
        s.file = file;
        s.node = pf.path;
        s.kind = "godotjson";
        s.asset = pathstr(pp.parent_path().filename()) + "/" + pathstr(pp.filename());
        s.count = (int)text.size();
        auto p = prev.find(source_key(s));
        if (p != prev.end()) s.enabled = p->second.enabled;
        int idx = (int)found.size();
        found.push_back(s);
        if (!s.enabled) {
            log("    - " + s.label() + " (tat - bat trong danh sach nguon neu can)");
            continue;
        }
        std::unordered_map<std::string, std::string> liveText;
        if (const PckFile* lf = hasBak ? cur.find(pf.path) : nullptr) {
            Bytes lb = cur.read(live, *lf);
            for (auto& kv : json_flat_read(std::string(lb.begin(), lb.end()))) liveText[kv.first] = kv.second;
        }
        for (auto& [key, t] : text) {
            Entry e;
            e.id = source_key(s) + "|" + key;
            e.src = idx;
            e.key = key;
            e.en = t;
            auto lv = liveText.find(key);
            if (lv != liveText.end() && lv->second != t) e.vi = lv->second;  // already translated in game
            out.push_back(std::move(e));
        }
        log("    + " + s.label() + " - " + std::to_string(text.size()) + " dong");
    }
}

// Ren'Py: the English template (one tl .rpy per script file, every say line and string still in English)
// is generated once by the game's own engine and kept in _unitytl_renpy/<lang>/ - the ".bak" of this
// mode. game/tl/<lang>/ is what inject writes; a line that differs there is an existing translation.
void Project::scan_renpy(std::vector<TextSource>& found, std::vector<Entry>& out, const LogFn& log) {
    std::map<std::string, TextSource> prev;
    for (auto& s : sources) prev[source_key(s)] = s;
    std::error_code ec;
    fs::path tpl = renpy_template();
    if (!fs::is_directory(tpl, ec) || fs::is_empty(tpl, ec)) {
        log("Ren'Py: sinh file dich '" + renpyLang + "' bang engine cua game (lan dau, mat khoang 1 phut) ...");
        if (!renpy_generate(root, renpyName, renpyLang, tpl, log)) return;
    }
    std::vector<fs::path> files;  // scripts in subfolders get tl files in the same subfolders
    for (auto& e : fs::recursive_directory_iterator(tpl, ec))
        if (e.is_regular_file() && e.path().extension() == ".rpy") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    fs::path liveDir = root / "game" / "tl" / u8path(renpyLang);
    for (auto& f : files) {
        std::string fn = pathstr(f.lexically_relative(tpl));
        std::replace(fn.begin(), fn.end(), '\\', '/');
        auto lines = renpy_tl_read(read_text(f));
        if (lines.empty()) continue;
        TextSource s;
        s.file = "game/tl/" + renpyLang;
        s.node = fn;
        s.kind = "renpytl";
        s.asset = fn;
        s.count = (int)lines.size();
        auto p = prev.find(source_key(s));
        if (p != prev.end()) s.enabled = p->second.enabled;
        int idx = (int)found.size();
        found.push_back(s);
        if (!s.enabled) {
            log("    - " + s.label() + " (tat - bat trong danh sach nguon neu can)");
            continue;
        }
        std::unordered_map<std::string, std::string> liveText;
        if (fs::exists(liveDir / u8path(fn), ec))
            for (auto& l : renpy_tl_read(read_text(liveDir / u8path(fn)))) liveText[l.key] = l.text;
        for (auto& l : lines) {
            Entry e;
            e.id = source_key(s) + "|" + l.key;
            e.src = idx;
            e.key = l.key;
            e.en = l.text;
            auto lv = liveText.find(l.key);
            if (lv != liveText.end() && lv->second != l.text) e.vi = lv->second;  // already translated in game
            out.push_back(std::move(e));
        }
        log("    + " + s.label() + " - " + std::to_string(lines.size()) + " dong");
    }
}

// Unity games that keep their text as loose StreamingAssets/Localization/<lang>/*.csv files instead of
// (or alongside) Addressables bundles - e.g. a custom in-house localization system (Magic Academy's
// "Cores.Localization") rather than I2 / Unity Localization / Dialogue System. "english" is the
// template; there is no original "vietnamese" folder to keep a .bak of, so inject_unity_loc generates
// one fresh each time (same idea as Ren'Py's generated tl/ folder) and a line that differs from the
// template in the live folder is an existing translation.
void Project::scan_unity_loc(std::vector<TextSource>& found, std::vector<Entry>& out, const LogFn& log) {
    std::error_code ec;
    fs::path locDir = dataDir / "StreamingAssets" / "Localization";
    if (!fs::is_directory(locDir, ec)) return;
    fs::path tpl;
    for (auto& e : fs::directory_iterator(locDir, ec)) {
        if (ec) break;
        if (e.is_directory(ec) && is_english(pathstr(e.path().filename()))) { tpl = e.path(); break; }
    }
    if (tpl.empty()) return;
    std::map<std::string, TextSource> prev;
    for (auto& s : sources) prev[source_key(s)] = s;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(tpl, ec))
        if (!ec && e.is_regular_file(ec) && lower(pathstr(e.path().extension())) == ".csv") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    fs::path live = unity_loc_live();
    for (auto& f : files) {
        auto lines = loc_csv_read(read_text(f));
        if (lines.empty()) continue;
        TextSource s;
        s.file = rel(f);
        s.kind = "unityloc";
        s.asset = pathstr(f.filename());
        s.count = (int)lines.size();
        auto p = prev.find(source_key(s));
        if (p != prev.end()) s.enabled = p->second.enabled;
        int idx = (int)found.size();
        found.push_back(s);
        if (!s.enabled) {
            log("    - " + s.label() + " (tat - bat trong danh sach nguon neu can)");
            continue;
        }
        std::unordered_map<std::string, std::string> liveText;
        fs::path lf = live / f.filename();
        if (fs::exists(lf, ec))
            for (auto& l : loc_csv_read(read_text(lf))) liveText[l.key] = l.text;
        for (auto& l : lines) {
            Entry e;
            e.id = source_key(s) + "|" + l.key;
            e.src = idx;
            e.key = l.key;
            e.en = l.text;
            auto lv = liveText.find(l.key);
            if (lv != liveText.end() && lv->second != l.text) e.vi = lv->second;  // already translated in game
            out.push_back(std::move(e));
        }
        log("    + " + s.label() + " - " + std::to_string(lines.size()) + " dong");
    }
}

void Project::inject_unity_loc(const LogFn& log) {
    bool any = false;
    for (auto& s : sources) if (s.kind == "unityloc") any = true;
    if (!any) return;
    std::map<int, std::map<std::string, std::string>> tr;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : entries)
            if (e.src >= 0 && !e.vi.empty() && (size_t)e.src < sources.size() && sources[e.src].kind == "unityloc")
                tr[e.src][e.key] = e.vi;
    }
    std::error_code ec;
    fs::path live = unity_loc_live();
    fs::create_directories(live, ec);
    int totalLines = 0, totalTr = 0;
    for (size_t i = 0; i < sources.size(); i++) {
        const TextSource& s = sources[i];
        if (s.kind != "unityloc") continue;
        try {
            auto lines = loc_csv_read(read_text(dataDir / u8path(s.file)));
            int n = 0;
            if (s.enabled) {
                auto& t = tr[(int)i];
                for (auto& l : lines) {
                    auto it = t.find(l.key);
                    if (it != t.end()) { l.text = it->second; n++; }
                }
            }
            write_file(live / u8path(s.asset), loc_csv_write(lines));  // disabled: template text, so ids still exist
            totalLines += (int)lines.size();
            totalTr += n;
            log("  " + s.label() + ": ghi " + std::to_string(n) + " dong");
        } catch (const std::exception& ex) {
            log("  ! LOI khi ghi " + s.file + ": " + ex.what());
        }
    }
    write_file(live / "meta.txt", "{\"_visibleName\":\"Ti\xe1\xba\xbfng Vi\xe1\xbb\x87t\",\"_author\":\"\"}");
    write_file(live / kUnityLocMarker,
               "unitytl: thu muc nay duoc sinh tu template 'english' - 'Khoi phuc file goc' se xoa\n");
    log("Da ghi " + std::to_string(totalTr) + "/" + std::to_string(totalLines) + " dong vao " + rel(live));
}

void Project::scan_unity_tr(std::vector<TextSource>& found, std::vector<Entry>& out, const LogFn& log) {
    std::error_code ec;
    fs::path trDir = dataDir / "StreamingAssets" / "Translations";
    if (!fs::is_directory(trDir, ec)) return;
    fs::path tpl;
    for (auto& e : fs::directory_iterator(trDir, ec)) {
        if (ec) break;
        if (e.is_directory(ec) && is_english(pathstr(e.path().filename()))) { tpl = e.path(); break; }
    }
    if (tpl.empty()) return;
    std::map<std::string, TextSource> prev;
    for (auto& s : sources) prev[source_key(s)] = s;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(tpl, ec))
        if (!ec && e.is_regular_file(ec) && lower(pathstr(e.path().extension())) == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    fs::path live = unity_tr_live();
    for (auto& f : files) {
        Json j;
        try {
            j = Json::parse(strip_bom_json(read_text(f)));
        } catch (const std::exception& ex) {
            log("    ! " + pathstr(f.filename()) + ": " + ex.what());
            continue;
        }
        if (!j.is_object() || j.members().empty()) continue;
        TextSource s;
        s.file = rel(f);
        s.kind = "unitytr";
        s.asset = pathstr(f.filename());
        s.count = (int)j.members().size();
        auto p = prev.find(source_key(s));
        if (p != prev.end()) s.enabled = p->second.enabled;
        int idx = (int)found.size();
        found.push_back(s);
        if (!s.enabled) {
            log("    - " + s.label() + " (tat - bat trong danh sach nguon neu can)");
            continue;
        }
        std::unordered_map<std::string, std::string> liveText;
        fs::path lf = live / f.filename();
        if (fs::exists(lf, ec)) {
            try {
                Json lj = Json::parse(strip_bom_json(read_text(lf)));
                if (lj.is_object())
                    for (auto& kv : lj.members())
                        if (kv.second.is_string()) liveText[kv.first] = kv.second.str();
            } catch (const std::exception&) {
            }
        }
        for (auto& kv : j.members()) {
            if (!kv.second.is_string()) continue;
            Entry e;
            e.id = source_key(s) + "|" + kv.first;
            e.src = idx;
            e.key = kv.first;
            e.en = kv.second.str();
            auto lv = liveText.find(kv.first);
            if (lv != liveText.end() && lv->second != e.en) e.vi = lv->second;  // already translated in game
            out.push_back(std::move(e));
        }
        log("    + " + s.label() + " - " + std::to_string(j.members().size()) + " dong");
    }
}

void Project::inject_unity_tr(const LogFn& log) {
    bool any = false;
    for (auto& s : sources) if (s.kind == "unitytr") any = true;
    if (!any) return;
    std::map<int, std::map<std::string, std::string>> tr;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : entries)
            if (e.src >= 0 && !e.vi.empty() && (size_t)e.src < sources.size() && sources[e.src].kind == "unitytr")
                tr[e.src][e.key] = e.vi;
    }
    std::error_code ec;
    fs::path live = unity_tr_live();
    fs::create_directories(live, ec);
    int totalLines = 0, totalTr = 0;
    for (size_t i = 0; i < sources.size(); i++) {
        const TextSource& s = sources[i];
        if (s.kind != "unitytr") continue;
        try {
            Json j = Json::parse(strip_bom_json(read_text(dataDir / u8path(s.file))));
            int n = 0;
            if (s.enabled && j.is_object()) {
                auto& t = tr[(int)i];
                for (auto& kv : j.members()) {
                    if (!kv.second.is_string()) continue;
                    auto it = t.find(kv.first);
                    if (it != t.end()) {
                        kv.second = Json(it->second);
                        n++;
                    }
                }
            }
            write_file(live / u8path(s.asset), j.dump(2));  // disabled: template text, so ids still exist
            totalLines += (int)j.members().size();
            totalTr += n;
            log("  " + s.label() + ": ghi " + std::to_string(n) + " dong");
        } catch (const std::exception& ex) {
            log("  ! LOI khi ghi " + s.file + ": " + ex.what());
        }
    }
    fs::path langFile = dataDir / "StreamingAssets" / "Translations" / "languages.json";
    if (fs::exists(langFile, ec)) {
        try {
            if (!fs::exists(bak_of(langFile), ec)) fs::copy_file(langFile, bak_of(langFile));
            Json lj = Json::parse(strip_bom_json(read_text(bak_of(langFile))));
            auto langs = lj.find("languages");
            bool has = false;
            if (langs && langs->is_array())
                for (auto& it : langs->items())
                    if (it.is_string() && it.str() == "Vietnamese") has = true;
            if (langs && langs->is_array() && !has) {
                langs->push(Json("Vietnamese"));
                write_file(langFile, lj.dump(2));
                log("  languages.json: them \"Vietnamese\"");
            }
        } catch (const std::exception& ex) {
            log("  ! languages.json: " + std::string(ex.what()));
        }
    }
    write_file(live / kUnityTrMarker,
               "unitytl: thu muc nay duoc sinh tu template 'English' - 'Khoi phuc file goc' se xoa\n");
    log("Da ghi " + std::to_string(totalTr) + "/" + std::to_string(totalLines) + " dong vao " + rel(live));
}

void Project::scan_unity_csv(std::vector<TextSource>& found, std::vector<Entry>& out, const LogFn& log) {
    std::error_code ec;
    fs::path locDir = dataDir / "StreamingAssets" / "Localization";
    if (!fs::is_directory(locDir, ec)) return;
    fs::path tpl;
    for (auto& e : fs::directory_iterator(locDir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec) || lower(pathstr(e.path().extension())) != ".csv") continue;
        if (is_english(pathstr(e.path().stem()))) { tpl = e.path(); break; }
    }
    if (tpl.empty()) return;
    std::map<std::string, TextSource> prev;
    for (auto& s : sources) prev[source_key(s)] = s;
    std::string text = read_text(tpl);
    auto rows = csv_parse(text.substr(csv_bom(text)), ',');
    if (rows.size() < 2) return;
    int keyCol = csv_find_column(rows, "key");
    int valCol = csv_find_column(rows, "value");
    if (keyCol < 0 || valCol < 0) return;
    TextSource s;
    s.file = rel(tpl);
    s.kind = "unitycsv";
    s.asset = pathstr(tpl.filename());
    s.count = (int)rows.size() - 1;
    auto p = prev.find(source_key(s));
    if (p != prev.end()) s.enabled = p->second.enabled;
    int idx = (int)found.size();
    found.push_back(s);
    if (!s.enabled) {
        log("    - " + s.label() + " (tat - bat trong danh sach nguon neu can)");
        return;
    }
    fs::path live = tpl.parent_path() / "Vietnamese.csv";
    std::unordered_map<std::string, std::string> liveText;
    if (fs::exists(live, ec)) {
        std::string lt = read_text(live);
        auto lrows = csv_parse(lt.substr(csv_bom(lt)), ',');
        int lk = csv_find_column(lrows, "key"), lv = csv_find_column(lrows, "value");
        if (lk >= 0 && lv >= 0)
            for (size_t r = 1; r < lrows.size(); r++)
                if ((size_t)lk < lrows[r].size() && (size_t)lv < lrows[r].size())
                    liveText[lrows[r][lk].value] = lrows[r][lv].value;
    }
    int n = 0;
    for (size_t r = 1; r < rows.size(); r++) {
        if ((size_t)keyCol >= rows[r].size() || (size_t)valCol >= rows[r].size()) continue;
        const std::string& key = rows[r][keyCol].value;
        const std::string& rawVal = rows[r][valCol].value;
        if (key.empty() || rawVal.empty()) continue;
        Entry e;
        e.id = source_key(s) + "|" + key;
        e.src = idx;
        e.key = key;
        e.en = unescape_bslash_space(rawVal);
        auto lv = liveText.find(key);
        if (lv != liveText.end()) {
            std::string liveVal = unescape_bslash_space(lv->second);
            if (liveVal != e.en) e.vi = liveVal;  // already translated in game
        }
        out.push_back(std::move(e));
        n++;
    }
    log("    + " + s.label() + " - " + std::to_string(n) + " dong");
}

void Project::inject_unity_csv(const LogFn& log) {
    bool any = false;
    for (auto& s : sources) if (s.kind == "unitycsv") any = true;
    if (!any) return;
    std::map<int, std::map<std::string, std::string>> tr;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : entries)
            if (e.src >= 0 && !e.vi.empty() && (size_t)e.src < sources.size() && sources[e.src].kind == "unitycsv")
                tr[e.src][e.key] = e.vi;
    }
    int totalLines = 0, totalTr = 0;
    fs::path liveOut;
    for (size_t i = 0; i < sources.size(); i++) {
        const TextSource& s = sources[i];
        if (s.kind != "unitycsv") continue;
        try {
            std::string text = read_text(dataDir / u8path(s.file));
            size_t bomLen = csv_bom(text);
            std::string body = text.substr(bomLen);
            auto rows = csv_parse(body, ',');
            int keyCol = csv_find_column(rows, "key");
            int valCol = csv_find_column(rows, "value");
            if (keyCol < 0 || valCol < 0) throw ParseError("khong thay cot key/value");
            std::string out = text.substr(0, bomLen);
            size_t prev = 0;
            int n = 0;
            if (s.enabled) {
                auto& t = tr[(int)i];
                for (size_t r = 1; r < rows.size(); r++) {
                    if ((size_t)keyCol >= rows[r].size() || (size_t)valCol >= rows[r].size()) continue;
                    auto it = t.find(rows[r][keyCol].value);
                    if (it == t.end() || it->second.empty()) continue;
                    const CsvField& f = rows[r][valCol];
                    bool escaped = f.value.find("\\ ") != std::string::npos;
                    std::string newVal = escaped ? escape_bslash_space(it->second) : it->second;
                    if (f.value == newVal) continue;
                    out += body.substr(prev, f.start - prev);
                    out += csv_encode(newVal, ',');
                    prev = f.end;
                    n++;
                }
            }
            out += body.substr(prev);  // disabled: template text, so ids still exist
            liveOut = (dataDir / u8path(s.file)).parent_path() / "Vietnamese.csv";
            write_file(liveOut, out);
            totalLines += (int)rows.size() - 1;
            totalTr += n;
            log("  " + s.label() + ": ghi " + std::to_string(n) + " dong");
        } catch (const std::exception& ex) {
            log("  ! LOI khi ghi " + s.file + ": " + ex.what());
        }
    }
    if (!liveOut.empty()) log("Da ghi " + std::to_string(totalTr) + "/" + std::to_string(totalLines) + " dong vao " + rel(liveOut));
}

// ------------------------------------------------------------------ Unreal Engine (unreal.cpp)
// Finds every localization target's source-culture .locres inside the game's own (never touched) pak -
// repak + UnrealLocres do the actual pak/locres reading. One TextSource per target. No "live" file to
// diff against, like unityloc/unitytr/unitycsv above: the translation state lives entirely in
// entries[].vi, and inject_unreal always regenerates the vi .locres fresh from the source CSV + that
// state rather than comparing against anything on disk (there's nothing to compare against - the
// original pak never gets a "vi" culture, only our own small supplementary pak does).
void Project::scan_unreal(std::vector<TextSource>& found, std::vector<Entry>& out, const LogFn& log) {
    UnrealTools tools = find_unreal_tools();
    if (!tools.ok()) {
        log("Khong thay repak.exe / UnrealLocres.exe trong thu muc tools/ canh unitytl.");
        return;
    }
    log("repak list " + pathstr(unrealPak.filename()) + " ...");
    auto list = unreal_pak_list(tools, unrealPak, log);
    if (list.empty()) return;
    auto targets = unreal_find_targets(list);
    if (targets.empty()) {
        log("Khong thay .locres nao trong pak.");
        return;
    }
    std::map<std::string, TextSource> prev;
    for (auto& s : sources) prev[source_key(s)] = s;
    fs::path extractDir = unreal_scratch() / "extract";
    for (auto& t : targets) {
        log("  " + t.name + " (" + t.sourceCulture + ") ...");
        auto rows = unreal_export_csv(tools, unrealPak, t.locresPak, extractDir, log);
        if (rows.empty()) continue;
        TextSource s;
        s.file = t.locresPak;
        s.kind = "unreallocres";
        s.asset = t.name;
        s.chosen = {t.sourceCulture};
        s.count = (int)rows.size();
        auto p = prev.find(source_key(s));
        if (p != prev.end()) s.enabled = p->second.enabled;
        int idx = (int)found.size();
        found.push_back(s);
        if (!s.enabled) {
            log("    - " + s.label() + " (tat - bat trong danh sach nguon neu can)");
            continue;
        }
        for (auto& r : rows) {
            Entry e;
            e.id = source_key(s) + "|" + r.key;
            e.src = idx;
            e.key = r.key;
            e.en = r.source;
            out.push_back(std::move(e));
        }
        log("    + " + s.label() + " - " + std::to_string(rows.size()) + " dong");
    }
}

void Project::inject_unreal(const LogFn& log) {
    bool any = false;
    for (auto& s : sources)
        if (s.kind == "unreallocres" && s.enabled) any = true;
    if (!any) {
        log("Khong co nguon Unreal nao de ghi (chua quet, hoac tat ca da bi tat).");
        return;
    }
    UnrealTools tools = find_unreal_tools();
    if (!tools.ok()) {
        log("Khong thay repak.exe / UnrealLocres.exe trong thu muc tools/ canh unitytl.");
        return;
    }
    fs::path extractDir = unreal_scratch() / "extract";
    fs::path buildDir = unreal_scratch() / "build";
    std::error_code ec;
    fs::remove_all(buildDir, ec);
    fs::path inputDir = buildDir / "input";
    std::set<std::string> locmetaDone;

    for (size_t i = 0; i < sources.size(); i++) {
        TextSource& s = sources[i];
        if (s.kind != "unreallocres" || !s.enabled) continue;
        std::string target = s.asset;
        std::string srcCulture = s.chosen.empty() ? "en" : s.chosen[0];
        std::string suffix = "/" + srcCulture + "/" + target + ".locres";
        if (!ends_with(s.file, suffix)) {
            log("  ! duong dan khong dung dang mong doi: " + s.file);
            continue;
        }
        std::string prefixWithTarget = s.file.substr(0, s.file.size() - suffix.size());

        auto enRows = unreal_export_csv(tools, unrealPak, s.file, extractDir, log);  // always fresh
        if (enRows.empty()) {
            log("  ! khong doc lai duoc " + s.file);
            continue;
        }
        std::map<std::string, std::string> tr;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (auto& e : entries)
                if (e.src == (int)i && !e.vi.empty()) tr[e.key] = e.vi;
        }
        std::vector<UnrealRow> rows;
        rows.reserve(enRows.size());
        int nTr = 0;
        for (auto& r : enRows) {
            UnrealRow row = r;
            auto it = tr.find(r.key);
            if (it != tr.end()) {
                row.target = it->second;
                nTr++;
            }
            rows.push_back(std::move(row));
        }

        fs::path localSrc = extractDir / u8path(s.file);
        fs::path outLocres = buildDir / "out" / u8path(target) / "Game_vi.locres";
        if (!unreal_import_csv(tools, localSrc, rows, outLocres, log)) continue;
        fs::path dest = inputDir / u8path(prefixWithTarget + "/vi/" + target + ".locres");
        fs::create_directories(dest.parent_path(), ec);
        write_file(dest, read_file(outLocres));
        log("  " + s.label() + ": ghi " + std::to_string(nTr) + "/" + std::to_string(rows.size()) + " dong");

        if (!locmetaDone.count(prefixWithTarget)) {
            locmetaDone.insert(prefixWithTarget);
            std::string locmetaRel = prefixWithTarget + "/" + target + ".locmeta";
            if (unreal_unpack_file(tools, unrealPak, locmetaRel, extractDir, log)) {
                try {
                    Bytes patched = unreal_patch_locmeta(read_file(extractDir / u8path(locmetaRel)));
                    fs::path locmetaDest = inputDir / u8path(locmetaRel);
                    fs::create_directories(locmetaDest.parent_path(), ec);
                    write_file(locmetaDest, patched);
                } catch (const std::exception& ex) {
                    log("  ! khong doc duoc " + locmetaRel + ": " + ex.what());
                }
            }
        }
    }

    // Vietnamese glyphs for every loose .ufont the pak holds (patched once, reused by every target).
    auto list = unreal_pak_list(tools, unrealPak, log);
    fs::path fontOut = inputDir;
    unreal_patch_fonts(tools, unrealPak, list, extractDir, fontOut, log);

    fs::path outPak = unrealPak.parent_path() / "zzz_vi_P.pak";
    std::error_code ec2;
    if (fs::exists(outPak, ec2)) {
        fs::remove(outPak, ec2);
        if (ec2) {
            log("  ! khong xoa duoc " + pathstr(outPak) + " (game dang mo?) - dong game roi 'GHI VAO GAME' lai.");
            return;
        }
    }
    if (!fs::exists(inputDir, ec)) {
        log("  Khong co gi de dong goi.");
        return;
    }
    if (unreal_pack(tools, inputDir, outPak, log))
        log("Da ghi " + pathstr(outPak) + ". Bam 'Ep Culture=vi' (o ben duoi) truoc khi mo game lan dau.");
    else
        log("  ! repak pack that bai.");
}

bool Project::unreal_force_culture(const LogFn& log) { return ::unreal_force_culture(unrealProjectName, log); }
bool Project::unreal_culture_forced() const { return ::unreal_culture_forced(unrealProjectName); }

// ------------------------------------------------------------------ persistence
void Project::save() const {
    Json j = Json::object();
    j["app"] = Json("unitytl");
    j["version"] = Json(1);
    Json src = Json::array();
    Json ents = Json::array();
    Json sizes = Json::object();
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& s : sources) src.push(source_to_json(s));
        for (auto& e : entries) {
            Json x = Json::object();
            x["id"] = Json(e.id);
            x["en"] = Json(e.en);
            x["vi"] = Json(e.vi);
            ents.push(std::move(x));
        }
        for (auto& kv : catalogSizes) sizes[kv.first] = Json((int64_t)kv.second);
    }
    j["sources"] = src;
    j["catalogSizes"] = sizes;
    if (gm) {
        j["gmFont"] = Json(gmFont);
        j["gmPixelFont"] = Json(gmPixelFont);
        j["gmFold3"] = Json(gmFold3);
    }
    j["entries"] = ents;
    write_file(work_file(), j.dump(1));
}

void Project::load() {
    std::error_code ec;
    if (!fs::exists(work_file(), ec)) return;
    Json j = Json::parse(read_text(work_file()));
    std::lock_guard<std::mutex> lk(mu);
    sources.clear();
    entries.clear();
    std::map<std::string, int> idx;
    for (auto& s : j.at("sources").items()) {
        sources.push_back(source_from_json(s));
        idx[source_key(sources.back())] = (int)sources.size() - 1;
    }
    if (auto f = j.find("gmFont")) gmFont = f->str();
    if (auto f = j.find("gmPixelFont")) gmPixelFont = f->str();
    if (auto f = j.find("gmFold3")) gmFold3 = f->boolean();
    if (auto cs = j.find("catalogSizes"))
        for (auto& kv : cs->members()) catalogSizes[kv.first] = (uint64_t)kv.second.int64();
    for (auto& x : j.at("entries").items()) {
        Entry e;
        e.id = x.get_str("id");
        e.en = x.get_str("en");
        e.vi = x.get_str("vi");
        size_t p = e.id.rfind('|');
        // id = file|node|pathId|key ; key itself may contain '|' so match the source prefix instead
        for (auto& [k, i] : idx)
            if (e.id.compare(0, k.size() + 1, k + "|") == 0) {
                e.src = i;
                e.key = e.id.substr(k.size() + 1);
                break;
            }
        (void)p;
        entries.push_back(std::move(e));
    }
}

int Project::translated_count() const {
    std::lock_guard<std::mutex> lk(mu);
    int n = 0;
    for (auto& e : entries) n += !e.vi.empty();
    return n;
}

// ------------------------------------------------------------------ translate
void Project::translate(bool all, std::atomic<bool>& stop, std::atomic<int>& done, std::atomic<int>& total,
                        const LogFn& log, EngineConfig engine) {
    if (engine.kind == EngineConfig::Local && !local_server_ready(engine.port)) {
        log("Model local chua chay (khong ket noi duoc 127.0.0.1:" + std::to_string(engine.port) +
            "). Bat model truoc roi dich lai.");
        return;
    }
    Translator tr(glossary, engine);
    std::vector<size_t> todo;
    std::unordered_map<std::string, std::string> cache;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = 0; i < entries.size(); i++) {
            bool srcOn = entries[i].src < 0 || sources[entries[i].src].enabled;
            if (!srcOn) continue;
            if (all || entries[i].vi.empty()) todo.push_back(i);
            else cache.emplace(entries[i].en, entries[i].vi);
        }
    }
    total = (int)todo.size();
    done = 0;
    int failed = 0;
    auto lastSave = std::chrono::steady_clock::now();
    for (size_t i : todo) {
        if (stop) break;
        std::string en;
        {
            std::lock_guard<std::mutex> lk(mu);
            en = entries[i].en;
        }
        std::optional<std::string> vi;
        auto c = cache.find(en);
        if (c != cache.end()) vi = c->second;
        else {
            vi = tr.translate(en);
            if (engine.kind == EngineConfig::Google)  // be gentle with the free endpoint
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (vi) cache[en] = *vi;
        }
        if (!vi) {
            failed++;
            log("  ! khong dich duoc (" + tr.last_error() + "): " + en.substr(0, 60));
        } else {
            std::lock_guard<std::mutex> lk(mu);
            entries[i].vi = *vi == en ? "" : *vi;
        }
        done++;
        if (std::chrono::steady_clock::now() - lastSave > std::chrono::seconds(10)) {
            save();
            lastSave = std::chrono::steady_clock::now();
        }
    }
    save();
    log(std::string(stop ? "Da dung. " : "Dich xong. ") + std::to_string(done) + "/" + std::to_string(total.load()) +
        " dong, loi " + std::to_string(failed) + ".");
}

void Project::translate_rows(const std::vector<int>& indices, std::atomic<bool>& stop, std::atomic<int>& done,
                             std::atomic<int>& total, const LogFn& log, EngineConfig engine) {
    if (engine.kind == EngineConfig::Local && !local_server_ready(engine.port)) {
        log("Model local chua chay (khong ket noi duoc 127.0.0.1:" + std::to_string(engine.port) +
            "). Bat model truoc roi dich lai.");
        return;
    }
    Translator tr(glossary, engine);
    std::vector<size_t> todo;
    std::unordered_map<std::string, std::string> cache;
    {
        std::lock_guard<std::mutex> lk(mu);
        std::unordered_set<int> wanted(indices.begin(), indices.end());
        for (size_t i = 0; i < entries.size(); i++) {
            bool srcOn = entries[i].src < 0 || sources[entries[i].src].enabled;
            if (wanted.count((int)i) && srcOn && entries[i].vi.empty()) todo.push_back(i);
            else if (!entries[i].vi.empty()) cache.emplace(entries[i].en, entries[i].vi);
        }
    }
    total = (int)todo.size();
    done = 0;
    int failed = 0;
    for (size_t i : todo) {
        if (stop) break;
        std::string en;
        {
            std::lock_guard<std::mutex> lk(mu);
            en = entries[i].en;
        }
        std::optional<std::string> vi;
        auto c = cache.find(en);
        if (c != cache.end()) vi = c->second;
        else {
            vi = tr.translate(en);
            if (engine.kind == EngineConfig::Google)  // be gentle with the free endpoint
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (vi) cache[en] = *vi;
        }
        if (!vi) {
            failed++;
            log("  ! khong dich duoc (" + tr.last_error() + "): " + en.substr(0, 60));
        } else {
            std::lock_guard<std::mutex> lk(mu);
            entries[i].vi = *vi == en ? "" : *vi;
        }
        done++;
    }
    save();
    log(std::string(stop ? "Da dung. " : "Dich xong. ") + std::to_string(done) + "/" + std::to_string(total.load()) +
        " dong, loi " + std::to_string(failed) + ".");
}

// ------------------------------------------------------------------ inject / restore
void Project::inject(const LogFn& log) {
    if (gm) return inject_gm(log);
    if (godot) return inject_godot(log);
    if (renpy) return inject_renpy(log);
    if (unreal) return inject_unreal(log);
    inject_unity_loc(log);
    inject_unity_tr(log);
    inject_unity_csv(log);
    std::map<std::string, std::vector<int>> byFile;
    std::map<std::string, std::map<std::string, std::string>> trBySource;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = 0; i < sources.size(); i++)
            if (sources[i].enabled && sources[i].kind != "unityloc" && sources[i].kind != "unitytr" &&
                sources[i].kind != "unitycsv")
                byFile[sources[i].file].push_back((int)i);
        for (auto& e : entries)
            if (e.src >= 0 && !e.vi.empty()) trBySource[source_key(sources[e.src])][e.key] = e.vi;
    }
    std::error_code ec;
    fs::path catalog = dataDir / "StreamingAssets" / "aa" / "catalog.bin";
    fs::path catalogJson = dataDir / "StreamingAssets" / "aa" / "catalog.json";
    bool useCatalogJson = !fs::exists(catalog, ec) && fs::exists(catalogJson, ec);
    for (auto& [file, idxs] : byFile) {
        fs::path live = dataDir / u8path(file);
        try {
            if (!fs::exists(bak_of(live), ec)) fs::copy_file(live, bak_of(live));
            Container c = Container::load(bak_of(live));
            for (int i : idxs) {
                const TextSource& s = sources[i];
                SerializedFile* sf = c.get(s.node);
                if (!sf) throw ParseError("khong thay " + s.node);
                int n = apply_source(*sf, s, trBySource[source_key(s)]);
                log("  " + s.label() + ": ghi " + std::to_string(n) + " dong");
            }
            Bytes out = c.save();
            uint64_t prevLive = fs::file_size(live, ec);
            write_file(live, out);
            if (c.isBundle && file.find("aa") != std::string::npos && (fs::exists(catalog, ec) || useCatalogJson)) {
                std::vector<uint64_t> sizes;
                if (catalogSizes.count(file)) sizes.push_back(catalogSizes[file]);
                sizes.push_back(prevLive);
                sizes.push_back(fs::file_size(bak_of(live), ec));
                const char* label = useCatalogJson ? "catalog.json" : "catalog.bin";
                CatalogPatch r = useCatalogJson ? patch_catalog_json(catalogJson, sizes, out.size())
                                                 : patch_catalog(catalog, pathstr(live.filename()), sizes, out.size());
                if (r.ok) {
                    catalogSizes[file] = out.size();
                    log(std::string("  ") + label + ": tat kiem tra CRC cho " + pathstr(live.filename()));
                } else
                    log(std::string("  ! ") + label + ": " + r.error + " -> game co the bao loi CRC / man hinh den");
            }
        } catch (const std::exception& ex) {
            log("  ! LOI khi ghi " + file + ": " + ex.what() + " (file game khong bi thay doi)");
        }
    }
    inject_unity_fonts(log);
    save();
    log("Ghi vao game xong. Mo game de kiem tra; neu loi bam 'Xem Player.log'.");
}

// Static TMP SDF fonts missing Vietnamese letters (unity_font.cpp): only worth the (large, uncompressed-
// bundle) cost when some translated line is actually about to show Vietnamese text. Runs over every
// candidate bundle/.assets file, same list the text scan uses; files a font asset isn't found in are
// left completely untouched (no decompress-recompress bloat).
void Project::inject_unity_fonts(const LogFn& log) {
    bool wide = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : entries) {
            if (e.vi.empty()) continue;
            if (std::any_of(e.vi.begin(), e.vi.end(), [](char c) { return (unsigned char)c >= 0x80; })) {
                wide = true;
                break;
            }
        }
    }
    if (!wide) return;
    std::error_code ec;
    fs::path catalog = dataDir / "StreamingAssets" / "aa" / "catalog.bin";
    fs::path catalogJson = dataDir / "StreamingAssets" / "aa" / "catalog.json";
    bool useCatalogJson = !fs::exists(catalog, ec) && fs::exists(catalogJson, ec);
    int totalFonts = 0;
    for (auto& live : candidate_files()) {
        try {
            if (!fs::exists(bak_of(live), ec)) fs::copy_file(live, bak_of(live));
            Container c = Container::load(live);
            bool changed = false;
            for (auto& [node, sf] : c.files) {
                int n = tmp_patch_vietnamese(sf, c.bundle, log);
                if (n > 0) {
                    changed = true;
                    totalFonts += n;
                }
            }
            if (!changed) continue;
            Bytes out = c.save();
            uint64_t prevLive = fs::file_size(live, ec);
            write_file(live, out);
            std::string relFile = rel(live);
            if (c.isBundle && relFile.find("aa") != std::string::npos && (fs::exists(catalog, ec) || useCatalogJson)) {
                std::vector<uint64_t> sizes;
                if (catalogSizes.count(relFile)) sizes.push_back(catalogSizes[relFile]);
                sizes.push_back(prevLive);
                sizes.push_back(fs::file_size(bak_of(live), ec));
                const char* label = useCatalogJson ? "catalog.json" : "catalog.bin";
                CatalogPatch r = useCatalogJson ? patch_catalog_json(catalogJson, sizes, out.size())
                                                 : patch_catalog(catalog, pathstr(live.filename()), sizes, out.size());
                if (r.ok) {
                    catalogSizes[relFile] = out.size();
                    log(std::string("  ") + label + ": tat kiem tra CRC cho " + pathstr(live.filename()));
                } else
                    log(std::string("  ! ") + label + ": " + r.error + " -> game co the bao loi CRC / man hinh den");
            }
        } catch (const std::exception& ex) {
            log("  ! LOI font trong " + rel(live) + ": " + ex.what());
        }
    }
    if (totalFonts > 0) log("Da them chu tieng Viet vao " + std::to_string(totalFonts) + " font TMP.");
}

void Project::inject_gm(const LogFn& log) {
    std::u32string used;
    std::error_code ec;
    for (size_t i = 0; i < sources.size(); i++) {
        const TextSource& s = sources[i];
        if (!s.enabled || (s.kind != "gmini" && s.kind != "gmjson" && s.kind != "gmcsv")) continue;
        std::map<std::string, std::string> tr;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (auto& e : entries)
                if (e.src == (int)i && !e.vi.empty()) {
                    tr[e.key] = gmFold3 ? gm_fold_3byte(e.vi) : e.vi;
                    used += utf8_to_u32(tr[e.key]);
                }
        }
        fs::path live = root / u8path(s.file);
        try {
            if (!fs::exists(bak_of(live), ec)) fs::copy_file(live, bak_of(live));
            int n = 0;
            std::string orig = read_text(bak_of(live));
            std::string out = s.kind == "gmjson" ? gm_json_apply(orig, s.chosen.at(0), tr, n)
                             : s.kind == "gmcsv"  ? gm_csv_apply(orig, s.chosen.at(0), tr, n)
                                                 : gm_ini_apply(orig, s.chosen.at(0), tr, n);
            write_file(live, out);
            log("  " + s.label() + ": ghi " + std::to_string(n) + " dong");
            // Fields of Mistria: checksums.json records each data file's byte size ("localization": 4126486)
            fs::path sums = root / "checksums.json";
            std::string stem = pathstr(live.stem());
            if (fs::exists(sums, ec)) {
                if (!fs::exists(bak_of(sums), ec)) fs::copy_file(sums, bak_of(sums));
                std::string t = read_text(bak_of(sums));
                std::string old = "\"" + stem + "\": " + std::to_string(orig.size());
                size_t at = t.find(old);
                if (at != std::string::npos) {
                    t.replace(at, old.size(), "\"" + stem + "\": " + std::to_string(out.size()));
                    write_file(sums, t);
                    log("  checksums.json: " + stem + " = " + std::to_string(out.size()) + " byte");
                }
            }
        } catch (const std::exception& ex) {
            log("  ! LOI khi ghi " + s.file + ": " + ex.what());
        }
    }
    // fonts: only when the text uses letters beyond ASCII
    bool wide = std::any_of(used.begin(), used.end(), [](char32_t c) { return c > 0x7E; });
    fs::path win = root / "data.win";
    if (wide) {
        try {
            if (!fs::exists(bak_of(win), ec)) {
                log("  sao luu data.win -> data.win.bak ...");
                fs::copy_file(win, bak_of(win));
            }
            Bytes d = read_file(bak_of(win));
            log("  ve font co dau tieng Viet (" + gmFont + ") vao data.win ...");
            if (gm_patch_fonts(d, used, gmFont, gmPixelFont, log, gmFold3)) {
                write_file(win, d);
                log("  data.win: da thay font.");
            } else {
                log("  data.win: font cua game da du chu, khong can thay.");
            }
        } catch (const std::exception& ex) {
            log("  ! LOI khi va font data.win: " + std::string(ex.what()) + " (data.win khong bi thay doi)");
        }
    }
    save();
    log("Ghi vao game xong. Mo game de kiem tra; neu game khong mo duoc bam 'Khoi phuc file goc'.");
}

// Fonts of the pack (imported .fontdata) whose TrueType file lacks Vietnamese letters get them added as
// composite glyphs (ttf.h). Slay the Spire 2: Kreon (body text) has none of U+1EA0-U+1EF9.
static void inject_godot_fonts(const Pck& pck, const fs::path& file, std::map<std::string, Bytes>& replace,
                               const LogFn& log) {
    for (auto& pf : pck.files) {
        if ((pf.flags & 3) || pf.path.size() < 9 || pf.path.compare(pf.path.size() - 9, 9, ".fontdata") != 0) continue;
        if (pf.size > 32ull * 1024 * 1024) continue;  // CJK fonts (+ their glyph cache): never Latin text fonts
        std::string name = pathstr(u8path(pf.path).filename());
        name = name.substr(0, name.rfind('-'));
        try {
            Bytes res = pck.read(file, pf), ttf;
            fontdata_ttf(res, ttf);
            int miss = ttf_missing_vietnamese(ttf);
            if (miss <= 0) continue;
            log("  font " + name + ": thieu " + std::to_string(miss) + " chu tieng Viet");
            if (ttf_add_vietnamese(ttf, log)) replace[pf.path] = fontdata_with_ttf(res, ttf);
        } catch (const std::exception& ex) {
            log("  ! font " + name + ": " + ex.what() + " (giu nguyen)");
        }
    }
}

void Project::inject_godot(const LogFn& log) {
    std::map<std::string, std::vector<int>> byFile;
    std::map<int, std::map<std::string, std::string>> trBySource;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = 0; i < sources.size(); i++)
            if (sources[i].enabled && sources[i].kind == "godotjson") byFile[sources[i].file].push_back((int)i);
        for (auto& e : entries)
            if (e.src >= 0 && !e.vi.empty()) trBySource[e.src][e.key] = e.vi;
    }
    std::error_code ec;
    for (auto& [file, idxs] : byFile) {
        fs::path live = root / u8path(file);
        try {
            if (!fs::exists(bak_of(live), ec)) {
                log("  sao luu " + file + " -> " + file + ".bak ...");
                fs::copy_file(live, bak_of(live));
            }
            Pck pck = Pck::load(bak_of(live));
            std::map<std::string, Bytes> replace;
            for (int i : idxs) {
                const TextSource& s = sources[i];
                const PckFile* pf = pck.find(s.node);
                if (!pf) throw ParseError("khong thay " + s.node + " trong " + file + ".bak");
                Bytes b = pck.read(bak_of(live), *pf);
                int n = 0;
                std::string text = json_flat_apply(std::string(b.begin(), b.end()), trBySource[i], n);
                if (n) replace[s.node] = Bytes(text.begin(), text.end());
                log("  " + s.label() + ": ghi " + std::to_string(n) + " dong");
            }
            bool wide = false;
            for (int i : idxs)
                for (auto& [k, v] : trBySource[i])
                    wide = wide || std::any_of(v.begin(), v.end(), [](char c) { return (unsigned char)c >= 0x80; });
            if (wide) inject_godot_fonts(pck, bak_of(live), replace, log);
            log("  ghi " + file + " (chep tu .bak, thay " + std::to_string(replace.size()) + " file) ...");
            pck_write(bak_of(live), pck, replace, live);
        } catch (const std::exception& ex) {
            log("  ! LOI khi ghi " + file + ": " + ex.what() + " (file game khong bi thay doi)");
        }
    }
    save();
    log("Ghi vao game xong. Mo game de kiem tra; neu loi bam 'Xem log game'.");
}

// Ren'Py: game/tl/<lang>/ is rebuilt from the template on every inject (nothing of the game is changed).
// Fonts lacking Vietnamese get a patched copy at tl/<lang>/<same path>: Ren'Py looks there first for any
// file while that language is active.
// The file selecting the language sits in game/, not in tl/<lang>/: with config.defer_tl_scripts (Between
// Humanity) Ren'Py loads tl/<lang>/ only once that language is chosen, so a selector in there never ran.
static const char* kRenpyMarker = "zz_unitytl_language.rpy";
static void remove_rpy(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
    fs::remove(fs::path(p).replace_extension(".rpyc"), ec);
}

void Project::inject_renpy(const LogFn& log) {
    std::map<int, std::map<std::string, std::string>> tr;
    bool wide = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : entries)
            if (e.src >= 0 && !e.vi.empty()) {
                tr[e.src][e.key] = e.vi;
                wide = wide || std::any_of(e.vi.begin(), e.vi.end(), [](char c) { return (unsigned char)c >= 0x80; });
            }
    }
    std::error_code ec;
    fs::path tpl = renpy_template(), live = root / "game" / "tl" / u8path(renpyLang);
    fs::create_directories(live, ec);
    for (size_t i = 0; i < sources.size(); i++) {
        const TextSource& s = sources[i];
        if (s.kind != "renpytl") continue;
        try {
            std::string text = read_text(tpl / u8path(s.node));
            int n = 0;
            if (s.enabled) text = renpy_tl_apply(text, tr[(int)i], n);
            fs::create_directories((live / u8path(s.node)).parent_path(), ec);
            write_file(live / u8path(s.node), text);  // disabled: the English template, so the ids still exist
            log("  " + s.label() + ": ghi " + std::to_string(n) + " dong");
        } catch (const std::exception& ex) {
            log("  ! LOI khi ghi " + s.node + ": " + ex.what());
        }
    }
    remove_rpy(live / kRenpyMarker);  // where older unitytl builds put it
    write_file(root / "game" / kRenpyMarker,
               "# unitytl: dung ban dich game/tl/" + renpyLang + " moi khi mo game ('Khoi phuc file goc' xoa ca hai)\n"
               "define config.language = \"" + renpyLang + "\"\n");
    if (wide) {
        // loose fonts under game/ and fonts packed in .rpa archives
        std::vector<std::pair<std::string, Bytes>> fonts;
        fs::path game = root / "game";
        std::error_code walk;
        for (auto it = fs::recursive_directory_iterator(game, walk); !walk && it != fs::recursive_directory_iterator();
             it.increment(walk)) {
            std::string r = pathstr(it->path().lexically_relative(game));
            std::replace(r.begin(), r.end(), '\\', '/');
            if (it->is_directory(ec)) {
                if (r == "tl" || r == "cache" || r == "saves") it.disable_recursion_pending();
                continue;
            }
            std::string ext = lower(pathstr(it->path().extension()));
            if (ext == ".ttf" || ext == ".otf") fonts.push_back({r, read_file(it->path())});
            if (ext == ".rpa") try {
                    for (auto& e : rpa_index(it->path())) {
                        std::string le = lower(e.name);
                        if (le.size() > 4 && (le.compare(le.size() - 4, 4, ".ttf") == 0 ||
                                              le.compare(le.size() - 4, 4, ".otf") == 0))
                            fonts.push_back({e.name, rpa_read(it->path(), e)});
                    }
                } catch (const std::exception& ex) {
                    log("  ! " + r + ": " + ex.what());
                }
        }
        for (auto& [name, ttf] : fonts) {
            int miss = ttf_missing_vietnamese(ttf);
            if (miss <= 0) continue;
            log("  font " + name + ": thieu " + std::to_string(miss) + " chu tieng Viet");
            try {
                if (ttf_add_vietnamese(ttf, log)) {
                    fs::path dst = live / u8path(name);
                    fs::create_directories(dst.parent_path(), ec);
                    write_file(dst, ttf);
                }
            } catch (const std::exception& ex) {
                log("  ! font " + name + ": " + ex.what() + " (giu nguyen)");
            }
        }
    }
    save();
    log("Ghi vao game xong (game/tl/" + renpyLang + "). Mo game de kiem tra; neu loi bam 'Xem log game'.");
}

void Project::restore(const LogFn& log) {
    std::error_code ec;
    int n = 0;
    if (unreal) {
        // Never touches the original pak at all - "restore" is just deleting our own supplementary one.
        fs::path outPak = unrealPak.parent_path() / "zzz_vi_P.pak";
        if (!fs::exists(outPak, ec)) {
            log("Chua co pak dich (" + pathstr(outPak) + ") de xoa.");
            return;
        }
        fs::remove(outPak, ec);
        log(ec ? "  ! khong xoa duoc " + pathstr(outPak) + ": " + ec.message()
               : "Da xoa " + pathstr(outPak) + " (ban dich van con trong _unitytl.json). File pak goc khong bi dong vao.");
        return;
    }
    if (renpy) {
        fs::path live = root / "game" / "tl" / u8path(renpyLang), marker = root / "game" / kRenpyMarker;
        if (!fs::exists(marker, ec) && !fs::exists(live / kRenpyMarker, ec)) {
            log("Khong co ban dich cua unitytl trong " + pathstr(live) + " - khong xoa gi.");
            return;
        }
        remove_rpy(marker);
        fs::remove_all(live, ec);
        log(ec ? "  ! khong xoa duoc " + pathstr(live) + ": " + ec.message()
               : "Da xoa game/tl/" + renpyLang + ": game ve lai tieng Anh. Ban dich van con trong _unitytl.json.");
        return;
    }
    std::vector<fs::path> files;
    for (auto& s : sources) files.push_back(dataDir / u8path(s.file));
    files.push_back(gm ? root / "data.win" : dataDir / "StreamingAssets" / "aa" / "catalog.bin");
    if (gm) files.push_back(root / "checksums.json");
    files.push_back(dataDir / "StreamingAssets" / "Translations" / "languages.json");
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    for (auto& f : files) {
        if (!fs::exists(bak_of(f), ec)) continue;
        fs::copy_file(bak_of(f), f, fs::copy_options::overwrite_existing, ec);
        if (ec) log("  ! khong khoi phuc duoc " + rel(f) + ": " + ec.message());
        else { n++; log("  khoi phuc " + rel(f)); }
    }
    fs::path locLive = unity_loc_live();
    if (fs::exists(locLive / kUnityLocMarker, ec)) {
        fs::remove_all(locLive, ec);
        log(ec ? "  ! khong xoa duoc " + rel(locLive) + ": " + ec.message()
               : "  da xoa " + rel(locLive) + " (ban dich van con trong _unitytl.json)");
    }
    fs::path trLive = unity_tr_live();
    if (fs::exists(trLive / kUnityTrMarker, ec)) {
        fs::remove_all(trLive, ec);
        log(ec ? "  ! khong xoa duoc " + rel(trLive) + ": " + ec.message()
               : "  da xoa " + rel(trLive) + " (ban dich van con trong _unitytl.json)");
    }
    for (auto& s : sources)
        if (s.kind == "unitycsv") {
            fs::path csvLive = (dataDir / u8path(s.file)).parent_path() / "Vietnamese.csv";
            if (fs::exists(csvLive, ec)) {
                fs::remove(csvLive, ec);
                log(ec ? "  ! khong xoa duoc " + rel(csvLive) + ": " + ec.message()
                       : "  da xoa " + rel(csvLive) + " (ban dich van con trong _unitytl.json)");
            }
            break;
        }
    catalogSizes.clear();
    save();
    log("Da khoi phuc " + std::to_string(n) + " file goc. Ban dich van con trong _unitytl.json.");
}

// ------------------------------------------------------------------ Player.log
fs::path Project::player_log() const {
    if (unreal) return unreal_log_path(unrealProjectName);
    if (renpy) {  // traceback.txt (script error, written next to log.txt) says more than log.txt
        std::error_code ec;
        return fs::exists(root / "traceback.txt", ec) ? root / "traceback.txt" : root / "log.txt";
    }
    if (godot) {  // user:// is %APPDATA%/<custom dir> or %APPDATA%/Godot/app_userdata/<project name>
        const char* ad = getenv("APPDATA");
        if (!ad || product.empty()) return {};
        std::error_code ec;
        fs::path custom = u8path(ad) / u8path(product) / "logs" / "godot.log";
        fs::path def = u8path(ad) / "Godot" / "app_userdata" / u8path(product) / "logs" / "godot.log";
        return fs::exists(custom, ec) || !fs::exists(def, ec) ? custom : def;
    }
    const char* up = getenv("USERPROFILE");
    if (!up || company.empty() || product.empty()) return {};
    return u8path(up) / "AppData" / "LocalLow" / u8path(company) / u8path(product) / "Player.log";
}

void Project::check_log(const LogFn& log) const {
    if (gm) {
        log("Game GameMaker khong ghi Player.log. Neu game khong mo duoc, bam 'Khoi phuc file goc'.");
        return;
    }
    fs::path p = player_log();
    std::error_code ec;
    if (p.empty() || !fs::exists(p, ec)) {
        log("Khong thay log game (" + pathstr(p) + "). Hay chay game 1 lan.");
        return;
    }
    std::string text = read_text(p);
    std::vector<const char*> keys = {"CRC Mismatch", "is corrupted", "Failed to read data for the AssetBundle",
                                     "Exception", "Crash!!!"};
    if (godot) keys = {"ERROR", "Exception", "Loading locale"};
    if (renpy) keys = {"Error", "error", "Exception", "File \"game/", "Traceback"};
    if (unreal) keys = {"Error:", "Fatal error", "Assertion failed", "LogLinker: Warning"};
    int shown = 0, errors = 0;
    size_t st = 0;
    while (st < text.size() && shown < 15) {
        size_t e = text.find('\n', st);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(st, e - st);
        st = e + 1;
        // Godot lists leaked RIDs/shaders at every exit: not caused by the translation
        if (godot && (line.find("leaked") != std::string::npos || line.find("never freed") != std::string::npos))
            continue;
        for (const char* k : keys)
            if (line.find(k) != std::string::npos) {
                log("  " + line.substr(0, 220));
                shown++;
                errors += std::string(k) != "Loading locale";
                break;
            }
    }
    log(errors ? "Log game co loi o tren (" + pathstr(p) + ")."
               : "Log game: khong thay loi (" + pathstr(p) + ").");
}
