// Ren'Py games (<root>/<name>.exe + <name>.py, renpy/, lib/, game/): translated the engine's own way,
// with game/tl/<language>/*.rpy files ("translate vietnamese <id>:" dialogue blocks and
// "translate vietnamese strings:" old/new pairs) that Ren'Py loads next to the original script.
#pragma once
#include <functional>
#include <map>

#include "bytes.h"

// <dir> (or its parent / a child) holding renpy/ and game/ next to <name>.py
bool renpy_find(const fs::path& any, fs::path& root, std::string& name);

// .rpa archive (RPA-3.0 / RPA-2.0): file list from the pickled index, read with a minimal pickle
// reader that only knows plain dict/list/tuple/str/bytes/int - nothing in the archive can run code.
struct RpaEntry {
    std::string name;  // "gui/fonts/BadComic.ttf"
    uint64_t offset = 0, size = 0;
    std::string prefix;  // bytes stored in the index in front of the data (usually empty)
};
std::vector<RpaEntry> rpa_index(const fs::path& rpa);
Bytes rpa_read(const fs::path& rpa, const RpaEntry& e);

// Runs the game's own Ren'Py ("<name>.py <copy> translate <lang>") on a temporary copy of game/ with
// the scripts unpacked from the .rpa archives (string scanning needs loose .rpy files; big archives are
// hard-linked, not copied), and moves the generated English template to `out` (one .rpy per script).
bool renpy_generate(const fs::path& root, const std::string& name, const std::string& lang, const fs::path& out,
                    const std::function<void(const std::string&)>& log);

// One tl file: every translatable line, in order. Dialogue key = translate id (+ "#n" for the n-th say
// line of a block), string key = "s|" + old text. Text is the content between the quotes with \" undone.
struct RenpyLine {
    std::string key, text;
    size_t line = 0;  // index of the line in the file
};
std::vector<RenpyLine> renpy_tl_read(const std::string& text);
// `text` with the lines in `tr` (key -> translation) replaced. Returns the number replaced.
std::string renpy_tl_apply(const std::string& text, const std::map<std::string, std::string>& tr, int& n);
