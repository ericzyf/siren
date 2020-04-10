cmake_minimum_required(VERSION 3.13.4)
project(siren)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

aux_source_directory(src DIR_SRCS)
add_executable(siren ${DIR_SRCS})
target_link_libraries(siren
    -lavformat
    -lavcodec
    -lavutil
    -lfmt
)
