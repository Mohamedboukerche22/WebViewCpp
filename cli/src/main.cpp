// wvc — the WebViewCpp command line tool.
//
//   wvc new <name>      scaffold a React + C++ project
//   wvc dev             run the Vite dev server + a hot-reloading native shell
//   wvc build           build the frontend and compile a standalone binary
//   wvc run             build then launch
//   wvc package         produce a distributable (.AppImage/.app/.exe)
//   wvc doctor          verify the toolchain
//   wvc info            print the resolved project configuration
#include "project.hpp"
#include "webviewcpp/version.hpp"

#include <clocale>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace wvc::cli;

namespace {

const char* kUsage = R"(wvc — WebViewCpp CLI

USAGE:
    wvc <command> [options]

COMMANDS:
    new <name>          Scaffold a new project in ./<name>
    dev                 Dev server + native shell with live reload
    build               Build the frontend and compile a native binary
    run                 Build and launch the app
    package             Build a distributable for the host platform
    doctor              Check the local toolchain
    info                Show the resolved project configuration
    help                Show this help

OPTIONS:
    -c, --config <file> Path to wvc.config.json (default: search upward)
    -r, --release       Build in release mode
        --no-frontend   Skip the npm build step (reuse existing dist/)
        --target <os>   Package target: linux | macos | windows
        --wvc-root <dir> Location of the WebViewCpp framework source
        --no-color      Disable coloured output
        --version       Print the CLI version
)";

fs::path find_config(fs::path start) {
    std::error_code ec;
    start = fs::absolute(start, ec);
    for (int depth = 0; depth < 64; ++depth) {
        fs::path candidate = start / "wvc.config.json";
        if (fs::exists(candidate)) return candidate;
        if (!start.has_parent_path() || start.parent_path() == start) break;
        start = start.parent_path();
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");
#if defined(_WIN32)
    // Enable ANSI escape processing on Windows 10+.
    std::system("");
#endif

    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) {
        std::printf("%s", kUsage);
        return 0;
    }
    for (auto& a : args)
        if (a == "--no-color" || a == "--plain") color_enabled() = false;

    std::string command = args[0];
    if (command == "help" || command == "-h" || command == "--help") {
        std::printf("%s", kUsage);
        return 0;
    }
    if (command == "version" || command == "--version" || command == "-v") {
        std::printf("wvc %s\n", wvc::version_string());
        return 0;
    }

    // Parse options.
    std::string config_path, target, wvc_root, new_dir;
    bool release = false, skip_frontend = false;
    std::vector<std::string> positional;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= args.size()) { log_error(std::string(flag) + " needs a value"); std::exit(2); }
            return args[++i];
        };
        if (a == "-c" || a == "--config") config_path = next("--config");
        else if (a == "-r" || a == "--release") release = true;
        else if (a == "--debug") release = false;
        else if (a == "--no-frontend") skip_frontend = true;
        else if (a == "--target") target = next("--target");
        else if (a == "--wvc-root") wvc_root = next("--wvc-root");
        else if (a == "--dir") new_dir = next("--dir");
        else if (a.rfind("-", 0) == 0) log_warn("ignoring unknown flag " + a);
        else positional.push_back(a);
    }

    try {
        if (command == "new") {
            if (positional.empty()) { log_error("`wvc new` needs a project name"); return 2; }
            return cmd_new(positional[0], new_dir);
        }
        if (command == "doctor") return cmd_doctor();

        // Every other command needs a project config.
        fs::path config_file = config_path.empty() ? find_config(fs::current_path())
                                                   : fs::path(config_path);
        if (config_file.empty() || !fs::exists(config_file)) {
            log_error("no wvc.config.json found (run inside a project or pass --config)");
            return 2;
        }
        fs::path project = config_file.parent_path();
        Config cfg = Config::load(config_file);
        if (!wvc_root.empty()) cfg.framework_root = wvc_root;

        if (command == "build")   return cmd_build(cfg, project, release, skip_frontend);
        if (command == "run")     return cmd_run(cfg, project, release);
        if (command == "dev")     return cmd_dev(cfg, project);
        if (command == "package") return cmd_package(cfg, project, target);
        if (command == "info")    return cmd_info(cfg, project);

        log_error("unknown command: " + command);
        std::printf("%s", kUsage);
        return 2;
    } catch (const std::exception& e) {
        log_error(e.what());
        return 1;
    }
}
