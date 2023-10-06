cmake_minimum_required(VERSION 3.8)
project(vpl)

find_package(VPL CONFIG)
find_package(OpenCV CONFIG)


add_library(vpl-module SHARED vpl-encode-module.cpp)
target_link_libraries(vpl-module vpl ${OpenCV_LIBS} pthread dl)

add_executable(vpl-demo vpl-encode-module-demo.cpp)
target_link_libraries(vpl-demo vpl-module ${OpenCV_LIBS})