# WebViewCpp

A lightweight, cross-platform desktop application framework in **Modern C++20**
that lets you build user interfaces with **HTML / CSS / JavaScript** — including
full React apps — and ship them as **standalone native binaries** for Windows,
macOS and Linux.

Unlike Electron it does **not** bundle Chromium or Node.js. It drives the
web engine that already ships with the operating system and talks to it through
a secure, typed IPC bridge:

| Platform | Rendering engine used        |
| -------- | ---------------------------- |
| Windows  | WebView2 (Edge / Chromium)   |
| macOS    | WKWebView (Cocoa)            |
| Linux    | WebKitGTK                    |

```
 Electron app  ≈ 150 MB   ·   WebViewCpp app  ≈ 0.5–3 MB
```

---

## Table of contents

- [How it works](#how-it-works)
- [Repository layout](#repository-layout)
- [Quick start](#quick-start)
- [Writing the C++ backend](#writing-the-c-backend)
- [Talking to React](#talking-to-react)
- [The `wvc` CLI](#the-wvc-cli)
- [Building the framework](#building-the-framework)
- [Packaging per platform](#packaging-per-platform)
- [Documentation](#documentation)

---

## How it works

```
 ┌──────────────────────────── your application ────────────────────────────┐
 │                                                                          │
 │   React / Vite  ──build──►  dist/  ──pack──►  app.wvcp  ──embed──►  .o    │
 │                                                                          │
 │   main.cpp  ──►  wvc::Window  ──►  IWebViewBackend (per-OS native view)  │
 │                       │                                                  │
 │                       └──►  wvc::IpcBridge  ◄──►  window.wvc (in JS)     │
 │                                                                          │
 └──────────────────────────────────────────────────────────────────────────┘
```

Four layers, each independently replaceable:

1. **Native WebView layer** (`src/platform/*`) — one implementation of
   `IWebViewBackend` per OS. It owns the native window, the webview, the
   `app://` custom-scheme handler, native dialogs and the clipboard. No
   `<windows.h>`, `<Cocoa/Cocoa.h>` or `<gtk/gtk.h>` ever leaks into core code.

2. **Asset layer** (`src/core/asset_store.*`) — packs the built frontend into a
   single compressed **WVCP** container and serves it from a custom
   `app://local/...` scheme. The container can live in a sidecar file or be
   compiled directly into the executable.

3. **IPC bridge** (`src/core/ipc.*`, `src/core/bootstrap_js.cpp`) — a two-way
   JSON protocol. JS calls native functions asynchronously and awaits Promises;
   C++ pushes events to the page. A ~2 KB bootstrap script installed at
   document-start exposes `window.wvc`.

4. **Toolchain** (`cli/`) — the `wvc` command line tool: scaffolds projects,
   runs the frontend build, packs and embeds assets, drives CMake and produces
   distributables.

## Repository layout

```
WebViewCpp/
├── CMakeLists.txt              # core library + subprojects
├── include/webviewcpp/         # public headers (the API you program against)
│   ├── window.hpp              #   wvc::Window  — the main entry point
│   ├── webview.hpp             #   IWebViewBackend + WindowOptions
│   ├── ipc.hpp                 #   wvc::IpcBridge
│   ├── asset_store.hpp         #   wvc::AssetStore + WVCP container
│   ├── embed.hpp               #   compile assets into the binary
│   ├── services.hpp            #   ready-made native services
│   ├── json.hpp                #   dependency-free JSON
│   └── ...
├── src/
│   ├── core/                   # portable bridge / assets / window logic
│   └── platform/
│       ├── webview_win32.cpp   #   WebView2
│       ├── webview_cocoa.mm    #   Cocoa / WKWebView
│       └── webview_gtk.cpp     #   WebKitGTK
├── cli/                        # the `wvc` tool (written in C++)
├── cmake/                      # EmbedAssets.cmake, package config
├── examples/react-counter/     # a complete React + C++ example
└── tests/                      # unit tests (JSON, assets, IPC)
```

## Quick start

Build and install the framework once:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix /usr/local      # installs libwebviewcpp.a + wvc
```

Then create an app:

```bash
wvc new MyApp
cd MyApp
npm install
wvc dev        # hot-reloading native shell (Vite + live reload)
wvc build      # standalone native binary in build/<plat>-release/
wvc package    # distributable for the host OS
```

That is the whole loop. `wvc dev` starts Vite and launches the native shell
pointed at the dev server; `wvc build` packs the production bundle into the
binary.

## Writing the C++ backend

```cpp
#include "webviewcpp/webviewcpp.hpp"
using namespace wvc;

int main() {
    Window window({.title = "My App", .width = 1100, .height = 720});

    // Built-in services: system.info, fs.*, dialog.*, clipboard.*, shell.*
    window.install_default_services();

    // Expose a native function to JavaScript.
    window.handle("app.greet", [](const Json& args) {
        return Json{{"message", "Hello, " + args["name"].string_or("world") + "!"}};
    });

    // Push an event to the UI when the document is ready.
    window.on_ready([&] { window.emit("native:ready", Json{{"ok", true}}); });

    window.load_app();          // app://local/index.html
    return window.run();
}
```

## Talking to React

The bridge is available on every page as `window.wvc` (typed in
`src/wvc.d.ts`):

```tsx
import { useEffect, useState } from 'react';

export default function App() {
  const [info, setInfo] = useState<any>(null);

  useEffect(() => {
    // JS -> C++ : asynchronous, returns a Promise
    window.wvc.invoke('system.info').then(setInfo);

    // C++ -> JS : subscribe to native events
    const off = window.wvc.on('native:ready', (e) => console.log('ready', e));
    return off;
  }, []);

  return <pre>{JSON.stringify(info, null, 2)}</pre>;
}
```

## The `wvc` CLI

```
wvc new <name>        Scaffold a React + C++ project
wvc dev               Vite dev server + hot-reloading native shell
wvc build             Build frontend, pack + embed assets, compile the binary
wvc run               Build then launch
wvc package           Produce a distributable (.AppImage / .app / .exe)
wvc doctor            Check that the local toolchain is complete
wvc info              Print the resolved configuration
```

Configuration lives in `wvc.config.json`:

```json
{
  "name": "My App",
  "identifier": "com.example.myapp",
  "version": "0.1.0",
  "binaryName": "my-app",
  "embed": true,
  "compress": true,
  "window":   { "width": 1100, "height": 720, "title": "My App", "devtools": true },
  "frontend": { "dist": "dist", "build": "npm run build", "devUrl": "http://localhost:5173" },
  "package":  { "category": "Utility", "icon": "" }
}
```

## Building the framework

Requirements:

| Platform | Toolchain |
| -------- | --------- |
| Linux    | CMake ≥ 3.24, GCC 11+/Clang 14+, `libwebkit2gtk-4.1-dev`, `libgtk-3-dev`, `zlib1g-dev` |
| macOS    | CMake ≥ 3.24, Xcode Command Line Tools (Cocoa + WebKit frameworks) |
| Windows  | CMake ≥ 3.24, MSVC 2022 or MinGW, the [WebView2 SDK](https://developer.microsoft.com/microsoft-edge/webview2/) |

```bash
# Linux
sudo apt install build-essential cmake libwebkit2gtk-4.1-dev libgtk-3-dev zlib1g-dev

# Configure. On Windows add the WebView2 SDK:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DWVC_WEBVIEW2_DIR="C:/webview2"     # Windows only
cmake --build build --parallel
ctest --test-dir build
```

Options: `-DWVC_BUILD_CLI=ON` (default), `-DWVC_BUILD_TESTS=ON` (default),
`-DWVC_BUILD_EXAMPLES=OFF`, `-DWVC_ENABLE_ZLIB=ON` (default).

## Packaging per platform

Full instructions — code signing, notarisation, AppImage recipes — are in
[`docs/building.md`](docs/building.md). In short:

```bash
wvc package --release
```

* **Linux** → `dist-package/<name>.AppImage` when `appimagetool` is installed,
  otherwise a portable `<name>-linux.tar.gz` plus a bare ELF binary.
* **macOS** → a `<Name>.app` bundle (and a `.dmg` via `hdiutil`).
* **Windows** → a zipped folder containing the `.exe` and `WebView2Loader.dll`.

The output binary only depends on the OS web runtime; there is no bundled
browser and no Node.js.

## Documentation

* [`docs/architecture.md`](docs/architecture.md) — layering, threading model, lifecycle.
* [`docs/ipc.md`](docs/ipc.md) — the protocol and the full channel reference.
* [`docs/building.md`](docs/building.md) — toolchain setup and packaging per OS.

## License

MIT.
