// Win32 + Direct3D 11. The standard ImGui host loop, with three departures
// worth naming.
//
// It is a WinMain, not a main: a console window flashing behind a graphing
// tool is the same reason the Python build passed --noconsole to PyInstaller.
//
// It does NOT spin. The default ImGui example redraws at the display rate
// forever, which for a tool that is idle most of the time is a laptop fan
// running to redraw an unchanged table. WaitMessage blocks until there is
// input, so an idle Echelle uses no CPU at all -- the immediate-mode cost
// model only bites if you actually issue frames nobody asked for.
#include <d3d11.h>
#include <tchar.h>
#include <windows.h>

#include <string>
#include <vector>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "implot.h"
#include "ui.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM,
                                                             LPARAM);

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_occluded = false;
UINT g_resize_w = 0, g_resize_h = 0;
ech::App* g_app = nullptr;
HWND g_hwnd = nullptr;

void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void release_rtv() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool create_device(HWND hwnd) {
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

    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2,
        D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_ctx);
    // A machine with no GPU, or an RDP session, has no hardware driver. WARP
    // is the software rasteriser and is plenty for a UI -- refusing to start
    // there would make the tool unusable over remote desktop.
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2,
            D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_ctx);
    if (FAILED(hr)) return false;
    create_rtv();
    return true;
}

void destroy_device() {
    release_rtv();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) return 0;
            g_resize_w = LOWORD(lp);
            g_resize_h = HIWORD(lp);
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;   // no alt menu
            break;
        case WM_DROPFILES: {
            // Dropping a CSV on the window opens it. The Python needed a file
            // dialog for this because Tk has no drop target.
            auto drop = reinterpret_cast<HDROP>(wp);
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t wide[MAX_PATH];
                if (!DragQueryFileW(drop, i, wide, MAX_PATH)) continue;
                const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                                                    nullptr, 0, nullptr, nullptr);
                std::string path(static_cast<std::size_t>(len > 0 ? len - 1 : 0), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, path.data(), len,
                                    nullptr, nullptr);
                if (g_app) g_app->open(path);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

namespace ech {

// The file dialog, declared in ui.hpp. Windows ships one; a drawn-in-ImGui
// file browser would be several hundred lines that behave subtly unlike every
// other Open dialog on the machine -- no network places, no pinned folders, no
// typing a path.
std::string ask_path(Ask what) {
    char buf[MAX_PATH] = {0};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof buf;
    const bool save = what == Ask::SaveSession || what == Ask::SaveFigure;
    switch (what) {
        case Ask::OpenTable:
            ofn.lpstrFilter = "Data files\0*.csv;*.tsv;*.txt\0All files\0*.*\0";
            ofn.lpstrTitle = "Open a table";
            break;
        case Ask::OpenSession:
        case Ask::SaveSession:
            ofn.lpstrFilter = "Echelle session\0*.ech\0All files\0*.*\0";
            ofn.lpstrDefExt = "ech";
            ofn.lpstrTitle = save ? "Save the session" : "Open a session";
            break;
        case Ask::SaveFigure:
            ofn.lpstrFilter = "gnuplot script\0*.gp\0All files\0*.*\0";
            ofn.lpstrDefExt = "gp";
            ofn.lpstrTitle = "Export the figure";
            break;
    }
    // NOCHANGEDIR: without it the dialog moves the PROCESS working directory,
    // so the next relative path written lands somewhere the user never chose.
    ofn.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT
                                        : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    if (!(save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn))) return {};

    std::string p(buf);
    // The figure export is given a STEM -- it appends .gp, .dat and .csv
    // itself -- so the extension the dialog helpfully added comes back off.
    if (what == Ask::SaveFigure && p.size() > 3 &&
        p.compare(p.size() - 3, 3, ".gp") == 0)
        p.resize(p.size() - 3);
    return p;
}

}  // namespace ech

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int) {
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wndproc, 0, 0, inst,
                   LoadIconW(inst, L"IDI_ICON1"), nullptr, nullptr, nullptr,
                   L"Echelle", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Echelle",
                              WS_OVERLAPPEDWINDOW, 100, 100, 1600, 900, nullptr,
                              nullptr, wc.hInstance, nullptr);
    g_hwnd = hwnd;
    if (!create_device(hwnd)) {
        destroy_device();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.",
                    L"Echelle", MB_ICONERROR);
        return 1;
    }
    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // The window remembers its own layout between runs; nothing else here is
    // worth persisting, and .ini beside the exe would be written into
    // Program Files on an installed copy.
    io.IniFilename = nullptr;
    ImGui::StyleColorsLight();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.FrameRounding = 2.0f;
    st.ScrollbarRounding = 2.0f;
    st.WindowPadding = ImVec2(8, 8);
    st.ItemSpacing = ImVec2(8, 6);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_ctx);

    ech::App app;
    g_app = &app;
    // Anything on the command line is a file to open, so `Echelle data.csv`
    // and dragging a CSV onto the exe both work.
    //
    // THE EMPTY CHECK IS NOT DEFENSIVE PADDING. wWinMain's cmdline excludes the
    // program name, so a plain double-click hands us "" -- and
    // CommandLineToArgvW("") does not return zero arguments, it returns ONE:
    // the path of the running executable. Echelle therefore opened itself,
    // parsed 5 MB of machine code as a CSV, and greeted every new user with a
    // table whose first column read "This program cannot be run in DOS mode."
    if (cmdline && *cmdline) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmdline, &argc);
        if (!argv) argc = 0;
        for (int i = 0; i < argc; ++i) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr,
                                                0, nullptr, nullptr);
            std::string path(static_cast<std::size_t>(len > 0 ? len - 1 : 0), '\0');
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, path.data(), len,
                                nullptr, nullptr);
            if (!path.empty()) app.open(path);
        }
        LocalFree(argv);
    }

    bool running = true;
    while (running) {
        MSG msg;
        // Block until something happens. The idle cost of the whole program is
        // this call.
        if (!PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE)) WaitMessage();
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_occluded = false;
        if (g_resize_w != 0 && g_resize_h != 0) {
            release_rtv();
            g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_rtv();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ech::draw(app);
        ImGui::Render();

        constexpr float kBg[4] = {0.941f, 0.941f, 0.941f, 1.0f};   // #f0f0f0
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, kBg);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_occluded = g_swap->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
