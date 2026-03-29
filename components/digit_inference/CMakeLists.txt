idf_build_get_property(PROJECT_DIR PROJECT_DIR)

set(MODEL_INPUT "${PROJECT_DIR}/models/mnist_tiny_int8.tflite")
set(MODEL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(MODEL_C "${MODEL_GEN_DIR}/mnist_tiny_model_data.c")
set(MODEL_H "${MODEL_GEN_DIR}/mnist_tiny_model_data.h")

# idf_component_register requires include dirs to exist during configure.
file(MAKE_DIRECTORY "${MODEL_GEN_DIR}")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT DEFINED Python3_EXECUTABLE)
        if(DEFINED _Python3_EXECUTABLE)
            set(Python3_EXECUTABLE "${_Python3_EXECUTABLE}")
        else()
            set(Python3_EXECUTABLE "python")
        endif()
    endif()

    add_custom_command(
        OUTPUT "${MODEL_C}" "${MODEL_H}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MODEL_GEN_DIR}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_DIR}/scripts/tflite_to_c_array.py"
                --input "${MODEL_INPUT}"
                --output-c "${MODEL_C}"
                --output-h "${MODEL_H}"
                --symbol "g_mnist_tiny_int8_model"
        DEPENDS "${MODEL_INPUT}" "${PROJECT_DIR}/scripts/tflite_to_c_array.py"
        COMMENT "Generating mnist_tiny_int8 model source"
        VERBATIM
    )

    add_custom_target(generate_mnist_tiny_model_data DEPENDS "${MODEL_C}" "${MODEL_H}")
endif()

idf_component_register(
    SRCS
        "digit_inference.cc"
        "digit_preprocess.c"
        "${MODEL_C}"
    INCLUDE_DIRS
        "include"
        "${MODEL_GEN_DIR}"
    REQUIRES
        esp-tflite-micro
        esp_timer
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_dependencies(${COMPONENT_LIB} generate_mnist_tiny_model_data)

    target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fno-rtti -fno-exceptions)
endif()
