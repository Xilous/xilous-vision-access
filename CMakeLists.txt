cmake_minimum_required(VERSION 3.20)
project(xilous_vision_access LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "xilous-vision-access targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the C++ runtime statically, so xva.dll loads on machines without the Visual C++
# Redistributable. Python ships only part of that runtime, not msvcp140.dll, which std::thread needs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(xva SHARED
    src/xva.cpp
    src/backends.cpp
    src/vmulti_backend.cpp
)
target_include_directories(xva
    PUBLIC include
    PRIVATE src
)
target_compile_definitions(xva PRIVATE XVA_BUILD UNICODE _UNICODE)
target_link_libraries(xva PRIVATE hid setupapi)

if(MSVC)
    target_compile_options(xva PRIVATE /W4 /permissive-)
endif()
