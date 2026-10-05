regkit_add_library(regkit_work
    key_watcher.cpp
    session.cpp
)
target_include_directories(regkit_work PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(regkit_work PRIVATE
    regkit_win32
    advapi32
)
