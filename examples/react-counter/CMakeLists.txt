# Enabled with -DWVC_BUILD_EXAMPLES=ON.
#
# The frontend assets are packed by the `wvc` CLI, which also generates
# native/wvc_assets.cpp. When that file is missing we fall back to a placeholder
# so the native shell still compiles (it simply has no embedded assets).
set(_wvc_assets "${CMAKE_CURRENT_SOURCE_DIR}/native/wvc_assets.cpp")
if(NOT EXISTS "${_wvc_assets}")
    file(WRITE "${_wvc_assets}"
        "// Placeholder: run `wvc build` to embed the frontend.\n#include \"webviewcpp/embed.hpp\"\n")
endif()

add_executable(react-counter native/main.cpp "${_wvc_assets}")
target_link_libraries(react-counter PRIVATE webviewcpp)
