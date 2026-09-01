cmake_minimum_required(VERSION 3.26)
set (CMAKE_CXX_STANDARD 23)
set (CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS "-O3")

project(tcp-echo-server)

find_package(Threads REQUIRED)
link_libraries(Threads::Threads)

find_package(ftxui QUIET CONFIG)
if(NOT ftxui_FOUND)
  include(FetchContent)
  FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG v7.0.3)
  FetchContent_MakeAvailable(ftxui)
endif()

add_subdirectory(./lib)
add_subdirectory(./sender)
add_subdirectory(./recv_simple)
add_subdirectory(./recv_epoll)
add_subdirectory(./recv_busy_poll)
add_subdirectory(./recv_busy_epoll)
add_subdirectory(./recv_iouring)
