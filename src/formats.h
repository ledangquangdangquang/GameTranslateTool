// Text formats found inside Unity games, without any runtime mod:
//   i2          I2 Localization LanguageSourceAsset (typetree OR stripped - parsed by hand)
//   stringtable Unity Localization package StringTable (m_TableData / m_Localized)
//   json        TextAsset holding {"English": {...}, "French": {...}} style JSON
//   dialogue    Pixel Crushers Dialogue System DialogueDatabase (field per language code)
//   locstring   many separate "LocalizedStringSO" MonoBehaviours (value/guid/skipExporting) in one file,
//               collected into a single source (TOEM 2) - no language field, "value" is the English text
//   bigtsv      one TextAsset holding a whole tab-separated table with a header row (a "KEY" column -
//               or column 0 if none - as the row id, one column per language e.g. "English") - TOEM 2's
//               authoring-spreadsheet export, which turned out to be what the game actually reads at
//               runtime (the "locstring" ScriptableObjects above are unused at runtime, editor-only)
#pragma once
#include <functional>
#include <map>

#include "serialized.h"

struct TextSource {
    std::string file;  // path relative to <Game>_Data, UTF-8
    std::string node;  // bundle node (CAB-...) or "" for plain .assets
    int64_t pathId = 0;
    std::string kind;  // i2 | stringtable | json | dialogue
    std::string asset;
    std::vector<std::string> options;  // i2/json: languages; dialogue: fields; stringtable: locale
    std::vector<std::string> chosen;   // what gets translated (and overwritten in place)
    int variant = 0;                   // i2 stripped layout: number of bool fields before mTerms
    int count = 0;
    bool enabled = true;

    std::string label() const;
};

using TextMap = std::vector<std::pair<std::string, std::string>>;  // key -> text, in file order

bool is_english(const std::string& languageNameOrCode);

void detect_sources(const SerializedFile& sf, const std::string& file, const std::string& node,
                    std::vector<TextSource>& out);
TextMap read_source(const SerializedFile& sf, const TextSource& s);
// Writes translations (key -> text) into sf; returns number of strings changed.
int apply_source(SerializedFile& sf, const TextSource& s, const std::map<std::string, std::string>& tr);
