// Unreal Engine games (<root>/<Project>/Content/Paks/*.pak): unitytl has no pak/locres parser of its
// own, so this shells out to two small bundled third-party tools in tools/ next to the app - the same
// idea as Ren'Py mode shelling out to the game's own python.exe, just with tools unitytl ships itself:
//   repak        (github.com/trumank/repak, Rust)   - .pak list/unpack/pack
//   UnrealLocres (github.com/akintos/UnrealLocres, C#) - .locres <-> CSV
// Vietnamese glyphs for whatever loose .ufont files the pak holds reuse ttf_add_vietnamese (ttf.cpp)
// completely unchanged - Unreal's "Runtime" Slate fonts turned out to be plain TrueType files, not an
// embedded/baked atlas like Unity TMP or Godot.
//
// unitytl never touches the game's own (often multi-GB) .pak: every Vietnamese string and patched font
// goes into one small supplementary pak (zzz_vi_P.pak) dropped next to it, which Unreal mounts
// automatically. The in-game language menu doesn't offer "Vietnamese" as an option (its list isn't read
// from Game.locmeta, which is patched anyway in case that's ever true for some other Unreal game) - so
// making the game actually use "vi" needs one more step outside the game's own files entirely:
// Culture=vi in the user's saved Engine.ini (see unreal_force_culture).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "bytes.h"

using LogCb = std::function<void(const std::string&)>;

struct UnrealTools {
    fs::path repak, unrealLocres;
    bool ok() const { return !repak.empty() && !unrealLocres.empty(); }
};
// Looks for tools/repak.exe and tools/UnrealLocres.exe next to the running exe (and up to 4 parent
// folders, same search depth as find_local_model()'s llm/ lookup in gui.cpp).
UnrealTools find_unreal_tools();

// <dir> (or up to 3 levels below it) holding <Project>/Content/Paks/*.pak - picks the largest .pak found
// that isn't one of unitytl's own supplementary paks (zzz_*.pak), since those are usually far smaller
// than the real game data.
bool locate_unreal(const fs::path& any, fs::path& root, fs::path& pak);

// `repak list`'s raw output, one pak-internal path per line (tens of thousands of lines for a real
// game - callers filter it, nothing parses the whole thing twice).
std::vector<std::string> unreal_pak_list(const UnrealTools& tools, const fs::path& pak, const LogCb& log);

struct UnrealTarget {
    std::string name;          // localization target, e.g. "Game"
    std::string sourceCulture; // e.g. "en" - whichever is_english() culture was found, else the first one
    std::string locresPak;     // pak-internal path to the SOURCE locres, e.g. "VotV/Content/Localization/Game/en/Game.locres"
};
// Groups every "<prefix>/<Target>/<culture>/<Target>.locres" path in `list` by target.
std::vector<UnrealTarget> unreal_find_targets(const std::vector<std::string>& list);

// repak-unpacks one pak-internal file under `extractDir` (same relative path), skipped if already there.
bool unreal_unpack_file(const UnrealTools& tools, const fs::path& pak, const std::string& relPath,
                        const fs::path& extractDir, const LogCb& log);

struct UnrealRow { std::string key, source, target; };
// repak-unpacks `locresPak` from `pak` under `extractDir` (skipped if already there) and UnrealLocres-
// exports it to CSV, returning the parsed rows (key, source, existing target if any).
std::vector<UnrealRow> unreal_export_csv(const UnrealTools& tools, const fs::path& pak, const std::string& locresPak,
                                         const fs::path& extractDir, const LogCb& log);

// Writes `rows` as a CSV (same key,source,target shape UnrealLocres itself exports) and UnrealLocres-
// imports it against `srcLocres` (the already-extracted source-culture file) into `outLocres`.
bool unreal_import_csv(const UnrealTools& tools, const fs::path& srcLocres, const std::vector<UnrealRow>& rows,
                       const fs::path& outLocres, const LogCb& log);

// Adds a "vi" entry to a Game.locmeta's compiled-culture list if it isn't already there. Returns `src`
// unchanged (never throws) if the binary doesn't match the expected layout - best-effort, like
// catalog.cpp's CRC-field heuristic. Confirmed (2026-11-02, Voices of the Void) that the in-game menu
// doesn't actually read this, but it's cheap and harmless, and may matter for some other Unreal game.
Bytes unreal_patch_locmeta(const Bytes& locmeta);

// Finds every loose *.ufont in `list`, repak-unpacks each (skipped if already extracted) and patches it
// with ttf_add_vietnamese; writes the patched copies under `outDir` at their original pak-relative paths.
// A font the CJK/CFF heuristic refuses (ttf_missing_vietnamese() == -1) or that already has every letter
// is skipped with a log line, not an error - `outDir` only gets the fonts actually changed.
void unreal_patch_fonts(const UnrealTools& tools, const fs::path& pak, const std::vector<std::string>& list,
                        const fs::path& extractDir, const fs::path& outDir, const LogCb& log);

// repak-packs `inputDir` (files already laid out at their final pak-relative paths) into `outPak`
// (version V11, uncompressed - matches every Unreal pak seen so far; a game on an older/newer engine
// version may need a different --version, not auto-detected).
bool unreal_pack(const UnrealTools& tools, const fs::path& inputDir, const fs::path& outPak, const LogCb& log);

// %LOCALAPPDATA%/<projectName>/Saved/Config/WindowsNoEditor/Engine.ini
fs::path unreal_engine_ini(const std::string& projectName);
fs::path unreal_log_path(const std::string& projectName);

// Adds "Culture=vi" under [Internationalization] to the user's saved Engine.ini - this is what actually
// makes the game render in Vietnamese; the in-game Settings menu doesn't offer "Vietnamese" as a choice
// at all (confirmed on Voices of the Void: its list is some other, unidentified ~26-entry mechanism, not
// Game.locmeta). Outside the game's own files, so this is never called automatically from inject() - the
// GUI exposes it as its own explicit button. No-op (returns true) if already set.
bool unreal_force_culture(const std::string& projectName, const LogCb& log);
bool unreal_culture_forced(const std::string& projectName);
