#include "test.hpp"
#include "webviewcpp/ipc.hpp"

#include <string>
#include <vector>

using namespace wvc;

namespace {
struct Capture {
    std::vector<std::string> sent;
    IpcBridge bridge;
    Capture() {
        bridge.set_sender([this](std::string_view js) { sent.emplace_back(js); });
        bridge.mark_ready();
    }
    const std::string& last() const { return sent.back(); }
};
}  // namespace

TEST(ipc_invoke_resolves) {
    Capture c;
    c.bridge.handle("math.add",
                    [](const Json& p) { return Json(p["a"].as_int() + p["b"].as_int()); });
    c.bridge.receive(R"({"id":5,"kind":"invoke","channel":"math.add","payload":{"a":2,"b":3}})");

    CHECK_EQ(c.sent.size(), std::size_t(1));
    CHECK(c.last().find("\"kind\":\"resolve\"") != std::string::npos);
    CHECK(c.last().find("\"payload\":5") != std::string::npos);
    CHECK(c.last().find("__dispatch") != std::string::npos);
}

TEST(ipc_unknown_channel_rejects) {
    Capture c;
    c.bridge.receive(R"({"id":1,"kind":"invoke","channel":"nope","payload":null})");
    CHECK(c.last().find("\"kind\":\"reject\"") != std::string::npos);
    CHECK(c.last().find("no handler") != std::string::npos);
}

TEST(ipc_handler_exception_rejects) {
    Capture c;
    c.bridge.handle("boom", [](const Json&) -> Json { throw std::runtime_error("kaboom"); });
    c.bridge.receive(R"({"id":2,"kind":"invoke","channel":"boom","payload":null})");
    CHECK(c.last().find("\"kind\":\"reject\"") != std::string::npos);
    CHECK(c.last().find("kaboom") != std::string::npos);
}

TEST(ipc_events_buffer_until_ready) {
    IpcBridge bridge;
    std::vector<std::string> sent;
    bridge.set_sender([&](std::string_view js) { sent.emplace_back(js); });

    bridge.emit("loaded", Json(1));
    CHECK_EQ(sent.size(), std::size_t(0));  // held back

    bridge.mark_ready();
    CHECK_EQ(sent.size(), std::size_t(1));
    CHECK(sent.front().find("\"kind\":\"event\"") != std::string::npos);
    CHECK(sent.front().find("\"channel\":\"loaded\"") != std::string::npos);
}

TEST(ipc_bad_json_is_ignored) {
    Capture c;
    c.bridge.receive("{not json");
    CHECK_EQ(c.sent.size(), std::size_t(0));
}

TEST(ipc_ready_message_marks_ready) {
    IpcBridge bridge;
    std::vector<std::string> sent;
    bridge.set_sender([&](std::string_view js) { sent.emplace_back(js); });
    CHECK(!bridge.ready());
    bridge.receive(R"({"kind":"ready"})");
    CHECK(bridge.ready());
    bridge.emit("a", Json::object{});
    CHECK_EQ(sent.size(), std::size_t(1));
}
