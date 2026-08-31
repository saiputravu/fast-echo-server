add_executable(sender ./main.cpp ./sender.cpp ./cli.cpp)
target_link_libraries(sender PRIVATE utils ftxui::ftxui)
