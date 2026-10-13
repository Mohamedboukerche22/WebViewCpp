#pragma once

#define WVC_VERSION_MAJOR 0
#define WVC_VERSION_MINOR 1
#define WVC_VERSION_PATCH 0
#define WVC_VERSION_STRING "0.1.0"

namespace wvc {
inline constexpr const char* version_string() { return WVC_VERSION_STRING; }
}  // namespace wvc
