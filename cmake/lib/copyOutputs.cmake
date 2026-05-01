function(copyOutputs TARGET_FOLDER)
    set(DLL_FOLDER "${TARGET_FOLDER}/F4SE/Plugins")

    message(STATUS "F4SE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
            VERBATIM
        )
    endif()

    set(PUBLIC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/public")
    set(PUBLIC_OUTPUT_DIR "${TARGET_FOLDER}")

    if(EXISTS "${PUBLIC_SOURCE_DIR}")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PUBLIC_SOURCE_DIR}" "${PUBLIC_OUTPUT_DIR}"
            VERBATIM
        )
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/FOV Slider F4SE.ini")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/FOV Slider F4SE.ini" "${DLL_FOLDER}/FOV Slider F4SE.ini"
            VERBATIM
        )
    endif()
endfunction()
