#include "test.hpp"

#include "webviewcpp/log.hpp"

namespace wvctest {
std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}
}  // namespace wvctest

int main() {
    wvc::Log::set_min_level(wvc::LogLevel::Warn);
    int passed = 0;
    for (auto& c : wvctest::registry()) {
        int before = wvctest::failed;
        std::printf("== %s\n", c.name.c_str());
        try {
            c.fn();
        } catch (const std::exception& e) {
            ++wvctest::failed;
            std::printf("  FAIL uncaught: %s\n", e.what());
        }
        if (wvctest::failed == before) ++passed;
    }
    std::printf("\n%d/%zu cases passed, %d checks, %d failures\n", passed,
                wvctest::registry().size(), wvctest::checks, wvctest::failed);
    return wvctest::failed == 0 ? 0 : 1;
}
