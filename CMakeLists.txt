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

project(voilut LANGUAGES CXX)

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
  set(PROJECT_WARNINGS /W4 /permissive-)
else()
  set(PROJECT_WARNINGS -Wall -Wextra -Wpedantic)
endif()

# ------------------------------------------------------------
# 依赖查找
# ------------------------------------------------------------
# TIFF：16 位灰度图像解码（系统包 / vcpkg 均可提供 TIFF::TIFF）。
find_package(TIFF REQUIRED)

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
# 职责：解码 16 位像素 → 统计（直方图 / p1 p99）→ 窗设置 → GPU 渲染管线。
add_library(voilut STATIC
  imaging/src/image_model.cxx
  imaging/src/image_renderer.cxx
  imaging/src/image_source.cxx
  imaging/src/viewport.cxx
  imaging/src/orientation.cxx
  imaging/src/decode/image_decode.cxx
  imaging/src/decode/tiff_decode.cxx
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

target_link_libraries(voilut PUBLIC voilut_core PRIVATE TIFF::TIFF gdcmMSFF)

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
add_executable(windower apps/windower.cxx)
target_compile_options(windower PRIVATE ${PROJECT_WARNINGS})
target_link_libraries(windower PRIVATE voilut imgui)

# 着色器随可执行文件分发：运行时通过可执行文件旁的 shaders/ 查找，
# 不依赖源码树的绝对路径。每次构建都同步一次，保证修改 WGSL 后立即生效。
add_custom_target(windower_shaders ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:windower>/shaders"
  COMMENT "复制 shaders/ 到可执行文件目录")
add_dependencies(windower windower_shaders)

# Windows：Dawn 的 D3D12 后端运行时需要 d3dcompiler_47.dll（由 Windows SDK 提供）。
if(WIN32)
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
