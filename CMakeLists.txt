cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------- #
#                    vcpkg setup                       #
# ---------------------------------------------------- #

macro(set_from_environment VARIABLE)
    if (NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
        set(${VARIABLE} $ENV{${VARIABLE}})
    endif ()
endmacro()

set_from_environment(VCPKG_ROOT)
set_from_environment(Fallout4Path)

if (DEFINED VCPKG_ROOT)
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
else()
    message(WARNING "VCPKG_ROOT is not set. Continuing without vcpkg.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")

# ---------------------------------------------------- #
#                Imports and definitions               #
# ---------------------------------------------------- #

include(cmake/lib/copyOutputs.cmake)
include(cmake/lib/automaticGameFolderOutput.cmake)
include(cmake/lib/autoCollectSources.cmake)

add_src_folder(src)
add_include_folder(include)

set(AUTHOR_NAME "Robert")
set(PRODUCT_NAME "FOVSliderF4SE")
set(BEAUTIFUL_NAME "FOV Slider F4SE")

project(${PRODUCT_NAME} VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# Boost MODULE finder from CommonLibF4 umbrella
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../PluginTemplate/CommonLibF4/cmake")

# ---------------------------------------------------- #
#                      Resources                       #
# ---------------------------------------------------- #

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    @ONLY)

set(RESOURCE_FILE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
set(RESOURCE_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/version.res)

add_custom_command(
    OUTPUT ${RESOURCE_OUTPUT}
    COMMAND rc /fo ${RESOURCE_OUTPUT} ${RESOURCE_FILE}
    DEPENDS ${RESOURCE_FILE}
)

add_custom_target(Resource ALL DEPENDS ${RESOURCE_OUTPUT})

# ---------------------------------------------------- #
#                  CommonLibF4 Setup                   #
# ---------------------------------------------------- #

if (NOT TARGET CommonLibF4)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../PluginTemplate/CommonLibF4/CommonLibF4" CommonLibF4)
endif()

# ---------------------------------------------------- #
#                Plugin Configuration                  #
# ---------------------------------------------------- #

find_package(spdlog REQUIRED CONFIG)

source_group(
  TREE ${CMAKE_CURRENT_SOURCE_DIR}
  FILES ${headers} ${sources}
)

add_library(${PROJECT_NAME} SHARED
    ${headers}
    ${sources}
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    BEAUTIFUL_NAME="${BEAUTIFUL_NAME}"
    MOD_NAME="${BEAUTIFUL_NAME}"
    _UNICODE
    UNICODE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
)

set(header_dirs "")

foreach(header ${headers})
    get_filename_component(dir ${header} DIRECTORY)
    list(APPEND header_dirs ${dir})
endforeach()

list(REMOVE_DUPLICATES header_dirs)

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
    ${header_dirs}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        CommonLibF4::CommonLibF4
        spdlog::spdlog
)

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /sdl /utf-8 /Zi /permissive- /Zc:preprocessor /EHsc /W4 /WX-
        /wd4099 /wd4100 /wd4189 /wd4244 /wd4302 /wd4311 /wd5054
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        "$<$<CONFIG:DEBUG>:/INCREMENTAL>"
        "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
    )
endif()

# ---------------------------------------------------- #
#                        Output                        #
# ---------------------------------------------------- #

# Force the DLL into Compile/F4SE/Plugins for ALL configurations. Setting
# CMAKE_RUNTIME_OUTPUT_DIRECTORY isn't enough on multi-config generators
# like Visual Studio after the target has already been created above, so
# we drive it through target properties.
set(FOV_SLIDER_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${FOV_SLIDER_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${FOV_SLIDER_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${FOV_SLIDER_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY         "${FOV_SLIDER_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG   "${FOV_SLIDER_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${FOV_SLIDER_OUTPUT_DIR}"
    PDB_OUTPUT_DIRECTORY             "${FOV_SLIDER_OUTPUT_DIR}"
    PDB_OUTPUT_DIRECTORY_DEBUG       "${FOV_SLIDER_OUTPUT_DIR}"
    PDB_OUTPUT_DIRECTORY_RELEASE     "${FOV_SLIDER_OUTPUT_DIR}"
)

# Copy default INI alongside DLL for first-time installs
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/FOV Slider F4SE.ini"
    "${FOV_SLIDER_OUTPUT_DIR}/FOV Slider F4SE.ini"
    COPYONLY
)

option(COPY_BUILD "Copy the build output to the Fallout 4 directory." OFF)
if (COPY_BUILD AND DEFINED Fallout4Path)
    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${Fallout4Path}/Data/F4SE/Plugins/"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${Fallout4Path}/Data/F4SE/Plugins/"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/FOV Slider F4SE.ini" "${Fallout4Path}/Data/F4SE/Plugins/"
    )
endif()

set(fallout4_mods_output true)
automaticGameFolderOutput(fallout4_mods_output)
