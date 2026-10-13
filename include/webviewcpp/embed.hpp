#pragma once

#include <cstddef>

namespace wvc {

// A blob produced by the build-time asset packer and compiled into the
// executable (or its neighbouring resources). Registration happens through a
// static initializer in the generated translation unit, which sidesteps the
// weak-symbol / MSVC differences entirely.
struct EmbeddedData {
    const unsigned char* data = nullptr;
    std::size_t size = 0;
    const char* name = "<embedded>";
};

// Called by generated code, e.g. WVC_EMBED_ASSETS(kBlob, kSize, "app.wvcp").
void set_embedded_assets(const unsigned char* data, std::size_t size, const char* name);
const EmbeddedData* embedded_assets();
bool has_embedded_assets();

}  // namespace wvc

// Convenience macro for the generated TU.
#define WVC_EMBED_ASSETS(blob, len, label)                                  \
    namespace {                                                             \
    struct WvcAssetRegistrar {                                              \
        WvcAssetRegistrar() { ::wvc::set_embedded_assets((blob), (len), (label)); } \
    };                                                                     \
    const WvcAssetRegistrar wvc_asset_registrar_instance;                   \
    }
