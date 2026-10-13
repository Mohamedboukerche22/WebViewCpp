#pragma once

#include "webviewcpp/types.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wvc {

class IpcBridge;
class AssetStore;

// Options consumed by every platform backend. Fields a given backend cannot
// honour are ignored (with a warning) rather than fatal.
struct WindowOptions {
    std::string title = "WebViewCpp";
    int width = 1024;
    int height = 720;
    int x = -1;                 // -1 => let the OS place it
    int y = -1;
    int min_width = 320;
    int min_height = 240;
    int max_width = 0;          // 0 => unbounded
    int max_height = 0;
    bool resizable = true;
    bool frameless = false;
    bool transparent = false;
    bool center = true;
    bool devtools = false;
    bool always_on_top = false;
    bool fullscreen = false;
    double zoom = 1.0;
    std::string icon;           // path to .ico/.png/.icns
    std::string user_agent;
    std::string background = "#00000000";  // used when transparent
    bool allow_file_access = false;
    bool context_menu = true;
};

enum class CursorShape { Default, Pointer, Text, Crosshair, Move, Wait, NotAllowed };

struct OpenFileDialogOptions {
    std::string title = "Open File";
    std::string default_path;
    std::vector<FileFilter> filters;
    bool allow_multiple = false;
    bool choose_directory = false;
};

struct SaveFileDialogOptions {
    std::string title = "Save File";
    std::string default_path;
    std::string default_name;
    std::vector<FileFilter> filters;
};

enum class MessageBoxType { Info, Warning, Error, Question };
enum class MessageBoxButtons { Ok, OkCancel, YesNo, YesNoCancel };

struct MessageBoxOptions {
    std::string title;
    std::string message;
    MessageBoxType type = MessageBoxType::Info;
    MessageBoxButtons buttons = MessageBoxButtons::Ok;
};

// Each platform implements exactly one of these. The core only ever talks to
// this interface; no <windows.h>, <Cocoa/Cocoa.h> or <gtk/gtk.h> leaks upward.
class IWebViewBackend {
public:
    virtual ~IWebViewBackend() = default;

    // Create the native window + webview. Returns false on failure (a message
    // is logged through wvc::Log). Must be called on the UI thread.
    virtual bool initialize(const WindowOptions& options) = 0;

    // Blocks in the platform event loop until the window closes or the
    // runtime is quit. Implementations share the process-wide loop.
    virtual void run() = 0;

    // Navigation and content.
    virtual void navigate(const std::string& url) = 0;
    virtual void load_asset(const std::string& path) = 0;  // e.g. "index.html"
    virtual void set_html(const std::string& html) = 0;
    virtual void reload() = 0;
    virtual void eval(std::string_view javascript) = 0;

    // Window geometry / chrome.
    virtual void set_title(std::string_view title) = 0;
    virtual void set_size(int width, int height) = 0;
    virtual void get_size(int& width, int& height) const = 0;
    virtual void set_min_size(int width, int height) = 0;
    virtual void set_max_size(int width, int height) = 0;
    virtual void set_position(int x, int y) = 0;
    virtual void center() = 0;
    virtual void set_resizable(bool resizable) = 0;
    virtual void set_fullscreen(bool fullscreen) = 0;
    virtual void set_always_on_top(bool on_top) = 0;
    virtual void set_zoom(double factor) = 0;
    virtual void set_cursor(CursorShape shape) = 0;
    virtual void minimize() = 0;
    virtual void maximize() = 0;
    virtual void restore() = 0;
    virtual void hide() = 0;
    virtual void show() = 0;
    virtual void set_devtools(bool enabled) = 0;

    // Native modal dialogs and the system clipboard. These block the UI thread
    // while open, exactly like their OS counterparts.
    virtual std::vector<std::string> open_file_dialog(const OpenFileDialogOptions& options) = 0;
    virtual std::string save_file_dialog(const SaveFileDialogOptions& options) = 0;
    virtual int message_box(const MessageBoxOptions& options) = 0;  // 0=Ok/Yes, 1=Cancel/No
    virtual std::string clipboard_read_text() = 0;
    virtual void clipboard_write_text(std::string text) = 0;

    virtual void request_close() = 0;
    virtual bool is_running() const = 0;
    virtual void dispatch_to_ui(std::function<void()> task) = 0;

    // --- callbacks wired by the core -------------------------------------
    std::function<void(std::string)> on_message;      // raw JSON from the page
    std::function<void()> on_ready;                   // document loaded
    std::function<void()> on_close;                   // window about to close
    std::function<void(int, int)> on_resize;
    std::function<void()> on_focus;

    // Services handed to the backend at construction.
    IpcBridge* ipc = nullptr;
    AssetStore* assets = nullptr;
};

// Implemented by src/platform/<backend>.cpp / .mm.
std::unique_ptr<IWebViewBackend> create_webview_backend(WindowOptions options,
                                                        IpcBridge& ipc,
                                                        AssetStore* assets);

// Process-wide event loop shared by all windows.
int platform_run_loop();
void platform_quit();
const char* platform_name();
const char* platform_family();

}  // namespace wvc
