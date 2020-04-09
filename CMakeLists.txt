cmake_minimum_required(VERSION 3.13.4)
project(siren)

aux_source_directory(src DIR_SRCS)
add_executable(siren ${DIR_SRCS})
