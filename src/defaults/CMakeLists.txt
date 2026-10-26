regkit_add_library(regkit_defaults
    default_data.cpp
    default_loader.cpp
)
target_link_libraries(regkit_defaults PRIVATE
    regkit_regfile
    regkit_registry
    regkit_win32
)
