regkit_add_library(regkit_records
    escaped_fields.cpp
    json.cpp
)
target_link_libraries(regkit_records PRIVATE
    regkit_win32
)
