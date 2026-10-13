// WebViewCpp example — a React front end driving a C++ backend.
//
// Demonstrates:
//   * ready-made services (system info, file system, dialogs, clipboard)
//   * a custom native function invoked from React (app.readFileViaDialog)
//   * native -> JS push events (native:fileLoaded)
//   * opening a second native window at runtime
#include "webviewcpp/webviewcpp.hpp"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace wvc;

namespace {

std::vector<std::unique_ptr<Window>>& live_windows() {
    static std::vector<std::unique_ptr<Window>> windows;
    return windows;
}

std::string read_text_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Registers the example's custom IPC channels on a window.
void wire_channels(Window& window) {
    // Ready-made native services.
    window.install_system_services();     // system.info, system.env, shell.open, ...
    window.install_fs_services();         // fs.readText, fs.writeText, fs.list, ...
    window.install_dialog_services();     // dialog.open, dialog.save, dialog.message
    window.install_clipboard_services();  // clipboard.readText / writeText

    // Native file picker + read, exposed as a single async call.
    window.handle("app.readFileViaDialog", [&window](const Json&) -> Json {
        IWebViewBackend* backend = window.native_backend();
        if (!backend) throw std::runtime_error("backend not ready");

        OpenFileDialogOptions options;
        options.title = "Open a text file";
        options.filters = {{"Text files", {"txt", "md", "json", "log", "csv"}},
                           {"All files", {"*"}}};
        auto paths = backend->open_file_dialog(options);
        if (paths.empty()) return Json{{"canceled", true}};

        std::string content = read_text_file(paths.front());
        if (content.size() > 200000) content.resize(200000);  // keep the payload sane
        Json result = Json::object{};
        result["canceled"] = false;
        result["path"] = paths.front();
        result["content"] = content;
        return result;
    });

    // Persist a snippet to a temporary file and report the path.
    window.handle("app.saveSnippet", [](const Json& payload) -> Json {
        std::string text = payload["text"].string_or("");
        std::filesystem::path target =
            std::filesystem::temp_directory_path() /
            ("wvc-snippet-" + std::to_string(static_cast<long long>(std::time(nullptr))) + ".txt");
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + target.string());
        out << text;
        return Json{{"ok", true},
                    {"path", target.string()},
                    {"bytes", static_cast<int64_t>(text.size())}};
    });

    // Show a native message box.
    window.handle("app.notify", [&window](const Json& payload) -> Json {
        MessageBoxOptions options;
        options.title = payload["title"].string_or("React Counter");
        options.message = payload["message"].string_or("");
        options.type = MessageBoxType::Info;
        int response = window.native_backend()->message_box(options);
        return Json{{"response", response}};
    });

    // Open an additional native window from the running app.
    window.handle("app.newWindow", [&window](const Json& payload) -> Json {
        auto child = std::make_unique<Window>(window.options());
        child->title(payload["title"].string_or("React Counter — second window"));
        wire_channels(*child);
        child->load_app();
        child->open();  // creates + shows the window on the UI thread
        live_windows().push_back(std::move(child));
        return Json{{"ok", true}, {"count", static_cast<int64_t>(live_windows().size())}};
    });
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    WindowOptions options;
    options.title = "React Counter";
    options.width = 900;
    options.height = 680;
    options.min_width = 520;
    options.min_height = 420;
    options.devtools = true;

    Window window(options);
    wire_channels(window);

    // Push the current time to the UI every time a file is loaded.
    window.on_ready([&window]() {
        window.emit("native:ready", Json{{"framework", "WebViewCpp"}, {"version", WVC_VERSION_STRING}});
    });

    if (const char* dev_url = std::getenv("WVC_DEV_URL")) {
        window.load_url(dev_url);
    } else {
        window.load_app();
    }

    return window.run();
}
