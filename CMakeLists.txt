# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# The ESP-IDF component graph can exceed Windows' CreateProcess command-line
# limit. Ask CMake's Ninja generator to put compiler and linker arguments in
# response files instead of passing every include path directly.
if(CMAKE_HOST_WIN32)
    set(ENV{CMAKE_NINJA_FORCE_RESPONSE_FILE} "1")
endif()

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.4.0")
project(xiaozhi)
