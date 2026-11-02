#include "downloader.h"

#include <windows.h>
#include <bcrypt.h>
#include <dxgi.h>
#include <winhttp.h>

#include <cstdio>

#include "json.h"
#include "translate.h"

namespace {

// Pinned, tested versions: newer builds are not picked up silently.
const char* kModelRepo = "tencent/Hy-MT2-1.8B-GGUF";
const char* kLlamaTag = "b11960";

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

struct Url {
    std::wstring host, path;
    int port = 443;
    bool https = true;
};
bool crack(const std::string& url, Url& out) {
    std::wstring w = widen(url);
    URL_COMPONENTS uc{sizeof uc};
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    wchar_t extra[2048];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return false;
    out.host = host;
    out.path = std::wstring(path) + extra;
    out.port = uc.nPort;
    out.https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

HttpResult get_url(const std::string& url) {
    Url u;
    if (!crack(url, u)) return {};
    return http_request(L"GET", u.host.c_str(), u.port, u.https, u.path, "", "", 30000);
}

std::string human(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30)) snprintf(buf, sizeof buf, "%.2f GB", b / 1073741824.0);
    else snprintf(buf, sizeof buf, "%.0f MB", b / 1048576.0);
    return buf;
}

// Streamed GET into `part`, resuming from its current size with a Range request.
// Returns 1 = complete, 0 = network error (retry), -1 = stopped by user.
int download_once(const std::string& url, const fs::path& part, uint64_t expected, std::atomic<bool>& stop,
                  std::atomic<uint64_t>& done, uint64_t doneBase) {
    std::error_code ec;
    uint64_t offset = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    if (expected && offset == expected) return 1;
    if (expected && offset > expected) {
        fs::remove(part, ec);
        offset = 0;
    }
    Url u;
    if (!crack(url, u)) return 0;
    HINTERNET ses = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) unitytl", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 30000, 30000, 30000, 60000);
    int result = 0;
    HINTERNET con = WinHttpConnect(ses, u.host.c_str(), (INTERNET_PORT)u.port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req) {
        std::wstring hdr = offset ? L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n" : L"";
        if (WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), (DWORD)-1,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, sz = sizeof status;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
            if (status == 416 && offset == expected) result = 1;
            else if (status == 200 || status == 206) {
                if (status == 200) offset = 0;  // server ignored Range: start over
                FILE* f = _wfopen(part.wstring().c_str(), offset ? L"ab" : L"wb");
                if (f) {
                    std::vector<char> buf(1 << 20);
                    done = doneBase + offset;
                    result = 1;
                    for (;;) {
                        if (stop) { result = -1; break; }
                        DWORD got = 0;
                        if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got)) { result = 0; break; }
                        if (!got) break;
                        if (fwrite(buf.data(), 1, got, f) != got) { result = 0; break; }
                        offset += got;
                        done = doneBase + offset;
                    }
                    fclose(f);
                    if (result == 1 && expected && offset != expected) result = 0;
                }
            }
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return result;
}

std::string sha256_file(const fs::path& p) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        FILE* f = _wfopen(p.wstring().c_str(), L"rb");
        if (f) {
            std::vector<unsigned char> buf(4 << 20);
            size_t n;
            while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) BCryptHashData(h, buf.data(), (ULONG)n, 0);
            fclose(f);
            unsigned char dig[32];
            if (BCryptFinishHash(h, dig, 32, 0) == 0) {
                char b[3];
                for (unsigned char c : dig) { snprintf(b, 3, "%02x", c); hex += b; }
            }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

bool extract_zip(const fs::path& zip, const fs::path& dir, const DlLog& log) {
    // tar.exe (bsdtar) ships with Windows 10 1803+ and unpacks .zip
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip.wstring() + L"\" -C \"" + dir.wstring() + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        log("  ! khong chay duoc tar.exe de giai nen");
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code) log("  ! giai nen loi (tar.exe tra ve " + std::to_string(code) + ")");
    return code == 0;
}

}  // namespace

GpuInfo detect_gpu() {
    GpuInfo g;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory1, (void**)&f))) return g;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        uint64_t vram = d.DedicatedVideoMemory >> 20;
        bool nv = d.VendorId == 0x10DE;
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (nv || (!g.nvidia && vram > g.vramMB))) {
            if (nv || !g.nvidia) {
                g.nvidia = nv;
                g.name = narrow(d.Description);
                g.vramMB = vram;
                LARGE_INTEGER ver;
                // UMD version 32.0.15.8195 -> NVIDIA driver 581.95
                if (nv && SUCCEEDED(a->CheckInterfaceSupport(IID_IDXGIDevice, &ver)))
                    g.driver = ((HIWORD(ver.LowPart) % 10) * 10000 + LOWORD(ver.LowPart)) / 100.0;
            }
        }
        a->Release();
    }
    f->Release();
    return g;
}

DlPlan make_plan(const fs::path& root) {
    DlPlan plan;
    plan.root = root;
    plan.gpu = detect_gpu();
    const GpuInfo& g = plan.gpu;
    std::error_code ec;

    // ---- model: Q8_0 fits 4 GB cards (~3.3 GB in use); smaller cards get Q4_K_M
    std::string quant = g.vramMB >= 3000 ? "Q8_0" : "Q4_K_M";
    std::string modelFile = "Hy-MT2-1.8B-" + quant + ".gguf";
    // ---- llama.cpp build
    std::vector<std::string> assets;
    std::string backend;
    if (g.nvidia && g.driver >= 580) {
        backend = "CUDA 13.4";
        assets = {std::string("llama-") + kLlamaTag + "-bin-win-cuda-13.4-x64.zip", "cudart-llama-bin-win-cuda-13.4-x64.zip"};
    } else if (g.nvidia && g.driver >= 528) {
        backend = "CUDA 12.4";
        assets = {std::string("llama-") + kLlamaTag + "-bin-win-cuda-12.4-x64.zip", "cudart-llama-bin-win-cuda-12.4-x64.zip"};
    } else {
        backend = "Vulkan";
        assets = {std::string("llama-") + kLlamaTag + "-bin-win-vulkan-x64.zip"};
    }
    plan.note = "Card: " + (g.name.empty() ? std::string("khong ro") : g.name) + ", " + std::to_string(g.vramMB) +
                " MB VRAM" + (g.nvidia ? ", driver " + std::to_string(g.driver).substr(0, 6) : "") + " -> model " + quant +
                ", llama.cpp " + backend + ".";

    bool haveServer = fs::exists(root / "llama.cpp" / "llama-server.exe", ec);

    // ---- model metadata from Hugging Face (size + LFS sha256)
    HttpResult hf = get_url(std::string("https://huggingface.co/api/models/") + kModelRepo + "/tree/main?expand=true");
    if (hf.status != 200) {
        plan.error = "Khong lay duoc thong tin model tu Hugging Face (HTTP " + std::to_string(hf.status) + "). Kiem tra mang.";
        return plan;
    }
    try {
        bool found = false;
        for (auto& f : Json::parse(hf.body).items()) {
            if (f.get_str("path") != modelFile) continue;
            DlItem it;
            it.label = "Model dich " + modelFile;
            it.file = modelFile;
            it.url = std::string("https://huggingface.co/") + kModelRepo + "/resolve/main/" + modelFile;
            it.size = (uint64_t)f.at("size").int64();
            if (auto lfs = f.find("lfs")) it.sha256 = lfs->get_str("oid");
            plan.items.push_back(it);
            found = true;
        }
        if (!found) plan.error = "Khong thay " + modelFile + " tren Hugging Face.";
    } catch (const std::exception& e) {
        plan.error = std::string("Doc thong tin Hugging Face loi: ") + e.what();
    }
    if (!plan.error.empty() || haveServer) return plan;

    // ---- llama.cpp release assets from GitHub (size + sha256 digest)
    HttpResult gh = get_url(std::string("https://api.github.com/repos/ggml-org/llama.cpp/releases/tags/") + kLlamaTag);
    if (gh.status != 200) {
        plan.error = "Khong lay duoc thong tin llama.cpp tu GitHub (HTTP " + std::to_string(gh.status) + ").";
        return plan;
    }
    try {
        Json rel = Json::parse(gh.body);
        for (auto& name : assets) {
            bool found = false;
            for (auto& a : rel.at("assets").items()) {
                if (a.get_str("name") != name) continue;
                DlItem it;
                it.label = "llama.cpp " + backend + (name.rfind("cudart", 0) == 0 ? " (thu vien CUDA)" : "");
                it.file = name;
                it.url = a.get_str("browser_download_url");
                it.size = (uint64_t)a.at("size").int64();
                std::string dg = a.get_str("digest");
                if (dg.rfind("sha256:", 0) == 0) it.sha256 = dg.substr(7);
                it.extract = true;
                plan.items.push_back(it);
                found = true;
            }
            if (!found) plan.error = "Ban llama.cpp " + std::string(kLlamaTag) + " khong co file " + name + ".";
        }
    } catch (const std::exception& e) {
        plan.error = std::string("Doc thong tin GitHub loi: ") + e.what();
    }
    return plan;
}

bool run_plan(const DlPlan& plan, std::atomic<bool>& stop, std::atomic<uint64_t>& done, std::atomic<uint64_t>& total,
              const DlLog& log, fs::path& serverExe, fs::path& model) {
    std::error_code ec;
    fs::create_directories(plan.root / "models", ec);
    fs::create_directories(plan.root / "dl", ec);
    fs::create_directories(plan.root / "llama.cpp", ec);
    total = plan.total();
    uint64_t base = 0;
    for (auto& it : plan.items) {
        fs::path target = (it.extract ? plan.root / "dl" : plan.root / "models") / u8path(it.file);
        fs::path part = target;
        part += ".part";
        bool ok = fs::exists(target, ec) && fs::file_size(target, ec) == it.size;
        if (!ok) {
            log("Dang tai " + it.file + " (" + human(it.size) + ")...");
            int r = 0;
            for (int attempt = 0; attempt < 6 && r == 0; attempt++) {
                if (attempt) {
                    log("  mat ket noi, thu tai tiep (lan " + std::to_string(attempt + 1) + ")...");
                    Sleep(2000 * attempt);
                }
                r = download_once(it.url, part, it.size, stop, done, base);
            }
            if (r < 0) {
                log("Da dung tai. Bam 'Tai model' lan nua de tai tiep tu cho dang do.");
                return false;
            }
            if (r == 0) {
                log("  ! tai " + it.file + " that bai (mang). Thu lai sau, phan da tai van duoc giu.");
                return false;
            }
            fs::remove(target, ec);
            fs::rename(part, target, ec);
        }
        if (!it.sha256.empty()) {
            log("  kiem tra SHA-256 " + it.file + "...");
            std::string h = sha256_file(target);
            if (h != it.sha256) {
                fs::remove(target, ec);
                log("  ! SHA-256 KHONG KHOP (" + h.substr(0, 12) + "... != " + it.sha256.substr(0, 12) +
                    "...). Da xoa file hong, hay tai lai.");
                return false;
            }
        } else {
            log("  (nguon khong cong bo SHA-256 cho " + it.file + ", chi kiem tra dung luong)");
        }
        if (it.extract) {
            log("  giai nen " + it.file + "...");
            if (!extract_zip(target, plan.root / "llama.cpp", log)) return false;
            fs::remove(target, ec);  // the zip is no longer needed once unpacked
        } else {
            model = target;
        }
        base += it.size;
        done = base;
    }
    serverExe = plan.root / "llama.cpp" / "llama-server.exe";
    if (!fs::exists(serverExe, ec)) {
        log("  ! khong thay llama-server.exe sau khi giai nen.");
        return false;
    }
    if (model.empty()) {
        for (auto& f : fs::directory_iterator(plan.root / "models", ec))
            if (f.path().extension() == ".gguf") { model = f.path(); break; }
    }
    log("Tai model xong: " + pathstr(model));
    return !model.empty();
}
