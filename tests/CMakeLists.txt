add_executable(storage_tests
    main.cpp
    test_format.cpp
    test_index.cpp
    test_json.cpp
    test_scanner.cpp
)
target_link_libraries(storage_tests PRIVATE storage::core)
add_test(NAME storage_tests COMMAND storage_tests)
