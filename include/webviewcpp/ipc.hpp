#pragma once

#include "webviewcpp/json.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wvc {

// Two-way bridge between the C++ host and the page's JS/React runtime.
//
//   JS  -> C++   wvc.invoke("channel", payload)  ->  Promise<result>
//   C++ -> JS    bridge.emit("channel", payload) ->  wvc.on("channel", cb)
//
// The wire protocol is a flat JSON object. Every message is dispatched through
// the platform's native message channel (WebView2 postMessage / WKScriptMessage
// / WebKit user-content message handler).
//
//   JS  -> native:  {"id":7,"kind":"invoke","channel":"fs.read","payload":{...}}
//   native -> JS:   {"id":7,"kind":"resolve","payload":{...}}
//                   {"id":7,"kind":"reject","error":"..."}
//                   {"kind":"event","channel":"tick","payload":{...}}
class IpcBridge {
public:
    using Handler = std::function<Json(const Json& payload)>;

    // Injects a JS snippet into the page (used for native -> JS push).
    using Sender = std::function<void(std::string_view script)>;

    // Marshals a closure onto the UI thread. Native backends install this so
    // background threads can `emit()` safely.
    using UiDispatcher = std::function<void(std::function<void()>)>;

    using EventHook = std::function<void(const std::string& channel, const Json& payload)>;

    void set_sender(Sender sender) { sender_ = std::move(sender); }
    void set_ui_dispatcher(UiDispatcher dispatcher) { ui_ = std::move(dispatcher); }
    void set_event_hook(EventHook hook) { event_hook_ = std::move(hook); }

    // --- C++ side registration ------------------------------------------
    void handle(const std::string& channel, Handler handler);
    void on(const std::string& channel, Handler handler) { handle(channel, std::move(handler)); }
    void remove_handler(const std::string& channel);
    bool has_handler(const std::string& channel) const;
    std::vector<std::string> channels() const;

    // --- C++ -> JS push --------------------------------------------------
    void emit(const std::string& channel, const Json& payload);
    void emit(const std::string& channel) { emit(channel, Json()); }

    // --- called by the native backend -----------------------------------
    // `raw` is a JSON document produced by the page. Runs synchronously on the
    // UI thread.
    void receive(std::string_view raw);

    // The webview signals that the document is live; queued events flush.
    void mark_ready();
    bool ready() const { return ready_; }

    // The script that defines `window.wvc` and `window.__wvc`. Injected at
    // document-start by every backend.
    static const std::string& bootstrap_script();

private:
    void send_json(const Json& message);
    std::string dispatch_script(const Json& message) const;

    std::map<std::string, Handler> handlers_;
    Sender sender_;
    UiDispatcher ui_;
    EventHook event_hook_;
    std::deque<Json> pending_events_;
    bool ready_ = false;
};

}  // namespace wvc
