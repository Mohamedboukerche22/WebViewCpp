add_executable(wvc_tests
    test_main.cpp
    test_json.cpp
    test_asset_store.cpp
    test_ipc.cpp)

target_link_libraries(wvc_tests PRIVATE webviewcpp)
find_package(Threads REQUIRED)
target_link_libraries(wvc_tests PRIVATE Threads::Threads)

add_test(NAME wvc_tests COMMAND wvc_tests)
