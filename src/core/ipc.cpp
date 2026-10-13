#include "webviewcpp/ipc.hpp"

#include "webviewcpp/log.hpp"

namespace wvc {

void IpcBridge::handle(const std::string& channel, Handler handler) {
    handlers_[channel] = std::move(handler);
}

void IpcBridge::remove_handler(const std::string& channel) {
    handlers_.erase(channel);
}

bool IpcBridge::has_handler(const std::string& channel) const {
    return handlers_.count(channel) != 0;
}

std::vector<std::string> IpcBridge::channels() const {
    std::vector<std::string> out;
    out.reserve(handlers_.size());
    for (const auto& [k, _] : handlers_) out.push_back(k);
    return out;
}

void IpcBridge::emit(const std::string& channel, const Json& payload) {
    Json message;
    message["kind"] = "event";
    message["channel"] = channel;
    message["payload"] = payload;

    if (!ready_) {
        // The document isn't live yet; hold events until bootstrap handshake.
        pending_events_.push_back(std::move(message));
        return;
    }
    if (ui_) {
        auto ui = ui_;
        ui([this, message = std::move(message)]() mutable { send_json(message); });
    } else {
        send_json(message);
    }
}

void IpcBridge::mark_ready() {
    ready_ = true;
    while (!pending_events_.empty()) {
        send_json(pending_events_.front());
        pending_events_.pop_front();
    }
}

void IpcBridge::send_json(const Json& message) {
    if (!sender_) {
        WVC_WARN("IpcBridge: no sender installed; dropping message");
        return;
    }
    sender_(dispatch_script(message));
}

std::string IpcBridge::dispatch_script(const Json& message) const {
    // The message is a JSON document, which is a strict subset of a JS object
    // literal, so it can be embedded directly.
    return "window.__wvc&&window.__wvc.__dispatch(" + message.dump() + ");";
}

void IpcBridge::receive(std::string_view raw) {
    Json message;
    try {
        message = Json::parse(raw);
    } catch (const std::exception& e) {
        WVC_ERROR(std::string("IpcBridge: bad message: ") + e.what());
        return;
    }

    const std::string kind = message["kind"].string_or("");

    if (kind == "ready") {
        mark_ready();
        return;
    }

    if (kind != "invoke") {
        WVC_DEBUG("IpcBridge: ignoring unknown kind '" + kind + "'");
        return;
    }

    const Json id = message["id"];
    const std::string channel = message["channel"].string_or("");
    const Json payload = message["payload"];

    if (event_hook_) event_hook_(channel, payload);

    auto it = handlers_.find(channel);
    if (it == handlers_.end()) {
        Json reply;
        reply["id"] = id;
        reply["kind"] = "reject";
        reply["error"] = "no handler registered for channel '" + channel + "'";
        send_json(reply);
        WVC_WARN("IpcBridge: unhandled channel '" + channel + "'");
        return;
    }

    Json reply;
    reply["id"] = id;
    try {
        Json result = it->second(payload);
        reply["kind"] = "resolve";
        reply["payload"] = result.is_null() ? Json::object{} : std::move(result);
    } catch (const std::exception& e) {
        reply["kind"] = "reject";
        reply["error"] = e.what();
        WVC_ERROR(std::string("IpcBridge: handler '") + channel + "' threw: " + e.what());
    } catch (...) {
        reply["kind"] = "reject";
        reply["error"] = "unknown native error";
    }
    send_json(reply);
}

}  // namespace wvc
