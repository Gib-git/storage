configure_file(include/storage/core/version.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/include/storage/core/version.hpp @ONLY)

add_library(storagecore STATIC
    src/category.cpp
    src/format.cpp
    src/index.cpp
    src/json.cpp
    src/local_fs.cpp
    src/node.cpp
    src/paths.cpp
    src/scanner.cpp
)
add_library(storage::core ALIAS storagecore)

target_include_directories(storagecore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include)
target_link_libraries(storagecore
    PUBLIC Threads::Threads
    PRIVATE sqlite3)
