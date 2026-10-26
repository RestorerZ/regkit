regkit_add_library(regkit_trace
    trace_data.cpp
    trace_loader.cpp
    trace_parser.cpp
    trace_paths.cpp
)
target_link_libraries(regkit_trace PRIVATE
    regkit_registry
    regkit_win32
)
