regkit_add_library(regkit_registry
    hive_files.cpp
    key_access.cpp
    live_registry.cpp
    offline_registry.cpp
    registry_path.cpp
    registry_store.cpp
    resource_list.cpp
    value_decoder.cpp
    value_format.cpp
    virtual_registry.cpp
)
target_include_directories(regkit_registry PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(regkit_registry PRIVATE
    regkit_win32
    advapi32
    authz
    crypt32
    ole32
    shlwapi
    ws2_32
)
