# Aeon Browser — Safe Post-Build Copy Script
# Prevents self-copy collisions when SRC and DST paths are identical.

if(DEFINED SRC AND DEFINED DST)
    string(STRIP "${SRC}" SRC_STRIPPED)
    string(STRIP "${DST}" DST_STRIPPED)
    if(NOT "${SRC_STRIPPED}" STREQUAL "" AND NOT "${DST_STRIPPED}" STREQUAL "")
        get_filename_component(SRC_ABS "${SRC_STRIPPED}" ABSOLUTE)
        get_filename_component(DST_ABS "${DST_STRIPPED}" ABSOLUTE)
        string(TOLOWER "${SRC_ABS}" SRC_LOWER)
        string(TOLOWER "${DST_ABS}" DST_LOWER)
        if(NOT SRC_LOWER STREQUAL DST_LOWER)
            execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_STRIPPED}" "${DST_STRIPPED}")
        endif()
    endif()
endif()
