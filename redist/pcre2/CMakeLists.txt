# PCRE2 10.48, 16 bit library only, no JIT
# https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.48/pcre2-10.48.tar.gz
# sha256 ebcc25aadf2a51fa1fefa9b8bc9e7a79b3dae86870a0f1152a22e42befd46888
# pcre2.h and config.h are the upstream .generic files, pcre2_chartables.c is pcre2_chartables.c.dist

set(PCRE2_SOURCES
    src/pcre2_auto_possess.c
    src/pcre2_chartables.c
    src/pcre2_chkdint.c
    src/pcre2_compile.c
    src/pcre2_compile_cgroup.c
    src/pcre2_compile_class.c
    src/pcre2_config.c
    src/pcre2_context.c
    src/pcre2_convert.c
    src/pcre2_dfa_match.c
    src/pcre2_error.c
    src/pcre2_extuni.c
    src/pcre2_find_bracket.c
    src/pcre2_jit_compile.c
    src/pcre2_maketables.c
    src/pcre2_match.c
    src/pcre2_match_data.c
    src/pcre2_match_next.c
    src/pcre2_newline.c
    src/pcre2_ord2utf.c
    src/pcre2_pattern_info.c
    src/pcre2_script_run.c
    src/pcre2_serialize.c
    src/pcre2_string_utils.c
    src/pcre2_study.c
    src/pcre2_substitute.c
    src/pcre2_substring.c
    src/pcre2_tables.c
    src/pcre2_ucd.c
    src/pcre2_valid_utf.c
    src/pcre2_xclass.c
)

add_library(pcre2_16 STATIC ${PCRE2_SOURCES})
target_compile_definitions(pcre2_16 PUBLIC
    PCRE2_CODE_UNIT_WIDTH=16
    PCRE2_STATIC
)
target_compile_definitions(pcre2_16 PRIVATE
    HAVE_CONFIG_H
    SUPPORT_PCRE2_16
    SUPPORT_UNICODE
    NEVER_BACKSLASH_C
    HAVE_MEMMOVE
    _CRT_SECURE_NO_WARNINGS
)
target_include_directories(pcre2_16 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if (MSVC)
    target_compile_options(pcre2_16 PRIVATE /W0)
endif()
