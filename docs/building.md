# Building and packaging

## 1. Toolchain per platform

### Linux

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake ninja-build pkg-config \
     libwebkit2gtk-4.1-dev libgtk-3-dev zlib1g-dev

# Fedora
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \
     webkit2gtk4.1-devel gtk3-devel zlib-devel

# Arch
sudo pacman -S base-devel cmake ninja pkgconf webkit2gtk-4.1 gtk3 zlib
```

Also install Node.js 18+ and, for `wvc package`, `appimagetool` (or let the CLI
fall back to a `.tar.gz`).

### macOS

```bash
xcode-select --install
brew install cmake ninja node
```

Cocoa and WebKit are system frameworks; nothing else is required.

### Windows

* Visual Studio 2022 (C++ desktop workload) or a recent MinGW-w64,
* CMake 3.24+,
* the **WebView2 SDK** (NuGet package `Microsoft.Web.WebView2`). Unzip it
  somewhere and point the build at it:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DWVC_WEBVIEW2_DIR="C:/deps/webview2" `
      -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

At runtime the app needs the **WebView2 Runtime**, which ships with Windows 11
and current Windows 10. `wvc doctor` reports whether it is present.

## 2. Build and install the framework

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DWVC_BUILD_CLI=ON -DWVC_BUILD_TESTS=ON -DWVC_BUILD_EXAMPLES=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /usr/local
```

Installed artefacts:

```
<prefix>/bin/wvc                         the CLI
<prefix>/lib/libwebviewcpp.a             the static core library
<prefix>/include/webviewcpp/             public headers
<prefix>/lib/cmake/WebViewCpp/           find_package() config + EmbedAssets.cmake
```

Consume it from another project either as a subproject …

```cmake
add_subdirectory(third_party/WebViewCpp)
target_link_libraries(myapp PRIVATE webviewcpp)
```

… or as an installed package:

```cmake
find_package(WebViewCpp REQUIRED)
include(${WebViewCpp_DIR}/EmbedAssets.cmake)
add_executable(myapp main.cpp)
wvc_embed_assets(myapp PACK "${CMAKE_SOURCE_DIR}/dist/app.wvcp")
target_link_libraries(myapp PRIVATE WebViewCpp::webviewcpp)
```

## 3. Create and build an application

```bash
wvc new MyApp
cd MyApp
npm install

# hot-reloading native shell (Vite dev server + live reload)
wvc dev

# production binary (frontend built, packed, embedded, compiled)
wvc build --release

# build then launch
wvc run
```

The build pipeline is:

```
npm install
  └─ npm run build           → dist/
       └─ pack               → native/build/app.wvcp   (zlib, per-entry)
            └─ embed         → native/wvc_assets.cpp   (byte array + registrar)
                 └─ cmake     → build/<plat>-<config>/<binary>
```

Useful flags: `--config/-c <path>`, `--release/-r`, `--debug`, `--no-frontend`
(skip npm and reuse the existing bundle), `--target <name>`, `--no-color`.

Configuration is read from `wvc.config.json`; see the README for the schema.

## 4. Packaging

```bash
wvc package --release
```

### Linux

With `appimagetool` on `PATH` the CLI produces a relocatable
`dist-package/<Name>.AppImage`. Otherwise it writes:

```
dist-package/<binary>                  the bare ELF
dist-package/<Name>.AppDir/            AppDir skeleton (icon + .desktop)
dist-package/<binary>-linux.tar.gz     portable tarball
```

Make the AppDir yourself if you want to control the desktop entry:

```bash
cat > usr/share/applications/myapp.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=My App
Exec=myapp
Icon=myapp
Categories=Utility;
EOF
appimagetool MyApp.AppDir MyApp-x86_64.AppImage
```

The AppImage expects `libwebkit2gtk-4.1` and `libgtk-3` on the target system
(they are part of essentially every desktop Linux install).

### macOS

```bash
wvc package --release
```

produces `<Name>.app`:

```
My App.app/Contents/
├── Info.plist            CFBundleIdentifier / Name / Version / NSHighResolutionCapable
├── MacOS/<binary>
└── Resources/AppIcon.icns
```

For distribution, sign and notarise:

```bash
codesign --deep --force --options runtime \
         --sign "Developer ID Application: You (TEAMID)" "dist-package/My App.app"
ditto -c -k --keepParent "dist-package/My App.app" MyApp.zip
xcrun notarytool submit MyApp.zip --keychain-profile AC --wait
xcrun stapler staple "dist-package/My App.app"
```

Add the `com.apple.security.network.client` entitlement only if the app needs
the network; `app://` is served internally and does not require it.

### Windows

```powershell
wvc package --release
```

writes `dist-package\<Name>-windows.zip` containing `<binary>.exe` and
`WebView2Loader.dll`. Distribute it as-is, or wrap it with an installer
(Inno Setup / WiX / MSIX). For MSIX packaging, declare the WebView2 runtime as a
dependency and consider the evergreen bootstrapper from the WebView2 SDK.

## 5. Continuous integration sketch

```yaml
jobs:
  build:
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-node@v4
        with: { node-version: 20 }
      - name: Linux deps
        if: runner.os == 'Linux'
        run: sudo apt-get update && sudo apt-get install -y libwebkit2gtk-4.1-dev libgtk-3-dev
      - name: Configure
        run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build --config Release --parallel
      - name: Test
        run: ctest --test-dir build --output-on-failure -C Release
```

## 6. Troubleshooting

| Symptom | Fix |
| ------- | --- |
| Configure fails: `webkit2gtk-4.1 not found` | Install the `-dev`/`-devel` package listed above. |
| Blank window on a headless/VM GPU | Set `WEBKIT_DISABLE_COMPOSITING_MODE=1` (Linux) or disable hardware acceleration. |
| `WebView2.h not found` (Windows) | Pass `-DWVC_WEBVIEW2_DIR=<sdk>`; the CLI also honours the `WVC_WEBVIEW2_DIR` env var. |
| Window never appears (macOS, agent) | The Cocoa backend needs a real GUI session; run it as a logged-in user, not over SSH. |
| Assets load but custom scheme is empty | Re-run `wvc build`; the `.wvcp` and `wvc_assets.cpp` must be regenerated after frontend changes. |
| `no handler registered for channel` | Register the channel with `window.handle(...)` before `run()`. |
