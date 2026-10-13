// Windows backend — Win32 + WebView2 (Microsoft Edge).
//
// Requires the WebView2 SDK headers (WebView2.h) and linker import library
// (WebView2Loader.lib). Point CMake at them with -DWVC_WEBVIEW2_DIR=...
// The WebView2 *runtime* itself is evergreen and present on Windows 10/11.
#include "webviewcpp/asset_store.hpp"
#include "webviewcpp/embed.hpp"
#include "webviewcpp/ipc.hpp"
#include "webviewcpp/log.hpp"
#include "webviewcpp/version.hpp"
#include "webviewcpp/webview.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl.h>
#include <webview2.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace wvc {
namespace {

int g_open_windows = 0;
bool g_loop_running = false;

const wchar_t* kWndClass = L"WebViewCppHostWindow";
const UINT kMsgTask = WM_APP + 0x100;

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), len);
    return out;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr,
                                  0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
}

std::wstring build_filter(const std::vector<FileFilter>& filters) {
    std::wstring out;
    for (const auto& f : filters) {
        std::wstring name = utf8_to_wide(f.name) + L'\0';
        std::wstring patterns;
        for (const auto& g : f.globs) {
            std::string pattern = g.find('*') == std::string::npos ? "*." + g : g;
            patterns += utf8_to_wide(pattern) + L";";
        }
        if (!patterns.empty() && patterns.back() == L';') patterns.pop_back();
        patterns += L'\0';
        out += name;
        out += patterns;
    }
    out += L'\0';
    return out;
}

std::string json_headers(const std::string& mime) {
    return "Content-Type: " + mime +
           "\nAccess-Control-Allow-Origin: *\nCache-Control: no-cache";
}

}  // namespace

class Win32Backend final : public IWebViewBackend {
public:
    explicit Win32Backend(WindowOptions options) : options_(std::move(options)) {}
    ~Win32Backend() override {
        if (controller_) controller_->Close();
        if (hwnd_) DestroyWindow(hwnd_);
    }

    bool initialize(const WindowOptions& options) override {
        options_ = options;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Win32Backend::wnd_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kWndClass;
        RegisterClassExW(&wc);  // idempotent: ignores "already registered"

        DWORD style = WS_OVERLAPPEDWINDOW;
        if (!options_.resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        if (options_.frameless) style = WS_POPUP | WS_THICKFRAME;
        if (options_.always_on_top) style |= WS_EX_TOPMOST;

        int x = options_.x, y = options_.y;
        if (options_.center || x < 0 || y < 0) {
            x = (GetSystemMetrics(SM_CXSCREEN) - options_.width) / 2;
            y = (GetSystemMetrics(SM_CYSCREEN) - options_.height) / 2;
        }

        hwnd_ = CreateWindowExW(options_.always_on_top ? WS_EX_TOPMOST : 0, kWndClass,
                                utf8_to_wide(options_.title).c_str(), style, x, y,
                                options_.width, options_.height, nullptr, nullptr, instance, this);
        if (!hwnd_) {
            WVC_ERROR("Win32: CreateWindowEx failed");
            return false;
        }

        if (options_.min_width || options_.min_height) {
            min_ = {options_.min_width, options_.min_height};
        }

        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);

        create_webview();
        running_ = true;
        ++g_open_windows;
        return true;
    }

    void run() override { platform_run_loop(); }

    void navigate(const std::string& url) override {
        pending_url_ = url;
        pending_html_.clear();
        pending_is_html_ = false;
        flush_pending();
    }

    void load_asset(const std::string& path) override { navigate("app://local/" + path); }

    void set_html(const std::string& html) override {
        pending_html_ = html;
        pending_is_html_ = true;
        pending_url_.clear();
        flush_pending();
    }

    void reload() override { if (webview_) webview_->Reload(); }

    void eval(std::string_view javascript) override {
        if (webview_) {
            webview_->ExecuteScript(utf8_to_wide(std::string(javascript)).c_str(), nullptr);
        } else {
            pending_scripts_.emplace_back(javascript);
        }
    }

    void set_title(std::string_view title) override {
        if (hwnd_) SetWindowTextW(hwnd_, utf8_to_wide(std::string(title)).c_str());
    }
    void set_size(int w, int h) override {
        if (hwnd_) SetWindowPos(hwnd_, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
    }
    void get_size(int& w, int& h) const override {
        w = options_.width; h = options_.height;
        if (hwnd_) { RECT r; GetClientRect(hwnd_, &r); w = r.right; h = r.bottom; }
    }
    void set_min_size(int w, int h) override { min_ = {w, h}; }
    void set_max_size(int w, int h) override { options_.max_width = w; options_.max_height = h; }
    void set_position(int x, int y) override {
        if (hwnd_) SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    void center() override {
        if (!hwnd_) return;
        RECT r; GetWindowRect(hwnd_, &r);
        int w = r.right - r.left, h = r.bottom - r.top;
        SetWindowPos(hwnd_, nullptr, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                     (GetSystemMetrics(SM_CYSCREEN) - h) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    void set_resizable(bool resizable) override {
        if (!hwnd_) return;
        LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        if (resizable) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        else style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
    }
    void set_fullscreen(bool fullscreen) override {
        if (!hwnd_) return;
        if (fullscreen) {
            saved_style_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
            SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED);
        } else {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, saved_style_);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
    }
    void set_always_on_top(bool on_top) override {
        if (hwnd_) SetWindowPos(hwnd_, on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                                SWP_NOMOVE | SWP_NOSIZE);
    }
    void set_zoom(double factor) override {
        if (controller_) controller_->put_ZoomFactor(factor);
    }
    void set_cursor(CursorShape) override {}
    void minimize() override { if (hwnd_) ShowWindow(hwnd_, SW_MINIMIZE); }
    void maximize() override { if (hwnd_) ShowWindow(hwnd_, SW_MAXIMIZE); }
    void restore() override { if (hwnd_) ShowWindow(hwnd_, SW_RESTORE); }
    void hide() override { if (hwnd_) ShowWindow(hwnd_, SW_HIDE); }
    void show() override { if (hwnd_) ShowWindow(hwnd_, SW_SHOW); }
    void set_devtools(bool enabled) override {
        if (webview_) webview_->OpenDevToolsWindow();
        (void)enabled;
    }
    void request_close() override { if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0); }
    bool is_running() const override { return running_; }

    void dispatch_to_ui(std::function<void()> task) override {
        auto* fn = new std::function<void()>(std::move(task));
        PostMessageW(hwnd_, kMsgTask, 0, reinterpret_cast<LPARAM>(fn));
    }

    // --- dialogs / clipboard --------------------------------------------
    std::vector<std::string> open_file_dialog(const OpenFileDialogOptions& options) override {
        std::vector<std::string> result;
        if (options.choose_directory) {
            BROWSEINFOW bi{};
            bi.hwndOwner = hwnd_;
            bi.lpszTitle = utf8_to_wide(options.title).c_str();
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
            if (idl) {
                wchar_t path[MAX_PATH]{};
                if (SHGetPathFromIDListW(idl, path)) result.push_back(wide_to_utf8(path));
                CoTaskMemFree(idl);
            }
            return result;
        }

        std::wstring filter = build_filter(options.filters);
        std::vector<wchar_t> buffer(65536, L'\0');
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = filter.empty() ? nullptr : filter.c_str();
        ofn.lpstrFile = buffer.data();
        ofn.nMaxFile = static_cast<DWORD>(buffer.size());
        ofn.lpstrTitle = options.title.empty() ? nullptr : utf8_to_wide(options.title).c_str();
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;
        if (options.allow_multiple) ofn.Flags |= OFN_ALLOWMULTISELECT;

        if (!GetOpenFileNameW(&ofn)) return result;
        const wchar_t* p = buffer.data();
        std::wstring dir = p;
        p += dir.size() + 1;
        if (*p == L'\0') {
            result.push_back(wide_to_utf8(dir));
        } else {
            while (*p) {
                std::wstring file = p;
                result.push_back(wide_to_utf8(dir + L"\\" + file));
                p += file.size() + 1;
            }
        }
        return result;
    }

    std::string save_file_dialog(const SaveFileDialogOptions& options) override {
        std::wstring filter = build_filter(options.filters);
        std::wstring name = options.default_name.empty()
                                ? std::wstring(L"Untitled")
                                : utf8_to_wide(options.default_name);
        std::vector<wchar_t> buffer(MAX_PATH, L'\0');
        wcsncpy_s(buffer.data(), buffer.size(), name.c_str(), _TRUNCATE);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = filter.empty() ? nullptr : filter.c_str();
        ofn.lpstrFile = buffer.data();
        ofn.nMaxFile = static_cast<DWORD>(buffer.size());
        ofn.lpstrTitle = options.title.empty() ? nullptr : utf8_to_wide(options.title).c_str();
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_EXPLORER;
        if (!GetSaveFileNameW(&ofn)) return {};
        return wide_to_utf8(buffer.data());
    }

    int message_box(const MessageBoxOptions& options) override {
        UINT flags = MB_TOPMOST;
        switch (options.type) {
            case MessageBoxType::Warning: flags |= MB_ICONWARNING; break;
            case MessageBoxType::Error: flags |= MB_ICONERROR; break;
            case MessageBoxType::Question: flags |= MB_ICONQUESTION; break;
            default: flags |= MB_ICONINFORMATION;
        }
        switch (options.buttons) {
            case MessageBoxButtons::Ok: flags |= MB_OK; break;
            case MessageBoxButtons::OkCancel: flags |= MB_OKCANCEL; break;
            case MessageBoxButtons::YesNo: flags |= MB_YESNO; break;
            case MessageBoxButtons::YesNoCancel: flags |= MB_YESNOCANCEL; break;
        }
        int r = MessageBoxW(hwnd_, utf8_to_wide(options.message).c_str(),
                            utf8_to_wide(options.title).c_str(), flags);
        return (r == IDOK || r == IDYES) ? 0 : 1;
    }

    std::string clipboard_read_text() override {
        std::string result;
        if (!OpenClipboard(hwnd_)) return result;
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (data) {
            auto* wide = static_cast<const wchar_t*>(GlobalLock(data));
            if (wide) { result = wide_to_utf8(wide); GlobalUnlock(data); }
        }
        CloseClipboard();
        return result;
    }
    void clipboard_write_text(std::string text) override {
        if (!OpenClipboard(hwnd_)) return;
        EmptyClipboard();
        std::wstring wide = utf8_to_wide(text);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
        if (mem) {
            auto* dst = static_cast<wchar_t*>(GlobalLock(mem));
            memcpy(dst, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
        CloseClipboard();
    }

private:
    void create_webview() {
        // A per-user data folder keeps cookies/storage app-scoped.
        std::wstring user_data;
        {
            wchar_t* appdata = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata))) {
                user_data = std::wstring(appdata) + L"\\WebViewCpp\\" +
                            utf8_to_wide(options_.title.empty() ? "app" : options_.title);
                CoTaskMemFree(appdata);
            }
        }

        auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        auto handler = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    WVC_ERROR("WebView2: environment creation failed (is the runtime installed?)");
                    return result;
                }
                env_ = env;
                env_->CreateCoreWebView2Controller(
                    hwnd_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(res) || !controller) {
                                WVC_ERROR("WebView2: controller creation failed");
                                return res;
                            }
                            controller_ = controller;
                            controller_->get_CoreWebView2(&webview_);
                            configure_webview();
                            flush_pending();
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            });

        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, user_data.empty() ? nullptr : user_data.c_str(), options.Get(), handler.Get());
        if (FAILED(hr)) {
            WVC_ERROR("WebView2: CreateCoreWebView2EnvironmentWithOptions failed; "
                      "install the WebView2 Evergreen Runtime");
        }
    }

    void configure_webview() {
        RECT bounds;
        GetClientRect(hwnd_, &bounds);
        controller_->put_Bounds(bounds);
        controller_->put_IsVisible(TRUE);
        if (options_.zoom != 1.0) controller_->put_ZoomFactor(options_.zoom);

        // Native -> JS message channel and page -> native handler.
        webview_->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    LPWSTR json = nullptr;
                    if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                        if (ipc) ipc->receive(wide_to_utf8(json));
                        CoTaskMemFree(json);
                    }
                    return S_OK;
                })
                .Get(),
            nullptr);

        // app:// custom scheme served straight from the AssetStore.
        webview_->AddWebResourceRequestedFilter(L"app://*",
                                                COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        webview_->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args)
                    -> HRESULT {
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    args->get_Request(&request);
                    LPWSTR uri = nullptr;
                    request->get_Uri(&uri);
                    std::string path = uri ? wide_to_utf8(uri) : "";
                    if (uri) CoTaskMemFree(uri);

                    auto asset = assets ? assets->resolve_url(path) : std::nullopt;
                    if (!asset) {
                        ComPtr<IStream> body = SHCreateMemStream(nullptr, 0);
                        ComPtr<ICoreWebView2WebResourceResponse> response;
                        env_->CreateWebResourceResponse(
                            body.Get(), 404, L"Not Found", L"Content-Type: text/plain", &response);
                        args->put_Response(response.Get());
                        return S_OK;
                    }
                    ComPtr<IStream> stream = SHCreateMemStream(asset->bytes.data(),
                                                               static_cast<UINT>(asset->bytes.size()));
                    ComPtr<ICoreWebView2WebResourceResponse> response;
                    env_->CreateWebResourceResponse(stream.Get(), 200, L"OK",
                                                    utf8_to_wide(json_headers(asset->mime)).c_str(),
                                                    &response);
                    args->put_Response(response.Get());
                    return S_OK;
                })
                .Get(),
            nullptr);

        // Fire the ready handshake when a document finishes loading.
        webview_->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                    if (on_ready) on_ready();
                    return S_OK;
                })
                .Get(),
            nullptr);

        // Inject the bootstrap at document-start, then flush queued scripts.
        std::string preamble =
            "window.__WVC_ENV__={platform:\"windows\",version:\"" WVC_VERSION_STRING "\"};";
        std::wstring script = utf8_to_wide(preamble + IpcBridge::bootstrap_script());
        webview_->AddScriptToExecuteOnDocumentCreated(
            script.c_str(),
            Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                [this](HRESULT, LPCWSTR) -> HRESULT {
                    for (auto& pending : pending_scripts_)
                        webview_->ExecuteScript(utf8_to_wide(pending).c_str(), nullptr);
                    pending_scripts_.clear();
                    return S_OK;
                })
                .Get());

        if (options_.devtools) webview_->OpenDevToolsWindow();

        WebViewSettings settings;
        settings.set_context_menu(webview_.Get(), options_.context_menu);
    }

    void flush_pending() {
        if (!webview_) return;
        if (pending_html_.empty() && pending_url_.empty()) return;
        if (pending_is_html_) {
            webview_->NavigateToString(utf8_to_wide(pending_html_).c_str());
        } else {
            webview_->Navigate(utf8_to_wide(pending_url_).c_str());
        }
    }

    std::string appdata_path() const { return {}; }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        Win32Backend* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Win32Backend*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Win32Backend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);

        switch (msg) {
            case WM_SIZE: {
                if (self->controller_) {
                    RECT bounds{0, 0, LOWORD(lparam), HIWORD(lparam)};
                    self->controller_->put_Bounds(bounds);
                }
                self->options_.width = LOWORD(lparam);
                self->options_.height = HIWORD(lparam);
                if (self->on_resize) self->on_resize(LOWORD(lparam), HIWORD(lparam));
                return 0;
            }
            case WM_GETMINMAXINFO: {
                auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
                if (self->min_.x) info->ptMinTrackSize = {self->min_.x, self->min_.y};
                if (self->options_.max_width)
                    info->ptMaxTrackSize = {self->options_.max_width, self->options_.max_height};
                return 0;
            }
            case WM_SETFOCUS:
                if (self->controller_) self->controller_->MoveFocus(
                    COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                if (self->on_focus) self->on_focus();
                return 0;
            case kMsgTask: {
                auto* fn = reinterpret_cast<std::function<void()>*>(lparam);
                (*fn)();
                delete fn;
                return 0;
            }
            case WM_CLOSE:
                if (self->on_close) self->on_close();
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY: {
                self->running_ = false;
                if (--g_open_windows <= 0) platform_quit();
                return 0;
            }
            default:
                break;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    // Small helper to toggle the WebView2 context menu on/off.
    struct WebViewSettings {
        void set_context_menu(ICoreWebView2* webview, bool enabled) {
            ComPtr<ICoreWebView2Settings> settings;
            if (SUCCEEDED(webview->get_Settings(&settings)) && settings)
                settings->put_AreDefaultContextMenusEnabled(enabled ? TRUE : FALSE);
        }
    };

    WindowOptions options_;
    HWND hwnd_ = nullptr;
    ComPtr<ICoreWebView2Environment> env_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    POINT min_{0, 0};
    LONG_PTR saved_style_ = 0;
    bool running_ = false;
    bool pending_is_html_ = false;
    std::string pending_url_;
    std::string pending_html_;
    std::vector<std::string> pending_scripts_;
    std::string user_data_dir_;
};

std::unique_ptr<IWebViewBackend> create_webview_backend(WindowOptions options, IpcBridge& ipc,
                                                        AssetStore* assets) {
    auto backend = std::make_unique<Win32Backend>(std::move(options));
    backend->ipc = &ipc;
    backend->assets = assets;
    return backend;
}

int platform_run_loop() {
    if (g_loop_running) return 0;
    g_loop_running = true;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_loop_running = false;
    return static_cast<int>(msg.wParam);
}

void platform_quit() { PostQuitMessage(0); }
const char* platform_name() { return "windows"; }
const char* platform_family() { return "win32"; }

}  // namespace wvc
