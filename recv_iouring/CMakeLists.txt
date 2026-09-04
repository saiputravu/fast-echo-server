add_executable(iouring ./main.cpp ./setup.cpp)
target_link_libraries(iouring PRIVATE utils uring)
