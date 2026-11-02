#include "formats.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "csv.h"
#include "json.h"
#include "typetree.h"

namespace {

const ObjectInfo* find_obj(const SerializedFile& sf, int64_t pathId) {
    for (auto& o : sf.objects)
        if (o.pathId == pathId) return &o;
    return nullptr;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Read a typed object and require an exact read->write round trip.
bool read_typed(const SerializedFile& sf, const ObjectInfo& o, Value& v) {
    const TTNode* t = sf.tree_of(o);
    return t && tt_read_checked(*t, sf.object_data(o), sf.bigEndian, v);
}

void write_typed(SerializedFile& sf, const ObjectInfo& o, const Value& v) {
    sf.set_object_data(o.pathId, tt_write(*sf.tree_of(o), v, sf.bigEndian));
}

// MonoBehaviour header without typetree: m_GameObject, m_Enabled, m_Script, m_Name
size_t mb_header_end(const Bytes& d, bool be, std::string* name = nullptr) {
    Reader r(d, be);
    r.skip(12);
    r.u8();
    r.align(4);
    r.skip(12);
    std::string n = r.ustr();
    if (name) *name = n;
    return r.pos;
}

// ------------------------------------------------------------- I2 (stripped)
struct I2Term {
    std::string term;
    int32_t type = 0;
    std::vector<std::string> langs;
    std::string flags;
    std::vector<std::string> touch;
};
struct I2Raw {
    size_t headEnd = 0, tailStart = 0;
    std::vector<I2Term> terms;
    std::vector<std::pair<std::string, std::string>> langs;  // name, code
};

bool parse_i2_raw(const Bytes& d, bool be, int bools, I2Raw& out) {
    try {
        Reader r(d, be);
        r.pos = mb_header_end(d, be);
        r.skip((size_t)bools * 4);
        out.headEnd = r.pos;
        int32_t n = r.i32();
        if (n <= 0 || n > 1000000) return false;
        out.terms.resize(n);
        auto strs = [&](std::vector<std::string>& v) {
            int32_t k = r.i32();
            if (k < 0 || k > 4096) throw ParseError("i2");
            v.resize(k);
            for (auto& s : v) s = r.ustr();
        };
        for (auto& t : out.terms) {
            t.term = r.ustr();
            t.type = r.i32();
            strs(t.langs);
            int32_t fl = r.i32();
            if (fl < 0 || fl > 4096) return false;
            t.flags = r.bytes((size_t)fl);
            r.align(4);
            strs(t.touch);
        }
        out.tailStart = r.pos;
        r.i32();  // CaseInsensitiveTerms
        r.i32();  // OnMissingTranslation
        r.ustr();  // mTerm_AppName
        int32_t nl = r.i32();
        if (nl <= 0 || nl > 4096) return false;
        for (int i = 0; i < nl; i++) {
            std::string name = r.ustr(), code = r.ustr();
            r.i32();  // flags (+align)
            out.langs.emplace_back(name, code);
        }
        for (auto& t : out.terms)
            if (t.langs.size() != out.langs.size()) return false;
        return true;
    } catch (const ParseError&) {
        return false;
    }
}

Bytes build_i2_raw(const Bytes& orig, bool be, const I2Raw& raw) {
    Writer w(be);
    w.raw(orig.data(), raw.headEnd);
    w.i32((int32_t)raw.terms.size());
    for (auto& t : raw.terms) {
        w.ustr(t.term);
        w.i32(t.type);
        w.i32((int32_t)t.langs.size());
        for (auto& s : t.langs) w.ustr(s);
        w.i32((int32_t)t.flags.size());
        w.raw(t.flags);
        w.align(4);
        w.i32((int32_t)t.touch.size());
        for (auto& s : t.touch) w.ustr(s);
    }
    w.raw(orig.data() + raw.tailStart, orig.size() - raw.tailStart);
    return w.buf;
}

int i2_variant(const Bytes& d, bool be, I2Raw& raw) {
    for (int bools : {3, 2, 4, 1, 0, 5}) {
        raw = I2Raw();
        if (parse_i2_raw(d, be, bools, raw) && build_i2_raw(d, be, raw) == d) return bools;
    }
    return -1;
}

int column_of(const TextSource& s) {
    for (size_t i = 0; i < s.options.size(); i++)
        if (!s.chosen.empty() && s.options[i] == s.chosen[0]) return (int)i;
    return -1;
}

// ------------------------------------------------------------- TextAsset
bool textasset_text(const SerializedFile& sf, const ObjectInfo& o, std::string& name, std::string& text) {
    try {
        Value v;
        if (sf.tree_of(o)) {
            if (!read_typed(sf, o, v)) return false;
            name = v.get("m_Name") ? v.get("m_Name")->s : "";
            const Value* sc = v.get("m_Script");
            if (!sc) return false;
            text = sc->s;
            return true;
        }
        Bytes d = sf.object_data(o);
        Reader r(d, sf.bigEndian);
        name = r.ustr();
        int32_t n = r.i32();
        if (n < 0) return false;
        text = r.bytes((size_t)n);
        return true;
    } catch (const ParseError&) {
        return false;
    }
}

void set_textasset_text(SerializedFile& sf, const ObjectInfo& o, const std::string& text) {
    if (sf.tree_of(o)) {
        Value v;
        if (!read_typed(sf, o, v)) throw ParseError("TextAsset khong round-trip duoc");
        v.get("m_Script")->s = text;
        write_typed(sf, o, v);
        return;
    }
    Bytes d = sf.object_data(o);
    Reader r(d, sf.bigEndian);
    std::string name = r.ustr();
    int32_t n = r.i32();
    r.skip((size_t)n);
    r.align(4);
    Writer w(sf.bigEndian);
    w.ustr(name);
    w.ustr(text);
    w.raw(d.data() + r.pos, d.size() - r.pos);
    sf.set_object_data(o.pathId, w.buf);
}

std::string strip_bom(const std::string& s) {
    return s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF ? s.substr(3) : s;
}

std::vector<std::string> json_sections(const Json& j) {
    std::vector<std::string> out;
    if (!j.is_object()) return out;
    for (auto& kv : j.members()) {
        if (!kv.second.is_object()) continue;
        int strs = 0;
        for (auto& m : kv.second.members()) strs += m.second.is_string();
        if (strs >= 20) out.push_back(kv.first);
    }
    return out;
}

// ------------------------------------------------------------- Dialogue System
const char* kNonText[] = {"Title", "Description", "Actor", "Conversant", "Sequence", "Articy Id", "InputId",
                          "Pictures", "IsPlayer", "Display Name", "Name", "Technical Name", "Conditions",
                          "Script", "Response Menu Sequence", "VoiceOverFile"};

bool is_lang_field(const std::string& t) {
    if (t == "Dialogue Text" || t == "Menu Text") return true;
    if (t.size() == 2) return std::islower((unsigned char)t[0]) && std::islower((unsigned char)t[1]);
    if (t.size() == 5 && (t[2] == '-' || t[2] == '_')) return std::islower((unsigned char)t[0]) && std::islower((unsigned char)t[1]);
    return false;
}

template <class F>
void each_dialogue_field(Value& db, F f) {
    Value* convs = db.get("conversations");
    if (!convs) return;
    for (auto& c : convs->arr) {
        const Value* cid = c.get("id");
        Value* entries = c.get("dialogueEntries");
        if (!cid || !entries) continue;
        for (auto& e : entries->arr) {
            const Value* eid = e.get("id");
            Value* fields = e.get("fields");
            if (!eid || !fields) continue;
            for (auto& fl : fields->arr) {
                Value* title = fl.get("title");
                Value* val = fl.get("value");
                if (title && val) f(std::to_string(cid->i) + ":" + std::to_string(eid->i) + ":" + title->s, title->s, *val);
            }
        }
    }
}

}  // namespace

bool is_english(const std::string& name) {
    std::string l = lower(name);
    return l == "english" || l == "en" || l == "eng" || l.rfind("en-", 0) == 0 || l.rfind("en_", 0) == 0 ||
           l.find("english") != std::string::npos;
}

std::string TextSource::label() const {
    std::string k = kind == "i2" ? "I2 Localization" : kind == "stringtable" ? "Unity Localization"
                  : kind == "json" ? "JSON da ngon ngu" : kind == "dialogue" ? "Dialogue System"
                  : kind == "gmini" ? "GameMaker INI" : kind == "gmjson" ? "GameMaker JSON"
                  : kind == "gmcsv" ? "GameMaker CSV" : kind == "locstring" ? "LocalizedStringSO"
                  : kind == "bigtsv" ? "Bang TSV da ngon ngu" : kind == "godotjson" ? "Godot JSON" : kind == "renpytl" ? "Ren'Py tl"
                  : kind == "unityloc" ? "Localization CSV" : kind == "unitytr" ? "Translations JSON"
                  : kind == "unitycsv" ? "Localization CSV (1 file/ngon ngu)" : kind == "unreallocres" ? "Unreal .locres" : kind;
    std::string f = file;
    size_t sl = f.find_last_of("/\\");
    if (sl != std::string::npos) f = f.substr(sl + 1);
    return k + ": " + asset + " (" + f + ")";
}

void detect_sources(const SerializedFile& sf, const std::string& file, const std::string& node,
                    std::vector<TextSource>& out) {
    // "LocalizedStringSO": one ScriptableObject per string (value/guid/skipExporting), scattered as many
    // top-level MonoBehaviour objects across a file rather than gathered in one table (TOEM 2). Collected
    // into a single "locstring" source per file so the UI shows one row, not hundreds.
    std::vector<int64_t> lsIds;
    for (auto& o : sf.objects) {
        if (o.classID == kTextAsset) {
            std::string name, text;
            if (o.byteSize < 64 || !textasset_text(sf, o, name, text)) continue;
            text = strip_bom(text);
            size_t p = text.find_first_not_of(" \t\r\n");
            if (p != std::string::npos && text[p] == '{') {
                Json j;
                try { j = Json::parse(text); } catch (const std::exception&) { continue; }
                auto secs = json_sections(j);
                if (secs.empty()) continue;
                TextSource s{file, node, o.pathId, "json", name, secs, {}};
                auto en = std::find_if(secs.begin(), secs.end(), is_english);
                s.chosen = {en != secs.end() ? *en : secs[0]};
                s.count = (int)read_source(sf, s).size();
                out.push_back(s);
                continue;
            }
            // A localization table in one TextAsset: header row with an English column - TOEM 2's
            // authoring-spreadsheet export (one huge tab-separated table), or Midnight Ritual's many
            // small per-category tables ("Key,English,Russian", comma-separated, as few as 2 rows -
            // header + 1 - each). Tab is tried first (the common case); comma only if that doesn't
            // find a qualifying table, so a tab-separated table with a stray comma in the text isn't
            // misread. The chosen delimiter is remembered in `variant` (0 tab, 1 comma) for
            // read_source/apply_source. >=2 rows (header + >=1 data row) + an explicit English-named
            // column is enough of a signature on its own to not need a row-count floor.
            for (char delim : {'\t', ','}) {
                auto rows = csv_parse(text, delim);
                if (rows.size() < 2 || rows[0].size() < 3) continue;
                int enCol = csv_find_column(rows, "English");
                if (enCol < 0)
                    for (size_t c = 0; c < rows[0].size(); c++)
                        if (is_english(rows[0][c].value)) { enCol = (int)c; break; }
                if (enCol < 0) continue;
                TextSource s{file, node, o.pathId, "bigtsv", name};
                s.variant = delim == ',' ? 1 : 0;
                for (auto& f : rows[0]) s.options.push_back(f.value);
                s.chosen = {rows[0][enCol].value};
                s.count = (int)read_source(sf, s).size();
                if (s.count) out.push_back(s);
                break;
            }
            continue;
        }
        if (o.classID != kMonoBehaviour || o.byteSize < 64) continue;

        if (sf.tree_of(o)) {
            // cheap name check on the typetree before a full read
            const TTNode* t = sf.tree_of(o);
            bool i2 = t->child("mSource"), st = t->child("m_TableData") && t->child("m_LocaleId");
            bool dlg = t->child("conversations") && t->child("actors");
            bool ls = t->child("value") && t->child("guid") && t->child("skipExporting");
            if (ls) { lsIds.push_back(o.pathId); continue; }
            if (!i2 && !st && !dlg) continue;
            Value v;
            if (!read_typed(sf, o, v)) continue;
            std::string name = v.get("m_Name") ? v.get("m_Name")->s : "";
            if (i2) {
                const Value* langs = v.path("mSource/mLanguages");
                if (!langs || !v.path("mSource/mTerms")) continue;
                TextSource s{file, node, o.pathId, "i2", name};
                for (auto& l : langs->arr) s.options.push_back(l.get("Name") ? l.get("Name")->s : "?");
                auto en = std::find_if(s.options.begin(), s.options.end(), is_english);
                if (s.options.empty()) continue;
                s.chosen = {en != s.options.end() ? *en : s.options[0]};
                s.count = (int)read_source(sf, s).size();
                out.push_back(s);
            } else if (st) {
                TextSource s{file, node, o.pathId, "stringtable", name};
                const Value* code = v.path("m_LocaleId/m_Code");
                s.options = {code ? code->s : "?"};
                s.chosen = s.options;
                s.enabled = is_english(s.options[0]);  // one table per language: translate English only
                s.count = (int)read_source(sf, s).size();
                if (s.count) out.push_back(s);
            } else {
                if (lower(name).find("backup") != std::string::npos) continue;  // editor auto-backup copy
                std::map<std::string, int> counts;
                each_dialogue_field(v, [&](const std::string&, const std::string& title, Value& val) {
                    if (std::find(std::begin(kNonText), std::end(kNonText), title) != std::end(kNonText)) return;
                    if (is_lang_field(title) && !val.s.empty()) counts[title]++;
                });
                if (counts.empty()) continue;
                TextSource s{file, node, o.pathId, "dialogue", name};
                for (auto& kv : counts) s.options.push_back(kv.first);
                for (const char* en : {"en", "en-US", "en_US"})
                    if (counts.count(en)) { s.chosen = {en}; break; }
                if (s.chosen.empty())
                    for (const char* f : {"Dialogue Text", "Menu Text"})
                        if (counts.count(f)) s.chosen.push_back(f);
                if (s.chosen.empty()) continue;
                s.count = (int)read_source(sf, s).size();
                out.push_back(s);
            }
            continue;
        }

        // stripped MonoBehaviour: try the I2 layout by hand
        if (o.byteSize < 1024) continue;
        Bytes d = sf.object_data(o);
        I2Raw raw;
        int variant = i2_variant(d, sf.bigEndian, raw);
        if (variant < 0) continue;
        std::string name;
        mb_header_end(d, sf.bigEndian, &name);
        TextSource s{file, node, o.pathId, "i2", name};
        for (auto& l : raw.langs) s.options.push_back(l.first);
        s.variant = variant;
        auto en = std::find_if(s.options.begin(), s.options.end(), is_english);
        s.chosen = {en != s.options.end() ? *en : s.options[0]};
        s.count = (int)read_source(sf, s).size();
        out.push_back(s);
    }
    if (!lsIds.empty()) {
        TextSource s{file, node, 0, "locstring", "Localized Strings"};
        s.count = (int)lsIds.size();
        out.push_back(s);
    }
}

TextMap read_source(const SerializedFile& sf, const TextSource& s) {
    TextMap out;
    if (s.kind == "locstring") {
        for (auto& o : sf.objects) {
            if (o.classID != kMonoBehaviour) continue;
            const TTNode* t = sf.tree_of(o);
            if (!t || !t->child("value") || !t->child("guid") || !t->child("skipExporting")) continue;
            Value v;
            if (!read_typed(sf, o, v)) continue;
            const Value* val = v.get("value");
            if (val && !val->s.empty()) out.emplace_back(std::to_string(o.pathId), val->s);
        }
        return out;
    }
    const ObjectInfo* o = find_obj(sf, s.pathId);
    if (!o) throw ParseError("khong thay object " + std::to_string(s.pathId) + " trong " + s.file);

    if (s.kind == "json") {
        std::string name, text;
        if (!textasset_text(sf, *o, name, text)) throw ParseError("doc TextAsset loi");
        Json j = Json::parse(strip_bom(text));
        const Json* sec = j.find(s.chosen.at(0));
        if (!sec) return out;
        for (auto& kv : sec->members())
            if (kv.second.is_string() && !kv.second.str().empty()) out.emplace_back(kv.first, kv.second.str());
        return out;
    }
    if (s.kind == "bigtsv") {
        std::string name, text;
        if (!textasset_text(sf, *o, name, text)) throw ParseError("doc TextAsset loi");
        auto rows = csv_parse(text, s.variant == 1 ? ',' : '\t');
        int keyCol = csv_find_column(rows, "KEY");
        if (keyCol < 0) keyCol = 0;
        int col = csv_find_column(rows, s.chosen.at(0));
        if (col < 0) return out;
        std::set<std::string> seen;
        for (size_t r = 1; r < rows.size(); r++) {
            if ((size_t)col >= rows[r].size() || (size_t)keyCol >= rows[r].size()) continue;
            const std::string& key = rows[r][keyCol].value;
            if (key.empty() || rows[r][col].value.empty()) continue;
            if (seen.insert(key).second) out.emplace_back(key, rows[r][col].value);
        }
        return out;
    }
    if (s.kind == "i2") {
        int col = column_of(s);
        if (col < 0) return out;
        if (sf.tree_of(*o)) {
            Value v;
            if (!read_typed(sf, *o, v)) throw ParseError("I2 khong round-trip duoc");
            for (auto& t : v.path("mSource/mTerms")->arr) {
                const Value* langs = t.get("Languages");
                if (langs && col < (int)langs->arr.size() && !langs->arr[col].s.empty())
                    out.emplace_back(t.get("Term")->s, langs->arr[col].s);
            }
        } else {
            I2Raw raw;
            if (!parse_i2_raw(sf.object_data(*o), sf.bigEndian, s.variant, raw)) throw ParseError("I2 doc loi");
            for (auto& t : raw.terms)
                if (!t.langs[col].empty()) out.emplace_back(t.term, t.langs[col]);
        }
        return out;
    }
    Value v;
    if (!read_typed(sf, *o, v)) throw ParseError("object khong round-trip duoc");
    if (s.kind == "stringtable") {
        for (auto& e : v.get("m_TableData")->arr) {
            const Value* id = e.get("m_Id");
            const Value* loc = e.get("m_Localized");
            if (id && loc && !loc->s.empty())
                out.emplace_back(std::to_string(id->kind == Value::UInt ? (int64_t)id->u : id->i), loc->s);
        }
    } else if (s.kind == "dialogue") {
        each_dialogue_field(v, [&](const std::string& key, const std::string& title, Value& val) {
            if (std::find(s.chosen.begin(), s.chosen.end(), title) != s.chosen.end() && !val.s.empty())
                out.emplace_back(key, val.s);
        });
    }
    return out;
}

int apply_source(SerializedFile& sf, const TextSource& s, const std::map<std::string, std::string>& tr) {
    if (s.kind == "locstring") {
        int changed = 0;
        for (auto& o : sf.objects) {
            if (o.classID != kMonoBehaviour) continue;
            const TTNode* t = sf.tree_of(o);
            if (!t || !t->child("value") || !t->child("guid") || !t->child("skipExporting")) continue;
            Value v;
            if (!read_typed(sf, o, v)) continue;
            auto it = tr.find(std::to_string(o.pathId));
            if (it == tr.end() || it->second.empty()) continue;
            Value* val = v.get("value");
            if (!val || val->s == it->second) continue;
            val->s = it->second;
            write_typed(sf, o, v);
            changed++;
        }
        return changed;
    }
    const ObjectInfo* o = find_obj(sf, s.pathId);
    if (!o) throw ParseError("khong thay object " + std::to_string(s.pathId));
    int changed = 0;
    auto lookup = [&](const std::string& key, std::string& text) {
        auto it = tr.find(key);
        if (it == tr.end() || it->second.empty() || it->second == text) return;
        text = it->second;
        changed++;
    };

    if (s.kind == "json") {
        std::string name, text;
        textasset_text(sf, *o, name, text);
        bool bom = text.size() >= 3 && (uint8_t)text[0] == 0xEF;
        std::string body = strip_bom(text);
        Json j = Json::parse(body);
        Json* sec = j.find(s.chosen.at(0));
        if (!sec) return 0;
        for (auto& kv : sec->members())
            if (kv.second.is_string()) lookup(kv.first, kv.second.str());
        std::string head = body.substr(0, 400);
        int indent = head.find("\n    \"") != std::string::npos ? 4 : head.find("\n  \"") != std::string::npos ? 2 : -1;
        std::string out = j.dump(indent);
        set_textasset_text(sf, *o, bom ? "\xEF\xBB\xBF" + out : out);
        return changed;
    }
    if (s.kind == "bigtsv") {
        std::string name, text;
        if (!textasset_text(sf, *o, name, text)) throw ParseError("doc TextAsset loi");
        auto rows = csv_parse(text, s.variant == 1 ? ',' : '\t');
        int keyCol = csv_find_column(rows, "KEY");
        if (keyCol < 0) keyCol = 0;
        int col = csv_find_column(rows, s.chosen.at(0));
        if (col < 0) return 0;
        std::string out;
        size_t prev = 0;
        for (size_t r = 1; r < rows.size(); r++) {
            if ((size_t)col >= rows[r].size() || (size_t)keyCol >= rows[r].size()) continue;
            auto it = tr.find(rows[r][keyCol].value);
            if (it == tr.end() || it->second.empty()) continue;
            const CsvField& f = rows[r][col];
            if (f.value == it->second) continue;
            out += text.substr(prev, f.start - prev);
            out += csv_encode(it->second, '\t');
            prev = f.end;
            changed++;
        }
        out += text.substr(prev);
        set_textasset_text(sf, *o, out);
        return changed;
    }
    if (s.kind == "i2" && !sf.tree_of(*o)) {
        int col = column_of(s);
        Bytes d = sf.object_data(*o);
        I2Raw raw;
        if (col < 0 || !parse_i2_raw(d, sf.bigEndian, s.variant, raw)) throw ParseError("I2 doc loi");
        for (auto& t : raw.terms) lookup(t.term, t.langs[col]);
        sf.set_object_data(o->pathId, build_i2_raw(d, sf.bigEndian, raw));
        return changed;
    }

    Value v;
    if (!read_typed(sf, *o, v)) throw ParseError("object khong round-trip duoc");
    if (s.kind == "i2") {
        int col = column_of(s);
        for (auto& t : v.path("mSource/mTerms")->arr) {
            Value* langs = t.get("Languages");
            if (langs && col >= 0 && col < (int)langs->arr.size()) lookup(t.get("Term")->s, langs->arr[col].s);
        }
    } else if (s.kind == "stringtable") {
        for (auto& e : v.get("m_TableData")->arr) {
            Value* id = e.get("m_Id");
            Value* loc = e.get("m_Localized");
            if (id && loc) lookup(std::to_string(id->kind == Value::UInt ? (int64_t)id->u : id->i), loc->s);
        }
    } else if (s.kind == "dialogue") {
        each_dialogue_field(v, [&](const std::string& key, const std::string& title, Value& val) {
            if (std::find(s.chosen.begin(), s.chosen.end(), title) != s.chosen.end()) lookup(key, val.s);
        });
    }
    write_typed(sf, *o, v);
    return changed;
}
