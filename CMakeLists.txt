cmake_minimum_required(VERSION 3.15)
project(KeySentry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# 版本号单一来源：从 src/Version.h 解析后生成清单。
# 清单 XML 没有预处理器，之前需要手工同步 assemblyIdentity version；
# 这里改由 configure_file 注入，修改 Version.h 后重新配置即可同步。
# ============================================================
file(STRINGS "${CMAKE_SOURCE_DIR}/src/Version.h" _ks_version_lines
     REGEX "^#define KS_VERSION_(MAJOR|MINOR|PATCH|BUILD)[ \t]+[0-9]+")
foreach(_line IN LISTS _ks_version_lines)
    if(_line MATCHES "^#define KS_VERSION_MAJOR[ \t]+([0-9]+)")
        set(KS_VERSION_MAJOR "${CMAKE_MATCH_1}")
    elseif(_line MATCHES "^#define KS_VERSION_MINOR[ \t]+([0-9]+)")
        set(KS_VERSION_MINOR "${CMAKE_MATCH_1}")
    elseif(_line MATCHES "^#define KS_VERSION_PATCH[ \t]+([0-9]+)")
        set(KS_VERSION_PATCH "${CMAKE_MATCH_1}")
    elseif(_line MATCHES "^#define KS_VERSION_BUILD[ \t]+([0-9]+)")
        set(KS_VERSION_BUILD "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT DEFINED KS_VERSION_MAJOR OR NOT DEFINED KS_VERSION_MINOR OR
   NOT DEFINED KS_VERSION_PATCH OR NOT DEFINED KS_VERSION_BUILD)
    message(FATAL_ERROR "无法从 src/Version.h 解析版本号")
endif()
set(KS_VERSION_DOTTED "${KS_VERSION_MAJOR}.${KS_VERSION_MINOR}.${KS_VERSION_PATCH}.${KS_VERSION_BUILD}")
# Version.h 变化时重新运行 CMake，刷新生成的清单
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/Version.h")
configure_file(res/KeySentry.manifest.in
               "${CMAKE_CURRENT_BINARY_DIR}/KeySentry.manifest" @ONLY)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/release)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/release)

add_executable(KeySentry WIN32
    src/Main.cpp
    src/Config.cpp
    src/Lang.cpp
    src/KeyboardHook.cpp
    src/Overlay.cpp
    src/BossKey.cpp
    src/WinDGuard.cpp
    src/SettingsDialog.cpp
    src/SettingsDialogPages.cpp
    src/SettingsDialogSubDialogs.h
    src/SettingsDialogInlineCapture.cpp
    src/SettingsDialogInlineCapture.h
    src/SettingsDialogKeyboard.cpp
    src/SettingsDialogCapture.cpp
    src/SettingsDialogBindings.cpp
    src/SettingsDialogHotkey.cpp
    src/FluentTheme.cpp
    src/HookThread.h
    src/Version.h
    src/Utils.h
    res/KeySentry.manifest.in
    res/KeySentry.rc
)

# 生成目录放在 res 之前：RC 的 "KeySentry.manifest" 从生成目录解析
target_include_directories(KeySentry PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" src res)

# RC 通过 #include 引用 src/Version.h；RC 编译器不生成头文件依赖信息，
# 必须显式声明，否则只改版本号不会重编资源，产物 FileVersion 会保持旧值。
# 同时依赖生成的清单：版本变化会同时改变清单内容。
set_source_files_properties(res/KeySentry.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/src/Version.h;${CMAKE_CURRENT_BINARY_DIR}/KeySentry.manifest")

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
    WINVER=0x0601
    NTDDI_VERSION=0x06010000
)

# 产物命名统一为 KeySentry-<架构>.exe：
#   32 位 KeySentry-x86.exe（Win7 SP1+ 全兼容）、64 位 KeySentry-x64.exe
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(KeySentry PROPERTIES OUTPUT_NAME "KeySentry-x64")
else()
    set_target_properties(KeySentry PROPERTIES OUTPUT_NAME "KeySentry-x86")
endif()

if(MSVC)
    # /WX：警告即错误，防止新警告被长期忽略
    # /guard:cf：控制流防护（Win8.1+ 生效，旧系统安全忽略）
    # /sdl：启用额外安全开发生命周期检查
    target_compile_options(KeySentry PRIVATE /utf-8 /W4 /WX /guard:cf /sdl)
    # 禁止链接器自动生成清单：自定义清单已由 res/KeySentry.rc 的 RT_MANIFEST 嵌入，
    # 否则链接器会再嵌入一份 UAC stub 清单，形成两个同 ID 资源、生效项不确定
    target_link_options(KeySentry PRIVATE /MANIFEST:NO)
    # 静态链接 VC++ 运行库，目标机器无需安装 VC++ Redistributable
    set_property(TARGET KeySentry PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    # -static: 全静态链接 libgcc/libstdc++/winpthread，产物仅依赖系统自带 DLL
    # gc-sections: 链接期剔除未引用代码段，大幅减小静态链接产物体积
    target_compile_options(KeySentry PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(KeySentry PRIVATE -municode -static -s -Wl,--gc-sections)
endif()

# ============================================================
# 可选：代码签名（消除 Edge 下载拦截与 SmartScreen "已保护你的电脑/未知发布者"）
# ============================================================
# 背景：未签名的 exe 从网络下载后会被打上 MOTW 标记，SmartScreen 对"无签名 + 无信誉"
#       的文件一律拦截；且本程序含全局键鼠低级钩子、跨进程窗口样式修改、结束关联进程等
#       行为特征，Defender 启发式对未签名产物更易误报。唯一根治手段是受信任的代码签名
#       证书（OV 需累积信誉，EV 立即可信）。
#
# 支持三种签名形态（未设置任一参数时本步骤为空操作，普通构建不受影响）：
#   A. .pfx 文件（个人/自签名证书）：KS_SIGN_PFX + KS_SIGN_PWD
#   B. 证书存储中的证书（OV/EV 硬件令牌、USB Key 插卡签名）：KS_SIGN_SHA1=证书指纹
#   C. 云签名 / 自定义 dlib（如 Azure Trusted Signing、DigiCert KeyLocker）：
#      KS_SIGN_EXTRA_ARGS="/dlib <dlib路径> /dmdf <配置json>"
# 用法示例：
#   cmake -S . -B build-win32-msvc -DKS_SIGN_PFX="D:/cert/KeySentry.pfx" -DKS_SIGN_PWD="密码"
#   cmake -S . -B build-win32-msvc -DKS_SIGN_SHA1="AB12...（证书指纹）"
set(KS_SIGN_PFX "" CACHE FILEPATH "签名证书 .pfx 路径（与 KS_SIGN_SHA1 二选一）")
set(KS_SIGN_PWD "" CACHE STRING "pfx 证书密码（优先读环境变量 KS_SIGN_PWD，避免写入 CMakeCache）")
set(KS_SIGN_SHA1 "" CACHE STRING "证书指纹（证书存储中，含硬件令牌；与 KS_SIGN_PFX 二选一）")
set(KS_SIGN_EXTRA_ARGS "" CACHE STRING "追加给 signtool sign 的参数（云签名/dlib）")
set(KS_SIGN_TIMESTAMP "http://timestamp.digicert.com" CACHE STRING "RFC3161 时间戳服务器；留空则不时间戳")

# 密码优先从环境变量读取：避免明文进入命令行历史与 CMakeCache.txt
if(NOT KS_SIGN_PWD AND DEFINED ENV{KS_SIGN_PWD})
    set(KS_SIGN_PWD "$ENV{KS_SIGN_PWD}")
endif()

if(MSVC AND (KS_SIGN_PFX OR KS_SIGN_SHA1 OR KS_SIGN_EXTRA_ARGS))
    # 在 Windows SDK 安装目录中查找版本最新的 signtool.exe（不在 PATH 中时也能找到）
    file(GLOB _ks_signtool_candidates
        "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/signtool.exe")
    list(SORT _ks_signtool_candidates)
    list(REVERSE _ks_signtool_candidates)  # 排序后取版本号最大者
    if(_ks_signtool_candidates)
        list(GET _ks_signtool_candidates 0 KS_SIGNTOOL)
    else()
        find_program(KS_SIGNTOOL NAMES signtool.exe signtool
            DOC "signtool.exe 路径（Windows SDK）")
    endif()
    if(KS_SIGNTOOL)
        set(_ks_sign_args sign /fd SHA256)
        if(KS_SIGN_PFX)
            list(APPEND _ks_sign_args /f "${KS_SIGN_PFX}" /p "${KS_SIGN_PWD}")
        elseif(KS_SIGN_SHA1)
            list(APPEND _ks_sign_args /sha1 "${KS_SIGN_SHA1}")
        endif()
        if(KS_SIGN_EXTRA_ARGS)
            separate_arguments(_ks_extra NATIVE_COMMAND "${KS_SIGN_EXTRA_ARGS}")
            list(APPEND _ks_sign_args ${_ks_extra})
        endif()
        if(KS_SIGN_TIMESTAMP)
            list(APPEND _ks_sign_args /tr "${KS_SIGN_TIMESTAMP}" /td SHA256)
        endif()
        list(APPEND _ks_sign_args "$<TARGET_FILE:KeySentry>")

        add_custom_command(TARGET KeySentry POST_BUILD
            COMMAND "${KS_SIGNTOOL}" ${_ks_sign_args}
            COMMENT "代码签名 $<TARGET_FILE_NAME:KeySentry>"
            VERBATIM)
    else()
        message(WARNING "已启用代码签名但未找到 signtool.exe，已跳过；请安装 Windows SDK 或将其加入 PATH")
    endif()
endif()

# ============================================================
# 自动化回归测试：配置 UTF-16/转义/绑定校验/上限/超大保存拦截
# 运行：cmake --build build --config Release && ctest --test-dir build -C Release
# ============================================================
include(CTest)
if(BUILD_TESTING)
    add_executable(KeySentryTests
        tests/ConfigTests.cpp
        src/Config.cpp
    )
    target_include_directories(KeySentryTests PRIVATE src res)
    target_compile_definitions(KeySentryTests PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        _WIN32_WINNT=0x0601
        WINVER=0x0601
        NTDDI_VERSION=0x06010000
    )
    if(MSVC)
        target_compile_options(KeySentryTests PRIVATE /utf-8 /W4 /WX)
        set_property(TARGET KeySentryTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    else()
        target_compile_options(KeySentryTests PRIVATE -ffunction-sections -fdata-sections)
        target_link_options(KeySentryTests PRIVATE -static)
    endif()
    target_link_libraries(KeySentryTests PRIVATE
        user32
        kernel32
        psapi
        advapi32
        shell32
        ole32
        oleaut32
    )
    set_target_properties(KeySentryTests PROPERTIES
        OUTPUT_NAME "KeySentryTests"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME ConfigTests COMMAND KeySentryTests)

    # 语言层：词典重复键/翻译直通（R8-A1）
    add_executable(KeySentryLangTests
        tests/LangTests.cpp
        src/Lang.cpp
    )
    target_include_directories(KeySentryLangTests PRIVATE src res)
    target_compile_definitions(KeySentryLangTests PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        _WIN32_WINNT=0x0601
        WINVER=0x0601
        NTDDI_VERSION=0x06010000
    )
    if(MSVC)
        target_compile_options(KeySentryLangTests PRIVATE /utf-8 /W4 /WX)
        set_property(TARGET KeySentryLangTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    endif()
    target_link_libraries(KeySentryLangTests PRIVATE
        user32
        comctl32
        kernel32
    )
    set_target_properties(KeySentryLangTests PROPERTIES
        OUTPUT_NAME "KeySentryLangTests"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME LangTests COMMAND KeySentryLangTests)

    # 键盘钩子纯逻辑：映射释放/模拟抬起的目标键去重、修饰键顺序与共享（R15-A01）
    add_executable(KeySentryHookLogicTests
        tests/HookLogicTests.cpp
    )
    target_include_directories(KeySentryHookLogicTests PRIVATE src res)
    target_compile_definitions(KeySentryHookLogicTests PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        _WIN32_WINNT=0x0601
        WINVER=0x0601
        NTDDI_VERSION=0x06010000
    )
    if(MSVC)
        target_compile_options(KeySentryHookLogicTests PRIVATE /utf-8 /W4 /WX)
        set_property(TARGET KeySentryHookLogicTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    endif()
    target_link_libraries(KeySentryHookLogicTests PRIVATE
        user32
        kernel32
    )
    set_target_properties(KeySentryHookLogicTests PROPERTIES
        OUTPUT_NAME "KeySentryHookLogicTests"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME HookLogicTests COMMAND KeySentryHookLogicTests)
endif()
