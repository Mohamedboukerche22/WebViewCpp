#pragma once

#include "webviewcpp/asset_store.hpp"
#include "webviewcpp/ipc.hpp"
#include "webviewcpp/json.hpp"
#include "webviewcpp/types.hpp"
#include "webviewcpp/webview.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wvc {

// A native top-level window hosting a webview, plus the IPC bridge and the
// asset store that back it.
//
//   wvc::Window win({.title="Notes", .width=900, .height=640});
//   win.handle("fs.read", [](const Json& p){ ... });
//   win.load_app();
//   win.run();
//
class Window {
public:
    using Handler = IpcBridge::Handler;

    static constexpr const char* kDefaultEntry = "app://local/index.html";

    explicit Window(WindowOptions options = {});
    explicit Window(std::string url, WindowOptions options = {});
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // --- fluent configuration (must happen before run()) ----------------
    Window& title(std::string value);
    Window& size(int width, int height);
    Window& min_size(int width, int height);
    Window& max_size(int width, int height);
    Window& position(int x, int y);
    Window& resizable(bool value);
    Window& frameless(bool value);
    Window& transparent(bool value);
    Window& devtools(bool value);
    Window& always_on_top(bool value);
    Window& zoom(double factor);
    Window& icon(std::string path);
    Window& user_agent(std::string value);
    Window& background(std::string css_color);

    // --- content ---------------------------------------------------------
    Window& load_app(std::string entry = kDefaultEntry);
    Window& load_url(std::string url);
    Window& load_html(std::string html);

    // Give the window explicit assets. If none is set the compiled-in bundle
    // (WVC_EMBED_ASSETS) is used.
    Window& use_assets(std::unique_ptr<AssetStore> store);
    AssetStore* assets();

    // --- ipc -------------------------------------------------------------
    Window& handle(const std::string& channel, Handler handler);
    Window& on(const std::string& channel, Handler handler) { return handle(channel, std::move(handler)); }
    Window& on_ready(std::function<void()> callback);
    Window& on_close(std::function<void()> callback);
    Window& on_resize(std::function<void(int, int)> callback);
    void emit(const std::string& channel, const Json& payload = Json());
    void eval(std::string_view javascript);

    // --- runtime window control -----------------------------------------
    void set_title(std::string_view value);
    void set_size(int width, int height);
    Rect get_size() const;
    void center();
    void minimize();
    void maximize();
    void restore();
    void set_fullscreen(bool value);
    void show();
    void hide();
    void reload();
    void navigate(std::string url);

    // Built-in, ready-made IPC service groups. Each registers channels on this
    // window's bridge (see docs/ipc.md for the full channel list).
    Window& install_system_services();                                   // system.*, shell.*
    Window& install_fs_services(std::optional<std::string> sandbox_root = std::nullopt);  // fs.*
    Window& install_dialog_services();                                   // dialog.*
    Window& install_clipboard_services();                                // clipboard.*
    Window& install_default_services();                                  // all of the above

    IpcBridge& ipc() { return ipc_; }
    const WindowOptions& options() const { return options_; }
    IWebViewBackend* native_backend() { return backend_.get(); }
    bool is_open() const;

    // Build the native window (idempotent) and enter the event loop.
    int run();

    // Create the native window immediately without entering the event loop.
    // Use this to open additional windows while another window is already
    // running the loop (call it from the UI thread, e.g. an IPC handler).
    Window& open();

    // Ask the underlying window to close.
    void close();

private:
    void ensure_backend();
    void apply_assets();
    void wire_callbacks();

    WindowOptions options_;
    IpcBridge ipc_;
    std::unique_ptr<AssetStore> assets_;
    std::unique_ptr<IWebViewBackend> backend_;
    std::string initial_url_;
    std::string initial_html_;
    std::vector<std::pair<std::string, Handler>> pending_handlers_;
    std::function<void()> user_on_ready_;
    std::function<void()> user_on_close_;
    std::function<void(int, int)> user_on_resize_;
    bool backend_ready_ = false;
    bool loaded_ = false;
};

}  // namespace wvc
