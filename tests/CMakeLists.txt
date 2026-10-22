add_executable(storage_tests
    main.cpp
    test_cleaner.cpp
    test_duplicates.cpp
    test_executor.cpp
    test_format.cpp
    test_index.cpp
    test_json.cpp
    test_organize.cpp
    test_scanner.cpp
    test_sha256.cpp
    test_ssh_fs.cpp
)
target_link_libraries(storage_tests PRIVATE storage::core)
add_test(NAME storage_tests COMMAND storage_tests)
