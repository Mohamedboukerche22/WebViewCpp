#include "webviewcpp/services.hpp"

#include "webviewcpp/log.hpp"
#include "webviewcpp/version.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <cstdlib>
#include <unistd.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace wvc::services {
namespace {

#if defined(_WIN32)
std::string env_or(const char* key, std::string fallback = "") {
    char buf[32767];
    DWORD n = GetEnvironmentVariableA(key, buf, sizeof(buf));
    return n ? std::string(buf, n) : std::string(fallback);
}
#else
std::string env_or(const char* key, std::string fallback = "") {
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string(fallback);
}
#endif

std::string arch_name() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "ia32";
#else
    return "unknown";
#endif
}

fs::path expand_and_abs(const fs::path& p) {
    std::string s = p.string();
    if (s.rfind("~", 0) == 0) s = env_or("HOME", env_or("USERPROFILE")) + s.substr(1);
    std::error_code ec;
    auto abs = fs::weakly_canonical(fs::path(s), ec);
    return ec ? fs::absolute(fs::path(s)) : abs;
}

// Guard against path traversal when a sandbox root is configured.
fs::path guard(const fs::path& root, const fs::path& requested) {
    if (root.empty()) return expand_and_abs(requested);
    auto target = expand_and_abs(root / requested);
    auto base = expand_and_abs(root);
    auto rel = fs::relative(target, base);
    if (rel.empty() || rel.native().rfind("..", 0) == 0)
        throw std::runtime_error("fs: path escapes sandbox root: " + requested.string());
    return target;
}

}  // namespace

std::string base64_encode(const uint8_t* data, std::size_t size) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < size; i += 3) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(kTable[(n >> 6) & 63]);
        out.push_back(kTable[n & 63]);
    }
    if (i < size) {
        uint32_t n = data[i] << 16;
        if (i + 1 < size) n |= data[i + 1] << 8;
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(i + 1 < size ? kTable[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::vector<uint8_t> base64_decode(const std::string& text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    int buf = 0, bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int v = value(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

bool shell_open(const std::string& target) {
    if (target.empty()) return false;
#if defined(_WIN32)
    // ShellExecute needs a wide string; convert with the ANSI code page is
    // wrong for unicode paths, so use MultiByteToWideChar.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, wide.data(), wlen);
    auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
#elif defined(__APPLE__)
    std::string cmd = "open \"" + target + "\" >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#else
    std::string cmd = "xdg-open \"" + target + "\" >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#endif
}

Json system_info() {
    Json info;
    info["framework"] = "WebViewCpp";
    info["frameworkVersion"] = WVC_VERSION_STRING;
    info["platform"] = platform_name();
    info["family"] = platform_family();
    info["arch"] = arch_name();
    info["cpus"] = static_cast<int64_t>(std::thread::hardware_concurrency());
    info["pid"] =
#if defined(_WIN32)
        static_cast<int64_t>(GetCurrentProcessId());
#else
        static_cast<int64_t>(::getpid());
#endif
    std::error_code ec;
    info["cwd"] = fs::current_path(ec).string();
    info["home"] = env_or("HOME", env_or("USERPROFILE")).c_str();
    info["temp"] = fs::temp_directory_path(ec).string().c_str();
#if defined(_WIN32)
    info["appData"] = env_or("APPDATA");
    info["configDir"] = env_or("APPDATA");
#elif defined(__APPLE__)
    info["appData"] = (env_or("HOME") + "/Library/Application Support");
    info["configDir"] = (env_or("HOME") + "/Library/Application Support");
#else
    info["appData"] = env_or("XDG_DATA_HOME", (env_or("HOME") + "/.local/share").c_str());
    info["configDir"] = env_or("XDG_CONFIG_HOME", (env_or("HOME") + "/.config").c_str());
#endif
    return info;
}

void install_system(IpcBridge& ipc) {
    ipc.handle("system.info", [](const Json&) { return system_info(); });
    ipc.handle("system.platform", [](const Json&) {
        return Json{{"platform", platform_name()}, {"family", platform_family()}};
    });
    ipc.handle("system.env", [](const Json& payload) {
        std::string key = payload["key"].string_or("");
        return Json{{"value", env_or(key.c_str())}};
    });
    ipc.handle("shell.open", [](const Json& payload) {
        std::string target = payload["target"].string_or(payload.string_or(""));
        if (target.empty()) throw std::runtime_error("shell.open: missing 'target'");
        bool ok = shell_open(target);
        return Json{{"ok", ok}};
    });
    ipc.handle("shell.showItemInFolder", [](const Json& payload) {
        std::string path = payload["path"].string_or("");
        fs::path p = expand_and_abs(path);
#if defined(_WIN32)
        std::string cmd = "explorer /select,\"" + p.string() + "\"";
#elif defined(__APPLE__)
        std::string cmd = "open -R \"" + p.string() + "\"";
#else
        // Best effort: xdg-open on the containing directory.
        std::string cmd = "xdg-open \"" + p.parent_path().string() + "\" >/dev/null 2>&1 &";
#endif
        std::system(cmd.c_str());
        return Json{{"ok", true}};
    });
}

void install_fs(IpcBridge& ipc, std::optional<std::string> sandbox_root) {
    const fs::path root = sandbox_root ? fs::path(*sandbox_root) : fs::path{};

    ipc.handle("fs.readText", [root](const Json& p) {
        auto path = guard(root, p["path"].string_or(""));
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("fs.readText: cannot open " + path.string());
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return Json{{"text", std::move(text)}, {"path", path.string()}};
    });

    ipc.handle("fs.writeText", [root](const Json& p) {
        auto path = guard(root, p["path"].string_or(""));
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("fs.writeText: cannot write " + path.string());
        std::string text = p["text"].string_or("");
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return Json{{"path", path.string()}, {"bytes", static_cast<int64_t>(text.size())}};
    });

    ipc.handle("fs.readBinary", [root](const Json& p) {
        auto path = guard(root, p["path"].string_or(""));
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("fs.readBinary: cannot open " + path.string());
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        return Json{{"data", base64_encode(data.data(), data.size())},
                    {"encoding", "base64"}, {"bytes", static_cast<int64_t>(data.size())}};
    });

    ipc.handle("fs.writeBinary", [root](const Json& p) {
        auto path = guard(root, p["path"].string_or(""));
        auto data = base64_decode(p["data"].string_or(""));
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("fs.writeBinary: cannot write " + path.string());
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        return Json{{"path", path.string()}, {"bytes", static_cast<int64_t>(data.size())}};
    });

    ipc.handle("fs.exists", [root](const Json& p) {
        auto path = guard(root, p["path"].string_or(""));
        std::error_code ec;
        return Json{{"exists", fs::exists(path, ec)},
                    {"isFile", fs::is_regular_file(path, ec)},
                    {"isDirectory", fs::is_directory(path, ec)}};
    });

    ipc.handle("fs.stat", [root](const Json& p) {
        auto path = guard(root, p["path"].string_or(""));
        std::error_code ec;
        if (!fs::exists(path, ec)) throw std::runtime_error("fs.stat: not found");
        Json out;
        out["path"] = path.string();
        out["size"] = static_cast<int64_t>(fs::file_size(path, ec));
        out["isDirectory"] = fs::is_directory(path, ec);
        return out;
    });

    ipc.handle("fs.list", [root](const Json& p) {
        auto path = guard(root, p["path"].string_or("."));
        Json entries = Json::array{};
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(path, ec)) {
            Json e;
            e["name"] = entry.path().filename().string();
            e["path"] = entry.path().string();
            e["isDirectory"] = entry.is_directory(ec);
            e["size"] = entry.is_regular_file(ec)
                            ? static_cast<int64_t>(entry.file_size(ec))
                            : 0;
            entries.push_back(std::move(e));
        }
        return Json{{"path", path.string()}, {"entries", std::move(entries)}};
    });

    ipc.handle("fs.mkdir", [root](const Json& p) {
        auto path = guard(root, p["path"].string_or(""));
        if (p["recursive"].bool_or(true)) fs::create_directories(path);
        else fs::create_directory(path);
        return Json{{"path", path.string()}};
    });

    ipc.handle("fs.remove", [root](const Json& p) {
        auto path = guard(root, p["path"].string_or(""));
        std::error_code ec;
        if (p["recursive"].bool_or(false)) fs::remove_all(path, ec);
        else fs::remove(path, ec);
        if (ec) throw std::runtime_error("fs.remove: " + ec.message());
        return Json{{"ok", true}};
    });

    ipc.handle("fs.join", [](const Json& p) {
        fs::path result;
        for (const auto& part : p["parts"].as_array()) result /= part.string_or("");
        return Json{{"path", result.string()}};
    });
}

void install_dialogs(IpcBridge& ipc, BackendAccessor backend) {
    ipc.handle("dialog.open", [backend](const Json& p) -> Json {
        auto* b = backend();
        if (!b) throw std::runtime_error("dialog.open: backend not ready");
        OpenFileDialogOptions options;
        options.title = p["title"].string_or("Open File");
        options.default_path = p["defaultPath"].string_or("");
        options.allow_multiple = p["multiple"].bool_or(false);
        options.choose_directory = p["directory"].bool_or(false);
        for (const auto& f : p["filters"].as_array()) {
            FileFilter filter;
            filter.name = f["name"].string_or("Files");
            for (const auto& g : f["extensions"].as_array()) filter.globs.push_back(g.string_or(""));
            options.filters.push_back(std::move(filter));
        }
        auto paths = b->open_file_dialog(options);
        Json arr = Json::array{};
        for (auto& path : paths) arr.push_back(path);
        return Json{{"paths", std::move(arr)},
                    {"path", paths.empty() ? "" : paths.front()},
                    {"canceled", paths.empty()}};
    });

    ipc.handle("dialog.save", [backend](const Json& p) -> Json {
        auto* b = backend();
        if (!b) throw std::runtime_error("dialog.save: backend not ready");
        SaveFileDialogOptions options;
        options.title = p["title"].string_or("Save File");
        options.default_path = p["defaultPath"].string_or("");
        options.default_name = p["defaultName"].string_or("");
        for (const auto& f : p["filters"].as_array()) {
            FileFilter filter;
            filter.name = f["name"].string_or("Files");
            for (const auto& g : f["extensions"].as_array()) filter.globs.push_back(g.string_or(""));
            options.filters.push_back(std::move(filter));
        }
        auto path = b->save_file_dialog(options);
        return Json{{"path", path}, {"canceled", path.empty()}};
    });

    ipc.handle("dialog.message", [backend](const Json& p) -> Json {
        auto* b = backend();
        if (!b) throw std::runtime_error("dialog.message: backend not ready");
        MessageBoxOptions options;
        options.title = p["title"].string_or("");
        options.message = p["message"].string_or("");
        std::string type = p["type"].string_or("info");
        options.type = type == "warning"  ? MessageBoxType::Warning
                       : type == "error"  ? MessageBoxType::Error
                       : type == "question" ? MessageBoxType::Question
                                            : MessageBoxType::Info;
        std::string buttons = p["buttons"].string_or("ok");
        options.buttons = buttons == "okCancel"   ? MessageBoxButtons::OkCancel
                          : buttons == "yesNo"    ? MessageBoxButtons::YesNo
                          : buttons == "yesNoCancel" ? MessageBoxButtons::YesNoCancel
                                                     : MessageBoxButtons::Ok;
        int result = b->message_box(options);
        return Json{{"response", result}};
    });
}

void install_clipboard(IpcBridge& ipc, BackendAccessor backend) {
    ipc.handle("clipboard.readText", [backend](const Json&) {
        auto* b = backend();
        return Json{{"text", b ? b->clipboard_read_text() : std::string{}}};
    });
    ipc.handle("clipboard.writeText", [backend](const Json& p) {
        auto* b = backend();
        if (b) b->clipboard_write_text(p["text"].string_or(""));
        return Json{{"ok", true}};
    });
}

}  // namespace wvc::services
