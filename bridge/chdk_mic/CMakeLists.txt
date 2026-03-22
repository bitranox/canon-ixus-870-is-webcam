cmake_minimum_required(VERSION 3.20)
project(chdk_mic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

add_library(chdk_mic SHARED chdk_mic.cpp chdk_mic.def)

target_compile_definitions(chdk_mic PRIVATE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(chdk_mic PRIVATE ole32 oleaut32 strmiids uuid)
