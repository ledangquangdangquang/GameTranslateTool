// A game being translated: finds <Game>_Data (Unity), data.win (GameMaker), <name>.pck (Godot) or renpy/ + game/ (Ren'Py), scans text sources,
// keeps progress in <game>/_unitytl.json, writes translations back (always rebuilding from the .bak originals).
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <mutex>

#include "formats.h"
#include "translate.h"

struct Entry {
    std::string id;  // file|node|pathId|key
    int src = -1;
    std::string key, en, vi;
};

using LogFn = std::function<void(const std::string&)>;

class Project {
public:
    fs::path root, dataDir;  // GameMaker: dataDir == root (folder of data.win)
    bool gm = false;         // GameMaker game: [english] section of an .ini + fonts patched in data.win
    std::string gmFont = "Constantia";  // Windows font drawn into data.win for missing Vietnamese letters
    std::string gmPixelFont = "Tahoma";  // same, without antialiasing, for pixel-art fonts
    bool gmFold3 = false;  // game crashes on 3-byte UTF-8 (Fields of Mistria): write ạ..ỹ as 2-byte stand-ins
    bool godot = false;    // Godot game: English .json files inside <name>.pck (dataDir == root, folder of the .pck)
    bool renpy = false;    // Ren'Py game: game/tl/<renpyLang>/*.rpy written from a template the game's engine generates
    std::string renpyName;                  // <root>/<renpyName>.py / .exe
    std::string renpyLang = "vietnamese";   // tl folder name; zz_unitytl_language.rpy selects it at start
    fs::path renpy_template() const { return root / "_unitytl_renpy" / u8path(renpyLang); }
    bool unreal = false;    // Unreal Engine game: <Project>/Content/Paks/*.pak, translated via a supplementary pak
    fs::path unrealPak;             // the game's own (untouched) main .pak
    std::string unrealProjectName;  // e.g. "VotV" - also the folder name under %LOCALAPPDATA%
    fs::path unreal_scratch() const { return root / "_unitytl_unreal"; }
    std::string company, product;
    std::vector<TextSource> sources;
    std::vector<Entry> entries;
    std::map<std::string, uint64_t> catalogSizes;  // bundle rel path -> size last written to catalog
    Glossary glossary;
    mutable std::mutex mu;  // guards entries[].vi while a translation thread runs

    static bool locate(const fs::path& any, fs::path& root, fs::path& dataDir);
    bool open(const fs::path& any, std::string& err);  // also loads saved progress
    std::string name() const { return product.empty() ? pathstr(root.filename()) : product; }

    fs::path work_file() const { return root / "_unitytl.json"; }
    fs::path glossary_file() const { return root / "_unitytl_glossary.json"; }

    void scan(const LogFn& log);
    void load();
    void save() const;
    void reload_glossary();

    // Machine-translate entries with empty vi (or all). Stops when *stop becomes true.
    void translate(bool all, std::atomic<bool>& stop, std::atomic<int>& done, std::atomic<int>& total, const LogFn& log,
                   EngineConfig engine = {});

    // Machine-translate just the still-untranslated entries among `indices` - e.g. the GUI's currently
    // filtered/searched rows, so a few prominent menu lines ("Start", "Settings", ...) can be test-translated
    // without running translate() over the whole game.
    void translate_rows(const std::vector<int>& indices, std::atomic<bool>& stop, std::atomic<int>& done,
                        std::atomic<int>& total, const LogFn& log, EngineConfig engine = {});

    void inject(const LogFn& log);
    void restore(const LogFn& log);
    fs::path player_log() const;
    void check_log(const LogFn& log) const;

    int translated_count() const;

    // Unreal only: forces the engine's active culture to "vi" via the user's saved Engine.ini, since the
    // in-game menu doesn't offer "Vietnamese" as a choice. Outside the game's own files, so - unlike
    // inject() - this is never run automatically; the GUI exposes it as its own explicit button.
    bool unreal_force_culture(const LogFn& log);
    bool unreal_culture_forced() const;

private:
    std::vector<fs::path> candidate_files() const;
    void scan_gm(std::vector<TextSource>& found, std::vector<Entry>& entries, const LogFn& log);
    void inject_gm(const LogFn& log);
    void scan_godot(std::vector<TextSource>& found, std::vector<Entry>& entries, const LogFn& log);
    void inject_godot(const LogFn& log);
    void scan_renpy(std::vector<TextSource>& found, std::vector<Entry>& entries, const LogFn& log);
    void inject_renpy(const LogFn& log);
    void scan_unity_loc(std::vector<TextSource>& found, std::vector<Entry>& entries, const LogFn& log);
    void inject_unity_loc(const LogFn& log);
    void scan_unity_tr(std::vector<TextSource>& found, std::vector<Entry>& entries, const LogFn& log);
    void inject_unity_tr(const LogFn& log);
    void scan_unity_csv(std::vector<TextSource>& found, std::vector<Entry>& entries, const LogFn& log);
    void inject_unity_csv(const LogFn& log);
    void inject_unity_fonts(const LogFn& log);
    void scan_unreal(std::vector<TextSource>& found, std::vector<Entry>& entries, const LogFn& log);
    void inject_unreal(const LogFn& log);
    fs::path unity_loc_live() const;
    fs::path unity_tr_live() const;
    std::string rel(const fs::path& p) const;
};
