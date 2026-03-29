set(SQUARELINE_DIR ${CMAKE_CURRENT_LIST_DIR}/ui_exports)
set(SQUARELINE_FILELIST ${SQUARELINE_DIR}/filelist.txt)

set(SQUARELINE_SRCS)
if(EXISTS "${SQUARELINE_FILELIST}")
    file(STRINGS "${SQUARELINE_FILELIST}" SQUARELINE_FILELIST_ENTRIES)
    foreach(_entry ${SQUARELINE_FILELIST_ENTRIES})
        string(STRIP "${_entry}" _entry)
        if(_entry STREQUAL "")
            continue()
        endif()

        set(_src "${SQUARELINE_DIR}/${_entry}")
        if(EXISTS "${_src}")
            list(APPEND SQUARELINE_SRCS "${_src}")
        else()
            message(STATUS "ui_app: skipping missing SquareLine filelist entry: ${_entry}")
        endif()
    endforeach()
endif()

if(NOT SQUARELINE_SRCS)
    file(GLOB_RECURSE SQUARELINE_SRCS
        "${SQUARELINE_DIR}/*.c"
    )
endif()

set(COMPONENT_SRCS 
    "ui_app.c"
    ${SQUARELINE_SRCS}
)

file(GLOB_RECURSE SQUARELINE_INCLUDES
    "${SQUARELINE_DIR}/*.h"
)

foreach(_hdr ${SQUARELINE_INCLUDES})
    get_filename_component(_dir ${_hdr} DIRECTORY)
    list(APPEND COMPONENT_ADD_INCLUDEDIRS ${_dir})
endforeach()

list(REMOVE_DUPLICATES COMPONENT_ADD_INCLUDEDIRS)

set(COMPONENT_PRIV_REQUIRES lvgl lvgl_esp32_drivers)

register_component()
