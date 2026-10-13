// macOS backend — Cocoa + WKWebView.
//
// Compiled with ARC (-fobjc-arc). The custom `app://` scheme is served through
// a WKURLSchemeHandler; page -> native messages arrive on the WKScriptMessage
// channel named "wvc".
#include "webviewcpp/asset_store.hpp"
#include "webviewcpp/embed.hpp"
#include "webviewcpp/ipc.hpp"
#include "webviewcpp/log.hpp"
#include "webviewcpp/version.hpp"
#include "webviewcpp/webview.hpp"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <string>
#include <vector>

using namespace wvc;

namespace wvc {
class CocoaBackend;  // forward declaration for the Objective-C delegates
}

namespace {
int g_open_windows = 0;
bool g_loop_running = false;
}  // namespace

// ---------------------------------------------------------------------------
// Objective-C collaborators
// ---------------------------------------------------------------------------
@interface WvcWindowDelegate : NSObject <NSWindowDelegate, WKNavigationDelegate,
                                         WKScriptMessageHandler, WKURLSchemeHandler>
@property(nonatomic, assign) wvc::CocoaBackend* backend;
@end

@interface WvcAppDelegate : NSObject <NSApplicationDelegate>
@end

// ---------------------------------------------------------------------------
// C++ backend
// ---------------------------------------------------------------------------
namespace wvc {

class CocoaBackend final : public IWebViewBackend {
public:
    explicit CocoaBackend(WindowOptions options) : options_(std::move(options)) {}
    ~CocoaBackend() override = default;

    bool initialize(const WindowOptions& options) override {
        @autoreleasepool {
            options_ = options;
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            static dispatch_once_t once;
            dispatch_once(&once, ^{
                WvcAppDelegate* delegate = [[WvcAppDelegate alloc] init];
                [NSApp setDelegate:delegate];
                install_menu();
            });

            NSRect frame = NSMakeRect(0, 0, options_.width, options_.height);
            NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                      NSWindowStyleMaskMiniaturizable;
            if (options_.resizable) style |= NSWindowStyleMaskResizable;
            if (options_.frameless) style = NSWindowStyleMaskBorderless;

            window_ = [[NSWindow alloc] initWithContentRect:frame
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
            window_.title = [NSString stringWithUTF8String:options_.title.c_str()];
            window_.releasedWhenClosed = NO;
            if (options_.min_width || options_.min_height)
                window_.minSize = NSMakeSize(options_.min_width, options_.min_height);
            if (options_.max_width || options_.max_height)
                window_.maxSize = NSMakeSize(options_.max_width ? options_.max_width : FLT_MAX,
                                             options_.max_height ? options_.max_height : FLT_MAX);
            window_.level = options_.always_on_top ? NSFloatingWindowLevel : NSNormalWindowLevel;
            if (options_.transparent) {
                window_.opaque = NO;
                window_.backgroundColor = [NSColor clearColor];
            }

            delegate_ = [[WvcWindowDelegate alloc] init];
            delegate_.backend = this;

            WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
            config.preferences.javaScriptCanOpenWindowsAutomatically = YES;
            [config setURLSchemeHandler:delegate_ forURLScheme:@"app"];

            WKUserContentController* ucc = config.userContentController;

            // Bootstrap + environment injected at document start.
            std::string preamble =
                "window.__WVC_ENV__={platform:\"macos\",version:\"" WVC_VERSION_STRING "\"};";
            NSString* source = [NSString
                stringWithUTF8String:(preamble + IpcBridge::bootstrap_script()).c_str()];
            WKUserScript* script = [[WKUserScript alloc]
                  initWithSource:source
                   injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                forMainFrameOnly:NO];
            [ucc addUserScript:script];
            [ucc addScriptMessageHandler:delegate_ name:@"wvc"];

            webview_ = [[WKWebView alloc] initWithFrame:frame configuration:config];
            webview_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            webview_.navigationDelegate = delegate_;
            window_.contentView = webview_;
            window_.delegate = delegate_;

#if defined(WVC_DEBUG)
            if (@available(macOS 13.3, *)) {
                if (options_.devtools) [webview_ setInspectable:YES];
            }
#endif

            if (options_.center) [window_ center];
            [window_ makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            if (options_.fullscreen) [window_ toggleFullScreen:nil];

            running_ = true;
            ++g_open_windows;
            return true;
        }
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
        pending_url_.clear();
        pending_is_html_ = true;
        flush_pending();
    }
    void reload() override { [webview_ reload]; }
    void eval(std::string_view javascript) override {
        if (!webview_) { pending_scripts_.emplace_back(javascript); return; }
        NSString* js = [NSString stringWithUTF8String:std::string(javascript).c_str()];
        [webview_ evaluateJavaScript:js completionHandler:nil];
    }

    void set_title(std::string_view title) override {
        window_.title = [NSString stringWithUTF8String:std::string(title).c_str()];
    }
    void set_size(int w, int h) override {
        [window_ setContentSize:NSMakeSize(w, h)];
    }
    void get_size(int& w, int& h) const override {
        NSRect frame = [window_ contentRectForFrameRect:window_.frame];
        w = static_cast<int>(frame.size.width);
        h = static_cast<int>(frame.size.height);
    }
    void set_min_size(int w, int h) override { window_.minSize = NSMakeSize(w, h); }
    void set_max_size(int w, int h) override { window_.maxSize = NSMakeSize(w, h); }
    void set_position(int x, int y) override {
        // Cocoa origin is bottom-left; flip from the top-left screen origin.
        NSScreen* screen = [NSScreen mainScreen];
        CGFloat flipped = screen.frame.size.height - y - options_.height;
        [window_ setFrameOrigin:NSMakePoint(x, flipped)];
    }
    void center() override { [window_ center]; }
    void set_resizable(bool resizable) override {
        if (resizable) window_.styleMask |= NSWindowStyleMaskResizable;
        else window_.styleMask &= ~NSWindowStyleMaskResizable;
    }
    void set_fullscreen(bool fullscreen) override {
        bool is_full = (window_.styleMask & NSWindowStyleMaskFullScreen) != 0;
        if (fullscreen != is_full) [window_ toggleFullScreen:nil];
    }
    void set_always_on_top(bool on_top) override {
        window_.level = on_top ? NSFloatingWindowLevel : NSNormalWindowLevel;
    }
    void set_zoom(double factor) override { webview_.pageZoom = factor; }
    void set_cursor(CursorShape shape) override {
        NSCursor* cursor = [NSCursor arrowCursor];
        switch (shape) {
            case CursorShape::Pointer: cursor = [NSCursor pointingHandCursor]; break;
            case CursorShape::Text: cursor = [NSCursor IBeamCursor]; break;
            case CursorShape::Crosshair: cursor = [NSCursor crosshairCursor]; break;
            case CursorShape::Move: cursor = [NSCursor openHandCursor]; break;
            case CursorShape::Wait: cursor = [NSCursor operationNotAllowedCursor]; break;
            case CursorShape::NotAllowed: cursor = [NSCursor operationNotAllowedCursor]; break;
            default: break;
        }
        [cursor set];
    }
    void minimize() override { [window_ miniaturize:nil]; }
    void maximize() override { [window_ zoom:nil]; }
    void restore() override { [window_ deminiaturize:nil]; }
    void hide() override { [window_ orderOut:nil]; }
    void show() override { [window_ makeKeyAndOrderFront:nil]; }
    void set_devtools(bool enabled) override {
#if defined(WVC_DEBUG)
        if (@available(macOS 13.3, *)) [webview_ setInspectable:enabled ? YES : NO];
#else
        (void)enabled;
#endif
    }
    void request_close() override { [window_ performClose:nil]; }
    bool is_running() const override { return running_; }
    void dispatch_to_ui(std::function<void()> task) override {
        auto* fn = new std::function<void()>(std::move(task));
        dispatch_async(dispatch_get_main_queue(), ^{
            (*fn)();
            delete fn;
        });
    }

    // --- dialogs / clipboard --------------------------------------------
    std::vector<std::string> open_file_dialog(const OpenFileDialogOptions& options) override {
        std::vector<std::string> result;
        @autoreleasepool {
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            panel.title = [NSString stringWithUTF8String:options.title.c_str()];
            panel.canChooseFiles = !options.choose_directory;
            panel.canChooseDirectories = options.choose_directory;
            panel.allowsMultipleSelection = options.allow_multiple;
            apply_types(panel, options.filters);
            if ([panel runModal] == NSModalResponseOK) {
                for (NSURL* url : panel.URLs)
                    result.emplace_back(url.path.UTF8String);
            }
        }
        return result;
    }

    std::string save_file_dialog(const SaveFileDialogOptions& options) override {
        @autoreleasepool {
            NSSavePanel* panel = [NSSavePanel savePanel];
            panel.title = [NSString stringWithUTF8String:options.title.c_str()];
            if (!options.default_name.empty())
                panel.nameFieldStringValue =
                    [NSString stringWithUTF8String:options.default_name.c_str()];
            apply_types(panel, options.filters);
            if ([panel runModal] == NSModalResponseOK)
                return panel.URL.path.UTF8String;
        }
        return {};
    }

    int message_box(const MessageBoxOptions& options) override {
        @autoreleasepool {
            NSAlert* alert = [[NSAlert alloc] init];
            alert.messageText = [NSString stringWithUTF8String:options.title.c_str()];
            alert.informativeText = [NSString stringWithUTF8String:options.message.c_str()];
            switch (options.type) {
                case MessageBoxType::Warning: alert.alertStyle = NSAlertStyleWarning; break;
                case MessageBoxType::Error: alert.alertStyle = NSAlertStyleCritical; break;
                default: alert.alertStyle = NSAlertStyleInformational;
            }
            switch (options.buttons) {
                case MessageBoxButtons::Ok:
                    [alert addButtonWithTitle:@"OK"];
                    break;
                case MessageBoxButtons::OkCancel:
                    [alert addButtonWithTitle:@"OK"];
                    [alert addButtonWithTitle:@"Cancel"];
                    break;
                case MessageBoxButtons::YesNo:
                case MessageBoxButtons::YesNoCancel:
                    [alert addButtonWithTitle:@"Yes"];
                    [alert addButtonWithTitle:@"No"];
                    if (options.buttons == MessageBoxButtons::YesNoCancel)
                        [alert addButtonWithTitle:@"Cancel"];
                    break;
            }
            NSModalResponse response = [alert runModal];
            return (response == NSAlertFirstButtonReturn) ? 0 : 1;
        }
    }

    std::string clipboard_read_text() override {
        NSString* text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        return text ? text.UTF8String : "";
    }
    void clipboard_write_text(std::string text) override {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        [pasteboard setString:[NSString stringWithUTF8String:text.c_str()]
                      forType:NSPasteboardTypeString];
    }

    // --- used by the Objective-C delegate -------------------------------
    AssetStore* asset_store() { return assets; }
    IpcBridge* bridge() { return ipc; }

    void handle_script_message(WKScriptMessage* message) {
        if (!ipc) return;
        id body = message.body;
        if (![NSJSONSerialization isValidJSONObject:body]) {
            if ([body isKindOfClass:[NSString class]]) {
                ipc->receive([(NSString*)body UTF8String]);
            }
            return;
        }
        NSData* data = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
        NSString* json = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        ipc->receive(json.UTF8String);
    }

    void handle_navigation_finished() { if (on_ready) on_ready(); }
    void handle_window_close() {
        running_ = false;
        if (on_close) on_close();
        if (--g_open_windows <= 0) platform_quit();
    }

private:
    void flush_pending() {
        if (!webview_) return;
        for (auto& pending : pending_scripts_) eval(pending);
        pending_scripts_.clear();
        if (pending_is_html_) {
            [webview_ loadHTMLString:[NSString stringWithUTF8String:pending_html_.c_str()]
                             baseURL:[NSURL URLWithString:@"app://local/"]];
        } else if (!pending_url_.empty()) {
            [webview_ loadRequest:[NSURLRequest
                                      requestWithURL:[NSURL URLWithString:
                                                               [NSString stringWithUTF8String:
                                                                             pending_url_.c_str()]]]];
        }
    }

    static void apply_types(NSSavePanel* panel, const std::vector<FileFilter>& filters) {
        NSMutableArray<NSString*>* types = [NSMutableArray array];
        for (const auto& filter : filters)
            for (const auto& glob : filter.globs) {
                std::string ext = glob;
                if (ext.rfind("*.", 0) == 0) ext = ext.substr(2);
                [types addObject:[NSString stringWithUTF8String:ext.c_str()]];
            }
        if (types.count > 0) {
            if ([panel isKindOfClass:[NSOpenPanel class]])
                ((NSOpenPanel*)panel).allowedFileTypes = types;
            else
                panel.allowedFileTypes = types;
        }
    }

    static void install_menu() {
        NSMenu* menubar = [[NSMenu alloc] init];
        NSMenuItem* app_item = [[NSMenuItem alloc] init];
        [menubar addItem:app_item];
        [NSApp setMainMenu:menubar];
        NSMenu* app_menu = [[NSMenu alloc] init];
        NSString* quit = [NSString stringWithFormat:@"Quit %@", NSProcessInfo.processInfo.processName];
        [app_menu addItemWithTitle:quit action:@selector(terminate:) keyEquivalent:@"q"];
        app_item.submenu = app_menu;
    }

    WindowOptions options_;
    NSWindow* window_ = nil;
    WKWebView* webview_ = nil;
    WvcWindowDelegate* delegate_ = nil;
    bool running_ = false;
    bool pending_is_html_ = false;
    std::string pending_url_;
    std::string pending_html_;
    std::vector<std::string> pending_scripts_;
};

}  // namespace wvc

// ---------------------------------------------------------------------------
// Objective-C delegate / scheme handler implementation
// ---------------------------------------------------------------------------
@implementation WvcWindowDelegate

- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message {
    (void)controller;
    if (self.backend) self.backend->handle_script_message(message);
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    (void)webView;
    (void)navigation;
    if (self.backend) self.backend->handle_navigation_finished();
}

- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    if (self.backend) self.backend->handle_window_close();
}

- (void)webView:(WKWebView*)webView
    startURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask {
    (void)webView;
    NSURL* url = urlSchemeTask.request.URL;
    if (!self.backend) {
        [urlSchemeTask didFailWithError:[NSError errorWithDomain:@"wvc" code:500 userInfo:nil]];
        return;
    }
    AssetStore* store = self.backend->asset_store();
    auto asset = store ? store->resolve_url(url.absoluteString.UTF8String) : std::nullopt;
    if (!asset) {
        [urlSchemeTask didFailWithError:[NSError errorWithDomain:@"wvc" code:404 userInfo:nil]];
        return;
    }
    NSData* data = [NSData dataWithBytes:asset->bytes.data() length:asset->bytes.size()];
    NSDictionary* headers = @{
        @"Content-Type" : [NSString stringWithUTF8String:asset->mime.c_str()],
        @"Access-Control-Allow-Origin" : @"*"
    };
    NSHTTPURLResponse* response =
        [[NSHTTPURLResponse alloc] initWithURL:url
                                    statusCode:200
                                   HTTPVersion:@"HTTP/1.1"
                                  headerFields:headers];
    [urlSchemeTask didReceiveResponse:response];
    [urlSchemeTask didReceiveData:data];
    [urlSchemeTask didFinish];
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask {
    (void)webView;
    (void)urlSchemeTask;
}

@end

@implementation WvcAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}
@end

// ---------------------------------------------------------------------------
// Platform entry points
// ---------------------------------------------------------------------------
namespace wvc {

std::unique_ptr<IWebViewBackend> create_webview_backend(WindowOptions options, IpcBridge& ipc,
                                                        AssetStore* assets) {
    auto backend = std::make_unique<CocoaBackend>(std::move(options));
    backend->ipc = &ipc;
    backend->assets = assets;
    return backend;
}

int platform_run_loop() {
    if (g_loop_running) return 0;
    g_loop_running = true;
    @autoreleasepool {
        [NSApp run];
    }
    g_loop_running = false;
    return 0;
}

void platform_quit() { dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; }); }
const char* platform_name() { return "macos"; }
const char* platform_family() { return "cocoa"; }

}  // namespace wvc
