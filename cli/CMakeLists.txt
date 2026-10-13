# The CLI reuses the asset-store implementation directly (it only needs the
# pack format, not the platform webview backends).
add_executable(wvc
    src/main.cpp
    src/project.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/asset_store.cpp)

target_include_directories(wvc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_compile_definitions(wvc PRIVATE
    WVC_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")

if(WVC_ENABLE_ZLIB)
    find_package(ZLIB)
    if(ZLIB_FOUND)
        target_compile_definitions(wvc PRIVATE WVC_HAVE_ZLIB=1)
        target_link_libraries(wvc PRIVATE ZLIB::ZLIB)
    endif()
endif()

install(TARGETS wvc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
