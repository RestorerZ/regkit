regkit_add_library(regkit_ui
    autocomplete.cpp
    default_font.cpp
    dialog_fit.cpp
    dialog_layout.cpp
    dialog_support.cpp
    feedback.cpp
    font_metrics.cpp
    font_picker.cpp
    gdi_cache.cpp
    icon_loader.cpp
    list_header.cpp
    list_view_support.cpp
    presets.cpp
    preset_editor.cpp
    tab_strip.cpp
    theme.cpp
    toolbar.cpp
)
target_include_directories(regkit_ui PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
target_include_directories(regkit_ui PRIVATE
    "${PROJECT_SOURCE_DIR}/resources"
)
target_link_libraries(regkit_ui PRIVATE
    regkit_win32
    advapi32
    comctl32
    comdlg32
    dwmapi
    ole32
    shell32
    uxtheme
)
