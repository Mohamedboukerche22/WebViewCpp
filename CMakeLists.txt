cmake_minimum_required(VERSION 3.24)

project(WebViewCpp
    VERSION 0.1.0
    DESCRIPTION "Lightweight cross-platform HTML/CSS/JS desktop framework"
    LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(WVC_BUILD_CLI      "Build the wvc command line tool"   ON)
option(WVC_BUILD_TESTS    "Build the unit tests"              ON)
option(WVC_BUILD_EXAMPLES "Build the example applications"    OFF)
option(WVC_ENABLE_ZLIB    "Use zlib to compress asset bundles" ON)

include(GNUInstallDirs)
include(CTest)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(webviewcpp STATIC
    src/core/asset_store.cpp
    src/core/bootstrap_js.cpp
    src/core/embed.cpp
    src/core/ipc.cpp
    src/core/services.cpp
    src/core/window.cpp
)

target_include_directories(webviewcpp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set(WVC_PLATFORM_LIBS "")

if(WIN32)
    target_sources(webviewcpp PRIVATE src/platform/webview_win32.cpp)
    target_compile_definitions(webviewcpp PRIVATE UNICODE _UNICODE NOMINMAX
        WIN32_LEAN_AND_MEAN)
    # WebView2 is consumed via the NuGet package or the evergreen runtime.
    # Point WVC_WEBVIEW2_DIR at a directory containing WebView2.h,
    # WebView2LoaderStatic.lib / WebView2Loader.dll.
    if(DEFINED WVC_WEBVIEW2_DIR)
        target_include_directories(webviewcpp PRIVATE "${WVC_WEBVIEW2_DIR}/include")
        target_link_directories(webviewcpp PRIVATE "${WVC_WEBVIEW2_DIR}/lib")
        if(EXISTS "${WVC_WEBVIEW2_DIR}/lib/WebView2LoaderStatic.lib")
            target_link_libraries(webviewcpp PRIVATE WebView2LoaderStatic)
        else()
            target_link_libraries(webviewcpp PRIVATE WebView2Loader)
        endif()
    endif()
    list(APPEND WVC_PLATFORM_LIBS comctl32 shlwapi user32 gdi32 ole32 shell32)
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(webviewcpp PRIVATE src/platform/webview_cocoa.mm)
    target_compile_options(webviewcpp PRIVATE "-fobjc-arc")
    list(APPEND WVC_PLATFORM_LIBS
        "-framework Cocoa"
        "-framework WebKit"
        "-framework Foundation")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
    pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
    target_sources(webviewcpp PRIVATE src/platform/webview_gtk.cpp)
    target_link_libraries(webviewcpp PRIVATE PkgConfig::WEBKIT PkgConfig::GTK)
    list(APPEND WVC_PLATFORM_LIBS pthread dl)
endif()

if(WVC_ENABLE_ZLIB)
    find_package(ZLIB)
    if(ZLIB_FOUND)
        target_compile_definitions(webviewcpp PRIVATE WVC_HAVE_ZLIB=1)
        target_link_libraries(webviewcpp PRIVATE ZLIB::ZLIB)
        message(STATUS "WebViewCpp: zlib compression enabled")
    else()
        message(STATUS "WebViewCpp: zlib not found, asset bundles stay uncompressed")
    endif()
endif()

target_link_libraries(webviewcpp PRIVATE ${WVC_PLATFORM_LIBS})

add_library(WebViewCpp::webviewcpp ALIAS webviewcpp)

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
if(WVC_BUILD_CLI)
    add_subdirectory(cli)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(WVC_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(WVC_BUILD_EXAMPLES)
    add_subdirectory(examples/react-counter)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS webviewcpp
    EXPORT WebViewCppTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT WebViewCppTargets
    NAMESPACE WebViewCpp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WebViewCpp)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/WebViewCppConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/WebViewCppConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WebViewCpp)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/WebViewCppConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/WebViewCppConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/WebViewCppConfigVersion.cmake
    cmake/EmbedAssets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WebViewCpp)
