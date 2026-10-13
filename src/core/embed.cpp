#include "webviewcpp/embed.hpp"

namespace wvc {
namespace {
EmbeddedData& slot() {
    static EmbeddedData data;
    return data;
}
}  // namespace

void set_embedded_assets(const unsigned char* data, std::size_t size, const char* name) {
    slot() = EmbeddedData{data, size, name ? name : "<embedded>"};
}

const EmbeddedData* embedded_assets() {
    return &slot();
}

bool has_embedded_assets() {
    return slot().data != nullptr && slot().size > 0;
}

}  // namespace wvc
