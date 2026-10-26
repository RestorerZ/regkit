regkit_add_library(regkit_search
    compare.cpp
    regex.cpp
    result_file.cpp
    search.cpp
)
target_link_libraries(regkit_search PRIVATE
    pcre2_16
    regkit_regfile
    regkit_records
    regkit_registry
    regkit_win32
    regkit_work
)
