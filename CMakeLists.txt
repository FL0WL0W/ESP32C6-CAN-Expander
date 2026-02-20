# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

# Create persistent stamp cache directory
set(PERSISTENT_STAMP_DIR "${CMAKE_SOURCE_DIR}/.cmake/fc-stamp")
file(MAKE_DIRECTORY "${PERSISTENT_STAMP_DIR}")

# Restore stamps from persistent cache
if(EXISTS "${PERSISTENT_STAMP_DIR}/")
  file(COPY "${PERSISTENT_STAMP_DIR}/" DESTINATION "${CMAKE_BINARY_DIR}/CMakeFiles/fc-stamp/")
endif()

message(STATUS "Checking for connection to GitHub...")
if (WIN32)
        set(ping_command "ping /n 1 /w 3 github.com")
else()
        set(ping_command "ping -c 1 -W 3 github.com")
endif()
execute_process(
        COMMAND ${ping_command}
        RESULT_VARIABLE NO_CONNECTION
)
if(NO_CONNECTION GREATER 0)
        set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
        message(WARNING "NO INTERNET CONNECTION: Using disconnected mode for FetchContent updates")
else()
        message(STATUS "GitHub connection established for FetchContent")
        set(FETCHCONTENT_UPDATES_DISCONNECTED OFF)
endif()

set(COMPONENTS esptool_py main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(main)

# Backup stamps to persistent cache
file(COPY "${CMAKE_BINARY_DIR}/CMakeFiles/fc-stamp/" DESTINATION "${PERSISTENT_STAMP_DIR}/")