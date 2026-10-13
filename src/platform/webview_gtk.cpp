// Linux backend — GTK3 + WebKitGTK 4.1 (pkg-config: webkit2gtk-4.1, gtk+-3.0).
//
// Implements the entire IWebViewBackend contract on top of WebKitGTK:
//   * a GtkWindow + WebKitWebView pair,
//   * an `app://` URI scheme handler backed by the AssetStore,
//   * the `wvc` script-message channel used by the IPC bridge,
//   * native file/message dialogs and the system clipboard.
#include "webviewcpp/asset_store.hpp"
#include "webviewcpp/embed.hpp"
#include "webviewcpp/ipc.hpp"
#include "webviewcpp/log.hpp"
#include "webviewcpp/version.hpp"
#include "webviewcpp/webview.hpp"

#include <gtk/gtk.h>
#include <jsc/jsc.h>
#include <webkit2/webkit2.h>

#include <cstring>
#include <functional>
#include <string>

namespace wvc {
namespace {

int g_open_windows = 0;
bool g_gtk_initialized = false;
bool g_loop_running = false;

struct IdleTask {
    std::function<void()> fn;
};

gboolean idle_trampoline(gpointer data) {
    auto* task = static_cast<IdleTask*>(data);
    task->fn();
    delete task;
    return G_SOURCE_REMOVE;
}

}  // namespace

class GtkBackend final : public IWebViewBackend {
public:
    explicit GtkBackend(WindowOptions options) : options_(std::move(options)) {}
    ~GtkBackend() override {
        if (window_) gtk_widget_destroy(window_);
    }

    bool initialize(const WindowOptions& options) override {
        options_ = options;
        if (!g_gtk_initialized) {
            gtk_init(nullptr, nullptr);
            g_gtk_initialized = true;
        }

        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window_), options_.title.c_str());
        gtk_window_set_default_size(GTK_WINDOW(window_), options_.width, options_.height);
        gtk_window_set_resizable(GTK_WINDOW(window_), options_.resizable);
        if (options_.center) gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);
        if (options_.always_on_top) gtk_window_set_keep_above(GTK_WINDOW(window_), TRUE);
        if (options_.frameless) gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);

        if (options_.min_width > 0 || options_.min_height > 0) {
            GdkGeometry geometry{};
            geometry.min_width = options_.min_width;
            geometry.min_height = options_.min_height;
            int hints = GDK_HINT_MIN_SIZE;
            if (options_.max_width > 0 && options_.max_height > 0) {
                geometry.max_width = options_.max_width;
                geometry.max_height = options_.max_height;
                hints |= GDK_HINT_MAX_SIZE;
            }
            gtk_window_set_geometry_hints(GTK_WINDOW(window_), nullptr, &geometry,
                                          static_cast<GdkWindowHints>(hints));
        }
        if (options_.x >= 0 && options_.y >= 0)
            gtk_window_move(GTK_WINDOW(window_), options_.x, options_.y);
        if (!options_.icon.empty())
            gtk_window_set_icon_from_file(GTK_WINDOW(window_), options_.icon.c_str(), nullptr);
        if (options_.transparent)
            gtk_widget_set_app_paintable(window_, TRUE);

        webview_ = webkit_web_view_new();
        ucm_ = webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(webview_));

        // Register the page -> native channel ("wvc"). WebKit requires the
        // signal to be connected *before* the handler name is registered.
        g_signal_connect(ucm_, "script-message-received::wvc",
                         G_CALLBACK(&GtkBackend::on_script_message), this);
        if (!webkit_user_content_manager_register_script_message_handler(ucm_, "wvc"))
            WVC_WARN("WebKitGTK: could not register the 'wvc' script message handler");

        // Inject the bootstrap + environment at document-start in every frame.
        std::string preamble = "window.__WVC_ENV__={platform:\"linux\",version:\"" +
                               std::string(WVC_VERSION_STRING) + "\"};";
        add_user_script(preamble + IpcBridge::bootstrap_script());

        // The custom scheme must be registered on the web context.
        WebKitWebContext* context = webkit_web_view_get_context(WEBKIT_WEB_VIEW(webview_));
        webkit_web_context_register_uri_scheme(
            context, "app",
            +[](WebKitURISchemeRequest* request, gpointer user_data) {
                static_cast<GtkBackend*>(user_data)->serve_scheme(request);
            },
            this, nullptr);

        configure_settings();

        gtk_container_add(GTK_CONTAINER(window_), webview_);

        g_signal_connect(window_, "delete-event", G_CALLBACK(&GtkBackend::on_delete), this);
        g_signal_connect(window_, "destroy", G_CALLBACK(&GtkBackend::on_destroy), this);
        g_signal_connect(webview_, "load-changed", G_CALLBACK(&GtkBackend::on_load_changed), this);
        g_signal_connect(window_, "size-allocate", G_CALLBACK(&GtkBackend::on_size_allocate), this);

        gtk_widget_show_all(window_);
        if (options_.fullscreen) gtk_window_fullscreen(GTK_WINDOW(window_));
        running_ = true;
        ++g_open_windows;
        return true;
    }

    void run() override { platform_run_loop(); }

    void navigate(const std::string& url) override {
        if (webview_) webkit_web_view_load_uri(WEBKIT_WEB_VIEW(webview_), url.c_str());
    }

    void load_asset(const std::string& path) override { navigate("app://local/" + path); }

    void set_html(const std::string& html) override {
        if (webview_)
            webkit_web_view_load_html(WEBKIT_WEB_VIEW(webview_), html.c_str(),
                                      "app://local/");
    }

    void reload() override { if (webview_) webkit_web_view_reload(WEBKIT_WEB_VIEW(webview_)); }

    void eval(std::string_view javascript) override {
        if (!webview_) return;
        webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(webview_), javascript.data(),
                                            static_cast<gssize>(javascript.size()),
                                            nullptr, nullptr, nullptr, nullptr, nullptr);
    }

    void set_title(std::string_view title) override {
        if (window_) gtk_window_set_title(GTK_WINDOW(window_), std::string(title).c_str());
    }
    void set_size(int w, int h) override { if (window_) gtk_window_resize(GTK_WINDOW(window_), w, h); }
    void get_size(int& w, int& h) const override {
        w = options_.width; h = options_.height;
        if (window_) gtk_window_get_size(GTK_WINDOW(window_), &w, &h);
    }
    void set_min_size(int w, int h) override { options_.min_width = w; options_.min_height = h; }
    void set_max_size(int w, int h) override { options_.max_width = w; options_.max_height = h; }
    void set_position(int x, int y) override { if (window_) gtk_window_move(GTK_WINDOW(window_), x, y); }
    void center() override {
        if (window_) gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);
    }
    void set_resizable(bool resizable) override {
        if (window_) gtk_window_set_resizable(GTK_WINDOW(window_), resizable);
    }
    void set_fullscreen(bool fs) override {
        if (!window_) return;
        if (fs) gtk_window_fullscreen(GTK_WINDOW(window_));
        else gtk_window_unfullscreen(GTK_WINDOW(window_));
    }
    void set_always_on_top(bool on_top) override {
        if (window_) gtk_window_set_keep_above(GTK_WINDOW(window_), on_top);
    }
    void set_zoom(double factor) override {
        if (webview_) webkit_web_view_set_zoom_level(WEBKIT_WEB_VIEW(webview_), factor);
    }
    void set_cursor(CursorShape shape) override {
        if (!webview_) return;
        const char* name = "default";
        switch (shape) {
            case CursorShape::Pointer: name = "pointer"; break;
            case CursorShape::Text: name = "text"; break;
            case CursorShape::Crosshair: name = "crosshair"; break;
            case CursorShape::Move: name = "move"; break;
            case CursorShape::Wait: name = "wait"; break;
            case CursorShape::NotAllowed: name = "not-allowed"; break;
            default: break;
        }
        GdkCursor* cursor = gdk_cursor_new_from_name(gdk_display_get_default(), name);
        GdkWindow* gdk_window = gtk_widget_get_window(webview_);
        if (gdk_window) gdk_window_set_cursor(gdk_window, cursor);
        if (cursor) g_object_unref(cursor);
    }
    void minimize() override { if (window_) gtk_window_iconify(GTK_WINDOW(window_)); }
    void maximize() override { if (window_) gtk_window_maximize(GTK_WINDOW(window_)); }
    void restore() override { if (window_) gtk_window_unmaximize(GTK_WINDOW(window_)); }
    void hide() override { if (window_) gtk_widget_hide(window_); }
    void show() override { if (window_) gtk_widget_show_all(window_); }
    void set_devtools(bool enabled) override {
        if (!webview_) return;
        WebKitSettings* settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(webview_));
        webkit_settings_set_enable_developer_extras(settings, enabled);
        if (enabled) webkit_web_inspector_show(webkit_web_view_get_inspector(WEBKIT_WEB_VIEW(webview_)));
    }

    void request_close() override {
        if (window_) gtk_widget_destroy(window_);
    }
    bool is_running() const override { return running_; }

    void dispatch_to_ui(std::function<void()> task) override {
        auto* idle = new IdleTask{std::move(task)};
        g_idle_add(idle_trampoline, idle);
    }

    // --- native dialogs / clipboard -------------------------------------
    std::vector<std::string> open_file_dialog(const OpenFileDialogOptions& options) override {
        std::vector<std::string> result;
        GtkFileChooserAction action = options.choose_directory
                                          ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER
                                          : GTK_FILE_CHOOSER_ACTION_OPEN;
        GtkWidget* dialog = gtk_file_chooser_dialog_new(
            options.title.c_str(), window_ ? GTK_WINDOW(window_) : nullptr, action,
            "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), options.allow_multiple);
        if (!options.default_path.empty())
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog),
                                                options.default_path.c_str());
        apply_filters(dialog, options.filters);
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            if (options.allow_multiple) {
                GSList* files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
                for (GSList* it = files; it; it = it->next) {
                    result.emplace_back(static_cast<const char*>(it->data));
                    g_free(it->data);
                }
                g_slist_free(files);
            } else {
                char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
                if (filename) { result.emplace_back(filename); g_free(filename); }
            }
        }
        gtk_widget_destroy(dialog);
        return result;
    }

    std::string save_file_dialog(const SaveFileDialogOptions& options) override {
        GtkWidget* dialog = gtk_file_chooser_dialog_new(
            options.title.c_str(), window_ ? GTK_WINDOW(window_) : nullptr,
            GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel", GTK_RESPONSE_CANCEL, "_Save",
            GTK_RESPONSE_ACCEPT, nullptr);
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
        if (!options.default_name.empty())
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog),
                                              options.default_name.c_str());
        if (!options.default_path.empty())
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog),
                                                options.default_path.c_str());
        apply_filters(dialog, options.filters);
        std::string result;
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            if (filename) { result = filename; g_free(filename); }
        }
        gtk_widget_destroy(dialog);
        return result;
    }

    int message_box(const MessageBoxOptions& options) override {
        GtkMessageType type = GTK_MESSAGE_INFO;
        switch (options.type) {
            case MessageBoxType::Warning: type = GTK_MESSAGE_WARNING; break;
            case MessageBoxType::Error: type = GTK_MESSAGE_ERROR; break;
            case MessageBoxType::Question: type = GTK_MESSAGE_QUESTION; break;
            default: type = GTK_MESSAGE_INFO;
        }
        GtkWidget* dialog = gtk_message_dialog_new(
            window_ ? GTK_WINDOW(window_) : nullptr, GTK_DIALOG_MODAL, type, GTK_BUTTONS_NONE,
            "%s", options.message.c_str());
        if (!options.title.empty())
            gtk_window_set_title(GTK_WINDOW(dialog), options.title.c_str());
        switch (options.buttons) {
            case MessageBoxButtons::Ok:
                gtk_dialog_add_button(GTK_DIALOG(dialog), "_OK", GTK_RESPONSE_OK);
                break;
            case MessageBoxButtons::OkCancel:
                gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL);
                gtk_dialog_add_button(GTK_DIALOG(dialog), "_OK", GTK_RESPONSE_OK);
                break;
            case MessageBoxButtons::YesNo:
            case MessageBoxButtons::YesNoCancel:
                if (options.buttons == MessageBoxButtons::YesNoCancel)
                    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL);
                gtk_dialog_add_button(GTK_DIALOG(dialog), "_No", GTK_RESPONSE_NO);
                gtk_dialog_add_button(GTK_DIALOG(dialog), "_Yes", GTK_RESPONSE_YES);
                break;
        }
        gint response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_YES) ? 0 : 1;
    }

    std::string clipboard_read_text() override {
        std::string result;
        GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        char* text = gtk_clipboard_wait_for_text(clipboard);
        if (text) { result = text; g_free(text); }
        return result;
    }
    void clipboard_write_text(std::string text) override {
        GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(clipboard, text.c_str(), static_cast<int>(text.size()));
    }

private:
    void configure_settings() {
        WebKitSettings* settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(webview_));
        webkit_settings_set_enable_developer_extras(settings, options_.devtools);
        webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
        webkit_settings_set_javascript_can_access_clipboard(settings, TRUE);
        webkit_settings_set_allow_file_access_from_file_urls(settings, options_.allow_file_access);
        webkit_settings_set_enable_webaudio(settings, TRUE);
        webkit_settings_set_enable_webgl(settings, TRUE);
        webkit_settings_set_hardware_acceleration_policy(
            settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);
        if (!options_.user_agent.empty())
            webkit_settings_set_user_agent(settings, options_.user_agent.c_str());
    }

    void add_user_script(const std::string& source) {
        WebKitUserScript* script = webkit_user_script_new(
            source.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
        webkit_user_content_manager_add_script(ucm_, script);
        webkit_user_script_unref(script);
    }

    static void apply_filters(GtkWidget* dialog, const std::vector<FileFilter>& filters) {
        for (const auto& filter : filters) {
            GtkFileFilter* gtk_filter = gtk_file_filter_new();
            gtk_file_filter_set_name(gtk_filter, filter.name.c_str());
            for (const auto& glob : filter.globs) {
                std::string pattern = glob;
                if (pattern.find('*') == std::string::npos) pattern = "*." + pattern;
                gtk_file_filter_add_pattern(gtk_filter, pattern.c_str());
            }
            gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), gtk_filter);
        }
    }

    void serve_scheme(WebKitURISchemeRequest* request) {
        const char* uri = webkit_uri_scheme_request_get_uri(request);
        if (!assets) {
            GError* error = g_error_new_literal(g_quark_from_static_string("wvc"), 404,
                                                "no asset store");
            webkit_uri_scheme_request_finish_error(request, error);
            g_error_free(error);
            return;
        }
        auto asset = assets->resolve_url(uri ? uri : "");
        if (!asset) {
            WVC_WARN(std::string("app:// 404: ") + (uri ? uri : ""));
            GError* error = g_error_new_literal(g_quark_from_static_string("wvc"), 404,
                                                "asset not found");
            webkit_uri_scheme_request_finish_error(request, error);
            g_error_free(error);
            return;
        }
        // WebKit takes ownership of the buffer through the free function.
        auto* copy = static_cast<guchar*>(g_malloc(asset->bytes.size()));
        if (!asset->bytes.empty()) std::memcpy(copy, asset->bytes.data(), asset->bytes.size());
        GInputStream* stream =
            g_memory_input_stream_new_from_data(copy, static_cast<gssize>(asset->bytes.size()),
                                                g_free);
        webkit_uri_scheme_request_finish(request, stream, static_cast<gssize>(asset->bytes.size()),
                                         asset->mime.c_str());
        g_object_unref(stream);
    }

    // The signal marshals the posted value as a WebKitJavascriptResult; the
    // underlying JSCValue is extracted from it.
    static void on_script_message(WebKitUserContentManager*, WebKitJavascriptResult* result,
                                  gpointer user_data) {
        auto* self = static_cast<GtkBackend*>(user_data);
        if (!result || !self->ipc) return;
        JSCValue* value = webkit_javascript_result_get_js_value(result);
        if (!value) return;
        gchar* json = jsc_value_to_json(value, 0);
        if (json) {
            self->ipc->receive(json);
            g_free(json);
        }
    }

    static gboolean on_delete(GtkWidget*, GdkEvent*, gpointer user_data) {
        auto* self = static_cast<GtkBackend*>(user_data);
        if (self->on_close) self->on_close();
        return FALSE;  // allow the default handler to destroy the window
    }

    static void on_destroy(GtkWidget*, gpointer user_data) {
        auto* self = static_cast<GtkBackend*>(user_data);
        self->running_ = false;
        if (--g_open_windows <= 0) platform_quit();
    }

    static void on_load_changed(WebKitWebView*, WebKitLoadEvent event, gpointer user_data) {
        auto* self = static_cast<GtkBackend*>(user_data);
        if (event == WEBKIT_LOAD_FINISHED && self->on_ready) self->on_ready();
    }

    static void on_size_allocate(GtkWidget*, GdkRectangle* rect, gpointer user_data) {
        auto* self = static_cast<GtkBackend*>(user_data);
        self->options_.width = rect->width;
        self->options_.height = rect->height;
        if (self->on_resize) self->on_resize(rect->width, rect->height);
    }

    WindowOptions options_;
    GtkWidget* window_ = nullptr;
    GtkWidget* webview_ = nullptr;
    WebKitUserContentManager* ucm_ = nullptr;
    bool running_ = false;
};

// ---------------------------------------------------------------------------
// Platform entry points.
// ---------------------------------------------------------------------------
std::unique_ptr<IWebViewBackend> create_webview_backend(WindowOptions options, IpcBridge& ipc,
                                                        AssetStore* assets) {
    auto backend = std::make_unique<GtkBackend>(std::move(options));
    backend->ipc = &ipc;
    backend->assets = assets;
    return backend;
}

int platform_run_loop() {
    if (g_loop_running) return 0;
    g_loop_running = true;
    gtk_main();
    g_loop_running = false;
    return 0;
}

void platform_quit() { gtk_main_quit(); }
const char* platform_name() { return "linux"; }
const char* platform_family() { return "gtk"; }

}  // namespace wvc
