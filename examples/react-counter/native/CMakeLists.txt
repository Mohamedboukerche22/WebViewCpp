# Standalone native shell for the React Counter example.
# Build it with the CLI:  wvc build     (packs the frontend + compiles this)
cmake_minimum_required(VERSION 3.24)
project(react-counter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED WVC_ROOT)
  if(DEFINED ENV{WVC_ROOT})
    set(WVC_ROOT $ENV{WVC_ROOT})
  else()
    get_filename_component(WVC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
  endif()
endif()

find_package(WebViewCpp CONFIG QUIET)
if(NOT TARGET WebViewCpp::webviewcpp)
  add_subdirectory(${WVC_ROOT} ${CMAKE_BINARY_DIR}/webviewcpp EXCLUDE_FROM_ALL)
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/wvc_assets.cpp")
  file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/wvc_assets.cpp"
       "// Placeholder: run `wvc build` to embed the frontend.\n#include \"webviewcpp/embed.hpp\"\n")
endif()

add_executable(react-counter main.cpp wvc_assets.cpp)
target_link_libraries(react-counter PRIVATE WebViewCpp::webviewcpp)
