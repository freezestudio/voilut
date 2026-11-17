# ============================================================
# voilut —— 16 位影像的窗宽窗位显示引擎 + Windower 应用
# ============================================================
# 分层结构：
#   core/     通用基础设施（WebGPU 环境、协程、着色器读取、窗口常量）
#   imaging/  核心库：解码（TIFF / DICOM）→ 统计 → 窗宽窗位 → 显示
#   apps/     windower：调窗为核心的可执行程序（Dear ImGui UI）
#   shaders/  WGSL 着色器（image.wgsl 承担窗宽窗位映射）
#
# 构建：
#   cmake -B build -G Ninja
#   cmake --build build
#   ./build/windower <图像或目录>
# ============================================================

cmake_minimum_required(VERSION 3.28)

# ------------------------------------------------------------
# Windows：自动接入 vcpkg 工具链
# ------------------------------------------------------------
# Windows 没有系统级 libtiff / GDCM，统一由 vcpkg 提供。
# 若设置了 VCPKG_ROOT，则自动加载其 toolchain；
# 手动指定（优先级更高）：-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
if(WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "vcpkg toolchain file")
  message(STATUS "使用 vcpkg 工具链：${CMAKE_TOOLCHAIN_FILE}")
endif()

project(voilut VERSION 0.1.0 LANGUAGES CXX)

# ------------------------------------------------------------
# 编译器运行时库 RPATH（仅 Linux/macOS）
# ------------------------------------------------------------
# 本地编译器可能自带较新的 libstdc++，而系统自带的 libstdc++ 可能过旧
# （缺 GLIBCXX_3.4.35/36）。终端中可通过 LD_LIBRARY_PATH 找到，但双击启动时
# 不继承该变量，窗口会静默退出。这里把编译器运行时目录写进 RPATH。
if(NOT APPLE AND NOT WIN32)
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.so.6
    OUTPUT_VARIABLE _compiler_libstdcpp
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  get_filename_component(_compiler_runtime_dir "${_compiler_libstdcpp}" DIRECTORY)
  if(_compiler_runtime_dir AND IS_DIRECTORY "${_compiler_runtime_dir}")
    set(CMAKE_BUILD_RPATH "${_compiler_runtime_dir}")
    set(CMAKE_INSTALL_RPATH "${_compiler_runtime_dir}")
    message(STATUS "编译器运行时目录（写入 RPATH）：${_compiler_runtime_dir}")
  endif()
endif()

# ------------------------------------------------------------
# C++ 标准
# ------------------------------------------------------------
# GCC/Clang：-std=c++26（std::expected / std::println / deducing this / = delete("原因")）。
# MSVC 没有 /std:c++26，C++23 档会映射为 /std:c++latest。
if(MSVC)
  set(CMAKE_CXX_STANDARD 23)
else()
  set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 警告只加给本项目目标；不要用全局 add_compile_options（会污染 Dawn 子模块）。
if(MSVC)
  # /utf-8：源码与执行字符集统一为 UTF-8。MSVC 在中文区域默认按 GBK（代码页 936）
  # 读取无 BOM 的 UTF-8 源码，会把中文注释 / 字符串误解析成语法错误。
  set(PROJECT_WARNINGS /W4 /permissive- /utf-8)
else()
  set(PROJECT_WARNINGS -Wall -Wextra -Wpedantic)
endif()

# ------------------------------------------------------------
# 纯计算库与单元测试（可无依赖构建）
# ------------------------------------------------------------
# 纯计算部分单独成库：不链接 Dawn / GDCM / libtiff，可以在无 GPU、无第三方依赖
# 的环境下直接构建与测试，也被 windower 与单元测试共用（避免重复编译）。
# CI 用 `-DVOILUT_MODEL_ONLY=ON` 只构建这一层，秒级反馈。
option(VOILUT_MODEL_ONLY "只构建 voilut_model 与单元测试（无 Dawn/GDCM/TIFF）" OFF)

add_library(voilut_model STATIC
  imaging/src/image_model.cxx
)
target_include_directories(voilut_model PUBLIC imaging/include)
target_compile_options(voilut_model PRIVATE ${PROJECT_WARNINGS})

include(CTest)
if(BUILD_TESTING)
  add_executable(window_model_test tests/window_model_test.cxx)
  target_link_libraries(window_model_test PRIVATE voilut_model)
  target_compile_options(window_model_test PRIVATE ${PROJECT_WARNINGS})
  add_test(NAME window_model_test COMMAND window_model_test)
endif()

if(VOILUT_MODEL_ONLY)
  message(STATUS "VOILUT_MODEL_ONLY：只构建 voilut_model 与 window_model_test")
  return()
endif()

# ------------------------------------------------------------
# 依赖查找
# ------------------------------------------------------------
# 发行选项：静态链接除 glibc 与系统显卡驱动外的全部依赖，产物只有一个可执行文件。
# glibc 不能静态化 —— Dawn / GLFW 用 dlopen 加载 libvulkan / libX11 / libwayland，
# 静态 glibc 下会启动即崩溃（已实测）。
option(VOILUT_PORTABLE "静态链接 libtiff / zlib / libstdc++ 等（单文件发行）" OFF)

# Windows 单文件发行的两个前提：静态运行库（/MT）与静态 vcpkg triplet。
# 两者任一没满足，exe 旁边就会多出 vcruntime140.dll 或依赖的 DLL。
if(VOILUT_PORTABLE AND WIN32)
  if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING
        "VOILUT_PORTABLE：静态运行库 /MT" FORCE)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
      message(WARNING
        "VOILUT_PORTABLE 建议用 -DCMAKE_BUILD_TYPE=Release：vcpkg 静态库是 Release 构建")
    endif()
  else()
    message(WARNING
      "VOILUT_PORTABLE 在 Windows 目前只支持 MSVC（/MT + 静态 vcpkg triplet），\n"
      "当前编译器为 ${CMAKE_CXX_COMPILER_ID}，请手确保静态运行库与静态依赖。")
  endif()
  if(DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TARGET_TRIPLET MATCHES "static$")
    message(WARNING
      "VOILUT_PORTABLE 需要静态 CRT 的 vcpkg triplet（如 x64-windows-static），\n"
      "当前为 ${VCPKG_TARGET_TRIPLET}：产物会依赖 MSVC 运行库或第三方 DLL。")
  endif()
endif()

# TIFF：16 位灰度图像解码（系统包 / vcpkg 均可提供 TIFF::TIFF）。
if(VOILUT_PORTABLE AND UNIX AND NOT APPLE)
  # FindTIFF 选到 .a 时不会带上 libtiff 自身的压缩库依赖，改用 pkg-config --static
  # 取完整依赖链，并逐个换成静态库（否则 -lxxx 仍优先命中 .so）。
  find_package(PkgConfig REQUIRED)
  execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --static --libs libtiff-4
    OUTPUT_VARIABLE _tiff_static_flags
    ERROR_VARIABLE _tiff_pc_error
    RESULT_VARIABLE _tiff_pc_result
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _tiff_pc_result EQUAL 0)
    message(FATAL_ERROR "pkg-config 未找到 libtiff-4：${_tiff_pc_error}")
  endif()
  separate_arguments(_tiff_static_flags UNIX_COMMAND "${_tiff_static_flags}")
  set(TIFF_LINK_LIBRARIES "")
  foreach(_flag IN LISTS _tiff_static_flags)
    if(_flag MATCHES "^-l(.+)$")
      set(_name "${CMAKE_MATCH_1}")
      if(_name MATCHES "^(m|dl|rt|pthread|c)$")
        # glibc 自带，必须保持动态：libm.a / libpthread.a 引用 glibc 内部符号
        # （如 _dl_x86_cpu_features），与动态 libc 链接会报未定义引用。
        list(APPEND TIFF_LINK_LIBRARIES "${_name}")
      else()
        find_library(VOILUT_STATIC_${_name} NAMES "lib${_name}.a")
        if(VOILUT_STATIC_${_name})
          list(APPEND TIFF_LINK_LIBRARIES "${VOILUT_STATIC_${_name}}")
        else()
          message(FATAL_ERROR
            "缺少静态库 lib${_name}.a（libtiff 的压缩支持依赖），请安装对应的开发包")
        endif()
      endif()
    endif()
  endforeach()
  message(STATUS "VOILUT_PORTABLE：libtiff 静态依赖 → ${TIFF_LINK_LIBRARIES}")

  # GDCM 以 GDCM_USE_SYSTEM_ZLIB=ON 链接系统 zlib；这里指定静态库，
  # 避免最终产物仍 NEEDED libz.so.1。必须在 add_subdirectory(GDCM) 之前设置。
  find_library(VOILUT_ZLIB_STATIC NAMES "libz.a" REQUIRED)
  set(ZLIB_LIBRARY "${VOILUT_ZLIB_STATIC}" CACHE FILEPATH "静态 zlib（VOILUT_PORTABLE）" FORCE)
  set(ZLIB_LIBRARY_RELEASE "${VOILUT_ZLIB_STATIC}" CACHE FILEPATH "静态 zlib（VOILUT_PORTABLE）" FORCE)
  set(ZLIB_USE_STATIC_LIBS ON CACHE BOOL "静态 zlib（VOILUT_PORTABLE）" FORCE)
else()
  find_package(TIFF REQUIRED)
  set(TIFF_LINK_LIBRARIES TIFF::TIFF)
endif()

# ------------------------------------------------------------
# Dawn 配置
# ------------------------------------------------------------
# Dawn 能力很多，全部启用会显著增加编译时间和依赖数量；这里仅启用当前平台所需的 GPU 后端。
set(DAWN_FETCH_DEPENDENCIES OFF)
set(DAWN_ENABLE_OPENGLES OFF)      # 不需要 GLES 后端
set(DAWN_ENABLE_DESKTOP_GL OFF)    # 不需要桌面 OpenGL 后端
if(WIN32)
  set(DAWN_ENABLE_VULKAN OFF)      # Windows 使用 D3D12，无需安装 Vulkan SDK
endif()
set(DAWN_ENABLE_SPIRV_VALIDATION OFF)
set(DAWN_BUILD_TESTS OFF)
set(DAWN_BUILD_PROTOBUF OFF)
set(TINT_BUILD_TESTS OFF)
set(TINT_BUILD_IR_BINARY OFF)
set(TINT_BUILD_SPV_READER OFF)
set(TINT_BUILD_SPV_WRITER ON)      # WGSL → SPIR-V（Vulkan 后端需要）
set(TINT_BUILD_GLSL_WRITER OFF)
set(TINT_BUILD_GLSL_VALIDATOR OFF)
set(TINT_BUILD_CMD_TOOLS OFF)
# 不启用 Dawn 自带的 DXC：否则 Windows 运行时还需要随包提供
# dxcompiler.dll / dxil.dll，破坏单文件发行。关闭后 D3D12 走系统自带的
# d3dcompiler_47.dll（FXC，Shader Model 5.1），Win10+ 均具备。
set(DAWN_USE_BUILT_DXC OFF CACHE BOOL "不启用内置 DXC（Windows 单文件发行）" FORCE)

# Dawn 缺失时自动调用 scripts/install_dawn.{sh,ps1} 获取（约 5~20 分钟，取决于网络）。
set(DAWN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dawn")
option(DAWN_AUTO_INSTALL "缺少 Dawn 时自动调用 scripts/install_dawn.* 获取" ON)

if(NOT EXISTS "${DAWN_DIR}/CMakeLists.txt")
  message(STATUS "未找到 Dawn（${DAWN_DIR}）")
  if(DAWN_AUTO_INSTALL)
    if(WIN32)
      find_program(PWSH_EXECUTABLE NAMES pwsh powershell)
      if(PWSH_EXECUTABLE)
        message(STATUS "调用 scripts/install_dawn.ps1 获取 Dawn（可能需要几分钟）...")
        execute_process(
          COMMAND "${PWSH_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
                  -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/install_dawn.ps1"
          RESULT_VARIABLE DAWN_INSTALL_RESULT)
      endif()
    else()
      find_program(BASH_EXECUTABLE NAMES bash)
      if(BASH_EXECUTABLE)
        message(STATUS "调用 scripts/install_dawn.sh 获取 Dawn（可能需要几分钟）...")
        execute_process(
          COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/install_dawn.sh"
          RESULT_VARIABLE DAWN_INSTALL_RESULT)
      endif()
    endif()
  endif()
  if(DEFINED DAWN_INSTALL_RESULT AND NOT DAWN_INSTALL_RESULT EQUAL 0)
    message(WARNING "Dawn 自动安装脚本返回非零退出码：${DAWN_INSTALL_RESULT}")
  endif()
  if(NOT EXISTS "${DAWN_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "缺少 Dawn。请先运行：\n"
      "  Linux/macOS : bash scripts/install_dawn.sh\n"
      "  Windows     : powershell -ExecutionPolicy Bypass -File scripts\\install_dawn.ps1")
  endif()
endif()

# ------------------------------------------------------------
# Dawn 内嵌 abseil 的 CRT 选择（单文件发行）
# ------------------------------------------------------------
# abseil 的 CMakeLists 会用 ABSL_MSVC_STATIC_RUNTIME 覆盖 CMAKE_MSVC_RUNTIME_LIBRARY。
# 它默认 OFF（/MD），与 VOILUT_PORTABLE 的 /MT 链接会报 LNK2038
# （RuntimeLibrary 不匹配），因此便携构建时必须改为静态 CRT。
if(VOILUT_PORTABLE AND WIN32)
  set(ABSL_MSVC_STATIC_RUNTIME ON CACHE BOOL "abseil 使用静态 CRT（VOILUT_PORTABLE）" FORCE)
  # 单文件发行不随附 d3dcompiler_47.dll，让 Dawn 从 System32 加载。
  # Dawn 默认对裸文件名使用 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR，会返回 ERROR_INVALID_PARAMETER。
  set(DAWN_FORCE_SYSTEM_COMPONENT_LOAD ON CACHE BOOL
      "Dawn 从系统目录加载 d3dcompiler_47.dll（VOILUT_PORTABLE）" FORCE)
endif()

add_subdirectory("third_party/dawn" EXCLUDE_FROM_ALL)

# ------------------------------------------------------------
# core：通用基础设施（静态库）
# ------------------------------------------------------------
add_library(voilut_core STATIC
  core/src/context.cxx
  core/src/shader_loader.cxx
)
target_include_directories(voilut_core PUBLIC core/include)
target_compile_options(voilut_core PRIVATE ${PROJECT_WARNINGS})

# 着色器目录：不写入源码树的绝对路径，避免构建产物泄露构建机路径。
# 运行时优先查找可执行文件旁的 shaders/；开发构建由下方的 windower_shaders 目标复制生成。
target_compile_definitions(voilut_core PRIVATE SHADERS_DIR="shaders")

# ------------------------------------------------------------
# 着色器嵌入（单文件发行）
# ------------------------------------------------------------
# 开启后把 shaders/*.wgsl 转成 C++ 字节数组编进二进制，运行时不再读文件。
# 生成头文件而不是用 #embed：MSVC 对 #embed 的支持不如 GCC/Clang 确定，
# 生成方案在所有编译器上行为一致（GCC 17 亦可用 #embed，二者等价）。
option(VOILUT_EMBED_SHADERS "把 shaders/*.wgsl 编入二进制（发行时不需附带 shaders/）" OFF)
if(VOILUT_EMBED_SHADERS)
  # 文件名 → 生成的 C++ 数组名（新增着色器时在此登记）
  set(VOILUT_EMBEDDED_SHADERS "image.wgsl=kEmbeddedImageWgsl")
  set(_embedded_header "${CMAKE_BINARY_DIR}/generated/embedded_shaders.h")
  set(_embedded_content [=[// ============================================================
// 由 CMakeLists.txt 生成，请勿手改：shaders/*.wgsl 的原始字节。
// ============================================================
#pragma once

#include <cstddef>
#include <string_view>

namespace voilut::embedded
{
    /// @brief 一段内嵌着色器（名称与字节内容）。
    struct ShaderSource
    {
        std::string_view name;
        const unsigned char *data;
        std::size_t size;
    };
]=])
  set(_embedded_entries "")
  foreach(_mapping IN LISTS VOILUT_EMBEDDED_SHADERS)
    string(REPLACE "=" ";" _pair "${_mapping}")
    list(GET _pair 0 _shader_file)
    list(GET _pair 1 _shader_symbol)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader_file}" _shader_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _shader_bytes "${_shader_hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n        " _shader_bytes "${_shader_bytes}")
    string(APPEND _embedded_content
      "\n    /// @brief ${_shader_file} 的原始字节。\n    inline constexpr unsigned char ${_shader_symbol}[] = {\n        ${_shader_bytes}\n    };\n")
    string(APPEND _embedded_entries "        {\"${_shader_file}\", ${_shader_symbol}, sizeof(${_shader_symbol})},\n")
    # shader 改动后重新配置，保证嵌入内容不过期
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader_file}")
  endforeach()
  string(APPEND _embedded_content
    "\n    /// @brief 全部内嵌着色器。\n    inline constexpr ShaderSource kShaders[] = {\n${_embedded_entries}    };\n} // namespace voilut::embedded\n")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
  file(WRITE "${_embedded_header}" "${_embedded_content}")
  target_include_directories(voilut_core PRIVATE "${CMAKE_BINARY_DIR}/generated")
  target_compile_definitions(voilut_core PRIVATE VOILUT_EMBED_SHADERS=1)
  message(STATUS "VOILUT_EMBED_SHADERS：着色器嵌入 ${_embedded_header}")
endif()

# Dawn 的 WebGPU 实现 + GLFW 的 WebGPU 适配层。
target_link_libraries(voilut_core PUBLIC webgpu_dawn webgpu_glfw glfw)

# ------------------------------------------------------------
# GDCM：DICOM 解码（含压缩传输语法）
# ------------------------------------------------------------
# Linux/macOS：源码位于 third_party/GDCM（不入库，由 scripts/install_gdcm.sh 获取）。
# Windows：使用 vcpkg 的 GDCM（避免在 MSVC 下从源码构建旧版代码）。
if(WIN32)
  find_package(GDCM CONFIG REQUIRED)
else()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/GDCM/CMakeLists.txt")
    message(FATAL_ERROR "缺少 GDCM 源码。请先运行：\n  bash scripts/install_gdcm.sh")
  endif()

  set(GDCM_BUILD_APPLICATIONS OFF CACHE BOOL "" FORCE)
  set(GDCM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GDCM_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(GDCM_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(GDCM_DOCUMENTATION OFF CACHE BOOL "" FORCE)
  set(GDCM_BUILD_DOCBOOK_MANPAGES OFF CACHE BOOL "" FORCE)
  set(GDCM_USE_SYSTEM_ZLIB ON CACHE BOOL "" FORCE)

  # GDCM 使用较旧的代码基线，按 C++14 编译；退出子目录后恢复本项目标准（C++26）。
  set(_project_cxx_standard ${CMAKE_CXX_STANDARD})
  set(CMAKE_CXX_STANDARD 14)
  add_subdirectory(third_party/GDCM EXCLUDE_FROM_ALL)
  set(CMAKE_CXX_STANDARD ${_project_cxx_standard})

  # GDCM 会把输出目录写入 cache，导致本项目目标输出到 third_party/GDCM/bin。
  unset(EXECUTABLE_OUTPUT_PATH CACHE)
  unset(LIBRARY_OUTPUT_PATH CACHE)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "")
endif()

# ------------------------------------------------------------
# imaging：窗宽窗位核心库（静态库）
# ------------------------------------------------------------
# 纯计算部分见文件开头的 voilut_model（可在无依赖环境下单独构建）。
# 职责：解码 16 位像素 → 统计（直方图 / p1 p99）→ 窗设置 → GPU 渲染管线。
add_library(voilut STATIC
  imaging/src/image_renderer.cxx
  imaging/src/image_source.cxx
  imaging/src/viewport.cxx
  imaging/src/orientation.cxx
  imaging/src/decode/image_decode.cxx
  imaging/src/decode/tiff_decode.cxx
  imaging/src/decode/tiff_region_source.cxx
  imaging/src/decode/dicom_region_source.cxx
  imaging/src/decode/region_source_factory.cxx
  imaging/src/decode/dicom_decode.cxx
)
target_include_directories(voilut PUBLIC imaging/include)
target_compile_options(voilut PRIVATE ${PROJECT_WARNINGS})

# Linux/macOS：GDCM 以源码子目录方式构建，头文件分散在各子目录，需手动补充。
# Windows：vcpkg 的 gdcmMSFF imported target 自带 include 目录。
if(NOT WIN32)
  target_include_directories(voilut PRIVATE
    third_party/GDCM/Source/Common
    third_party/GDCM/Source/DataStructureAndEncodingDefinition
    third_party/GDCM/Source/MediaStorageAndFileFormat
    third_party/GDCM/Source/MessageExchangeDefinition
    third_party/GDCM/Source/DataDictionary
    third_party/GDCM/Source/InformationObjectDefinition
    ${CMAKE_BINARY_DIR}/third_party/GDCM/Source/Common)
endif()

target_link_libraries(voilut PUBLIC voilut_core PRIVATE voilut_model ${TIFF_LINK_LIBRARIES} gdcmMSFF)

# ------------------------------------------------------------
# Dear ImGui：Windower 的 UI（GLFW + WebGPU/Dawn 后端）
# ------------------------------------------------------------
# 源码位于 third_party/imgui（不入库，由 scripts/install_imgui.sh 获取）。
# 第三方代码不应用本项目的严格警告。
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.h")
  message(FATAL_ERROR
    "缺少 Dear ImGui 源码。请先运行：\n"
    "  bash scripts/install_imgui.sh\n"
    "并把版本保持在 1.92.x（imgui_impl_wgpu 的初始化结构体字段会随小版本变化）。")
endif()

add_library(imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_glfw.cpp
  third_party/imgui/backends/imgui_impl_wgpu.cpp
)
target_include_directories(imgui PUBLIC
  third_party/imgui
  third_party/imgui/backends)
target_link_libraries(imgui PUBLIC voilut_core)
# imgui_impl_wgpu 需要显式选择后端实现（Dawn）。
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
if(MSVC)
  target_compile_options(imgui PRIVATE /w)
else()
  target_compile_options(imgui PRIVATE -w)
endif()

# ------------------------------------------------------------
# windower：可执行程序
# ------------------------------------------------------------
add_executable(windower
  apps/windower.cxx
  apps/app_ops.cxx
  apps/image_io.cxx
  apps/input.cxx
  apps/ui.cxx
)
target_compile_options(windower PRIVATE ${PROJECT_WARNINGS})
target_compile_definitions(windower PRIVATE VOILUT_VERSION="${PROJECT_VERSION}")
target_link_libraries(windower PRIVATE voilut imgui)

if(VOILUT_PORTABLE AND UNIX AND NOT APPLE)
  # libstdc++ / libgcc 静态化：去掉对构建机 /opt/gcc-17 的 RUNPATH 依赖。
  target_link_options(windower PRIVATE -static-libstdc++ -static-libgcc)
  # 单文件发行不需要 RUNPATH（glibc 由系统 ld.so 解析）。
  set_target_properties(windower PROPERTIES BUILD_RPATH "" INSTALL_RPATH "")
endif()

# 着色器随可执行文件分发：运行时通过可执行文件旁的 shaders/ 查找，
# 不依赖源码树的绝对路径。每次构建都同步一次，保证修改 WGSL 后立即生效。
# 嵌入模式（VOILUT_EMBED_SHADERS=ON）下不再复制，产物目录只有一个可执行文件。
if(NOT VOILUT_EMBED_SHADERS)
  add_custom_target(windower_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:windower>/shaders"
    COMMENT "复制 shaders/ 到可执行文件目录")
  add_dependencies(windower windower_shaders)
endif()

# Windows：Dawn 的 D3D12 后端运行时需要 d3dcompiler_47.dll（由 Windows SDK 提供）。
# 单文件发行（VOILUT_PORTABLE）时不复制：Windows 10 及以上系统自带该 DLL，
# 放在 exe 旁反而会多出一个暴露给用户的文件。
if(WIN32 AND NOT VOILUT_PORTABLE)
  find_file(D3DCOMPILER47_DLL d3dcompiler_47.dll
    PATHS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Redist"
    PATH_SUFFIXES D3D/x64
    DOC "d3dcompiler_47.dll（Dawn D3D12 运行时依赖）")
  if(D3DCOMPILER47_DLL)
    add_custom_command(TARGET windower POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${D3DCOMPILER47_DLL}" "$<TARGET_FILE_DIR:windower>"
      COMMENT "复制 d3dcompiler_47.dll 到可执行文件目录")
  endif()
endif()

# ------------------------------------------------------------
# 解码测试（需要 libtiff；fixtures 由测试代码现场生成，不入库）
# ------------------------------------------------------------
if(BUILD_TESTING)
  add_executable(decode_test
    tests/decode_test.cxx
    imaging/src/decode/tiff_decode.cxx
  )
  target_include_directories(decode_test PRIVATE imaging/include)
  target_compile_options(decode_test PRIVATE ${PROJECT_WARNINGS})
  target_link_libraries(decode_test PRIVATE voilut_model ${TIFF_LINK_LIBRARIES})
  add_test(NAME decode_test COMMAND decode_test)

  add_executable(region_source_test
    tests/region_source_test.cxx
    imaging/src/decode/tiff_region_source.cxx
    imaging/src/decode/tiff_decode.cxx
  )
  target_include_directories(region_source_test PRIVATE imaging/include)
  target_compile_options(region_source_test PRIVATE ${PROJECT_WARNINGS})
  target_link_libraries(region_source_test PRIVATE voilut_model ${TIFF_LINK_LIBRARIES})
  add_test(NAME region_source_test COMMAND region_source_test)
endif()

# ------------------------------------------------------------
# API 文档（Doxygen）
# ------------------------------------------------------------
find_program(DOXYGEN_EXECUTABLE doxygen)
if(DOXYGEN_EXECUTABLE)
  add_custom_target(docs
    COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "生成 Doxygen API 文档 → build/doxygen/html")
else()
  message(STATUS "未找到 doxygen，跳过 docs 目标")
endif()
