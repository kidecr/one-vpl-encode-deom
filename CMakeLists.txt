cmake_minimum_required(VERSION 3.8)
project(vpl)

find_package(VPL CONFIG)
find_package(OpenCV CONFIG)

include_directories(include)

add_library(vpl-module SHARED src/vpl-encode-module.cpp)
target_link_libraries(vpl-module vpl ${OpenCV_LIBS} pthread dl)

add_executable(vpl-demo src/vpl-encode-module-demo.cpp)
target_link_libraries(vpl-demo vpl-module ${OpenCV_LIBS})