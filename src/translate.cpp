#include "translate.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "bytes.h"
#include "json.h"

// ------------------------------------------------------------------ glossary
Glossary Glossary::load_or_default(const std::string& pathUtf8) {
    Glossary g;
    fs::path p = u8path(pathUtf8);
    if (fs::exists(p)) {
        Json j = Json::parse(read_text(p));
        if (auto k = j.find("keep"))
            for (auto& s : k->items()) g.keep.push_back(s.str());
        if (auto t = j.find("terms"))
            for (auto& kv : t->members()) g.terms[kv.first] = kv.second.str();
        if (auto w = j.find("whole"))
            for (auto& kv : w->members()) g.whole[kv.first] = kv.second.str();
        return g;
    }
    // Short UI words Google gets wrong out of context ("No" -> "KHONG", "Back" -> "Mat sau")
    g.whole = {{"Yes", "Có"}, {"No", "Không"}, {"OK", "OK"}, {"Back", "Quay lại"}, {"Cancel", "Hủy"},
               {"Leave", "Rời đi"}, {"Select", "Chọn"}, {"Confirm", "Xác nhận"}, {"Continue", "Tiếp tục"},
               {"New Game", "Trò chơi mới"}, {"Load Game", "Tải trò chơi"}, {"Settings", "Cài đặt"},
               {"Options", "Tùy chọn"}, {"Quit", "Thoát"}, {"Exit", "Thoát"}, {"Resume", "Tiếp tục"},
               {"Default", "Mặc định"}, {"On", "Bật"}, {"Off", "Tắt"}, {"Apply", "Áp dụng"},
               {"Credits", "Danh đề"}, {"Language", "Ngôn ngữ"}, {"Volume", "Âm lượng"}};
    g.save(pathUtf8);
    return g;
}

void Glossary::save(const std::string& pathUtf8) const {
    Json j = Json::object();
    j["_huong_dan"] = Json(
        "keep: ten rieng giu nguyen | terms: thuat ngu dich co dinh trong cau | whole: dich san ca dong");
    Json k = Json::array();
    for (auto& s : keep) k.push(Json(s));
    j["keep"] = k;
    Json t = Json::object();
    for (auto& kv : terms) t[kv.first] = Json(kv.second);
    j["terms"] = t;
    Json w = Json::object();
    for (auto& kv : whole) w[kv.first] = Json(kv.second);
    j["whole"] = w;
    write_file(u8path(pathUtf8), j.dump(1));
}

// ------------------------------------------------------------------ helpers
// GameMaker Scribble command tags: [c_red] [/shake] [delay] [change_skel,o_mouse]. Returns the index of
// the closing ']' or 0. Our own " [N] " tokens start with a digit, so they never match.
static size_t scribble_end(const std::string& s, size_t i) {
    if (s[i] != '[' || i + 1 >= s.size()) return 0;
    char c = s[i + 1];
    if (!std::isalpha((unsigned char)c) && c != '_' && c != '/') return 0;
    for (size_t e = i + 1; e < s.size() && e - i < 80; e++) {
        if (s[e] == ']') return e;
        if (s[e] == '[' || s[e] == ' ') return 0;
    }
    return 0;
}

// Placeholder starting at s[i] == '{': {0} {a} {} (most games, Starvester) or a SmartFormat expression
// {Selector:formatter(args):A|B} / {Selector:A|B} with nested placeholders in the branches (Slay the
// Spire 2). Returns the index of the matching '}' or 0.
// Also Ren'Py text tags: {/i}, {#month} (a translator comment), {w=0.5}, {size=+10}, {a=https://...}.
static size_t placeholder_end(const std::string& s, size_t i) {
    size_t j = i + 1;
    while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '.')) j++;
    if (j >= s.size()) return 0;
    if (s[j] == '}') return j;
    if ((j == i + 1 && (s[j] == '/' || s[j] == '#')) || (j > i + 1 && s[j] == '=')) {
        for (size_t k = j + 1; k < s.size() && k - i < 120; k++) {
            if (s[k] == '}') return k;
            if (s[k] == '{') return 0;
        }
        return 0;
    }
    if (s[j] != ':') return 0;
    int depth = 0;
    for (size_t k = i; k < s.size(); k++) {
        if (s[k] == '{') depth++;
        else if (s[k] == '}' && --depth == 0) return k;
    }
    return 0;
}

// Splits a placeholder into its fixed head "{Sel:formatter:" and its translatable branches (split on
// top-level '|'). No branches: {X}, {X:diff()}, {X:energyIcons(1)} - nothing to translate inside.
static bool smart_branches(const std::string& t, std::string& head, std::vector<std::string>& branches) {
    branches.clear();
    size_t colon = t.find(':');
    if (t.size() < 3 || t[0] != '{' || colon == std::string::npos || colon > t.find_first_of("{}", 1)) return false;
    // the selector must run straight into the ':' ({a=https://x} is a Ren'Py link, not SmartFormat)
    for (size_t k = 1; k < colon; k++)
        if (!std::isalnum((unsigned char)t[k]) && t[k] != '_' && t[k] != '.') return false;
    std::string rest = t.substr(colon + 1, t.size() - colon - 2);
    size_t f = 0;
    while (f < rest.size() && (std::isalnum((unsigned char)rest[f]) || rest[f] == '_')) f++;
    if (f > 0 && f < rest.size() && rest[f] == '(') {
        size_t close = rest.find(')', f);
        f = close == std::string::npos ? 0 : close + 1;
    }
    size_t fmtLen = 0;
    if (f > 0 && f == rest.size()) return false;  // formatter only, e.g. {Damage:diff()}
    if (f > 0 && rest[f] == ':') fmtLen = f + 1;   // {Cards:plural:card|cards}; else {IsRevealed:A|B}
    head = t.substr(0, colon + 1 + fmtLen);
    std::string fmt = rest.substr(fmtLen);
    int depth = 0;
    size_t st = 0;
    for (size_t k = 0; k <= fmt.size(); k++) {
        if (k == fmt.size() || (fmt[k] == '|' && depth == 0)) {
            branches.push_back(fmt.substr(st, k - st));
            st = k + 1;
        } else if (fmt[k] == '{') depth++;
        else if (fmt[k] == '}') depth--;
    }
    return true;
}

// Length of a SmartFormat `cond` branch's condition, e.g. ">0?" in "{X:cond:>0? to ALL enemies|}" or
// ">=1&<5?"; 0 when the branch has none. It is code, not text: the model dropped the ">" ("cond:0?").
static size_t cond_prefix(const std::string& b) {
    size_t i = 0;
    for (;;) {
        while (i < b.size() && b[i] && std::strchr("<>=!", b[i])) i++;
        if (i < b.size() && b[i] == '-') i++;
        size_t d = i;
        while (i < b.size() && (std::isdigit((unsigned char)b[i]) || b[i] == '.')) i++;
        if (i == d || i >= b.size()) return 0;
        if (b[i] == '?') return i + 1;
        if (b[i] != '&' && b[i] != '/') return 0;
        i++;
    }
}

// [begin, end) of every tag, in order
static std::vector<std::pair<size_t, size_t>> tag_spans(const std::string& s) {
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t i = 0; i < s.size(); i++) {
        size_t e = 0;
        if (s[i] == '<') {
            size_t g = s.find('>', i + 1);
            if (g != std::string::npos && s.find('<', i + 1) > g) e = g;
        } else if (s[i] == '{') e = placeholder_end(s, i);
        else e = scribble_end(s, i);
        if (e) {
            out.push_back({i, e + 1});
            i = e;
        }
    }
    return out;
}

std::vector<std::string> find_tags(const std::string& s) {
    std::vector<std::string> out;
    for (auto [a, b] : tag_spans(s)) out.push_back(s.substr(a, b - a));
    return out;
}

// ------------------------------------------------------------------ markup the model never sees
// Markup tag = <b>/</b>, [gold]/[/gold], {i}/{/i}, {size=+10}... (a pair) or a Ren'Py control tag ({w}
// {p} {nw} {fast} {#comment}). Values - {0}, {name}, [player], {X:plural:a|b} - are text, never markup.
struct TagKind {
    std::string name;  // lower case, without '/' and arguments
    bool closer = false;
    bool smart = false;  // SmartFormat expression / numeric placeholder: a value
};

static TagKind tag_kind(const std::string& t) {
    TagKind k;
    if (t.size() < 2) return k;
    size_t i = 1;
    if (t[i] == '/') { k.closer = true; i++; }
    if (t[0] == '{') {
        size_t sel = 1;
        while (sel < t.size() && (std::isalnum((unsigned char)t[sel]) || t[sel] == '_' || t[sel] == '.')) sel++;
        if (sel < t.size() && t[sel] == ':') k.smart = true;
        if (t.size() > 2 && std::isdigit((unsigned char)t[1])) k.smart = true;
    }
    if (t[0] == '{' && t[i] == '#') { k.name = "#"; return k; }
    while (i < t.size() && !std::strchr("=]}> ,", t[i])) k.name += (char)std::tolower((unsigned char)t[i++]);
    return k;
}

// Ren'Py pauses: {w} {w=0.5} {p} {p=1}
static bool pause_tag(const std::string& t) {
    TagKind k = tag_kind(t);
    return t[0] == '{' && !k.closer && !k.smart && (k.name == "w" || k.name == "p");
}

// Ren'Py tags that stand alone at the end/start of a line and only steer the text box
static bool control_tag(const std::string& t) {
    TagKind k = tag_kind(t);
    return t[0] == '{' && !k.closer && !k.smart &&
           (k.name == "w" || k.name == "p" || k.name == "nw" || k.name == "fast" || k.name == "done" ||
            k.name == "clear" || k.name == "#");
}

// Splits off the markup that wraps the whole line ("[sine][orange]...[/orange][/sine]", "...{w}",
// "{i}...{/i}") with the spaces next to it. An opening tag goes only with its closing tag in the tail.
// False when there is nothing to peel.
static bool peel_markup(const std::string& s, std::string& pre, std::string& core, std::string& suf) {
    auto spans = tag_spans(s);
    // leading run of tags (only whitespace between), trailing run likewise
    size_t lead = 0, pos = 0;
    while (lead < spans.size() && s.find_first_not_of(" \t", pos) == spans[lead].first) pos = spans[lead++].second;
    size_t trail = spans.size(), end = s.size();
    while (trail > lead) {
        size_t last = s.find_last_not_of(" \t", end ? end - 1 : 0);
        if (last == std::string::npos || last + 1 != spans[trail - 1].second) break;
        end = spans[--trail].first;
    }
    auto text = [&](size_t i) { return s.substr(spans[i].first, spans[i].second - spans[i].first); };
    // shrink until every peeled tag is a control tag or one half of a pair peeled at both ends
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < lead && !changed; i++) {
            std::string t = text(i);
            TagKind k = tag_kind(t);
            if (control_tag(t)) continue;
            bool ok = !k.closer && !k.smart && !k.name.empty();
            if (ok) {
                ok = false;
                for (size_t j = trail; j < spans.size(); j++) {
                    TagKind c = tag_kind(text(j));
                    if (c.closer && c.name == k.name && s[spans[j].first] == t[0]) ok = true;
                }
            }
            if (!ok) { lead = i; changed = true; }
        }
        for (size_t j = spans.size(); j > trail && !changed; j--) {
            std::string t = text(j - 1);
            TagKind k = tag_kind(t);
            if (control_tag(t)) continue;
            bool ok = k.closer && !k.name.empty();
            if (ok) {
                ok = false;
                for (size_t i = 0; i < lead; i++) {
                    TagKind o = tag_kind(text(i));
                    if (!o.closer && o.name == k.name && s[spans[i].first] == t[0]) ok = true;
                }
            }
            if (!ok) { trail = j; changed = true; }
        }
    }
    if (lead == 0 && trail == spans.size()) return false;
    size_t a = lead ? spans[lead - 1].second : 0, b = trail < spans.size() ? spans[trail].first : s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    if (a >= b) return false;  // nothing but tags
    pre = s.substr(0, a);
    core = s.substr(a, b - a);
    suf = s.substr(b);
    return true;
}

// Punctuation runs (. , ! ? ; : … —) - where a pause tag sits relative to them carries over to the translation.
// Kind of a run ending at `end`: 'E' ellipsis ("..." / "…"), 'S' sentence end, 'C' comma-like, 'D' dash.
static char punct_kind(const std::string& s, size_t end) {
    size_t a = end;
    while (a > 0 && (std::strchr(".,!?;:", s[a - 1]) || (unsigned char)s[a - 1] >= 0x80) && s[a - 1]) a--;
    std::string r = s.substr(a, end - a);
    if (r.find("..") != std::string::npos || r.find("\xE2\x80\xA6") != std::string::npos) return 'E';
    if (r.find("\xE2\x80\x94") != std::string::npos) return 'D';
    if (r.find_first_of(".!?") != std::string::npos) return 'S';
    return 'C';
}

static std::vector<size_t> punct_ends(const std::string& s) {
    std::vector<size_t> out;
    for (size_t i = 0; i < s.size();) {
        auto at = [&](size_t k) {
            if (k < s.size() && s[k] && std::strchr(".,!?;:", s[k])) return (size_t)1;
            if (k + 2 < s.size() && (unsigned char)s[k] == 0xE2 && (unsigned char)s[k + 1] == 0x80 &&
                ((unsigned char)s[k + 2] == 0xA6 || (unsigned char)s[k + 2] == 0x94))
                return (size_t)3;  // … —
            return (size_t)0;
        };
        size_t n = at(i);
        if (!n) { i++; continue; }
        while (size_t m = at(i)) i += m;
        out.push_back(i);
    }
    return out;
}

// Pause tags in the middle of a line, taken out so the model only sees the sentence. Each is put back
// after the n-th punctuation run of the same kind in the translation ("How... {w}beautiful." ->
// "Thật… {w}đẹp quá."), or dropped when the runs of that kind do not line up.
struct Pauses {
    std::vector<std::pair<int, std::string>> at;  // punctuation run index (-1: none before it), tag
};

// runs of one kind, in order
static std::vector<size_t> runs_of(const std::string& s, char kind) {
    std::vector<size_t> out;
    for (size_t e : punct_ends(s))
        if (punct_kind(s, e) == kind) out.push_back(e);
    return out;
}

static std::string take_pauses(const std::string& s, Pauses& p) {
    std::string out;
    size_t pos = 0;
    for (auto [a, b] : tag_spans(s)) {
        std::string t = s.substr(a, b - a);
        if (!pause_tag(t)) continue;
        out += s.substr(pos, a - pos);
        size_t back = out.find_last_not_of(" \t");
        int run = -1;
        auto ends = punct_ends(out);
        if (!ends.empty() && back != std::string::npos && ends.back() == back + 1) run = (int)ends.size() - 1;
        p.at.push_back({run, t});
        pos = b;
    }
    return out + s.substr(pos);
}

static std::string put_pauses(const std::string& src, const std::string& tr, const Pauses& p) {
    auto se = punct_ends(src);
    std::vector<std::pair<size_t, std::string>> ins;
    for (auto& [run, t] : p.at) {
        if (run < 0 || run >= (int)se.size()) continue;  // no punctuation before it: dropped
        char kind = punct_kind(src, se[run]);
        auto sk = runs_of(src, kind), tk = runs_of(tr, kind);
        if (sk.size() != tk.size()) continue;  // can't tell where it goes: dropped
        size_t nth = std::find(sk.begin(), sk.end(), se[run]) - sk.begin();
        size_t k = tk[nth];
        while (k < tr.size() && tr[k] == ' ') k++;
        ins.push_back({k, t});
    }
    std::stable_sort(ins.begin(), ins.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::string out = tr;
    for (auto it = ins.rbegin(); it != ins.rend(); ++it) out.insert(it->first, it->second);
    return out;
}

// Paired style tags ({i}..{/i}, [gold]..[/gold], <b>..</b>) and pauses removed: the last resort when the
// model can't keep them - the line loses its italics/colour, not its translation.
static std::string drop_style(const std::string& s) {
    auto spans = tag_spans(s);
    std::vector<bool> drop(spans.size(), false);
    for (size_t i = 0; i < spans.size(); i++) {
        std::string t = s.substr(spans[i].first, spans[i].second - spans[i].first);
        if (pause_tag(t)) { drop[i] = true; continue; }
        TagKind k = tag_kind(t);
        if (k.closer || k.smart || k.name.empty() || k.name == "a") continue;  // {a=link} keeps its target
        for (size_t j = i + 1; j < spans.size(); j++) {
            std::string c = s.substr(spans[j].first, spans[j].second - spans[j].first);
            TagKind ck = tag_kind(c);
            if (!drop[j] && ck.closer && ck.name == k.name && c[0] == t[0]) {
                drop[i] = drop[j] = true;
                break;
            }
        }
    }
    std::string out;
    size_t pos = 0;
    for (size_t i = 0; i < spans.size(); i++) {
        if (!drop[i]) continue;
        out += s.substr(pos, spans[i].first - pos);
        pos = spans[i].second;
    }
    out += s.substr(pos);
    // "the {i}not{/i} acquired" -> no doubled spaces where a tag stood
    std::string t;
    for (char c : out)
        if (!(c == ' ' && !t.empty() && t.back() == ' ')) t += c;
    return t;
}

// What must survive translation: tags as they are, but for a SmartFormat placeholder only its head,
// branch count and the tags inside each branch (the branch text itself gets translated).
static std::string tag_shape(const std::string& t);
static std::string tags_shape(const std::string& s) {
    std::vector<std::string> x;
    for (auto& t : find_tags(s)) x.push_back(tag_shape(t));
    std::sort(x.begin(), x.end());
    std::string out;
    for (auto& t : x) out += t + '\x1f';
    return out;
}
static std::string tag_shape(const std::string& t) {
    std::string head;
    std::vector<std::string> br;
    if (!smart_branches(t, head, br)) return t;
    std::string out = head;
    for (size_t i = 0; i < br.size(); i++)
        out += (i ? "|" : "") + br[i].substr(0, cond_prefix(br[i])) + tags_shape(br[i]);
    return out + "}";
}

bool same_tags(const std::string& a, const std::string& b) { return tags_shape(a) == tags_shape(b); }

static std::wstring widen(const std::string& s);
static std::string narrow(const std::wstring& w);

// A CJK / Hangul / kana / Thai / Cyrillic letter the source doesn't have: Hy-MT2 sometimes leaves a
// word in another language ("các 설정 của Steam").
static bool foreign_script(wchar_t c) {
    return (c >= 0x0400 && c <= 0x04FF) || (c >= 0x0E00 && c <= 0x0E7F) || (c >= 0x1100 && c <= 0x11FF) ||
           (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
           (c >= 0xFF00 && c <= 0xFFEF);
}

// Final check on a translated line, on top of same_tags: find_tags only sees well-formed tags, so the
// model's inventions slip past it - "{Sát thương}{Damage}" (SmartFormat throws on an unknown selector),
// "[Hướng dẫn nâng cấp][gold]", a leftover token "[3]", "[hoa vàng]</[gold]". Every bracket character
// must occur as often as in the source, and no more tag pairs may hold no word than in the source
// ("bộ bài của bạn[gold][/gold]." / "[gold].[/gold]": the word moved out of its colour tag).
static int empty_pairs(const std::string& s) {
    int n = 0;
    for (size_t c = s.find("[/"); c != std::string::npos; c = s.find("[/", c + 2)) {
        size_t e = s.find(']', c);
        if (e == std::string::npos) break;
        std::string open = "[" + s.substr(c + 2, e - c - 1);
        size_t o = s.rfind(open, c);
        if (o == std::string::npos) continue;
        bool word = false;
        for (size_t i = o + open.size(); i < c && !word; i++)
            word = std::isalnum((unsigned char)s[i]) || (unsigned char)s[i] >= 0x80 || s[i] == '{';
        n += !word;
    }
    return n;
}

bool translation_ok(const std::string& src, const std::string& out) {
    if (!same_tags(src, out) || empty_pairs(out) > empty_pairs(src)) return false;
    for (char c : std::string("{}[]<>"))
        if (std::count(src.begin(), src.end(), c) != std::count(out.begin(), out.end(), c)) return false;
    std::wstring ws = widen(src);
    for (wchar_t c : widen(out))
        if (foreign_script(c) && ws.find(c) == std::wstring::npos) return false;
    return true;
}

// translation_ok, then drop the Markdown-style "  \n" the model likes to add
static bool accept_translation(const std::string& src, std::string& out) {
    if (!translation_ok(src, out)) return false;
    if (src.find(" \n") == std::string::npos && src.find("\t\n") == std::string::npos) {
        std::string t;
        for (char c : out) {
            if (c == '\n')
                while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
            t += c;
        }
        out = t;
    }
    return true;
}

// SmartFormat expressions -> {p0} {p1}...: a small model keeps a bare {name} intact but mangles
// "{Cards:plural:card|cards}". Returns the text unchanged (and `smart` empty) when there are none.
static std::string simplify_smart(const std::string& s, std::vector<std::string>& smart) {
    smart.clear();
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        size_t e = s[i] == '{' ? placeholder_end(s, i) : 0;
        size_t sel = i + 1;
        while (sel < s.size() && (std::isalnum((unsigned char)s[sel]) || s[sel] == '_' || s[sel] == '.')) sel++;
        if (e && sel < e && s[sel] == ':') {
            out += "{p" + std::to_string(smart.size()) + "}";
            smart.push_back(s.substr(i, e - i + 1));
            i = e;
        } else out += s[i];
    }
    return out;
}

// plural branches that only differ by an English plural ending: card|cards, [blue]{}[/blue] turn|... turns
static bool english_plural_of(const std::string& one, const std::string& many) {
    return many == one + "s" || many == one + "es";
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static bool has_ascii_letter(const std::string& s) {
    for (unsigned char c : s) if (std::isalpha(c)) return true;
    return false;
}

static bool all_upper(const std::string& s) {
    std::wstring w = widen(s);
    bool letter = false;
    for (wchar_t c : w) {
        if (IsCharAlphaW(c)) {
            letter = true;
            if (IsCharLowerW(c)) return false;
        }
    }
    return letter;
}

// "KHÔNG" for "No": only keep all-caps when the source was all-caps
static std::string fix_caps(const std::string& src, const std::string& out) {
    if (!all_upper(out) || all_upper(src)) return out;
    std::wstring w = widen(out);
    if (w.size() > 1) CharLowerBuffW(w.data() + 1, (DWORD)(w.size() - 1));
    return narrow(w);
}

// Change letter case outside <tags> and {placeholders} (tag names / colors must stay as they are).
static std::string recase(const std::string& s, bool upper) {
    std::wstring w = widen(s);
    for (size_t i = 0; i < w.size(); i++) {
        if (w[i] == L'<' || w[i] == L'{' || w[i] == L'[') {
            size_t e = w.find(w[i] == L'<' ? L'>' : w[i] == L'{' ? L'}' : L']', i);
            if (e != std::wstring::npos) { i = e; continue; }
        }
        if (upper) CharUpperBuffW(&w[i], 1);
        else CharLowerBuffW(&w[i], 1);
    }
    return narrow(w);
}

// Small models translate ALL-CAPS badly: send "Refuel {0}% of fuel on loop", upper-case the result.
static std::string sentence_case(const std::string& s) {
    std::string low = recase(s, false);
    for (size_t i = 0; i < low.size(); i++) {
        if (low[i] == '<' || low[i] == '{' || low[i] == '[') {
            size_t e = low.find(low[i] == '<' ? '>' : low[i] == '{' ? '}' : ']', i);
            if (e != std::string::npos) { i = e; continue; }
        }
        if (std::isalpha((unsigned char)low[i])) { low[i] = (char)std::toupper((unsigned char)low[i]); break; }
    }
    return low;
}

static bool word_char(char c) { return std::isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

static std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

// ------------------------------------------------------------------ HTTP
HttpResult http_request(const wchar_t* method, const wchar_t* host, int port, bool https, const std::wstring& path,
                        const std::string& contentType, const std::string& body, int timeoutMs) {
    HttpResult res;
    HINTERNET ses = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) unitytl",
                                https ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return res;
    WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET con = WinHttpConnect(ses, host, (INTERNET_PORT)port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req) {
        std::wstring hdr = contentType.empty() ? L"" : L"Content-Type: " + widen(contentType) + L"\r\n";
        if (WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), (DWORD)-1,
                               body.empty() ? WINHTTP_NO_REQUEST_DATA : (void*)body.data(), (DWORD)body.size(),
                               (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, sz = sizeof status;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz,
                                nullptr);
            res.status = status;
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail) {
                std::string chunk(avail, 0);
                DWORD got = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &got)) break;
                res.body.append(chunk.data(), got);
            }
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return res;
}

// ------------------------------------------------------------------ local model (llama-server)
static const char* lang_name(const std::string& code) {
    if (code == "en") return "English";
    if (code == "zh") return "Chinese";
    if (code == "ja") return "Japanese";
    if (code == "ko") return "Korean";
    return "Vietnamese";
}

bool local_server_ready(int port) {
    return http_request(L"GET", L"127.0.0.1", port, false, L"/health", "", "", 1500).status == 200;
}

std::optional<std::string> local_translate(const std::string& text, const std::string& to, int port, double temperature) {
    // prompt + sampling from the Hy-MT2 model card (no system prompt)
    std::string prompt = std::string("Translate the following text into ") + lang_name(to) +
                         ". Note that you should only output the translated result without any additional "
                         "explanation:\n\n" + text;
    Json msg = Json::object();
    msg["role"] = Json("user");
    msg["content"] = Json(prompt);
    Json msgs = Json::array();
    msgs.push(msg);
    Json req = Json::object();
    req["messages"] = msgs;
    req["temperature"] = Json(temperature);
    req["top_p"] = Json(0.6);
    req["top_k"] = Json(20);
    req["repeat_penalty"] = Json(1.05);
    req["max_tokens"] = Json(1024);
    req["stream"] = Json(false);
    std::string body = req.dump();
    for (int attempt = 0; attempt < 3; attempt++) {
        HttpResult r = http_request(L"POST", L"127.0.0.1", port, false, L"/v1/chat/completions", "application/json",
                                    body, 120000);
        if (r.status != 200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        try {
            Json j = Json::parse(r.body);
            std::string out = j.at("choices").items().at(0).at("message").get_str("content");
            size_t a = out.find_first_not_of(" \t\r\n"), b = out.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) return std::nullopt;
            return out.substr(a, b - a + 1);
        } catch (const std::exception&) {
            continue;
        }
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ Google
std::optional<std::string> google_translate(const std::string& text, const std::string& from, const std::string& to) {
    std::string query = "client=gtx&sl=" + from + "&tl=" + to + "&dt=t&q=" + url_encode(text);
    for (int attempt = 0; attempt < 5; attempt++) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(1500 * attempt));
        HttpResult r = http_request(L"POST", L"translate.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, true,
                                    L"/translate_a/single", "application/x-www-form-urlencoded;charset=utf-8", query);
        if (r.status != 200 || r.body.empty()) continue;
        try {
            Json j = Json::parse(r.body);
            std::string out;
            for (auto& seg : j.items().at(0).items())
                if (seg.is_array() && !seg.items().empty() && seg.items()[0].is_string()) out += seg.items()[0].str();
            return out;
        } catch (const std::exception&) {
            continue;
        }
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ Translator
Translator::Translator(const Glossary& g, EngineConfig engine, std::string from, std::string to)
    : g_(g), eng_(engine), from_(std::move(from)), to_(std::move(to)) {
    for (auto& k : g_.keep) terms_.emplace_back(k, k);
    for (auto& kv : g_.terms) terms_.emplace_back(kv.first, kv.second);
    std::sort(terms_.begin(), terms_.end(), [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
}

std::optional<std::string> Translator::segment(const std::string& seg) {
    std::vector<std::string> tokens;
    auto token = [&](const std::string& v) {
        tokens.push_back(v);
        return " [" + std::to_string(tokens.size() - 1) + "] ";
    };
    // 1) tags and placeholders (a SmartFormat placeholder comes back with its branches translated)
    std::string clean;
    auto tags = find_tags(seg);
    std::vector<std::string> restored;
    size_t pos = 0;
    for (auto& t : tags) {
        size_t at = seg.find(t, pos);
        restored.push_back(t[0] == '{' ? translate_branches(t) : t);
        clean += seg.substr(pos, at - pos) + token(restored.back());
        pos = at + t.size();
    }
    clean += seg.substr(pos);
    if (!has_ascii_letter(clean)) {
        std::string s = seg;
        size_t p = 0;
        for (size_t ti = 0; ti < tags.size(); ti++) {
            p = s.find(tags[ti], p);
            s.replace(p, tags[ti].size(), restored[ti]);
            p += restored[ti].size();
        }
        return s;
    }
    // 2) names + glossary terms, whole words, longest first
    for (auto& [en, vi] : terms_) {
        size_t at = 0;
        while ((at = clean.find(en, at)) != std::string::npos) {
            bool okL = at == 0 || !word_char(clean[at - 1]);
            bool okR = at + en.size() >= clean.size() || !word_char(clean[at + en.size()]);
            if (!okL || !okR) { at += en.size(); continue; }
            std::string tk = token(vi);
            clean.replace(at, en.size(), tk);
            at += tk.size();
        }
    }
    std::string stripped;
    for (size_t i = 0; i < clean.size(); i++) {
        if (clean[i] == '[') { size_t e = clean.find(']', i); if (e != std::string::npos) { i = e; continue; } }
        stripped += clean[i];
    }
    std::string out;
    if (!has_ascii_letter(stripped)) out = clean;
    else {
        size_t a = clean.find_first_not_of(' '), b = clean.find_last_not_of(' ');
        auto r = engine(clean.substr(a, b - a + 1));
        if (!r) return std::nullopt;
        out = *r;
    }
    // 3) restore tokens (Google may add spaces inside the brackets)
    for (size_t i = 0; i < tokens.size(); i++) {
        std::string num = std::to_string(i);
        bool found = false;
        for (size_t at = out.find('['); at != std::string::npos; at = out.find('[', at + 1)) {
            size_t j = at + 1;
            while (j < out.size() && out[j] == ' ') j++;
            if (out.compare(j, num.size(), num) != 0) continue;
            size_t k = j + num.size();
            while (k < out.size() && out[k] == ' ') k++;
            if (k >= out.size() || out[k] != ']') continue;
            size_t st = at, en = k + 1;
            while (st > 0 && out[st - 1] == ' ') st--;
            while (en < out.size() && out[en] == ' ') en++;
            out.replace(st, en - st, " " + tokens[i] + " ");
            found = true;
            break;
        }
        if (!found) return std::nullopt;
    }
    // 4) tidy spacing: no space before punctuation / after an opening tag
    std::string t;
    for (size_t i = 0; i < out.size(); i++) {
        char c = out[i];
        if (c == ' ' && i + 1 < out.size() && std::strchr(".,!?:;)>", out[i + 1])) continue;
        if (c == ' ' && !t.empty() && t.back() == ' ') continue;
        if (c == ' ' && !t.empty() && t.back() == '>') {
            size_t lt = t.rfind('<');
            if (lt != std::string::npos && lt + 1 < t.size() && t[lt + 1] != '/') continue;
        }
        t += c;
    }
    // tokens come back as " tag ": drop the spaces the source didn't have ("[shake]Run[/shake]")
    for (size_t ti = 0; ti < tags.size(); ti++) {
        size_t at = seg.find(tags[ti]);
        const std::string& tag = restored[ti];
        bool spaceBefore = at > 0 && seg[at - 1] == ' ';
        bool spaceAfter = at + tags[ti].size() < seg.size() && seg[at + tags[ti].size()] == ' ';
        for (size_t p = t.find(tag); p != std::string::npos; p = t.find(tag, p + tag.size())) {
            if (!spaceAfter && p + tag.size() < t.size() && t[p + tag.size()] == ' ') t.erase(p + tag.size(), 1);
            if (!spaceBefore && p > 0 && t[p - 1] == ' ') { t.erase(p - 1, 1); p--; }
        }
    }
    size_t a = t.find_first_not_of(' '), b = t.find_last_not_of(' ');
    t = a == std::string::npos ? "" : t.substr(a, b - a + 1);
    size_t lead = seg.find_first_not_of(" \t");
    if (lead != std::string::npos && lead > 0) t = seg.substr(0, lead) + t;
    return fix_caps(seg, t);
}

// {Cards:plural:card|cards} -> {Cards:plural:lá bài|lá bài}: each branch is translated on its own
// (keeping its leading/trailing spaces and line breaks); a branch that fails stays in English.
std::string Translator::translate_branches(const std::string& placeholder) {
    std::string head;
    std::vector<std::string> br;
    if (!smart_branches(placeholder, head, br)) return placeholder;
    std::string out = head;
    bool plural = head.size() > 8 && head.compare(head.size() - 8, 8, ":plural:") == 0;
    std::vector<std::string> done;
    for (size_t i = 0; i < br.size(); i++) {
        if (plural && i > 0 && english_plural_of(br[0], br[i])) {  // Vietnamese has no plural form
            done.push_back(done[0]);
            out += "|" + done[0];
            continue;
        }
        size_t cp = cond_prefix(br[i]);  // ">0?" stays as it is, only the text after it is translated
        const std::string cond = br[i].substr(0, cp), b = br[i].substr(cp);
        std::string res = b;
        size_t a = b.find_first_not_of(" \t\r\n"), z = b.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) {
            std::string core = b.substr(a, z - a + 1);
            auto c = branchCache_.find(core);
            std::optional<std::string> r;
            if (c != branchCache_.end()) r = c->second;
            else {
                bool nf = netFail_;
                r = translate(core);
                netFail_ = nf || netFail_;
                if (r) branchCache_[core] = *r;
            }
            if (r) res = b.substr(0, a) + *r + b.substr(z + 1);
        }
        done.push_back(cond + res);
        out += (i ? "|" : "") + done.back();
    }
    return out + "}";
}

std::optional<std::string> Translator::engine(const std::string& text, double temperature) {
    std::optional<std::string> r;
    if (eng_.kind == EngineConfig::Local) {
        bool caps = all_upper(text);
        r = local_translate(caps ? sentence_case(text) : text, to_, eng_.port, temperature);
        if (r && caps) r = recase(*r, true);
    } else {
        r = google_translate(text, from_, to_);
    }
    if (!r) netFail_ = true;
    return r;
}

std::string Translator::last_error() const {
    return netFail_ ? (eng_.kind == EngineConfig::Local ? "khong ket noi duoc model local" : "loi mang / Google tu choi")
                    : (eng_.kind == EngineConfig::Local ? "model lam mat tag/bien {0}" : "Google lam mat tag/bien {0}");
}

std::optional<std::string> Translator::translate(const std::string& text) {
    netFail_ = false;
    size_t a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
    if (a != std::string::npos) {
        std::string core = text.substr(a, b - a + 1);
        auto w = g_.whole.find(core);
        if (w != g_.whole.end()) return text.substr(0, a) + w->second + text.substr(b + 1);
    }
    std::vector<std::string> smart;
    std::string simple = simplify_smart(text, smart);
    if (!smart.empty()) {
        auto r = translate(simple);
        if (!r) return std::nullopt;
        std::string out = *r;
        for (size_t i = 0; i < smart.size(); i++) {
            std::string p = "{p" + std::to_string(i) + "}";
            size_t at = out.find(p);
            if (at == std::string::npos) return std::nullopt;
            out.replace(at, p.size(), translate_branches(smart[i]));
        }
        if (!accept_translation(text, out)) return std::nullopt;
        return out;
    }
    // markup around the whole line never reaches the model
    std::string pre, core, suf;
    if (peel_markup(text, pre, core, suf)) {
        auto r = translate(core);
        if (!r) return std::nullopt;
        return pre + *r + suf;
    }
    // pauses inside the line: translate the sentence alone, put them back by punctuation
    Pauses pauses;
    std::string noPause = take_pauses(text, pauses);
    if (!pauses.at.empty()) {
        auto r = translate(noPause);
        if (!r) return std::nullopt;
        return put_pauses(noPause, *r, pauses);
    }
    if (auto r = translate_line(text)) return r;
    if (netFail_) return std::nullopt;
    // style tags the model could not keep: the translation without them beats the English line
    std::string plain = drop_style(text);
    if (plain != text && has_ascii_letter(plain)) return translate_line(plain);
    return std::nullopt;
}

std::optional<std::string> Translator::translate_line(const std::string& text) {
    // A local LLM keeps tags and line breaks by itself and translates better with the whole line as
    // context, so send the raw text first; fall back to the token-protected path when the line uses
    // glossary terms or the model dropped a tag.
    if (eng_.kind == EngineConfig::Local && has_ascii_letter(text)) {
        bool usesTerm = false;
        for (auto& t : terms_) {
            size_t at = text.find(t.first);
            if (at != std::string::npos && (at == 0 || !word_char(text[at - 1])) &&
                (at + t.first.size() >= text.size() || !word_char(text[at + t.first.size()]))) {
                usesTerm = true;
                break;
            }
        }
        if (!usesTerm) {
            auto newlines = [](const std::string& s) { return std::count(s.begin(), s.end(), '\n'); };
            // sampling is random: a dropped {0}, or a collapsed multi-line box text, usually comes back
            // clean on a retry at low temperature
            for (double temp : {0.7, 0.2, 0.2}) {
                auto r = engine(text, temp);
                if (!r) return std::nullopt;  // server unreachable: no point retrying
                if (accept_translation(text, *r) &&
                    (text.find("\\n") == std::string::npos) == (r->find("\\n") == std::string::npos) &&
                    newlines(text) == newlines(*r))
                    return fix_caps(text, *r);
            }
        }
    }
    // split on literal "\n" (two chars) and real line breaks; translate pieces separately
    std::string out, cur;
    auto flush = [&]() -> bool {
        if (cur.find_first_not_of(" \t") == std::string::npos) { out += cur; cur.clear(); return true; }
        auto r = segment(cur);
        if (!r) return false;
        out += *r;
        cur.clear();
        return true;
    };
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '{') {  // a placeholder stays whole, even when its branches hold line breaks
            if (size_t e = placeholder_end(text, i)) {
                cur += text.substr(i, e - i + 1);
                i = e;
                continue;
            }
        }
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
            if (!flush()) return std::nullopt;
            out += "\\n";
            i++;
        } else if (text[i] == '\r' || text[i] == '\n') {
            if (!flush()) return std::nullopt;
            out += text[i];
        } else cur += text[i];
    }
    if (!flush()) return std::nullopt;
    if (!accept_translation(text, out)) return std::nullopt;
    return out;
}
