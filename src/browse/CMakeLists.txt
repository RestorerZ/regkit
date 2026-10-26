regkit_add_library(regkit_browse
    browse_pane.cpp
    key_tree.cpp
    value_table.cpp
)
target_link_libraries(regkit_browse PRIVATE
    regkit_registry
    regkit_work
    regkit_win32
    comctl32
)
