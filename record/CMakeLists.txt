cmake_minimum_required(VERSION 3.11)
project(zlib-record C)

set(CMAKE_C_STANDARD 11)

if (DEFINED UTHASH_PREFIX)
    include_directories(${UTHASH_PREFIX}/include)
endif ()
set(TARGET z-record)
add_library(${TARGET} SHARED zlib-record.c)
# TODO: -pedantic
target_compile_options(${TARGET} PRIVATE -Wall -Wextra -Werror -pthread)
target_link_libraries(${TARGET} dl z)
configure_file(zlib-record zlib-record COPYONLY)
