cmake_minimum_required(VERSION 3.20)
project(glm-ime-e2e CXX)
set(CMAKE_CXX_STANDARD 17)
add_executable(e2e_host main.cpp)
target_compile_options(e2e_host PRIVATE /utf-8)
target_compile_definitions(e2e_host PRIVATE UNICODE _UNICODE)

