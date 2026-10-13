#include "project.hpp"

#include "webviewcpp/asset_store.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace wvc::cli {
namespace {

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

fs::path build_dir(const fs::path& project, const std::string& config) {
    return project / "build" / (host_platform() + "-" + config);
}

fs::path find_binary(const fs::path& dir, const std::string& name) {
    std::vector<fs::path> candidates = {
        dir / name,
        dir / (name + ".exe"),
        dir / "Release" / name,
        dir / "Release" / (name + ".exe"),
        dir / "Debug" / name,
    };
#if defined(__APPLE__)
    candidates.push_back(dir / (name + ".app") / "Contents" / "MacOS" / name);
    candidates.push_back(dir / "Release" / (name + ".app") / "Contents" / "MacOS" / name);
#endif
    for (const auto& candidate : candidates)
        if (fs::exists(candidate)) return candidate;
    return {};
}

bool ensure_frontend(Config& cfg, const fs::path& project, bool skip_frontend) {
    fs::path frontend_dir = project / cfg.frontend.root;
    if (skip_frontend) return true;

    if (!fs::exists(frontend_dir / "node_modules")) {
        if (!command_exists("npm")) {
            log_warn("npm not found; assuming dist/ is already built");
            return fs::exists(frontend_dir / cfg.frontend.dist);
        }
        log_step("Installing frontend dependencies: " + cfg.frontend.install);
        auto r = run("cd " + quote_arg(frontend_dir.string()) + " && " + cfg.frontend.install);
        if (r.code != 0) { log_error("dependency install failed"); return false; }
    }
    log_step("Building frontend: " + cfg.frontend.build);
    auto r = run("cd " + quote_arg(frontend_dir.string()) + " && " + cfg.frontend.build);
    if (r.code != 0) { log_error("frontend build failed"); return false; }
    return true;
}

bool pack_frontend(Config& cfg, const fs::path& project, const fs::path& out_pack,
                   const fs::path& out_cpp) {
    fs::path dist = project / cfg.frontend.root / cfg.frontend.dist;
    if (!fs::exists(dist)) {
        log_error("frontend output not found: " + dist.string());
        return false;
    }
    auto entries = AssetStore::read_directory(dist);
    if (entries.empty()) {
        log_error("frontend output is empty: " + dist.string());
        return false;
    }
    std::size_t raw = 0;
    for (auto& entry : entries) raw += entry.bytes.size();

    std::string error;
    if (!AssetStore::write_pack(entries, out_pack, cfg.compress, &error)) {
        log_error("failed to write bundle: " + error);
        return false;
    }
    std::size_t packed = fs::file_size(out_pack);
    char ratio[64];
    std::snprintf(ratio, sizeof(ratio), "%.0f%%", raw ? 100.0 * packed / raw : 0.0);
    log_info("packed " + std::to_string(entries.size()) + " assets, " +
             std::to_string(raw / 1024) + " KiB -> " + std::to_string(packed / 1024) +
             " KiB (" + ratio + ")");

    if (cfg.embed) {
        if (!generate_embed_source(out_pack, out_cpp, "app.wvcp")) {
            log_error("failed to generate " + out_cpp.string());
            return false;
        }
    }
    return true;
}

int configure_and_build(Config& cfg, const fs::path& project, bool release) {
    fs::path framework = resolve_framework_root(cfg, project);
    fs::path source = project / "native";
    fs::path bdir = build_dir(project, release ? "release" : "debug");

    log_step("Configuring native build");
    std::ostringstream cmd;
    cmd << "cmake -S " << quote_arg(source.string()) << " -B " << quote_arg(bdir.string())
        << " -DWVC_ROOT=" << quote_arg(framework.string())
        << " -DCMAKE_BUILD_TYPE=" << (release ? "Release" : "Debug");
    auto configure = run(cmd.str());
    if (configure.code != 0) { log_error("CMake configure failed"); return 1; }

    log_step("Compiling native executable");
    std::ostringstream build;
    build << "cmake --build " << quote_arg(bdir.string()) << " --config "
          << (release ? "Release" : "Debug") << " --parallel";
    auto compile = run(build.str());
    if (compile.code != 0) { log_error("build failed"); return 1; }

    auto binary = find_binary(bdir, cfg.binary_name);
    if (binary.empty()) { log_error("built binary not found under " + bdir.string()); return 1; }
    log_step("Built " + green(binary.string()));
    return 0;
}

// ---------------------------------------------------------------------------
// Scaffold templates
// ---------------------------------------------------------------------------
const char* kConfigJson = R"JSON({
  "name": "@NAME@",
  "identifier": "@ID@",
  "version": "0.1.0",
  "binaryName": "@BINARY@",
  "embed": true,
  "compress": true,
  "window": {
    "width": 1024,
    "height": 720,
    "minWidth": 480,
    "minHeight": 360,
    "title": "@TITLE@",
    "resizable": true,
    "frameless": false,
    "devtools": true
  },
  "frontend": {
    "root": ".",
    "dist": "dist",
    "install": "npm install",
    "build": "npm run build",
    "dev": "npm run dev",
    "devUrl": "http://localhost:5173"
  },
  "package": {
    "category": "Utility",
    "icon": "",
    "extraFiles": []
  }
}
)JSON";

const char* kPackageJson = R"JSON({
  "name": "@BINARY@",
  "private": true,
  "version": "0.1.0",
  "type": "module",
  "scripts": {
    "dev": "vite",
    "build": "tsc -b && vite build",
    "preview": "vite preview"
  },
  "dependencies": {
    "react": "^18.3.1",
    "react-dom": "^18.3.1"
  },
  "devDependencies": {
    "@types/react": "^18.3.12",
    "@types/react-dom": "^18.3.1",
    "@vitejs/plugin-react": "^4.3.4",
    "typescript": "^5.6.3",
    "vite": "^5.4.11"
  }
}
)JSON";

const char* kViteConfig = R"TS(import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// `base: './'` makes every asset URL relative so the bundle works from the
// app:// custom scheme without an HTTP server.
export default defineConfig({
  plugins: [react()],
  base: './',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    assetsDir: 'assets',
    target: 'es2020'
  }
});
)TS";

const char* kIndexHtml = R"HTML(<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>@TITLE@</title>
  </head>
  <body>
    <div id="root"></div>
    <script type="module" src="/src/main.tsx"></script>
  </body>
</html>
)HTML";

const char* kMainTsx = R"TSX(import React from 'react';
import { createRoot } from 'react-dom/client';
import App from './App';
import './styles.css';

createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>
);
)TSX";

const char* kAppTsx = R"TSX(import { useEffect, useState } from 'react';

// The native bridge is installed by WebViewCpp at document-start as `wvc`.
interface SystemInfo {
  platform: string;
  arch: string;
  cpus: number;
  home: string;
}

export default function App() {
  const [info, setInfo] = useState<SystemInfo | null>(null);
  const [count, setCount] = useState(0);
  const [greeting, setGreeting] = useState<string>('');

  useEffect(() => {
    window.wvc.invoke<SystemInfo>('system.info').then((data) => {
      setInfo(data);
      console.log('[demo] system.info ->', JSON.stringify(data));
    });
    window.wvc
      .invoke<{ message: string }>('app.greet', { name: 'React' })
      .then((r) => console.log('[demo] app.greet ->', r.message))
      .catch((e) => console.error('[demo] app.greet failed', e));
    // Native -> JS push: the C++ app.greet handler emits this event.
    const unsubscribe = window.wvc.on('native:greeted', (payload: { name: string; at: number }) => {
      console.log('[demo] native:greeted ->', payload.name, payload.at);
    });
    return unsubscribe;
  }, []);

  async function greet() {
    const result = await window.wvc.invoke<{ message: string }>('app.greet', { name: 'React' });
    setGreeting(result.message);
  }

  return (
    <main className="app">
      <h1>@TITLE@</h1>
      <p className="subtitle">React + C++ + WebViewCpp</p>

      <section className="card">
        <button onClick={() => setCount((c) => c + 1)}>Count: {count}</button>
        <button onClick={greet}>Call native app.greet</button>
        {greeting && <p className="result">{greeting}</p>}
      </section>

      {info && (
        <section className="card">
          <h2>System</h2>
          <ul>
            <li>platform: {info.platform}</li>
            <li>arch: {info.arch}</li>
            <li>cpus: {info.cpus}</li>
            <li>home: {info.home}</li>
          </ul>
        </section>
      )}
    </main>
  );
}
)TSX";

const char* kStyles = R"CSS(:root {
  color-scheme: dark;
  font-family: system-ui, -apple-system, "Segoe UI", sans-serif;
}
body { margin: 0; background: #0f1115; color: #e6e9ef; }
.app { padding: 2rem; display: grid; gap: 1.25rem; }
h1 { margin: 0; font-size: 1.9rem; }
.subtitle { margin: 0; color: #8b93a7; }
.card { background: #171a21; border: 1px solid #262b36; border-radius: 12px; padding: 1rem 1.25rem; }
button {
  background: #3b82f6; color: white; border: 0; border-radius: 8px;
  padding: 0.55rem 1rem; font-size: 0.95rem; cursor: pointer; margin-right: 0.5rem;
}
button:hover { background: #2563eb; }
.result { color: #4ade80; }
ul { margin: 0.5rem 0 0; padding-left: 1.1rem; color: #b6bdcc; }
)CSS";

const char* kTsConfig = R"JSON({
  "compilerOptions": {
    "target": "ES2020",
    "module": "ESNext",
    "moduleResolution": "bundler",
    "jsx": "react-jsx",
    "strict": true,
    "skipLibCheck": true,
    "noEmit": true,
    "types": []
  },
  "include": ["src"]
}
)JSON";

const char* kBridgeTypes = R"TS(// Global typing for the WebViewCpp bridge injected into every page.
export {};

declare global {
  interface WvcBridge {
    invoke<T = unknown>(channel: string, payload?: unknown): Promise<T>;
    call<T = unknown>(channel: string, payload?: unknown): Promise<T>;
    send(channel: string, payload?: unknown): void;
    on(channel: string, cb: (payload: any) => void): () => void;
    off(channel: string, cb: (payload: any) => void): void;
    once(channel: string, cb: (payload: any) => void): () => void;
    platform: string;
    version: string;
    isNative: boolean;
  }

  interface Window {
    wvc: WvcBridge;
    __wvc: WvcBridge;
  }
}
)TS";

const char* kMainCpp = R"CPP(// Application entry point generated by `wvc new`.
//
// This file is yours: add IPC handlers, menus, multiple windows, etc.
#include "webviewcpp/window.hpp"

#include <cstdlib>
#include <string>

using namespace wvc;

int main(int /*argc*/, char** /*argv*/) {
    WindowOptions options;
    options.title    = "@TITLE@";
    options.width    = @WIDTH@;
    options.height   = @HEIGHT@;
    options.min_width  = @MINW@;
    options.min_height = @MINH@;
    options.devtools = @DEVTOOLS@;

    Window window(options);

    // Ready-made native services (system info, file system, dialogs, clipboard).
    window.install_default_services();

    // Application-specific native function callable from React:
    //   const { message } = await window.wvc.invoke('app.greet', { name: 'X' });
    // It also pushes an event back to the page to demonstrate C++ -> JS.
    window.handle("app.greet", [&window](const Json& payload) {
        std::string name = payload["name"].string_or("world");
        window.emit("native:greeted", Json{{"name", name}, {"at", 1}});
        return Json{{"message", "Hello, " + name + " - from native C++!"}};
    });

    // In `wvc dev`, WVC_DEV_URL points at the Vite dev server for hot reload.
    if (const char* dev_url = std::getenv("WVC_DEV_URL")) {
        window.load_url(dev_url);
    } else {
        window.load_app();  // app://local/index.html from the embedded bundle
    }

    return window.run();
}
)CPP";

const char* kNativeCMake = R"CMAKE(cmake_minimum_required(VERSION 3.24)
project(@BINARY@ VERSION @VERSION@ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED WVC_ROOT)
  if(DEFINED ENV{WVC_ROOT})
    set(WVC_ROOT $ENV{WVC_ROOT})
  else()
    message(FATAL_ERROR "WVC_ROOT is not set. Pass -DWVC_ROOT=/path/to/WebViewCpp")
  endif()
endif()

find_package(WebViewCpp CONFIG QUIET)
if(NOT TARGET WebViewCpp::webviewcpp)
  add_subdirectory(${WVC_ROOT} ${CMAKE_BINARY_DIR}/webviewcpp EXCLUDE_FROM_ALL)
endif()

add_executable(${PROJECT_NAME}
  main.cpp
  wvc_assets.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE WebViewCpp::webviewcpp)

if(APPLE)
  set_target_properties(${PROJECT_NAME} PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "@NAME@"
    MACOSX_BUNDLE_GUI_IDENTIFIER "@ID@"
    MACOSX_BUNDLE_BUNDLE_VERSION "@VERSION@"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "@VERSION@")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../icons/app.icns")
    set_target_properties(${PROJECT_NAME} PROPERTIES
      MACOSX_BUNDLE_ICON_FILE app.icns)
    set_source_files_properties(../icons/app.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(${PROJECT_NAME} PRIVATE ../icons/app.icns)
  endif()
endif()

if(WIN32)
  set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION .)
)CMAKE";

const char* kGitignore = R"GIT(build/
dist/
node_modules/
*.wvcp
native/wvc_assets.cpp
)GIT";

const char* kReadme = R"MD(# @NAME@

A WebViewCpp desktop application (React + Modern C++).

    npm install
    wvc dev      # hot-reloading dev shell
    wvc build    # produce a standalone native binary
    wvc package  # produce a distributable for the host platform
)MD";

struct Template {
    const char* name;
    const char* body;
};

const Template kTemplates[] = {
    {"wvc.config.json", kConfigJson},
    {"package.json", kPackageJson},
    {"vite.config.ts", kViteConfig},
    {"index.html", kIndexHtml},
    {"src/main.tsx", kMainTsx},
    {"src/App.tsx", kAppTsx},
    {"src/styles.css", kStyles},
    {"src/wvc.d.ts", kBridgeTypes},
    {"tsconfig.json", kTsConfig},
    {"native/main.cpp", kMainCpp},
    {"native/CMakeLists.txt", kNativeCMake},
    {".gitignore", kGitignore},
    {"README.md", kReadme},
};

}  // namespace

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
int cmd_new(const std::string& name, const fs::path& dir) {
    fs::path target = dir.empty() ? fs::path(name) : dir;
    if (fs::exists(target) && !fs::is_directory(target)) {
        log_error(target.string() + " is not a directory");
        return 1;
    }
    fs::create_directories(target);

    Config cfg;
    cfg.name = name;
    cfg.binary_name = Config::sanitize(name);
    cfg.identifier = "com.example." + cfg.binary_name;
    cfg.window.title = name;

    log_step("Scaffolding " + bold(name) + " in " + target.string());
    for (const auto& entry : kTemplates) {
        std::string body = entry.body;
        body = replace_all(body, "@NAME@", cfg.name);
        body = replace_all(body, "@TITLE@", cfg.window.title);
        body = replace_all(body, "@ID@", cfg.identifier);
        body = replace_all(body, "@BINARY@", cfg.binary_name);
        body = replace_all(body, "@VERSION@", cfg.version);
        body = replace_all(body, "@WIDTH@", std::to_string(cfg.window.width));
        body = replace_all(body, "@HEIGHT@", std::to_string(cfg.window.height));
        body = replace_all(body, "@MINW@", std::to_string(cfg.window.min_width));
        body = replace_all(body, "@MINH@", std::to_string(cfg.window.min_height));
        body = replace_all(body, "@DEVTOOLS@", cfg.window.devtools ? "true" : "false");
        write_file(target / entry.name, body);
        log_info(entry.name);
    }
    // Placeholder embed TU so the project compiles before the first build.
    write_file(target / "native" / "wvc_assets.cpp",
               "// Placeholder. `wvc build` regenerates this with the packed frontend.\n"
               "#include \"webviewcpp/embed.hpp\"\n");
    fs::create_directories(target / "icons");

    std::printf("\n%s Created %s\n", green("done:").c_str(), target.string().c_str());
    std::printf("  cd %s && npm install && wvc dev\n", target.string().c_str());
    return 0;
}

fs::path resolve_framework_root(const Config& cfg, const fs::path& project) {
    if (!cfg.framework_root.empty()) {
        fs::path p = cfg.framework_root;
        if (fs::exists(p / "CMakeLists.txt")) return fs::absolute(p);
    }
    if (const char* env = std::getenv("WVC_ROOT")) {
        fs::path p = env;
        if (fs::exists(p / "CMakeLists.txt")) return fs::absolute(p);
    }
    // Installed layout: <prefix>/bin/wvc -> <prefix>/share/webviewcpp
#if defined(WVC_SOURCE_ROOT)
    if (fs::exists(fs::path(WVC_SOURCE_ROOT) / "CMakeLists.txt"))
        return fs::path(WVC_SOURCE_ROOT);
#endif
    fs::path next_to_project = project / ".." / "WebViewCpp";
    if (fs::exists(next_to_project / "CMakeLists.txt")) return fs::absolute(next_to_project);
    throw std::runtime_error(
        "cannot locate the WebViewCpp framework; pass --wvc-root or set WVC_ROOT");
}

bool generate_embed_source(const fs::path& pack_path, const fs::path& out_cpp,
                           const std::string& label) {
    std::ifstream in(pack_path, std::ios::binary);
    if (!in) return false;
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    std::ostringstream out;
    out << "// Generated by `wvc build`. Do not edit.\n"
        << "// Embeds " << bytes.size() << " bytes of packed frontend assets (" << label
        << ") directly into the executable.\n"
        << "#include \"webviewcpp/embed.hpp\"\n\n"
        << "namespace {\n"
        << "alignas(16) const unsigned char kAssetsBlob[] =\n    "
        << to_c_string_literal(bytes, 32) << ";\n"
        << "}  // namespace\n\n"
        << "WVC_EMBED_ASSETS(kAssetsBlob, sizeof(kAssetsBlob) - 1, \"" << label << "\");\n";
    return write_file(out_cpp, out.str());
}

int cmd_build(Config& cfg, const fs::path& project, bool release, bool skip_frontend) {
    if (!ensure_frontend(cfg, project, skip_frontend)) return 1;

    fs::path native_dir = project / "native";
    if (!fs::exists(native_dir)) {
        log_error("no native/ directory (run `wvc new` or add one)");
        return 1;
    }
    fs::path out_pack = native_dir / "build" / "app.wvcp";
    fs::path out_cpp = native_dir / "wvc_assets.cpp";
    if (!pack_frontend(cfg, project, out_pack, out_cpp)) return 1;
    return configure_and_build(cfg, project, release);
}

int cmd_run(Config& cfg, const fs::path& project, bool release) {
    int rc = cmd_build(cfg, project, release, /*skip_frontend=*/false);
    if (rc != 0) return rc;
    auto binary = find_binary(build_dir(project, release ? "release" : "debug"), cfg.binary_name);
    if (binary.empty()) { log_error("binary not found"); return 1; }
    log_step("Launching " + binary.string());
    return run(quote_arg(binary.string())).code;
}

int cmd_dev(Config& cfg, const fs::path& project) {
    if (!command_exists("npm")) {
        log_error("`wvc dev` needs npm on PATH");
        return 1;
    }
    fs::path frontend_dir = project / cfg.frontend.root;
    if (!fs::exists(frontend_dir / "node_modules")) {
        log_step("Installing frontend dependencies");
        run("cd " + quote_arg(frontend_dir.string()) + " && " + cfg.frontend.install);
    }

    log_step("Starting dev server (background)");
#if defined(_WIN32)
    std::string start = "start \"wvc-dev\" cmd /c \"cd /d " + frontend_dir.string() + " && " +
                        cfg.frontend.dev + "\"";
#else
    std::string start = "cd " + quote_arg(frontend_dir.string()) + " && " + cfg.frontend.dev +
                        " >/tmp/wvc-dev.log 2>&1 &";
#endif
    run(start);

    // Build the native shell (assets are irrelevant in dev; the URL wins).
    fs::path native_dir = project / "native";
    if (!fs::exists(native_dir / "wvc_assets.cpp")) {
        write_file(native_dir / "wvc_assets.cpp",
                   "#include \"webviewcpp/embed.hpp\"\n");
    }
    int rc = configure_and_build(cfg, project, /*release=*/false);
    if (rc != 0) return rc;

    auto binary = find_binary(build_dir(project, "debug"), cfg.binary_name);
    if (binary.empty()) { log_error("binary not found"); return 1; }

    log_step("Launching dev shell against " + cfg.frontend.dev_url);
    std::string command = "WVC_DEV_URL=" + quote_arg(cfg.frontend.dev_url) + " " +
                          quote_arg(binary.string());
    return run(command).code;
}

int cmd_package(Config& cfg, const fs::path& project, const std::string& target) {
    std::string platform = target.empty() ? host_platform() : target;
    if (platform != host_platform()) {
        log_warn("cross-compiling to '" + platform + "' from '" + host_platform() +
                 "' is not supported by this toolchain yet; packaging the host instead");
        platform = host_platform();
    }
    int rc = cmd_build(cfg, project, /*release=*/true, /*skip_frontend=*/false);
    if (rc != 0) return rc;

    fs::path bdir = build_dir(project, "release");
    auto binary = find_binary(bdir, cfg.binary_name);
    if (binary.empty()) { log_error("binary not found"); return 1; }

    fs::path out = project / "dist-package";
    fs::remove_all(out);
    fs::create_directories(out);

    if (platform == "linux") {
        fs::path appdir = out / (cfg.name + ".AppDir");
        fs::create_directories(appdir / "usr" / "bin");
        fs::copy_file(binary, appdir / "usr" / "bin" / cfg.binary_name,
                      fs::copy_options::overwrite_existing);

        std::string desktop =
            "[Desktop Entry]\nType=Application\nName=" + cfg.name + "\nExec=" +
            cfg.binary_name + "\nIcon=" + cfg.binary_name + "\nCategories=" +
            cfg.package.category + ";\nTerminal=false\n";
        write_file(appdir / (cfg.binary_name + ".desktop"), desktop);
        std::string apprun = "#!/bin/sh\nHERE=\"$(dirname \"$(readlink -f \"$0\")\")\"\n"
                             "exec \"$HERE/usr/bin/" + cfg.binary_name + "\" \"$@\"\n";
        write_file(appdir / "AppRun", apprun);
        fs::permissions(appdir / "AppRun",
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add);

        if (command_exists("appimagetool")) {
            log_step("Building AppImage");
            auto r = run("ARCH=" + std::string(sizeof(void*) == 8 ? "x86_64" : "i686") +
                         " appimagetool " + quote_arg(appdir.string()) + " " +
                         quote_arg((out / (cfg.name + ".AppImage")).string()));
            if (r.code == 0) {
                log_step("Packaged " + green((out / (cfg.name + ".AppImage")).string()));
                return 0;
            }
            log_warn("appimagetool failed; falling back to a portable tarball");
        } else {
            log_info("appimagetool not found; producing a portable tarball instead");
        }
        std::string tarball = (out / (cfg.binary_name + "-" + platform + ".tar.gz")).string();
        run("tar -C " + quote_arg(appdir.string()) + " -czf " + quote_arg(tarball) + " .");
        // Also ship a bare binary.
        fs::copy_file(binary, out / cfg.binary_name, fs::copy_options::overwrite_existing);
        log_step("Packaged " + green(tarball));
        return 0;
    }

    if (platform == "macos") {
        fs::path app = binary;
        // find_binary may return the inner executable; walk up to the .app bundle.
        if (app.string().find(".app/") != std::string::npos)
            app = fs::path(app.string().substr(0, app.string().find(".app/") + 4));
        fs::path dest = out / app.filename();
        if (fs::is_directory(app)) {
            fs::copy(app, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        } else {
            fs::create_directories(dest / "Contents" / "MacOS");
            fs::copy_file(binary, dest / "Contents" / "MacOS" / cfg.binary_name,
                          fs::copy_options::overwrite_existing);
        }
        if (command_exists("hdiutil")) {
            run("hdiutil create -volname " + quote_arg(cfg.name) + " -srcfolder " +
                quote_arg(dest.string()) + " -ov -format UDZO " +
                quote_arg((out / (cfg.name + ".dmg")).string()));
        }
        log_step("Packaged " + green((out / app.filename()).string()));
        return 0;
    }

    // windows
    fs::path win_out = out / cfg.name;
    fs::create_directories(win_out);
    fs::copy_file(binary, win_out / binary.filename(), fs::copy_options::overwrite_existing);
    for (const char* dll : {"WebView2Loader.dll"}) {
        fs::path candidate = binary.parent_path() / dll;
        if (fs::exists(candidate))
            fs::copy_file(candidate, win_out / dll, fs::copy_options::overwrite_existing);
    }
    std::string zip = (out / (cfg.binary_name + "-" + platform + ".zip")).string();
    run("powershell -NoProfile -Command \"Compress-Archive -Path '" + win_out.string() +
        "\\*' -DestinationPath '" + zip + "' -Force\"");
    log_step("Packaged " + green(zip));
    return 0;
}

int cmd_doctor() {
    struct Check { const char* name; bool ok; std::string detail; };
    std::vector<Check> checks;

    checks.push_back({"cmake", command_exists("cmake"),
                      run("cmake --version", true).output.substr(0, 40)});
    checks.push_back({"C++ compiler", command_exists("c++") || command_exists("clang++") || command_exists("g++"),
                      "needed to build the native shell"});

    std::string platform = host_platform();
    if (platform == "linux") {
        bool webkit = run("pkg-config --exists webkit2gtk-4.1", true).code == 0;
        bool gtk = run("pkg-config --exists gtk+-3.0", true).code == 0;
        checks.push_back({"webkit2gtk-4.1", webkit, "install libwebkit2gtk-4.1-dev"});
        checks.push_back({"gtk+-3.0", gtk, "install libgtk-3-dev"});
    } else if (platform == "windows") {
        checks.push_back({"WebView2 runtime", true, "evergreen runtime ships with Win10/11"});
    } else if (platform == "macos") {
        checks.push_back({"Xcode CLT", command_exists("xcrun"), "xcode-select --install"});
    }

    bool node = command_exists("node");
    bool npm = command_exists("npm");
    checks.push_back({"node", node, node ? run("node --version", true).output : "install Node.js"});
    checks.push_back({"npm", npm, npm ? run("npm --version", true).output : "install Node.js"});

    std::printf("\n%s\n", bold("WebViewCpp doctor").c_str());
    std::printf("platform: %s\n\n", platform.c_str());
    int failures = 0;
    for (auto& check : checks) {
        std::string detail = check.detail;
        for (char& c : detail) if (c == '\n' || c == '\r') c = ' ';
        while (!detail.empty() && detail.back() == ' ') detail.pop_back();
        if (detail.size() > 42) detail = detail.substr(0, 39) + "...";
        std::printf("  %s %-18s %s\n", check.ok ? green("ok  ").c_str() : red("miss").c_str(),
                    check.name, dim(detail).c_str());
        if (!check.ok) ++failures;
    }
    std::printf("\n%s\n", failures == 0 ? green("All checks passed.").c_str()
                                        : yellow("Some prerequisites are missing.").c_str());
    return failures == 0 ? 0 : 1;
}

int cmd_info(Config& cfg, const fs::path& project) {
    std::printf("\n%s\n", bold(cfg.name).c_str());
    std::printf("  identifier   %s\n", cfg.identifier.c_str());
    std::printf("  version      %s\n", cfg.version.c_str());
    std::printf("  binary       %s\n", cfg.binary_name.c_str());
    std::printf("  window       %dx%d \"%s\"\n", cfg.window.width, cfg.window.height,
                cfg.window.title.c_str());
    std::printf("  frontend     %s (dist: %s)\n", cfg.frontend.root.c_str(),
                cfg.frontend.dist.c_str());
    std::printf("  embed        %s (compress: %s)\n", cfg.embed ? "yes" : "no",
                cfg.compress ? "yes" : "no");
    try {
        std::printf("  framework    %s\n", resolve_framework_root(cfg, project).string().c_str());
    } catch (const std::exception& e) {
        std::printf("  framework    %s\n", red(e.what()).c_str());
    }
    return 0;
}

}  // namespace wvc::cli
