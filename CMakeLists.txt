cmake_minimum_required(VERSION 3.13.4)
project(siren)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

find_package(cxxopts CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(portaudio CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

aux_source_directory(src DIR_SRCS)
add_executable(siren ${DIR_SRCS})

target_link_libraries(siren PRIVATE cxxopts::cxxopts)
target_link_libraries(siren PRIVATE ${FFMPEG_LIBRARIES})
target_link_libraries(siren PRIVATE fmt::fmt)
target_link_libraries(siren PRIVATE portaudio_static)
target_link_libraries(siren PRIVATE spdlog::spdlog)