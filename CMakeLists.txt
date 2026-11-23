# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# Record this firmware repository's revision in project_description.json's
# git_revision field. ESP-IDF defaults that field to IDF_VER (the ESP-IDF
# checkout revision), which is not the application revision.
execute_process(
    COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short HEAD
    OUTPUT_VARIABLE FW_GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" status --porcelain --untracked-files=no
    OUTPUT_VARIABLE FW_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(FW_GIT_REV)
    if(FW_GIT_STATUS)
        string(APPEND FW_GIT_REV "-dirty")
    endif()
    idf_build_set_property(IDF_VER "${FW_GIT_REV}")
    message(STATUS "Firmware git revision for project description: ${FW_GIT_REV}")
endif()

set(PROJECT_VER "2.7.4")
project(xiaozhi)
