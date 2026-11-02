// UnityTL GUI: Dear ImGui on Win32 + DirectX 11.
#include <d3d11.h>
#include <shobjidl.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "downloader.h"
#include "json.h"
#include "misc/cpp/imgui_stdlib.h"
#include "probe.h"
#include "project.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ------------------------------------------------------------------ D3D boilerplate
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;

static void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}
static void cleanup_rtv() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}
static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)  // no GPU driver: software renderer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (hr != S_OK) return false;
    create_rtv();
    return true;
}
static void cleanup_device() {
    cleanup_rtv();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_resizeW = LOWORD(lParam);
            g_resizeH = HIWORD(lParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ------------------------------------------------------------------ helpers
static std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string pick_folder(HWND owner) {
    std::string out;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS);
        dlg->SetTitle(L"Chọn thư mục game (Unity / GameMaker / Godot / Ren'Py)");
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    out = to_utf8(p);
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return out;
}

static fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

// ------------------------------------------------------------------ app state
struct App {
    Project proj;
    bool opened = false;
    std::string path;
    std::thread job;
    std::atomic<bool> busy{false}, stop{false};
    std::atomic<int> done{0}, total{0};
    std::string jobName;

    std::mutex logMu;
    std::vector<std::string> logLines;
    bool logScroll = false;

    // translation engine: 0 = Google (online), 1 = local model through llama-server
    int engine = 0;
    std::string serverExe, modelPath;
    int port = 8080;
    PROCESS_INFORMATION server{};
    std::atomic<bool> serverReady{false}, quitting{false};
    std::thread poller;

    EngineConfig engine_config() const {
        EngineConfig c;
        c.kind = engine == 1 ? EngineConfig::Local : EngineConfig::Google;
        c.port = port;
        return c;
    }

    // default: <...>/GAME/llm/llama.cpp/llama-server.exe and the first .gguf in <...>/GAME/llm/models
    void find_local_model() {
        fs::path base = exe_dir();
        std::error_code ec;
        for (int up = 0; up < 4 && (serverExe.empty() || modelPath.empty()); up++, base = base.parent_path()) {
            fs::path llm = base / "llm";
            if (serverExe.empty() && fs::exists(llm / "llama.cpp" / "llama-server.exe", ec))
                serverExe = pathstr(llm / "llama.cpp" / "llama-server.exe");
            if (modelPath.empty() && fs::exists(llm / "models", ec))
                for (auto& f : fs::directory_iterator(llm / "models", ec))
                    if (f.path().extension() == ".gguf") { modelPath = pathstr(f.path()); break; }
        }
    }

    // ---- "Tai model"
    DlPlan plan;
    std::atomic<bool> planReady{false};
    std::atomic<uint64_t> dlDone{0}, dlTotal{0};
    std::mutex pendingMu;
    std::string pendingServer, pendingModel;  // set by the download thread, applied on the UI thread

    bool local_model_missing() const {
        std::error_code ec;
        return serverExe.empty() || modelPath.empty() || !fs::exists(u8path(serverExe), ec) ||
               !fs::exists(u8path(modelPath), ec);
    }
    // existing <...>/llm folder next to the app (or one of its parents), else <app>/llm
    fs::path download_root() const {
        fs::path base = exe_dir();
        std::error_code ec;
        for (int up = 0; up < 4; up++, base = base.parent_path())
            if (fs::is_directory(base / "llm", ec)) return base / "llm";
        return exe_dir() / "llm";
    }
    void check_download() {
        fs::path root = download_root();
        run("Kiểm tra", [this, root] {
            log("Đang kiểm tra card đồ họa và thông tin model (Hugging Face, GitHub)...");
            plan = make_plan(root);
            planReady = true;
        });
    }
    void start_download() {
        run("Tải", [this] {
            fs::path srv, mdl;
            if (run_plan(plan, stop, dlDone, dlTotal, logger(), srv, mdl)) {
                std::lock_guard<std::mutex> lk(pendingMu);
                pendingServer = pathstr(srv);
                pendingModel = pathstr(mdl);
            }
        });
    }

    bool server_running() const {
        DWORD code = 0;
        return server.hProcess && GetExitCodeProcess(server.hProcess, &code) && code == STILL_ACTIVE;
    }
    void start_server() {
        if (server_running()) return;
        std::wstring cmd = L"\"" + u8path(serverExe).wstring() + L"\" -m \"" + u8path(modelPath).wstring() +
                           L"\" -ngl 99 -c 4096 --host 127.0.0.1 --port " + std::to_wstring(port);
        STARTUPINFOW si{sizeof si};
        std::wstring dir = u8path(serverExe).parent_path().wstring();
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si,
                            &server)) {
            log("Không bật được llama-server: kiểm tra đường dẫn llama-server.exe và file model .gguf.");
            server = {};
            return;
        }
        CloseHandle(server.hThread);
        server.hThread = nullptr;
        log("Đang bật model local (GPU)... đợi đèn báo chuyển sang 'đang chạy'.");
    }
    void stop_server() {
        if (server.hProcess) {
            TerminateProcess(server.hProcess, 0);
            CloseHandle(server.hProcess);
            server = {};
            log("Đã tắt model local.");
        }
    }

    int filter = 0;  // 0 all, 1 untranslated, 2 translated, 3 tag errors
    std::string search;
    int selected = -1;
    bool sourcesDirty = false;
    HWND hwnd = nullptr;

    void log(const std::string& s) {
        std::lock_guard<std::mutex> lk(logMu);
        logLines.push_back(s);
        if (logLines.size() > 3000) logLines.erase(logLines.begin(), logLines.begin() + 1000);
        logScroll = true;
    }
    LogFn logger() {
        return [this](const std::string& s) { log(s); };
    }

    void run(const std::string& name, std::function<void()> fn) {
        if (busy) return;
        if (job.joinable()) job.join();
        busy = true;
        stop = false;
        jobName = name;
        job = std::thread([this, fn] {
            try {
                fn();
            } catch (const std::exception& e) {
                log(std::string("LỖI: ") + e.what());
            }
            busy = false;
        });
    }

    void open_game(const std::string& p) {
        if (busy) return;
        std::string err;
        selected = -1;
        if (!proj.open(u8path(p), err)) {
            opened = false;
            log(err);
            return;
        }
        opened = true;
        path = p;
        save_settings();
        log("Mở game: " + proj.name() + " (" + pathstr(proj.dataDir) + ")");
        if (proj.sources.empty()) run("Quét", [this] { proj.scan(logger()); });
        else log("Đã nạp tiến độ cũ: " + std::to_string(proj.translated_count()) + "/" + std::to_string(proj.entries.size()) + " dòng đã dịch.");
    }

    void save_settings() {
        Json j = Json::object();
        j["lastGame"] = Json(path);
        j["engine"] = Json(engine);
        j["serverExe"] = Json(serverExe);
        j["model"] = Json(modelPath);
        j["port"] = Json(port);
        try { write_file(exe_dir() / "unitytl_settings.json", j.dump(1)); } catch (...) {}
    }
    void load_settings() {
        try {
            Json j = Json::parse(read_text(exe_dir() / "unitytl_settings.json"));
            path = j.get_str("lastGame");
            serverExe = j.get_str("serverExe");
            modelPath = j.get_str("model");
            if (auto e = j.find("engine")) engine = (int)e->int64();
            if (auto p = j.find("port")) port = (int)p->int64();
        } catch (...) {}
        find_local_model();
    }
};

static bool tag_error(const Entry& e) { return !e.vi.empty() && !translation_ok(e.en, e.vi); }

static void draw(App& a) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    bool busy = a.busy;

    // ---- game folder
    ImGui::TextUnformatted("Thư mục game:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 260);
    bool enter = ImGui::InputText("##path", &a.path, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Chọn...")) {
        std::string p = pick_folder(a.hwnd);
        if (!p.empty()) { a.path = p; enter = true; }
    }
    ImGui::SameLine();
    if (ImGui::Button("Mở game") || enter) a.open_game(a.path);
    ImGui::EndDisabled();

    if (!a.opened) {
        ImGui::Spacing();
        ImGui::TextWrapped("Chọn thư mục chứa game Unity (thư mục có file .exe và thư mục <Tên>_Data) "
                           "hoặc game GameMaker (có file data.win), game Godot (có file .pck) hoặc game Ren'Py "
                           "(có thư mục renpy và game). "
                           "App tự tìm chữ trong file game - hỗ trợ I2 Localization, Unity Localization, "
                           "file JSON đa ngôn ngữ, Dialogue System, file translations.ini / .csv của GameMaker, "
                           "file JSON tiếng Anh trong .pck của Godot và lời thoại Ren'Py (game/tl) - "
                           "rồi dịch và ghi thẳng vào game, không cần mod hay XUnity AutoTranslator.");
    }

    float bottomH = 190;
    float leftW = 380;
    ImGui::BeginChild("left", ImVec2(leftW, -bottomH), ImGuiChildFlags_Borders);
    if (a.opened) {
        int tr = a.proj.translated_count();
        ImGui::Text("%s", a.proj.name().c_str());
        ImGui::Text("Đã dịch %d / %zu dòng", tr, a.proj.entries.size());
        ImGui::Separator();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Quét lại game", ImVec2(-1, 0))) a.run("Quét", [&a] { a.proj.scan(a.logger()); a.sourcesDirty = false; });
        if (a.proj.sources.empty()) {
            if (ImGui::Button("Dò file tìm chữ (probe)", ImVec2(-1, 0)))
                a.run("Probe", [&a] { probe_dir(a.proj.root, 30, a.logger()); });
            ImGui::TextWrapped(
                "Không tìm thấy nguồn chữ nào. Dò toàn bộ file trong thư mục game, xem file nào có "
                "nhiều chữ tiếng Anh nhất (xem kết quả ở khung log dưới).");
        }
        ImGui::EndDisabled();

        // ---- translation engine
        const char* engines[] = {"Google (online)", "Model local (GPU)"};
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(busy);
        if (ImGui::Combo("##engine", &a.engine, engines, 2)) a.save_settings();
        ImGui::EndDisabled();
        if (a.engine == 1) {
            bool ready = a.serverReady;
            ImGui::TextColored(ready ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(1, 0.6f, 0.3f, 1),
                               ready ? "Model: đang chạy" : (a.server_running() ? "Model: đang nạp..." : "Model: chưa chạy"));
            ImGui::SameLine();
            bool missing = a.local_model_missing();
            if (a.server_running()) {
                if (ImGui::SmallButton("Tắt model")) a.stop_server();
            } else if (ready) {
                // llama-server đang chạy nhưng không phải do app này bật (ví dụ chạy tay từ dòng lệnh) -
                // app chỉ tắt tiến trình nó tự bật, nên không có nút "Tắt model" ở đây là có chủ ý.
                ImGui::TextDisabled("(chạy ngoài app)");
            } else if (!missing && ImGui::SmallButton("Bật model")) {
                a.save_settings();
                a.start_server();
            }
            if (missing && !ready) {
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Chưa có model trên máy.");
                ImGui::BeginDisabled(busy);
                if (ImGui::Button("Tải model...", ImVec2(-1, 0))) a.check_download();
                ImGui::EndDisabled();
            }
            if (ImGui::TreeNode("Đường dẫn model")) {
                ImGui::BeginDisabled(busy || a.server_running());
                if (ImGui::SmallButton("Tải lại / kiểm tra model...")) a.check_download();
                ImGui::EndDisabled();
                ImGui::TextUnformatted("llama-server.exe:");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##srv", &a.serverExe)) a.save_settings();
                ImGui::TextUnformatted("File model .gguf:");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##mdl", &a.modelPath)) a.save_settings();
                ImGui::SetNextItemWidth(120);
                if (ImGui::InputInt("Cổng", &a.port)) a.save_settings();
                ImGui::TreePop();
            }
        }
        ImGui::BeginDisabled(busy || (a.engine == 1 && !a.serverReady));
        std::string trLabel = std::string("Dịch các dòng chưa dịch (") + (a.engine == 1 ? "model local" : "Google") + ")";
        if (ImGui::Button(trLabel.c_str(), ImVec2(-1, 0)))
            a.run("Dịch", [&a] {
                a.proj.reload_glossary();
                a.proj.translate(false, a.stop, a.done, a.total, a.logger(), a.engine_config());
            });
        if (ImGui::Button("Dịch lại TẤT CẢ", ImVec2(-1, 0))) ImGui::OpenPopup("confirm_all");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(busy);
        if (a.proj.gm) {
            // GameMaker fonts are pre-rendered: missing Vietnamese letters get drawn with this Windows font
            ImGui::TextUnformatted("Font có dấu (font Windows):");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##gmfont", &a.proj.gmFont)) a.proj.save();
            ImGui::TextUnformatted("Font cho chữ pixel (vẽ không khử răng cưa):");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##gmpixel", &a.proj.gmPixelFont)) a.proj.save();
        }
        if (ImGui::Button("GHI VÀO GAME", ImVec2(-1, 34)))
            a.run("Ghi", [&a] { a.proj.save(); a.proj.inject(a.logger()); });
        if (a.proj.unreal) {
            // Unreal's own Settings menu doesn't offer "Vietnamese" as a language choice at all (its list
            // isn't read from Game.locmeta) - forcing the engine's active culture via the user's saved
            // Engine.ini is what actually makes the game render in Vietnamese. Outside the game's own
            // files, so unlike "GHI VÀO GAME" this never runs automatically.
            bool forced = a.proj.unreal_culture_forced();
            if (ImGui::Button(forced ? "Đã ép Culture=vi (bấm lại nếu cần)" : "Ép Culture=vi (bắt buộc để chơi tiếng Việt)",
                              ImVec2(-1, 0)))
                a.run("Culture=vi", [&a] { a.proj.unreal_force_culture(a.logger()); });
            ImGui::TextWrapped(
                "Ghi vào Engine.ini của người dùng (%%LOCALAPPDATA%%\\...\\Saved\\Config\\WindowsNoEditor), "
                "ngoài thư mục game. Cần mở game ít nhất 1 lần trước khi bấm (file Engine.ini chỉ được "
                "tạo sau lần chạy đầu).");
        }
        if (ImGui::Button("Khôi phục file gốc", ImVec2(-1, 0))) ImGui::OpenPopup("confirm_restore");
        if (ImGui::Button(a.proj.godot    ? "Xem log game (godot.log)"
                          : a.proj.renpy  ? "Xem log game (log.txt / traceback.txt)"
                          : a.proj.unreal ? "Xem log game (.log)"
                                          : "Xem Player.log",
                          ImVec2(-1, 0)))
            a.proj.check_log(a.logger());
        if (ImGui::Button("Sửa bảng thuật ngữ", ImVec2(-1, 0))) {
            a.proj.reload_glossary();
            ShellExecuteW(nullptr, L"open", L"notepad.exe", a.proj.glossary_file().wstring().c_str(), nullptr, SW_SHOW);
        }
        ImGui::EndDisabled();
        if (busy) {
            if (ImGui::Button("DỪNG", ImVec2(-1, 0))) a.stop = true;
        }

        if (ImGui::BeginPopupModal("confirm_all", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Dịch lại toàn bộ sẽ ghi đè cả những dòng bạn đã sửa tay. Tiếp tục?");
            if (ImGui::Button("Dịch lại hết")) {
                a.run("Dịch", [&a] {
                    a.proj.reload_glossary();
                    a.proj.translate(true, a.stop, a.done, a.total, a.logger(), a.engine_config());
                });
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Thôi")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal("confirm_restore", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Chép đè các file .bak (bản gốc) lên file game. Bản dịch vẫn được giữ trong _unitytl.json.");
            if (ImGui::Button("Khôi phục")) {
                a.run("Khôi phục", [&a] { a.proj.restore(a.logger()); });
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Thôi")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Nguồn chữ tìm thấy:");
        ImGui::BeginDisabled(busy);
        for (size_t i = 0; i < a.proj.sources.size(); i++) {
            TextSource& s = a.proj.sources[i];
            ImGui::PushID((int)i);
            if (ImGui::Checkbox("##on", &s.enabled)) a.sourcesDirty = true;
            ImGui::SameLine();
            ImGui::TextWrapped("%s", s.label().c_str());
            if ((s.kind == "i2" || s.kind == "json") && s.options.size() > 1) {
                ImGui::SetNextItemWidth(-1);
                std::string cur = s.chosen.empty() ? "" : s.chosen[0];
                if (ImGui::BeginCombo("##lang", ("Ngôn ngữ gốc: " + cur).c_str())) {
                    for (auto& o : s.options)
                        if (ImGui::Selectable(o.c_str(), o == cur)) {
                            s.chosen = {o};
                            a.sourcesDirty = true;
                        }
                    ImGui::EndCombo();
                }
            }
            ImGui::TextDisabled("   %d dòng", s.count);
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        if (a.sourcesDirty && !busy) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Đã đổi nguồn - bấm 'Quét lại game'.");
            a.proj.save();
        }
    }
    ImGui::EndChild();

    // ---- entries table
    ImGui::SameLine();
    ImGui::BeginChild("right", ImVec2(0, -bottomH), ImGuiChildFlags_Borders);
    if (a.opened) {
        const char* filters[] = {"Tất cả", "Chưa dịch", "Đã dịch", "Lỗi tag"};
        ImGui::SetNextItemWidth(140);
        ImGui::Combo("##filter", &a.filter, filters, 4);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##search", "Tìm trong bản gốc / bản dịch / key...", &a.search);

        std::vector<int> rows;
        {
            std::lock_guard<std::mutex> lk(a.proj.mu);
            for (int i = 0; i < (int)a.proj.entries.size(); i++) {
                const Entry& e = a.proj.entries[i];
                if (a.filter == 1 && !e.vi.empty()) continue;
                if (a.filter == 2 && e.vi.empty()) continue;
                if (a.filter == 3 && !tag_error(e)) continue;
                if (!a.search.empty() && e.en.find(a.search) == std::string::npos &&
                    e.vi.find(a.search) == std::string::npos && e.key.find(a.search) == std::string::npos)
                    continue;
                rows.push_back(i);
            }
        }
        if (!a.search.empty()) {
            ImGui::BeginDisabled(a.busy || rows.empty() || (a.engine == 1 && !a.serverReady));
            std::string label = "Dịch " + std::to_string(rows.size()) + " dòng đang lọc";
            if (ImGui::Button(label.c_str(), ImVec2(-1, 0)))
                a.run("Dịch", [&a, rows] {
                    a.proj.reload_glossary();
                    a.proj.translate_rows(rows, a.stop, a.done, a.total, a.logger(), a.engine_config());
                });
            ImGui::EndDisabled();
            ImGui::TextWrapped(
                "Chỉ dịch các dòng đang hiện ở bảng dưới (theo bộ lọc + ô tìm kiếm) và còn trống - hợp để "
                "dịch thử vài dòng menu dễ thấy (Start, Settings, Back...) thay vì dịch cả game.");
        }
        float editorH = a.selected >= 0 ? 170.0f : 0.0f;
        if (ImGui::BeginTable("entries", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_BordersInnerV,
                              ImVec2(0, -editorH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 200);
            ImGui::TableSetupColumn("Bản gốc");
            ImGui::TableSetupColumn("Bản dịch");
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin((int)rows.size());
            std::lock_guard<std::mutex> lk(a.proj.mu);
            while (clip.Step()) {
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                    int i = rows[r];
                    Entry& e = a.proj.entries[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(i);
                    if (ImGui::Selectable(e.key.c_str(), a.selected == i, ImGuiSelectableFlags_SpanAllColumns)) a.selected = i;
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    std::string en = e.en.substr(0, 200);
                    std::replace(en.begin(), en.end(), '\n', ' ');
                    ImGui::TextUnformatted(en.c_str());
                    ImGui::TableNextColumn();
                    std::string vi = e.vi.substr(0, 200);
                    std::replace(vi.begin(), vi.end(), '\n', ' ');
                    if (vi.empty()) ImGui::TextDisabled("(chưa dịch)");
                    else if (tag_error(e)) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", vi.c_str());
                    else ImGui::TextUnformatted(vi.c_str());
                }
            }
            ImGui::EndTable();
        }
        if (a.selected >= 0 && a.selected < (int)a.proj.entries.size()) {
            std::lock_guard<std::mutex> lk(a.proj.mu);
            Entry& e = a.proj.entries[a.selected];
            ImGui::Separator();
            ImGui::Text("Gốc (%s):", e.key.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30);
            if (ImGui::SmallButton("X")) a.selected = -1;
            ImGui::InputTextMultiline("##en", &e.en, ImVec2(-1, 55), ImGuiInputTextFlags_ReadOnly);
            ImGui::TextUnformatted("Bản dịch (sửa trực tiếp, tự lưu):");
            if (tag_error(e)) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "  thiếu/thừa tag, ngoặc hoặc có chữ lạ so với bản gốc!");
            }
            ImGui::InputTextMultiline("##vi", &e.vi, ImVec2(-1, 55));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                a.proj.mu.unlock();
                a.proj.save();
                a.proj.mu.lock();
            }
        }
    }
    ImGui::EndChild();

    // ---- download confirmation (plan made in the background, confirmed here)
    {
        std::lock_guard<std::mutex> lk(a.pendingMu);
        if (!a.pendingServer.empty()) {
            a.serverExe = a.pendingServer;
            a.modelPath = a.pendingModel;
            a.pendingServer.clear();
            a.save_settings();
            a.log("Đã điền đường dẫn model. Bấm 'Bật model' để dùng.");
        }
    }
    if (a.planReady && !a.busy) {
        a.planReady = false;
        if (!a.plan.error.empty()) a.log("Không tải được: " + a.plan.error);
        else ImGui::OpenPopup("download");
    }
    ImGui::SetNextWindowSize(ImVec2(760, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("download", nullptr, ImGuiWindowFlags_NoResize)) {
        const DlPlan& p = a.plan;
        ImGui::TextWrapped("%s", p.note.c_str());
        ImGui::Spacing();
        if (p.items.empty()) {
            ImGui::TextUnformatted("Đã có đủ model và llama.cpp, không cần tải gì.");
        } else {
            ImGui::TextUnformatted("Sẽ tải các file sau (chỉ tải khi bạn bấm 'Tải'):");
            if (ImGui::BeginTable("dl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("File");
                ImGui::TableSetupColumn("Nguồn", ImGuiTableColumnFlags_WidthFixed, 230);
                ImGui::TableSetupColumn("Dung lượng", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableHeadersRow();
                for (auto& it : p.items) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s\n%s", it.label.c_str(), it.file.c_str());
                    ImGui::TableNextColumn();
                    std::string host = it.url.find("huggingface") != std::string::npos ? "huggingface.co (Tencent)" : "github.com (ggml-org)";
                    ImGui::TextUnformatted(host.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f MB", it.size / 1048576.0);
                }
                ImGui::EndTable();
            }
            std::error_code ec;
            fs::path probe = p.root;
            while (!probe.empty() && !fs::exists(probe, ec)) probe = probe.parent_path();
            auto sp = fs::space(probe, ec);
            ImGui::Text("Tổng: %.2f GB. Ổ đĩa còn trống: %.1f GB.", p.total() / 1073741824.0,
                        ec ? 0.0 : sp.available / 1073741824.0);
            ImGui::TextWrapped("Lưu vào: %s", pathstr(p.root).c_str());
            ImGui::TextDisabled("Kiểm tra SHA-256 sau khi tải. Bị ngắt giữa chừng thì lần sau tải tiếp.");
        }
        ImGui::Spacing();
        if (!p.items.empty() && ImGui::Button("Tải", ImVec2(120, 0))) {
            a.start_download();
            ImGui::CloseCurrentPopup();
        }
        if (!p.items.empty()) ImGui::SameLine();
        if (ImGui::Button(p.items.empty() ? "Đóng" : "Thôi", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- progress + log
    if (a.busy && a.jobName == "Tải" && a.dlTotal > 0) {
        char buf[96];
        snprintf(buf, sizeof buf, "Đang tải %.0f / %.0f MB", a.dlDone / 1048576.0, a.dlTotal / 1048576.0);
        ImGui::ProgressBar((float)((double)a.dlDone / (double)a.dlTotal), ImVec2(-1, 0), buf);
    } else if (a.busy && a.jobName == "Dịch" && a.total > 0) {
        char buf[64];
        snprintf(buf, sizeof buf, "Đang dịch %d / %d", a.done.load(), a.total.load());
        ImGui::ProgressBar((float)a.done / (float)a.total, ImVec2(-1, 0), buf);
    } else if (a.busy) {
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), (a.jobName + "...").c_str());
    } else {
        ImGui::ProgressBar(0, ImVec2(-1, 0), "Sẵn sàng");
    }
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
        std::lock_guard<std::mutex> lk(a.logMu);
        for (auto& l : a.logLines) ImGui::TextUnformatted(l.c_str());
        if (a.logScroll) {
            ImGui::SetScrollHereY(1.0f);
            a.logScroll = false;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ImGui_ImplWin32_EnableDpiAwareness();
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, LoadIconW(hInst, L"IDI_APP"), nullptr, nullptr,
                      nullptr, L"UnityTL", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"UnityTL - Việt hóa game Unity / GameMaker / Godot / Ren'Py", WS_OVERLAPPEDWINDOW, 100, 60,
                              (int)(1300 * scale), (int)(820 * scale), nullptr, nullptr, hInst, nullptr);
    if (!create_device(hwnd)) {
        cleanup_device();
        MessageBoxW(nullptr, L"Không khởi tạo được DirectX 11.", L"UnityTL", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FrameRounding = 4;

    // Segoe UI has every Vietnamese glyph; fall back to Arial, then ImGui's built-in font
    const wchar_t* fonts[] = {L"C:\\Windows\\Fonts\\segoeui.ttf", L"C:\\Windows\\Fonts\\arial.ttf"};
    bool loaded = false;
    for (auto f : fonts) {
        if (GetFileAttributesW(f) == INVALID_FILE_ATTRIBUTES) continue;
        io.Fonts->AddFontFromFileTTF(to_utf8(f).c_str(), 18.0f * scale, nullptr, io.Fonts->GetGlyphRangesVietnamese());
        loaded = true;
        break;
    }
    if (!loaded) io.Fonts->AddFontDefault();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    App app;
    app.hwnd = hwnd;
    app.load_settings();
    app.log("UnityTL sẵn sàng. Chọn thư mục game rồi bấm 'Mở game'.");
    if (!app.path.empty()) app.open_game(app.path);
    app.poller = std::thread([&app] {  // llama-server health, off the UI thread
        while (!app.quitting) {
            app.serverReady = local_server_ready(app.port);
            for (int i = 0; i < 20 && !app.quitting; i++) Sleep(100);
        }
    });

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (g_resizeW && g_resizeH) {
            cleanup_rtv();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            create_rtv();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw(app);
        ImGui::Render();
        const float clear[4] = {0.1f, 0.1f, 0.12f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    app.stop = true;
    app.quitting = true;
    if (app.job.joinable()) app.job.join();
    if (app.poller.joinable()) app.poller.join();
    app.stop_server();  // only a server this app started
    if (app.opened) app.proj.save();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
