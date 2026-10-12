regkit_add_library(regkit_regfile
    batch_script.cpp
    powershell_script.cpp
    reg_exe_syntax.cpp
    reg_file.cpp
    registry_transfer.cpp
    script_convert.cpp
)
target_include_directories(regkit_regfile PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(regkit_regfile PRIVATE
    regkit_registry
    regkit_win32
    advapi32
)
