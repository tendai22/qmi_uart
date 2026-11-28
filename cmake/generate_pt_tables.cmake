
#
# Protothread table generation
#
# Usage:
#   include(cmake/generate_pt_tables.cmake)
#
#   generate_pt_tables(target
#       source1.c
#       source2.c
#       ...
#   )
#
# Call generate_pt_tables() after add_executable().
#

include_guard(GLOBAL)

function(generate_pt_tables TARGET_NAME)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    # Protothread table generator

    set(GEN_TABLE_PY
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_table.py"
    )

    if(NOT EXISTS "${GEN_TABLE_PY}")
        message(FATAL_ERROR
            "Protothread table generator not found: ${GEN_TABLE_PY}"
        )
    endif()

    set(GENERATED_TABLE_SRCS)
    set(GENERATED_TABLE_HDRS)

    # Generate tables for each source file

    foreach(SRC IN LISTS ARGN)

        get_filename_component(BASE "${SRC}" NAME_WE)

        set(SOURCE_FILE
            "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}"
        )
        set(GENERATED_H
            "${CMAKE_CURRENT_BINARY_DIR}/${BASE}_table.h"
        )
        set(GENERATED_C
            "${CMAKE_CURRENT_BINARY_DIR}/${BASE}_table.c"
        )

        if(NOT EXISTS "${SOURCE_FILE}")
            message(FATAL_ERROR
                "Protothread source not found: ${SOURCE_FILE}"
            )
        endif()

        add_custom_command(
            OUTPUT
                "${GENERATED_H}"
                "${GENERATED_C}"
            COMMAND
                "${Python3_EXECUTABLE}"
                "${GEN_TABLE_PY}"
                "${SOURCE_FILE}"
                "${GENERATED_H}"
                "${GENERATED_C}"
            DEPENDS
                "${SOURCE_FILE}"
                "${GEN_TABLE_PY}"
            COMMENT "Generating ${BASE}_table.[ch]"
            VERBATIM
        )

        list(APPEND GENERATED_TABLE_SRCS
            "${GENERATED_C}"
        )
        list(APPEND GENERATED_TABLE_HDRS
            "${GENERATED_H}"
        )

    endforeach()

    # Table generation target

    add_custom_target(${TARGET_NAME}_generate_pt_tables
        DEPENDS
            ${GENERATED_TABLE_SRCS}
            ${GENERATED_TABLE_HDRS}
    )

    # Register generated source files

    target_sources(${TARGET_NAME} PRIVATE
        ${GENERATED_TABLE_SRCS}
    )

    # Include directory for generated header files

    target_include_directories(${TARGET_NAME} PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
    )

    # Generate tables before compiling the target

    add_dependencies(
        ${TARGET_NAME}
        ${TARGET_NAME}_generate_pt_tables
    )

endfunction()
