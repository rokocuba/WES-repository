set(SQUARELINE_DIR ${CMAKE_CURRENT_LIST_DIR}/ui_exports)
file(GLOB_RECURSE SQUARELINE_SRCS
    "${SQUARELINE_DIR}/*.c"
)

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
