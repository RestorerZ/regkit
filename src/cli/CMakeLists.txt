regkit_add_library(regkit_cli
    reg_command.cpp
)
target_link_libraries(regkit_cli PRIVATE
    regkit_regfile
    regkit_registry
    regkit_win32
)
