#include "unreal.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <map>

#include "csv.h"
#include "formats.h"  // is_english
#include "ttf.h"

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool ends_with(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring quote(const std::wstring& a) {
    if (a.find(L' ') == std::wstring::npos && !a.empty()) return a;
    return L"\"" + a + L"\"";
}

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Runs `exe args...` (cwd optional), captures combined stdout+stderr as UTF-8. Returns the exit code, or
// -1 if the process couldn't even start. Same CreateProcessW/CREATE_NO_WINDOW idea as renpy.cpp's
// template-generation call, plus a pipe to actually read the tool's own output (repak/UnrealLocres are
// external tools whose errors matter - unlike the game's own Ren'Py, whose stdout was never needed).
int run_capture(const fs::path& exe, const std::vector<std::wstring>& args, std::string& out) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = quote(exe.wstring());
    for (auto& a : args) cmd += L" " + quote(a);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    bool ran = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ran) {
        CloseHandle(rd);
        return -1;
    }
    std::string raw;
    char chunk[8192];
    DWORD n;
    while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n > 0) raw.append(chunk, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    out = raw;
    return (int)code;
}

fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

fs::path local_appdata() {
    const wchar_t* p = _wgetenv(L"LOCALAPPDATA");
    return p ? fs::path(p) : fs::path();
}

bool read_fstr(const Bytes& b, size_t& off, std::string& out) {
    if (off + 4 > b.size()) return false;
    int32_t n;
    std::memcpy(&n, b.data() + off, 4);
    if (n < 0 || off + 4 + (size_t)n > b.size()) return false;
    out.assign((const char*)b.data() + off + 4, (size_t)n);
    off += 4 + (size_t)n;
    return true;
}

}  // namespace

UnrealTools find_unreal_tools() {
    UnrealTools t;
    std::error_code ec;
    fs::path base = exe_dir();
    for (int up = 0; up < 4; up++, base = base.parent_path()) {
        fs::path tools = base / "tools";
        if (t.repak.empty() && fs::exists(tools / "repak.exe", ec)) t.repak = tools / "repak.exe";
        if (t.unrealLocres.empty() && fs::exists(tools / "UnrealLocres.exe", ec)) t.unrealLocres = tools / "UnrealLocres.exe";
        if (!t.repak.empty() && !t.unrealLocres.empty()) break;
    }
    return t;
}

bool locate_unreal(const fs::path& any, fs::path& root, fs::path& pak) {
    std::error_code ec;
    fs::path start = fs::is_directory(any, ec) ? any : any.parent_path();
    std::vector<std::pair<fs::path, int>> stack{{start, 0}};
    fs::path bestPak;
    uintmax_t bestSize = 0;
    while (!stack.empty()) {
        auto [dir, depth] = stack.back();
        stack.pop_back();
        fs::path paksDir = dir / "Content" / "Paks";
        if (fs::is_directory(paksDir, ec))
            for (auto it = fs::directory_iterator(paksDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec) || lower(pathstr(it->path().extension())) != ".pak") continue;
                std::string name = pathstr(it->path().filename());
                if (lower(name).rfind("zzz_", 0) == 0) continue;  // our own supplementary pak
                uintmax_t sz = fs::file_size(it->path(), ec);
                if (!ec && sz > bestSize) {
                    bestSize = sz;
                    bestPak = it->path();
                }
            }
        if (depth < 3)
            for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                if (it->is_directory(ec)) stack.push_back({it->path(), depth + 1});
    }
    if (bestPak.empty()) return false;
    root = start;
    pak = bestPak;
    return true;
}

std::vector<std::string> unreal_pak_list(const UnrealTools& tools, const fs::path& pak, const LogCb& log) {
    std::string out;
    int code = run_capture(tools.repak, {L"list", pak.wstring()}, out);
    if (code != 0) {
        log("  ! repak list loi (" + std::to_string(code) + "): " + trim(out));
        return {};
    }
    std::vector<std::string> lines;
    size_t i = 0, n = out.size();
    while (i < n) {
        size_t e = out.find('\n', i);
        std::string line = out.substr(i, (e == std::string::npos ? n : e) - i);
        i = (e == std::string::npos) ? n : e + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<UnrealTarget> unreal_find_targets(const std::vector<std::string>& list) {
    std::map<std::string, std::map<std::string, std::string>> byTarget;  // target -> culture -> full path
    for (auto& line : list) {
        if (!ends_with(line, ".locres")) continue;
        std::vector<std::string> parts;
        size_t p = 0;
        while (p <= line.size()) {
            size_t sl = line.find('/', p);
            parts.push_back(line.substr(p, (sl == std::string::npos ? line.size() : sl) - p));
            if (sl == std::string::npos) break;
            p = sl + 1;
        }
        if (parts.size() < 3) continue;
        std::string filename = parts.back();
        std::string culture = parts[parts.size() - 2];
        std::string target = parts[parts.size() - 3];
        if (filename != target + ".locres") continue;  // not the standard <Target>/<culture>/<Target>.locres shape
        byTarget[target][culture] = line;
    }
    std::vector<UnrealTarget> out;
    for (auto& [target, byCulture] : byTarget) {
        auto it = byCulture.begin();
        for (auto cit = byCulture.begin(); cit != byCulture.end(); ++cit)
            if (is_english(cit->first)) { it = cit; break; }
        if (it == byCulture.end()) continue;
        out.push_back({target, it->first, it->second});
    }
    return out;
}

bool unreal_unpack_file(const UnrealTools& tools, const fs::path& pak, const std::string& relPath,
                        const fs::path& extractDir, const LogCb& log) {
    std::error_code ec;
    fs::path local = extractDir / u8path(relPath);
    if (fs::exists(local, ec)) return true;
    fs::create_directories(extractDir, ec);
    std::string out;
    int code =
        run_capture(tools.repak, {L"unpack", L"-f", L"-o", extractDir.wstring(), L"--include", to_wide(relPath), pak.wstring()}, out);
    if (code != 0) {
        log("  ! repak unpack loi (" + relPath + "): " + trim(out));
        return false;
    }
    return true;
}

std::vector<UnrealRow> unreal_export_csv(const UnrealTools& tools, const fs::path& pak, const std::string& locresPak,
                                         const fs::path& extractDir, const LogCb& log) {
    if (!unreal_unpack_file(tools, pak, locresPak, extractDir, log)) return {};
    fs::path local = extractDir / u8path(locresPak);
    fs::path csvOut = extractDir / u8path(locresPak + ".csv");
    std::string out;
    int code = run_capture(tools.unrealLocres, {L"export", local.wstring(), L"-f", L"csv", L"-o", csvOut.wstring()}, out);
    if (code != 0) {
        log("  ! UnrealLocres export loi: " + trim(out));
        return {};
    }
    std::string text = read_text(csvOut);
    auto rows = csv_parse(text.substr(csv_bom(text)), ',');
    std::vector<UnrealRow> result;
    for (size_t r = 1; r < rows.size(); r++) {  // skip header
        if (rows[r].size() < 2) continue;
        UnrealRow row;
        row.key = rows[r][0].value;
        row.source = rows[r][1].value;
        row.target = rows[r].size() > 2 ? rows[r][2].value : "";
        result.push_back(std::move(row));
    }
    return result;
}

bool unreal_import_csv(const UnrealTools& tools, const fs::path& srcLocres, const std::vector<UnrealRow>& rows,
                       const fs::path& outLocres, const LogCb& log) {
    std::error_code ec;
    fs::create_directories(outLocres.parent_path(), ec);
    fs::path csvPath = outLocres;
    csvPath.replace_extension(".csv");
    std::string text = "key,source,target\r\n";
    for (auto& r : rows)
        text += csv_encode(r.key, ',') + "," + csv_encode(r.source, ',') + "," + csv_encode(r.target, ',') + "\r\n";
    write_file(csvPath, text);
    std::string out;
    int code = run_capture(tools.unrealLocres,
                           {L"import", srcLocres.wstring(), csvPath.wstring(), L"-f", L"csv", L"-o", outLocres.wstring()},
                           out);
    if (code != 0) {
        log("  ! UnrealLocres import loi: " + trim(out));
        return false;
    }
    return true;
}

Bytes unreal_patch_locmeta(const Bytes& src) {
    if (src.size() < 17) return src;
    size_t off = 16 + 1;  // 16-byte magic GUID + 1 version byte
    std::string nativeCulture, nativeLocRes;
    if (!read_fstr(src, off, nativeCulture)) return src;
    if (!read_fstr(src, off, nativeLocRes)) return src;
    if (off + 4 > src.size()) return src;
    int32_t count;
    std::memcpy(&count, src.data() + off, 4);
    size_t countOff = off;
    size_t scan = off + 4;
    for (int32_t i = 0; i < count; i++) {
        std::string c;
        if (!read_fstr(src, scan, c)) return src;  // malformed for our assumptions - don't touch
        if (c.size() == 3 && c[0] == 'v' && c[1] == 'i' && c[2] == '\0') return src;  // already has it
    }
    if (scan != src.size()) return src;  // trailing bytes we don't understand - don't risk it
    Bytes out(src.begin(), src.begin() + countOff);
    int32_t newCount = count + 1;
    out.insert(out.end(), (const uint8_t*)&newCount, (const uint8_t*)&newCount + 4);
    out.insert(out.end(), src.begin() + countOff + 4, src.end());
    int32_t three = 3;
    out.insert(out.end(), (const uint8_t*)&three, (const uint8_t*)&three + 4);
    const char vi[] = "vi";
    out.insert(out.end(), vi, vi + 3);  // "vi\0", 3 bytes incl. the terminator (matches every other entry)
    return out;
}

void unreal_patch_fonts(const UnrealTools& tools, const fs::path& pak, const std::vector<std::string>& list,
                        const fs::path& extractDir, const fs::path& outDir, const LogCb& log) {
    std::vector<std::string> fonts;
    for (auto& p : list)
        if (ends_with(p, ".ufont")) fonts.push_back(p);
    if (fonts.empty()) return;
    log("  " + std::to_string(fonts.size()) + " file .ufont trong pak - kiem tra chu tieng Viet...");
    std::error_code ec;
    for (auto& fp : fonts) {
        if (!unreal_unpack_file(tools, pak, fp, extractDir, log)) continue;
        fs::path local = extractDir / u8path(fp);
        try {
            Bytes ttf = read_file(local);
            int missing = ttf_missing_vietnamese(ttf);
            if (missing < 0) {
                log("    - " + fp + ": khong phai font Latin/TrueType thuong (CJK/CFF/symbol) - bo qua");
                continue;
            }
            if (missing == 0) {
                log("    - " + fp + ": da du chu");
                continue;
            }
            bool ok = ttf_add_vietnamese(ttf, [&](const std::string& s) { log("    " + fp + ": " + s); });
            if (!ok) {
                log("    ! khong va duoc " + fp);
                continue;
            }
            fs::path out = outDir / u8path(fp);
            fs::create_directories(out.parent_path(), ec);
            write_file(out, ttf);
        } catch (const std::exception& ex) {
            log("    ! loi doc " + fp + ": " + ex.what());
        }
    }
}

bool unreal_pack(const UnrealTools& tools, const fs::path& inputDir, const fs::path& outPak, const LogCb& log) {
    std::error_code ec;
    fs::create_directories(outPak.parent_path(), ec);
    std::string out;
    int code = run_capture(tools.repak, {L"pack", L"--version", L"V11", inputDir.wstring(), outPak.wstring()}, out);
    log("  " + trim(out));
    return code == 0;
}

fs::path unreal_engine_ini(const std::string& projectName) {
    fs::path base = local_appdata();
    if (base.empty() || projectName.empty()) return {};
    return base / u8path(projectName) / "Saved" / "Config" / "WindowsNoEditor" / "Engine.ini";
}

fs::path unreal_log_path(const std::string& projectName) {
    fs::path base = local_appdata();
    if (base.empty() || projectName.empty()) return {};
    return base / u8path(projectName) / "Saved" / "Logs" / u8path(projectName + ".log");
}

bool unreal_culture_forced(const std::string& projectName) {
    std::error_code ec;
    fs::path p = unreal_engine_ini(projectName);
    if (p.empty() || !fs::exists(p, ec)) return false;
    return read_text(p).find("Culture=vi") != std::string::npos;
}

bool unreal_force_culture(const std::string& projectName, const LogCb& log) {
    fs::path p = unreal_engine_ini(projectName);
    std::error_code ec;
    if (p.empty() || !fs::exists(p, ec)) {
        log("  Chua co " + pathstr(p) + " (game chua chay lan nao?) - mo game 1 lan roi thu lai.");
        return false;
    }
    std::string text = read_text(p);
    if (text.find("Culture=vi") != std::string::npos) {
        log("  Da co Culture=vi trong " + pathstr(p));
        return true;
    }
    size_t pos = text.find("[Internationalization]");
    if (pos != std::string::npos) {
        size_t nl = text.find('\n', pos);
        size_t insertAt = (nl == std::string::npos) ? text.size() : nl + 1;
        text.insert(insertAt, "Culture=vi\r\n");
    } else {
        text = "[Internationalization]\r\nCulture=vi\r\n\r\n" + text;
    }
    write_file(p, text);
    log("  Da them Culture=vi vao " + pathstr(p));
    return true;
}
