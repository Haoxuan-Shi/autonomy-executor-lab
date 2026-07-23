cmake_minimum_required(VERSION 3.20)
project(ael_package_consumer LANGUAGES CXX)

find_package(ael CONFIG REQUIRED)
add_executable(ael-package-consumer main.cpp)
target_link_libraries(ael-package-consumer PRIVATE ael::ael)
