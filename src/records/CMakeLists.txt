regkit_add_library(regkit_records
    escaped_fields.cpp
    json.cpp
)
target_include_directories(regkit_records PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
