cmake_minimum_required(VERSION 3.11)
project(zlib-replay C)

set(CMAKE_C_STANDARD 11)

set(TARGET zlib-replay)
add_executable(${TARGET} zlib-replay.c)
target_compile_options(${TARGET} PRIVATE -Wall -Wextra -pedantic -Werror)
target_link_libraries(${TARGET} z)
