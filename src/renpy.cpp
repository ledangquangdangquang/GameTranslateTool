#include "renpy.h"

#include <windows.h>

#include <zlib.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>

static std::string lower_ascii(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool ends_with(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

static std::vector<std::string> split_lines_any(const std::string& text) {
    std::vector<std::string> out;
    size_t st = 0;
    while (st <= text.size()) {
        size_t e = text.find('\n', st);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(st, e - st);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (out.empty() && l.rfind("\xEF\xBB\xBF", 0) == 0) l = l.substr(3);
        out.push_back(std::move(l));
        st = e + 1;
    }
    return out;
}

static bool is_script(const std::string& name) {
    std::string l = lower_ascii(name);
    return ends_with(l, ".rpy") || ends_with(l, ".rpyc") || ends_with(l, ".rpym") || ends_with(l, ".rpymc") ||
           ends_with(l, "_ren.py");
}

// ------------------------------------------------------------------ locate
static bool renpy_root(const fs::path& dir, std::string& name) {
    std::error_code ec;
    if (!fs::is_directory(dir / "renpy", ec) || !fs::is_directory(dir / "game", ec)) return false;
    std::string any;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || lower_ascii(pathstr(e.path().extension())) != ".py") continue;
        std::string stem = pathstr(e.path().stem());
        if (fs::exists(dir / u8path(stem + ".exe"), ec)) {
            name = stem;
            return true;
        }
        any = stem;
    }
    name = any;
    return !any.empty();
}

bool renpy_find(const fs::path& any, fs::path& root, std::string& name) {
    std::error_code ec;
    fs::path start = fs::is_directory(any, ec) ? any : any.parent_path();
    std::vector<fs::path> cand{start, start.parent_path()};
    for (auto it = fs::directory_iterator(start, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec)) cand.push_back(it->path());
    for (auto& d : cand)
        if (renpy_root(d, name)) {
            root = d;
            return true;
        }
    return false;
}

// ------------------------------------------------------------------ .rpa
namespace {
// What a pickled RPA index can hold. Anything else (a class, a reduce call) is refused.
struct PVal {
    enum Kind { None, Int, Str, List, Dict, Mark } kind = None;
    int64_t i = 0;
    std::string s;  // str (UTF-8) or bytes
    std::vector<std::shared_ptr<PVal>> items;
    std::vector<std::pair<std::shared_ptr<PVal>, std::shared_ptr<PVal>>> dict;
};
using PV = std::shared_ptr<PVal>;

PV pv(PVal::Kind k) {
    auto p = std::make_shared<PVal>();
    p->kind = k;
    return p;
}

PV unpickle(const Bytes& b) {
    size_t at = 0;
    auto need = [&](size_t n) {
        if (at + n > b.size()) throw ParseError("rpa: index pickle bi cat");
    };
    auto le = [&](size_t n) {
        need(n);
        uint64_t v = 0;
        for (size_t k = 0; k < n; k++) v |= (uint64_t)b[at + k] << (8 * k);
        at += n;
        return v;
    };
    auto str = [&](size_t n) {
        need(n);
        std::string s((const char*)b.data() + at, n);
        at += n;
        return s;
    };
    std::vector<PV> st;
    std::vector<size_t> marks;
    std::map<uint64_t, PV> memo;
    auto pop = [&]() {
        if (st.empty()) throw ParseError("rpa: pickle rong");
        PV v = st.back();
        st.pop_back();
        return v;
    };
    auto pop_mark = [&]() {
        if (marks.empty()) throw ParseError("rpa: pickle thieu MARK");
        size_t m = marks.back();
        marks.pop_back();
        std::vector<PV> v(st.begin() + m, st.end());
        st.resize(m);
        return v;
    };
    auto push_int = [&](int64_t v) {
        PV p = pv(PVal::Int);
        p->i = v;
        st.push_back(p);
    };
    auto push_str = [&](std::string s) {
        PV p = pv(PVal::Str);
        p->s = std::move(s);
        st.push_back(p);
    };
    auto tuple = [&](std::vector<PV> v) {
        PV p = pv(PVal::List);
        p->items = std::move(v);
        st.push_back(p);
    };
    while (true) {
        need(1);
        uint8_t op = b[at++];
        switch (op) {
            case 0x80: at++; break;                  // PROTO
            case 0x95: le(8); break;                 // FRAME
            case '}': st.push_back(pv(PVal::Dict)); break;
            case ']': case ')': st.push_back(pv(PVal::List)); break;
            case '(': marks.push_back(st.size()); break;
            case 'X': push_str(str(le(4))); break;   // BINUNICODE
            case 0x8c: push_str(str(le(1))); break;  // SHORT_BINUNICODE
            case 0x8d: push_str(str(le(8))); break;  // BINUNICODE8
            case 'T': case 'B': push_str(str(le(4))); break;  // BINSTRING / BINBYTES
            case 'U': case 'C': push_str(str(le(1))); break;  // SHORT_BINSTRING / SHORT_BINBYTES
            case 0x8e: push_str(str(le(8))); break;           // BINBYTES8
            case 'J': push_int((int32_t)le(4)); break;
            case 'K': push_int((int64_t)le(1)); break;
            case 'M': push_int((int64_t)le(2)); break;
            case 0x8a: case 0x8b: {  // LONG1 / LONG4: little-endian two's complement
                size_t n = op == 0x8a ? le(1) : le(4);
                if (n > 8) throw ParseError("rpa: so qua lon");
                need(n);
                uint64_t v = 0;
                for (size_t k = 0; k < n; k++) v |= (uint64_t)b[at + k] << (8 * k);
                if (n && n < 8 && (b[at + n - 1] & 0x80)) v |= ~0ull << (8 * n);
                at += n;
                push_int((int64_t)v);
                break;
            }
            case 'N': st.push_back(pv(PVal::None)); break;
            case 0x88: push_int(1); break;
            case 0x89: push_int(0); break;
            case 'q': memo[le(1)] = st.empty() ? nullptr : st.back(); break;
            case 'r': memo[le(4)] = st.empty() ? nullptr : st.back(); break;
            case 0x94: memo[memo.size()] = st.empty() ? nullptr : st.back(); break;
            case 'h': case 'j': {
                auto m = memo.find(le(op == 'h' ? 1 : 4));
                if (m == memo.end() || !m->second) throw ParseError("rpa: memo sai");
                st.push_back(m->second);
                break;
            }
            case 't': tuple(pop_mark()); break;
            case 0x85: { PV a = pop(); tuple({a}); break; }
            case 0x86: { PV b2 = pop(), a = pop(); tuple({a, b2}); break; }
            case 0x87: { PV c = pop(), b2 = pop(), a = pop(); tuple({a, b2, c}); break; }
            case 'a': { PV v = pop(); if (st.empty()) throw ParseError("rpa"); st.back()->items.push_back(v); break; }
            case 'e': { auto v = pop_mark(); if (st.empty()) throw ParseError("rpa"); for (auto& x : v) st.back()->items.push_back(x); break; }
            case 's': { PV v = pop(), k = pop(); if (st.empty()) throw ParseError("rpa"); st.back()->dict.push_back({k, v}); break; }
            case 'u': {
                auto v = pop_mark();
                if (st.empty() || v.size() % 2) throw ParseError("rpa: SETITEMS le");
                for (size_t k = 0; k + 1 < v.size(); k += 2) st.back()->dict.push_back({v[k], v[k + 1]});
                break;
            }
            case '.': return pop();
            default: throw ParseError("rpa: index co kieu pickle khong ho tro (0x" + std::to_string(op) + ")");
        }
    }
}
}  // namespace

static Bytes inflate_all(const Bytes& z) {
    z_stream s{};
    if (inflateInit(&s) != Z_OK) throw ParseError("zlib");
    Bytes out;
    uint8_t buf[1 << 16];
    s.next_in = (Bytef*)z.data();
    s.avail_in = (uInt)z.size();
    int r;
    do {
        s.next_out = buf;
        s.avail_out = sizeof buf;
        r = inflate(&s, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) {
            inflateEnd(&s);
            throw ParseError("rpa: index zlib hong");
        }
        out.insert(out.end(), buf, buf + (sizeof buf - s.avail_out));
    } while (r != Z_STREAM_END);
    inflateEnd(&s);
    return out;
}

std::vector<RpaEntry> rpa_index(const fs::path& rpa) {
    std::ifstream f(rpa, std::ios::binary);
    std::string hdr;
    std::getline(f, hdr);
    // "RPA-3.0 <index offset hex> <xor key hex>" / "RPA-2.0 <index offset hex>"
    std::vector<std::string> w;
    for (size_t st = 0, sp; st < hdr.size(); st = sp + 1) {
        sp = hdr.find(' ', st);
        if (sp == std::string::npos) sp = hdr.size();
        if (sp > st) w.push_back(hdr.substr(st, sp - st));
    }
    bool v3 = !w.empty() && w[0] == "RPA-3.0";
    if (w.size() < (v3 ? 3u : 2u) || (!v3 && w[0] != "RPA-2.0"))
        throw ParseError(pathstr(rpa.filename()) + ": khong phai RPA-3.0/2.0");
    uint64_t off = std::stoull(w[1], nullptr, 16);
    uint64_t key = v3 ? std::stoull(w[2], nullptr, 16) : 0;
    f.seekg(0, std::ios::end);
    uint64_t end = (uint64_t)f.tellg();
    if (off >= end) throw ParseError("rpa: offset index sai");
    Bytes z(end - off);
    f.seekg((std::streamoff)off);
    f.read((char*)z.data(), (std::streamsize)z.size());
    Bytes raw = inflate_all(z);
    PV idx = unpickle(raw);
    std::vector<RpaEntry> out;
    if (idx->kind != PVal::Dict) throw ParseError("rpa: index khong phai dict");
    for (auto& [k, v] : idx->dict) {
        if (k->kind != PVal::Str || v->kind != PVal::List || v->items.empty()) continue;
        PV t = v->items[0];
        if (t->kind != PVal::List || t->items.size() < 2) continue;
        RpaEntry e;
        e.name = k->s;
        e.offset = (uint64_t)t->items[0]->i ^ key;
        e.size = (uint64_t)t->items[1]->i ^ key;
        if (t->items.size() > 2 && t->items[2]->kind == PVal::Str) e.prefix = t->items[2]->s;
        if (e.offset + (e.size - std::min<uint64_t>(e.size, e.prefix.size())) <= end) out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return out;
}

Bytes rpa_read(const fs::path& rpa, const RpaEntry& e) {
    std::ifstream f(rpa, std::ios::binary);
    Bytes out(e.prefix.begin(), e.prefix.end());
    size_t rest = e.size > e.prefix.size() ? (size_t)(e.size - e.prefix.size()) : 0;
    out.resize(e.prefix.size() + rest);
    f.seekg((std::streamoff)e.offset);
    f.read((char*)out.data() + e.prefix.size(), (std::streamsize)rest);
    return out;
}

// ------------------------------------------------------------------ screen text the generator misses
// Ren'Py's own scan only takes _("...") strings and menu choices, but Text translates any string with
// an "old" entry when it is shown. Literals in `text "..."`, `textbutton "..."`, `label "..."`, the
// first string of a `use some_screen(` call (Interact With Me's main menu: use nav_img_button_main(
// "Interact", ...)), Character("Name") and Notify("...") get their own strings block - minus those the
// generated files already have, since Ren'Py refuses two translations of one string.
static std::string first_literal(const std::string& s, size_t from) {
    size_t q = s.find('"', from);
    if (q == std::string::npos) return {};
    for (size_t k = q + 1; k < s.size(); k++) {
        if (s[k] == '\\') k++;
        else if (s[k] == '"') return s.substr(q + 1, k - q - 1);
    }
    return {};
}

static bool ui_text(const std::string& t) {
    if (t.empty() || t.size() > 600) return false;
    std::string words;  // without [interpolations] and {tags}
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '[' || t[i] == '{') {
            size_t e = t.find(t[i] == '[' ? ']' : '}', i);
            if (e != std::string::npos) { i = e; continue; }
        }
        words += t[i];
    }
    if (std::none_of(words.begin(), words.end(), [](char c) { return std::isalpha((unsigned char)c); })) return false;
    std::string l = lower_ascii(t);
    for (const char* ext : {".png", ".jpg", ".webp", ".ogg", ".mp3", ".wav", ".opus", ".ttf", ".otf", ".rpy", ".webm"})
        if (l.find(ext) != std::string::npos) return false;
    return t.find('/') == std::string::npos || t.find(' ') != std::string::npos;
}

static void extra_strings(const fs::path& gameDir, const fs::path& out, const std::string& lang) {
    std::set<std::string> have;
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(out, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".rpy") continue;
        auto lines = split_lines_any(read_text(e.path()));
        for (auto& l : lines) {
            size_t f = l.find_first_not_of(' ');
            if (f != std::string::npos && l.compare(f, 5, "old \"") == 0) have.insert(first_literal(l, f));
        }
    }
    std::vector<std::pair<std::string, std::string>> found;  // text, "file:line"
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(gameDir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::string r = pathstr(it->path().lexically_relative(gameDir));
        if (it->is_directory() && (r == "tl" || r == "cache" || r == "saves")) it.disable_recursion_pending();
        else if (it->is_regular_file() && lower_ascii(pathstr(it->path().extension())) == ".rpy") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        auto lines = split_lines_any(read_text(f));
        bool pendingUse = false;
        for (size_t i = 0; i < lines.size(); i++) {
            const std::string& l = lines[i];
            size_t a = l.find_first_not_of(" \t");
            if (a == std::string::npos || l[a] == '#') continue;
            std::string lit;
            auto kw = [&](const char* k) {
                size_t n = std::strlen(k);
                return l.compare(a, n, k) == 0 && a + n < l.size() && l[a + n] == ' ' &&
                       l.find_first_not_of(' ', a + n) != std::string::npos && l[l.find_first_not_of(' ', a + n)] == '"';
            };
            if (kw("text") || kw("textbutton") || kw("label")) lit = first_literal(l, a);
            else if (pendingUse && l[a] == '"') lit = first_literal(l, a);
            else if (l.compare(a, 4, "use ") == 0 && l.find('(') != std::string::npos) {
                size_t p = l.find('(');
                lit = first_literal(l, p);
                if (!lit.empty() && l.find('"', p) > l.find(',', p)) lit.clear();  // only the first argument
                pendingUse = lit.empty() && l.find(')', p) == std::string::npos;
                if (lit.empty()) continue;
            }
            for (const char* call : {"Character(\"", "Notify(\"", "renpy.notify(\""})
                if (size_t p = l.find(call); lit.empty() && p != std::string::npos) lit = first_literal(l, p);
            pendingUse = false;
            if (!ui_text(lit) || !have.insert(lit).second) continue;
            std::string where = pathstr(f.lexically_relative(gameDir.parent_path()));
            std::replace(where.begin(), where.end(), '\\', '/');
            found.push_back({lit, where + ":" + std::to_string(i + 1)});
        }
    }
    if (found.empty()) return;
    std::string t = "# unitytl: chu hien tren man hinh ma Ren'Py khong quet (text \"...\", textbutton, use ...(\"...\"))\n\n"
                    "translate " + lang + " strings:\n\n";
    for (auto& [s, where] : found) t += "    # " + where + "\n    old \"" + s + "\"\n    new \"" + s + "\"\n\n";
    write_file(out / "zz_unitytl_extra.rpy", "\xEF\xBB\xBF" + t);
}

// ------------------------------------------------------------------ generate
static fs::path find_python(const fs::path& root) {
    std::error_code ec;
    for (const char* d : {"py3-windows-x86_64", "windows-x86_64", "py2-windows-x86_64", "windows-i686", "py3-windows-i686"})
        if (fs::exists(root / "lib" / d / "python.exe", ec)) return root / "lib" / d / "python.exe";
    return {};
}

// Hard link (same volume, no copy) or copy. Only for files Ren'Py never writes: archives and media.
static void link_or_copy(const fs::path& from, const fs::path& to) {
    if (CreateHardLinkW(to.wstring().c_str(), from.wstring().c_str(), nullptr)) return;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}

bool renpy_generate(const fs::path& root, const std::string& name, const std::string& lang, const fs::path& out,
                    const std::function<void(const std::string&)>& log) {
    std::error_code ec;
    fs::path py = find_python(root), script = root / u8path(name + ".py");
    if (py.empty() || !fs::exists(script, ec)) {
        log("  ! khong tim thay lib/*/python.exe hoac " + name + ".py cua game");
        return false;
    }
    fs::path tmp = root / "_unitytl_renpy_tmp", game = root / "game";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "game", ec);
    int unpacked = 0, sources = 0;
    std::error_code walk;  // kept apart: the loop stops on its error only
    for (auto it = fs::recursive_directory_iterator(game, walk); !walk && it != fs::recursive_directory_iterator();
         it.increment(walk)) {
        std::string r = pathstr(it->path().lexically_relative(game));
        std::replace(r.begin(), r.end(), '\\', '/');
        std::string lr = lower_ascii(r);
        if (it->is_directory(ec)) {
            // the existing translation must not count as done; cache/saves are rebuilt by Ren'Py
            if (lr == "cache" || lr == "saves" || lr == "tl/" + lower_ascii(lang)) it.disable_recursion_pending();
            else fs::create_directories(tmp / "game" / u8path(r), ec);
            continue;
        }
        fs::path dst = tmp / "game" / u8path(r);
        if (lower_ascii(pathstr(it->path().extension())) == ".rpa") {
            std::vector<RpaEntry> idx;
            try {
                idx = rpa_index(it->path());
            } catch (const std::exception& ex) {
                log("  ! " + r + ": " + ex.what());
            }
            if (std::none_of(idx.begin(), idx.end(), [](const RpaEntry& e) { return is_script(e.name); })) {
                link_or_copy(it->path(), dst);
                continue;
            }
            for (auto& e : idx) {  // the script archive: unpacked, so Ren'Py sees loose .rpy files
                fs::path d = tmp / "game" / u8path(e.name);
                if (pathstr(d.lexically_normal()).rfind(pathstr((tmp / "game").lexically_normal()), 0) != 0) continue;
                fs::create_directories(d.parent_path(), ec);
                if (fs::exists(d, ec)) continue;
                write_file(d, rpa_read(it->path(), e));
                unpacked++;
                sources += ends_with(lower_ascii(e.name), ".rpy");
            }
            continue;
        }
        sources += ends_with(lr, ".rpy");
        if (!is_script(r) && fs::file_size(it->path(), ec) > (1u << 20)) link_or_copy(it->path(), dst);
        else fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
    }
    log("  ban sao tam: " + std::to_string(unpacked) + " file giai nen tu .rpa, " + std::to_string(sources) + " file .rpy");
    // Scripts shipped only as .rpyc (Between Humanity): `translate` writes dialogue only for files whose
    // .rpy exists, and scans strings only from .rpy text. Loaded scripts still have every translate block
    // and menu choice, so list them for it - in the copy only.
    write_file(tmp / "game" / "zz_unitytl_gen.rpy",
               "init 999 python:\n"
               "    import os\n"
               "    _unitytl_t = renpy.game.script.translator\n"
               // older Ren'Py builds (this game's SDK predates some of these config vars) raise a plain
               // Exception, not AttributeError, for an unknown config.* name - hasattr() would not catch
               // it, so each optional feature below is its own try/except instead, and a build lacking
               // one just skips that extra - the core `translate <lang>` dialogue generation still runs.
               "    try:\n"
               "        for _unitytl_f in list(_unitytl_t.file_translates.keys()):\n"
               "            if not os.path.exists(_unitytl_f) and _unitytl_f not in config.translate_files:\n"
               "                config.translate_files.append(_unitytl_f)\n"
               "    except Exception:\n"
               "        pass\n"
               "    def _unitytl_strings():\n"
               "        for f, lines in list(_unitytl_t.additional_strings.items()):\n"
               "            if not os.path.exists(f):\n"
               "                for line, s in lines:\n"
               "                    yield (f, line, s)\n"
               "    try:\n"
               "        config.translate_additional_strings_callbacks.append(_unitytl_strings)\n"
               "    except Exception:\n"
               "        pass\n"
               // literals in the loaded screens (textbutton \"Start\", text \"PLAYTIME\", use x(\"Interact\")):
               // shown through Text, which translates any string with an `old` entry
               // store `list` is Ren'Py's RevertableList: the AST's plain lists need the builtin one
               "    def _unitytl_screen_strings():\n"
               "        import ast, re\n"
               "        try:\n"
               "            import builtins as B\n"
               "        except ImportError:\n"
               "            import __builtin__ as B\n"
               "        sl = renpy.sl2.slast\n"
               "        gettext = '_' + '('\n"
               "        seen, out = set(), []\n"
               "        def lit(code):\n"
               "            if not isinstance(code, (str, type(u''))):\n"
               "                return None\n"
               "            c = code.strip()\n"
               "            if c.startswith(gettext) and c.endswith(')'):\n"
               "                c = c[2:-1].strip()\n"
               "            if not c or c[0] not in '\"\\'':\n"
               "                return None\n"
               "            try:\n"
               "                v = ast.literal_eval(c)\n"
               "            except Exception:\n"
               "                return None\n"
               "            return v if isinstance(v, (str, type(u''))) else None\n"
               "        def add(s):\n"
               "            if not s or len(s.strip()) < 2 or len(s) > 600:\n"
               "                return\n"
               "            words = re.sub(r'\\[[^\\]]*\\]|\\{[^}]*\\}', '', s)\n"
               "            if not re.search(r'[A-Za-z]', words):\n"
               "                return\n"
               "            if re.search(r'\\.(png|jpg|jpeg|webp|ogg|mp3|wav|opus|ttf|otf|webm|rpy)\\b', s.lower()):\n"
               "                return\n"
               "            if ('/' in s and ' ' not in s) or (re.match(r'^[a-z0-9_.]+$', s) and ('_' in s or '.' in s)):\n"
               "                return\n"
               "            out.append(s)\n"
               "        def walk(n, depth):\n"
               "            if n is None or depth > 80 or id(n) in seen:\n"
               "                return\n"
               "            seen.add(id(n))\n"
               "            if isinstance(n, sl.SLDisplayable):\n"
               "                for p in (n.positional or []):\n"
               "                    add(lit(p))\n"
               "            if isinstance(n, sl.SLUse) and getattr(n, 'args', None) is not None:\n"
               "                for a in (getattr(n.args, 'arguments', None) or []):\n"
               "                    add(lit(a[1]))\n"
               "            for attr in ('children', 'entries', 'block'):\n"
               "                v = getattr(n, attr, None)\n"
               "                if isinstance(v, (B.list, B.tuple)):\n"
               "                    for c in v:\n"
               "                        walk(c[1] if isinstance(c, B.tuple) else c, depth + 1)\n"
               "                elif isinstance(v, sl.SLNode):\n"
               "                    walk(v, depth + 1)\n"
               "        for scr in list(renpy.display.screen.screens.values()):\n"
               "            name = getattr(scr, 'name', None)\n"
               "            name = name[0] if isinstance(name, B.tuple) and name else (name or '')\n"
               "            if not str(name).startswith('_'):  # Ren'Py's own (_exception, _director...): developer only\n"
               "                walk(getattr(scr, 'ast', None), 0)\n"
               "        fn = os.path.join(config.gamedir, 'unitytl_screens.rpy')\n"
               "        for i, s in enumerate(out):\n"
               "            yield (fn, i + 1, s)\n"
               "    try:\n"
               "        config.translate_additional_strings_callbacks.append(_unitytl_screen_strings)\n"
               "    except Exception:\n"
               "        pass\n");

    std::wstring cmd = L"\"" + py.wstring() + L"\" \"" + script.wstring() + L"\" \"" + tmp.wstring() +
                       L"\" translate " + u8path(lang).wstring();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    log("  chay Ren'Py cua game: translate " + lang + " ...");
    bool ran = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                              root.wstring().c_str(), &si, &pi);
    DWORD code = 1;
    if (ran) {
        if (WaitForSingleObject(pi.hProcess, 15 * 60 * 1000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    fs::path made = tmp / "game" / "tl" / u8path(lang);
    bool ok = ran && fs::is_directory(made, ec) && !fs::is_empty(made, ec);
    if (ok) {
        fs::remove_all(out, ec);
        fs::create_directories(out.parent_path(), ec);
        fs::rename(made, out, ec);
        if (ec) {
            ec.clear();
            fs::copy(made, out, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        }
        std::vector<fs::path> junk;  // only the .rpy text is needed (tl files can sit in subfolders)
        for (auto& e : fs::recursive_directory_iterator(out, ec))
            if (e.is_regular_file() && e.path().extension() != ".rpy") junk.push_back(e.path());
        for (auto& j : junk) fs::remove(j, ec);
        fs::remove(out / "zz_unitytl_gen.rpy", ec);  // our helper is not part of the game
        extra_strings(tmp / "game", out, lang);
    } else {
        log("  ! Ren'Py khong sinh duoc file dich (ma thoat " + std::to_string(code) + ")");
        fs::path tb = tmp / "traceback.txt";
        if (fs::exists(tb, ec)) {
            std::string t = read_text(tb);
            log(t.substr(0, 1500));
        }
    }
    fs::remove_all(tmp, ec);
    return ok;
}

// ------------------------------------------------------------------ tl files
namespace {
struct Span {
    std::string key;
    size_t line = 0, a = 0, b = 0;  // content between the quotes: [a, b) of that line
};

// end of a quoted string starting at s[q] == '"' (index of the closing quote), npos if unterminated
size_t quote_end(const std::string& s, size_t q) {
    for (size_t k = q + 1; k < s.size(); k++) {
        if (s[k] == '\\') k++;
        else if (s[k] == '"') return k;
    }
    return std::string::npos;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t st = 0;
    while (true) {
        size_t e = text.find('\n', st);
        lines.push_back(text.substr(st, e == std::string::npos ? std::string::npos : e - st));
        if (e == std::string::npos) break;
        st = e + 1;
    }
    return lines;
}

std::vector<Span> spans(const std::vector<std::string>& lines) {
    std::vector<Span> out;
    std::string block, old;
    bool strings = false, haveOld = false;
    int n = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string l = lines[i];
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (i == 0 && l.rfind("\xEF\xBB\xBF", 0) == 0) l = l.substr(3);
        if (l.rfind("translate ", 0) == 0 && !l.empty() && l.back() == ':') {
            size_t sp = l.find(' ', 10);
            block = sp == std::string::npos ? "" : l.substr(sp + 1, l.size() - sp - 2);
            strings = block == "strings";
            haveOld = false;
            n = 0;
            continue;
        }
        size_t first = l.find_first_not_of(' ');
        if (first == std::string::npos || l[first] == '#' || first < 4 || block.empty()) continue;
        size_t q = l.find('"', first);
        if (q == std::string::npos) continue;
        size_t e = quote_end(l, q);
        if (e == std::string::npos) continue;
        std::string head = l.substr(first, q - first);
        if (strings) {
            if (head == "old ") {
                old = l.substr(q + 1, e - q - 1);
                haveOld = true;
            } else if (head == "new " && haveOld) {
                out.push_back({"s|" + old, i, q + 1, e});
                haveOld = false;
            }
            continue;
        }
        if (head.rfind("$", 0) == 0 || head.rfind("python", 0) == 0 || head.rfind("voice ", 0) == 0 ||
            head.rfind("nvl ", 0) == 0 || head.find('(') != std::string::npos)
            continue;
        // "Eileen" "Hello": the first string is the speaker's name
        size_t q2 = l.find_first_not_of(' ', e + 1);
        if (q2 != std::string::npos && l[q2] == '"') {
            size_t e2 = quote_end(l, q2);
            if (e2 != std::string::npos) q = q2, e = e2;
        }
        out.push_back({block + (n ? "#" + std::to_string(n) : ""), i, q + 1, e});
        n++;
    }
    return out;
}

std::string unescape_quotes(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '"') continue;
        o += s[i];
    }
    return o;
}

std::string escape_quotes(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '"' && (i == 0 || s[i - 1] != '\\')) o += '\\';
        if (s[i] == '\n') { o += "\\n"; continue; }  // a real line break would end the statement
        if (s[i] == '\r') continue;
        o += s[i];
    }
    return o;
}
}  // namespace

std::vector<RenpyLine> renpy_tl_read(const std::string& text) {
    auto lines = split_lines(text);
    std::vector<RenpyLine> out;
    for (auto& s : spans(lines)) out.push_back({s.key, unescape_quotes(lines[s.line].substr(s.a, s.b - s.a)), s.line});
    return out;
}

std::string renpy_tl_apply(const std::string& text, const std::map<std::string, std::string>& tr, int& n) {
    auto lines = split_lines(text);
    n = 0;
    for (auto& s : spans(lines)) {
        auto t = tr.find(s.key);
        if (t == tr.end() || t->second.empty()) continue;
        lines[s.line] = lines[s.line].substr(0, s.a) + escape_quotes(t->second) + lines[s.line].substr(s.b);
        n++;
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) out += (i ? "\n" : "") + lines[i];
    return out;
}
