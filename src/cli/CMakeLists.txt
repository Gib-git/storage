add_executable(storagectl main.cpp args.hpp)
target_link_libraries(storagectl PRIVATE storage::core)
install(TARGETS storagectl RUNTIME DESTINATION bin)
