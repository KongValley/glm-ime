cmake_minimum_required(VERSION 3.20)
project(glm-ime-tip CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# libIME2（vendored, LGPL-2.1, 动态链接）
add_subdirectory(libIME2)

add_library(glm-ime-tip SHARED
    src/dllmain.cpp
    src/GlmTextService.cpp
    src/PipeClient.cpp
)

target_include_directories(glm-ime-tip PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/libIME2/src
)

# COM 导出：x64 无装饰；x86 __stdcall 有装饰名（@参数字节数），用 def 别名
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tip_exports.def" [=[
LIBRARY glm-ime-tip
EXPORTS
    DllGetClassObject   PRIVATE
    DllCanUnloadNow     PRIVATE
    DllRegisterServer   PRIVATE
    DllUnregisterServer PRIVATE
]=])
else()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tip_exports.def" [=[
LIBRARY glm-ime-tip
EXPORTS
    DllGetClassObject   = _DllGetClassObject@12     PRIVATE
    DllCanUnloadNow     = _DllCanUnloadNow@0        PRIVATE
    DllRegisterServer   = _DllRegisterServer@0      PRIVATE
    DllUnregisterServer = _DllUnregisterServer@0    PRIVATE
]=])
endif()

target_sources(glm-ime-tip PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tip_exports.def")
target_link_libraries(glm-ime-tip PRIVATE libIME2_static)
target_compile_options(glm-ime-tip PRIVATE /utf-8)
    # SEH 防护需要 /EHa（替换 CMake 默认注入的 /EHsc）
    foreach(flag_var CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_DEBUG)
        string(REPLACE "/EHsc" "/EHa" ${flag_var} "${${flag_var}}")
    endforeach()
    target_link_options(glm-ime-tip PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/Release/glm-ime-tip.map" "/MAPINFO:EXPORTS")
target_compile_definitions(glm-ime-tip PRIVATE
    _UNICODE UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

# 输出名 glm-ime-tip.dll
set_target_properties(glm-ime-tip PROPERTIES OUTPUT_NAME "glm-ime-tip")
