# The only third-party code is SQLite, vendored as its public-domain amalgamation
# in third_party/sqlite. Nothing is downloaded or required from the system at
# build time (besides Qt for the optional GUI).
find_package(Threads REQUIRED)

add_library(sqlite3 STATIC ${PROJECT_SOURCE_DIR}/third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_DEPRECATED)
set_target_properties(sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)
else()
    target_compile_options(sqlite3 PRIVATE -w)
endif()
target_link_libraries(sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
