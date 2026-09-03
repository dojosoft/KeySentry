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

# 32 位为主输出（KeySentry.exe，Win7 SP1+ 全兼容）；64 位另名 KeySentry-x64.exe
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(KeySentry PROPERTIES OUTPUT_NAME "KeySentry-x64")
endif()

if(MSVC)
    target_compile_options(KeySentry PRIVATE /utf-8)
    # 静态链接 VC++ 运行库，目标机器无需安装 VC++ Redistributable
    set_property(TARGET KeySentry PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    # -static: 全静态链接 libgcc/libstdc++/winpthread，产物仅依赖系统自带 DLL
    # gc-sections: 链接期剔除未引用代码段，大幅减小静态链接产物体积
    target_compile_options(KeySentry PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(KeySentry PRIVATE -municode -static -s -Wl,--gc-sections)
endif()
