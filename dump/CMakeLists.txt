cmake_minimum_required(VERSION 3.11)
project(zlib-dump C)

set(CMAKE_C_STANDARD 11)

if (DEFINED UTHASH_PREFIX)
    include_directories(${UTHASH_PREFIX}/include)
endif ()
set(TARGET z-dump)
add_library(${TARGET} SHARED zlib-dump.c)
# TODO: -pedantic
target_compile_options(${TARGET} PRIVATE -Wall -Wextra -Werror -pthread)
target_link_libraries(${TARGET} dl)
if (APPLE)
    target_link_libraries(${TARGET} z)
endif ()
configure_file(zlib-dump zlib-dump COPYONLY)
