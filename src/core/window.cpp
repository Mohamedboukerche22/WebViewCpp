#include "webviewcpp/window.hpp"

#include "webviewcpp/embed.hpp"
#include "webviewcpp/log.hpp"
#include "webviewcpp/services.hpp"

#include <optional>
#include <stdexcept>
#include <utility>

namespace wvc {

Window::Window(WindowOptions options) : options_(std::move(options)) {}

Window::Window(std::string url, WindowOptions options)
    : options_(std::move(options)), initial_url_(std::move(url)) {}

Window::~Window() = default;

Window& Window::title(std::string value) { options_.title = std::move(value); if (backend_ready_) backend_->set_title(options_.title); return *this; }
Window& Window::size(int w, int h) { options_.width = w; options_.height = h; if (backend_ready_) backend_->set_size(w, h); return *this; }
Window& Window::min_size(int w, int h) { options_.min_width = w; options_.min_height = h; if (backend_ready_) backend_->set_min_size(w, h); return *this; }
Window& Window::max_size(int w, int h) { options_.max_width = w; options_.max_height = h; if (backend_ready_) backend_->set_max_size(w, h); return *this; }
Window& Window::position(int x, int y) { options_.x = x; options_.y = y; if (backend_ready_) backend_->set_position(x, y); return *this; }
Window& Window::resizable(bool v) { options_.resizable = v; if (backend_ready_) backend_->set_resizable(v); return *this; }
Window& Window::frameless(bool v) { options_.frameless = v; return *this; }
Window& Window::transparent(bool v) { options_.transparent = v; return *this; }
Window& Window::devtools(bool v) { options_.devtools = v; if (backend_ready_) backend_->set_devtools(v); return *this; }
Window& Window::always_on_top(bool v) { options_.always_on_top = v; if (backend_ready_) backend_->set_always_on_top(v); return *this; }
Window& Window::zoom(double factor) { options_.zoom = factor; if (backend_ready_) backend_->set_zoom(factor); return *this; }
Window& Window::icon(std::string path) { options_.icon = std::move(path); return *this; }
Window& Window::user_agent(std::string value) { options_.user_agent = std::move(value); return *this; }
Window& Window::background(std::string css) { options_.background = std::move(css); return *this; }

Window& Window::load_app(std::string entry) {
    initial_html_.clear();
    initial_url_ = std::move(entry);
    if (backend_ready_) backend_->navigate(initial_url_);
    return *this;
}

Window& Window::load_url(std::string url) {
    initial_html_.clear();
    initial_url_ = std::move(url);
    if (backend_ready_) backend_->navigate(initial_url_);
    return *this;
}

Window& Window::load_html(std::string html) {
    initial_url_.clear();
    initial_html_ = std::move(html);
    if (backend_ready_) backend_->set_html(initial_html_);
    return *this;
}

Window& Window::use_assets(std::unique_ptr<AssetStore> store) {
    if (backend_ready_) throw std::logic_error("Window::use_assets() must be called before run()");
    assets_ = std::move(store);
    return *this;
}

AssetStore* Window::assets() { return assets_.get(); }

Window& Window::handle(const std::string& channel, Handler handler) {
    ipc_.handle(channel, std::move(handler));
    return *this;
}

Window& Window::on_ready(std::function<void()> callback) { user_on_ready_ = std::move(callback); return *this; }
Window& Window::on_close(std::function<void()> callback) { user_on_close_ = std::move(callback); return *this; }
Window& Window::on_resize(std::function<void(int, int)> callback) { user_on_resize_ = std::move(callback); return *this; }

void Window::emit(const std::string& channel, const Json& payload) { ipc_.emit(channel, payload); }
void Window::eval(std::string_view js) { if (backend_) backend_->eval(js); }

Window& Window::install_system_services() {
    services::install_system(ipc_);
    return *this;
}

Window& Window::install_fs_services(std::optional<std::string> sandbox_root) {
    services::install_fs(ipc_, std::move(sandbox_root));
    return *this;
}

Window& Window::install_dialog_services() {
    services::install_dialogs(ipc_, [this]() { return backend_.get(); });
    return *this;
}

Window& Window::install_clipboard_services() {
    services::install_clipboard(ipc_, [this]() { return backend_.get(); });
    return *this;
}

Window& Window::install_default_services() {
    return install_system_services().install_fs_services().install_dialog_services()
        .install_clipboard_services();
}

void Window::set_title(std::string_view value) { options_.title = std::string(value); if (backend_) backend_->set_title(value); }
void Window::set_size(int w, int h) { options_.width = w; options_.height = h; if (backend_) backend_->set_size(w, h); }
Rect Window::get_size() const {
    Rect r{options_.x, options_.y, options_.width, options_.height};
    if (backend_) backend_->get_size(r.width, r.height);
    return r;
}
void Window::center() { if (backend_) backend_->center(); }
void Window::minimize() { if (backend_) backend_->minimize(); }
void Window::maximize() { if (backend_) backend_->maximize(); }
void Window::restore() { if (backend_) backend_->restore(); }
void Window::set_fullscreen(bool v) { options_.fullscreen = v; if (backend_) backend_->set_fullscreen(v); }
void Window::show() { if (backend_) backend_->show(); }
void Window::hide() { if (backend_) backend_->hide(); }
void Window::reload() { if (backend_) backend_->reload(); }
void Window::navigate(std::string url) { initial_url_ = url; if (backend_) backend_->navigate(url); }
void Window::close() { if (backend_) backend_->request_close(); }
bool Window::is_open() const { return backend_ && backend_->is_running(); }

void Window::apply_assets() {
    if (assets_) return;
    if (has_embedded_assets()) {
        const auto* blob = embedded_assets();
        assets_ = AssetStore::from_memory(blob->data, blob->size, blob->name);
    } else {
        WVC_WARN("Window: no assets supplied and none embedded; "
                 "the webview will only be able to load external URLs");
    }
}

void Window::wire_callbacks() {
    backend_->on_message = [this](std::string raw) { ipc_.receive(raw); };
    backend_->on_ready = [this]() {
        ipc_.mark_ready();
        if (user_on_ready_) user_on_ready_();
    };
    backend_->on_close = [this]() {
        if (user_on_close_) user_on_close_();
    };
    backend_->on_resize = [this](int w, int h) {
        if (user_on_resize_) user_on_resize_(w, h);
    };
}

void Window::ensure_backend() {
    if (backend_) return;

    apply_assets();

    backend_ = create_webview_backend(options_, ipc_, assets_.get());
    if (!backend_) throw std::runtime_error("Window: platform backend unavailable");

    wire_callbacks();

    ipc_.set_sender([this](std::string_view script) { backend_->eval(script); });
    ipc_.set_ui_dispatcher([this](std::function<void()> task) { backend_->dispatch_to_ui(std::move(task)); });

    if (!backend_->initialize(options_))
        throw std::runtime_error("Window: failed to initialise native webview");

    backend_ready_ = true;

    // Perform the initial load now that the native view exists.
    if (!initial_html_.empty()) {
        backend_->set_html(initial_html_);
    } else {
        if (initial_url_.empty()) initial_url_ = kDefaultEntry;
        backend_->navigate(initial_url_);
    }
    loaded_ = true;
}

Window& Window::open() {
    ensure_backend();
    return *this;
}

int Window::run() {
    ensure_backend();
    int code = platform_run_loop();
    backend_.reset();
    backend_ready_ = false;
    return code;
}

}  // namespace wvc
