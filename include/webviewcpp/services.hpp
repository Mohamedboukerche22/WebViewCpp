#pragma once

#include "webviewcpp/ipc.hpp"
#include "webviewcpp/webview.hpp"

#include <functional>
#include <optional>
#include <string>

namespace wvc::services {

using BackendAccessor = std::function<IWebViewBackend*()>;

void install_system(IpcBridge& ipc);
void install_fs(IpcBridge& ipc, std::optional<std::string> sandbox_root);
void install_dialogs(IpcBridge& ipc, BackendAccessor backend);
void install_clipboard(IpcBridge& ipc, BackendAccessor backend);

// Opens a URL or file with the OS default handler. Returns true on success.
bool shell_open(const std::string& target);

// Portable system description, also what the `system.info` channel returns.
Json system_info();

std::string base64_encode(const uint8_t* data, std::size_t size);
std::vector<uint8_t> base64_decode(const std::string& text);

}  // namespace wvc::services
