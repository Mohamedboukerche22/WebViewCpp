# Architecture

WebViewCpp is organised as four layers with a single, narrow seam between the
portable core and each operating system. The core never includes a platform
header; the platform code never includes application code.

```
            ┌───────────────────────────────────────────────┐
   app      │  main.cpp  ·  your IPC handlers  ·  React UI   │
            └───────────────┬───────────────────────┬───────┘
                            │                       │
            ┌───────────────▼───────────┐   ┌───────▼───────────────┐
   core     │  wvc::Window              │   │  window.wvc (JS)      │
            │   ├─ IpcBridge            │◄─►│   invoke/on/emit      │
            │   ├─ AssetStore           │   └───────────────────────┘
            │   └─ services (fs, ...)   │
            └───────────────┬───────────┘
                            │  IWebViewBackend (abstract)
            ┌───────────────▼───────────────────────────────┐
 platform   │ Win32/WebView2 │ Cocoa/WKWebView │ GTK/WebKitGTK │
            └───────────────────────────────────────────────┘
```

## 1. The platform seam: `IWebViewBackend`

`include/webviewcpp/webview.hpp` declares the complete contract a platform must
implement — lifecycle, navigation, geometry, dialogs, clipboard — plus three
callbacks the core installs (`on_message`, `on_ready`, `on_close`) and two
services the platform receives (`IpcBridge*`, `AssetStore*`).

The only platform-dependent symbols the core references are the four free
functions a backend must define:

```cpp
std::unique_ptr<IWebViewBackend> create_webview_backend(WindowOptions, IpcBridge&, AssetStore*);
int  platform_run_loop();
void platform_quit();
const char* platform_name();   // "windows" | "macos" | "linux"
```

`wvc::Window` never names a concrete backend. Swapping GTK for another Linux
engine (e.g. CEF or WebKitGTK 6/GTK4) means adding one file under
`src/platform/` and one line in `CMakeLists.txt`.

### Why one interface, not one class per concern

Windows, macOS and Linux all fuse "window" and "web view" into a single native
object with a single event loop. Splitting them into separate C++ abstractions
would force the core to mirror the differences it is trying to hide. Instead one
backend object owns both and exposes a flat, uniform surface.

## 2. Assets: the WVCP container and the `app://` scheme

The frontend is packed into a single **WVCP** file (see
`AssetStore::pack`/`from_memory`). The format is little-endian and versioned, so
a bundle built on x86 loads on ARM and vice versa:

```
'W','V','C','P' | u16 version | u16 flags | u32 count
per entry: u16 name | name | u16 mime | mime | u32 raw_len | u32 stored_len | data
```

Flags bit 0 marks the entries as zlib/deflate compressed. `AssetStore::pack`
compresses each entry and *only* sets the flag when compression actually shrinks
the payload, so tiny assets are never inflated.

At runtime every platform registers a custom scheme (`app://`) and routes it to
`AssetStore::resolve_url`, which normalises the path, strips query/fragment,
percent-decodes, and applies a **SPA fallback** — a request for an
extension-less route that misses the store is answered with `index.html`, so
client-side routing (`/settings/profile`) works exactly like on a web server.

Three embedding strategies, all interchangeable:

| Strategy | Where | When |
| -------- | ----- | ---- |
| `wvc build` byte-array TU | `native/wvc_assets.cpp` | default, fully portable |
| `wvc_embed_assets()` in CMake | `cmake/EmbedAssets.cmake` | pure-CMake projects |
| Sidecar `app.wvcp` | next to the binary | dynamic / hot-swappable assets |

All three funnel through `wvc::set_embedded_assets()`, a registration hook that
avoids weak-symbol differences between ELF, Mach-O and PE.

## 3. Threading model

* **UI thread** — every native object (window, web view, dialogs, clipboard) is
  created and mutated on the platform's main thread. `platform_run_loop()` runs
  there.
* **IPC handlers** — `IpcBridge::receive()` is called by the platform on the UI
  thread, so handlers run on the UI thread too. Keep them short; offload heavy
  work to a worker and reply later with `emit()`.
* **Background threads** — `IpcBridge::emit()` and every backend mutation are
  marshalled onto the UI thread automatically:
  * GTK: `g_idle_add`
  * Cocoa: `dispatch_async(dispatch_get_main_queue(), ...)`
  * Win32: `PostMessage(WM_APP + ...)`

`Window::open()` must be called from the UI thread (an IPC handler already is),
which is what makes "open another window" a one-liner.

## 4. Lifecycle and the event loop

```
Window ctor ──► run() ──► ensure_backend()
                             ├─ create AssetStore (embedded / sidecar)
                             ├─ create_webview_backend()
                             ├─ wire_callbacks()          // on_message/on_ready/on_close
                             ├─ backend->initialize()      // native window + webview shown
                             └─ navigate(initial_url)      // app://local/index.html
                          platform_run_loop()              // blocks
                          backend.reset()
```

Each backend tracks the number of live windows and calls `platform_quit()` when
the last one is destroyed. The loop is process-wide and idempotent: the first
`run()` blocks, later windows added via `open()` simply appear.

The IPC handshake is asynchronous and race-free: the injected bootstrap script
posts `{kind:"ready"}` from the page once it has installed `window.wvc`. Until
that arrives, `IpcBridge::emit()` queues events; `mark_ready()` flushes them in
order. No event is ever delivered before the page can receive it.

## 5. Memory and ownership

* `Window` owns its `AssetStore` and its backend. Both use `unique_ptr`.
* The backend borrows `IpcBridge*` and `AssetStore*`; `Window` (or the caller)
  guarantees they outlive the backend.
* `Window` is non-copyable. Additional windows are held in
  `std::vector<std::unique_ptr<Window>>` by the application.
* The bootstrap JS owns the pending-Promise map on the page; native owns the
  handler registry. Neither side leaks across a navigation: a fresh document
  re-runs the bootstrap and starts from `id = 1` again.

## 6. Error handling

* Dialog failures surface to JS as rejected Promises (`Error` with the native
  message), never as exceptions crossing the bridge.
* Unhandled channels reject with `no handler registered for channel '<x>'` and
  log a warning.
* A malformed message from the page is logged and dropped; it can never crash
  the host.
* Backend initialisation failures (e.g. WebView2 runtime missing) throw from
  `initialize()` and are reported through `wvc::Log`.

## 7. Logging

`wvc::Log` is a tiny sink. By default it prints `[wvc][level] message` to
stderr. A host can redirect it to a file, a UI panel, or the `log` IPC channel:

```cpp
Log::set_sink([](LogLevel level, std::string_view msg) { /* ... */ });
Log::set_min_level(LogLevel::Debug);
```
