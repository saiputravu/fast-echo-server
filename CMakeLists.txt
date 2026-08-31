cmake_minimum_required(VERSION 3.26)
set (CMAKE_CXX_STANDARD 23)
set (CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS "-O3")

project(tcp-echo-server)

find_package(Threads REQUIRED)
link_libraries(Threads::Threads)

add_subdirectory(./lib)
add_subdirectory(./sender)
add_subdirectory(./recv_simple)
add_subdirectory(./recv_simple2)
add_subdirectory(./recv_epoll)
