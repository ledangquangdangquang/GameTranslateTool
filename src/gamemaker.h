// GameMaker games (data.win): text lives in a [language]-sectioned .ini the game reads at startup
// (Crypt Custodian, Well Dweller), or a multi-language .csv (one row per key, one column per language -
// Starvester); fonts are pre-rendered glyph atlases inside data.win.
#pragma once
#include <functional>
#include <map>

#include "bytes.h"
#include "formats.h"

bool gm_is_game_dir(const fs::path& dir);  // has data.win

// ---- [section] key=value .ini (no BOM, one '=' per entry line, no escaping)
std::vector<std::string> gm_ini_sections(const std::string& text);
TextMap gm_ini_read(const std::string& text, const std::string& section);
// Replaces the values of `section` keys found in tr; everything else stays byte-identical.
std::string gm_ini_apply(const std::string& text, const std::string& section,
                         const std::map<std::string, std::string>& tr, int& changed);

// ---- {"eng": {"key": "text"}, "jpn": {...}} localization .json (Fields of Mistria)
std::vector<std::string> gm_json_sections(const std::string& text);  // members that are objects of strings
TextMap gm_json_read(const std::string& text, const std::string& section);
std::string gm_json_apply(const std::string& text, const std::string& section,
                          const std::map<std::string, std::string>& tr, int& changed);

// ---- flat {"key": "text"} .json, one file per language (Godot: Slay the Spire 2 localization/eng/*.json).
// Non-string members are left alone; apply splices only the changed values.
TextMap json_flat_read(const std::string& text);
std::string json_flat_apply(const std::string& text, const std::map<std::string, std::string>& tr, int& changed);

// ---- key,English,French,... .csv (first column is the key, one column per language; Starvester). The
// chosen language column is overwritten in place with the translation, same as the .ini/.json section.
std::vector<std::string> gm_csv_sections(const std::string& text);  // column headers, minus the key column
TextMap gm_csv_read(const std::string& text, const std::string& column);
std::string gm_csv_apply(const std::string& text, const std::string& column,
                         const std::map<std::string, std::string>& tr, int& changed);

// ---- fonts
// Makes the Latin fonts of data.win able to draw `chars` (plus the full Vietnamese alphabet),
// rendered with the installed Windows font `family` (`pixelFamily`, without antialiasing, for
// pixel-art fonts). Nothing in the file moves: each Latin font's glyph table is rewritten into the
// (much bigger) record of a CJK font, the two FONT table entries are swapped, and the new atlas goes
// onto a texture page only CJK fonts used, or into a CJK font's rectangle of a shared page (decoded,
// redrawn there only). Pages are re-encoded as PNGs no larger than the original blobs. The CJK
// languages of the game lose their font in exchange. Returns false when nothing could be patched.
bool gm_patch_fonts(Bytes& data, const std::u32string& chars, const std::string& family,
                    const std::string& pixelFamily, const std::function<void(const std::string&)>& log,
                    bool fold3 = false);

// Fields of Mistria crashes on any character that takes 3+ bytes in UTF-8 (U+0800 and up) in its English
// text, while 2-byte ones (à, ă, đ, ơ...) are fine. gm_fold_3byte moves the 90 Vietnamese letters of
// U+1EA0..U+1EF9 (ạ ả ấ ... ỹ) to the unused 2-byte codes U+0500..U+0559 and turns other 3-byte
// characters into ASCII (… -> ..., – -> -); gm_patch_fonts(fold3 = true) draws the real letters there.
std::string gm_fold_3byte(const std::string& utf8);

std::u32string utf8_to_u32(const std::string& s);

// A texture page blob (PNG or GameMaker QOI "fioq") -> BGRA pixels.
bool gm_decode_texture(const uint8_t* p, size_t size, int& w, int& h, std::vector<uint8_t>& bgra);
