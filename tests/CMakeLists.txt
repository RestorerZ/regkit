add_executable(regkit_security_tests
    security_tests.cpp
)
regkit_configure_executable(regkit_security_tests)
target_link_libraries(regkit_security_tests PRIVATE
    regkit_editors
    regkit_registry
    regkit_win32
    advapi32
)
add_test(NAME security_tests COMMAND regkit_security_tests)
