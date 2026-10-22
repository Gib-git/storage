configure_file(include/storage/core/version.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/include/storage/core/version.hpp @ONLY)

add_library(storagecore STATIC
    src/category.cpp
    src/format.cpp
    src/index.cpp
    src/journal.cpp
    src/json.cpp
    src/duplicates.cpp
    src/exif.cpp
    src/executor.cpp
    src/file_io.cpp
    src/local_fs.cpp
    src/local_hash.cpp
    src/local_ops.cpp
    src/node.cpp
    src/organize.cpp
    src/paths.cpp
    src/scanner.cpp
    src/sha256.cpp
)
add_library(storage::core ALIAS storagecore)

target_include_directories(storagecore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include)
target_link_libraries(storagecore
    PUBLIC Threads::Threads
    PRIVATE sqlite3)

if(APPLE)
    enable_language(OBJCXX)
    target_sources(storagecore PRIVATE src/trash_mac.mm)
    target_link_libraries(storagecore PUBLIC "-framework Foundation")
elseif(WIN32)
    target_sources(storagecore PRIVATE src/trash.cpp)
    target_link_libraries(storagecore PUBLIC ole32 shell32)
else()
    target_sources(storagecore PRIVATE src/trash.cpp)
endif()
