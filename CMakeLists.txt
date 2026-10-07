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

if(MSVC)
    add_compile_options(/W4 /permissive-)
endif()
add_compile_definitions(UNICODE _UNICODE)

# Finds the VMulti device; shared by the DLL and the setup program.
add_library(vmulti_device OBJECT src/vmulti_device.cpp)
target_include_directories(vmulti_device PUBLIC src)
target_link_libraries(vmulti_device PUBLIC hid setupapi)

add_library(xva SHARED
    src/xva.cpp
    src/backends.cpp
    src/vmulti_backend.cpp
)
target_include_directories(xva PUBLIC include)
target_compile_definitions(xva PRIVATE XVA_BUILD)
target_link_libraries(xva PRIVATE vmulti_device)

# Installs and removes the VMulti driver; always runs elevated.
add_executable(xva-setup setup/xva_setup.cpp)
target_link_libraries(xva-setup PRIVATE vmulti_device newdev bcrypt wintrust crypt32 urlmon ole32)
if(MSVC)
    target_link_options(xva-setup PRIVATE "/MANIFESTUAC:level='requireAdministrator'")
endif()
