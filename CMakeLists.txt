cmake_minimum_required(VERSION 3.28)
set (CMAKE_CXX_STANDARD 23)
set (CMAKE_EXPORT_COMPILE_COMMANDS ON)


project(tcp-echo-server)

add_subdirectory(./sender)
add_subdirectory(./recv_simple)
