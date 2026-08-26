cmake_minimum_required(VERSION 3.15)
project(KeySentry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/release)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/release)

add_executable(KeySentry WIN32
    src/Main.cpp
    src/Config.cpp
    src/KeyboardHook.cpp
    src/Overlay.cpp
    src/BossKey.cpp
    src/WinDGuard.cpp
    src/SettingsDialog.cpp
    src/Utils.h
    res/KeySentry.rc
)

target_include_directories(KeySentry PRIVATE src res)

target_link_libraries(KeySentry PRIVATE
    comctl32
    ole32
    oleaut32
    mmdevapi
    advapi32
    shell32
    user32
    gdi32
    kernel32
    psapi
)

target_compile_definitions(KeySentry PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    _WIN32_WINNT=0x0601
)

if(MSVC)
    target_compile_options(KeySentry PRIVATE /utf-8)
else()
    target_link_options(KeySentry PRIVATE -municode -s)
endif()
