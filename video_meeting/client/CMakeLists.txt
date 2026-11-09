# =============================================================================
# video_meeting 客户端构建脚本（Windows / MSVC / libmediasoupclient[webrtc m140]）
#
# ── 如何指定 mediasoupclient.lib ─────────────────────────────────────────────
# 三种方式任选其一（优先级从高到低）：
#
# 方式 A（推荐，最明确）：三个变量全给绝对路径
#   cmake -B build -G "Visual Studio 17 2022" -A x64 ^
#     -DMSC_LIB=D:\work\libmediasoupclient\build\Release\mediasoupclient.lib ^
#     -DLIBWEBRTC_BINARY_PATH=D:\work\libmediasoupclient\deps\libwebrtc\out\msvc\x64\Release ^
#     -DLIBWEBRTC_INCLUDE_PATH=D:\work\libmediasoupclient\deps\libwebrtc\src
#
# 方式 B：给 MSC_DIR（libmediasoupclient 工程根目录），按典型布局自动推导
#   cmake -B build -G "Visual Studio 17 2022" -A x64 -DMSC_DIR=D:\work\libmediasoupclient
#
# 方式 C：链接期才指定（不推荐）：只给 MSC_DIR，找不到再手补路径
#
# ── 三个路径怎么找（对照你当初编译 libmediasoupclient 的配置）────────────────
#   MSC_LIB              你编译产物 mediasoupclient.lib 的完整路径
#   LIBWEBRTC_BINARY_PATH 含 webrtc.lib（或多个 webrtc 组件 lib）的目录
#                        —— 即你当初 -DLIBWEBRTC_BINARY_PATH 传给 libmediasoupclient 的同一个值
#   LIBWEBRTC_INCLUDE_PATH webrtc 源码根目录（其下能直接看到 api/ rtc_base/ modules/ 等子目录）
#                        —— 即你当初 -DLIBWEBRTC_INCLUDE_PATH 的同一个值
# 自检：dir /s /b D:\work\libmediasoupclient\mediasoupclient.lib
#       dir /b <LIBWEBRTC_INCLUDE_PATH>\api   ← 应能看到 peer_connection_interface.h 等
# ────────────────────────────────────────────────────────────────────────────
#
# 硬编解码开关：
#   -DVM_HW_INJECT=ON  (默认) 要求 libmediasoupclient 已按 ../patches/README.md 打补丁
#   -DVM_HW_INJECT=OFF 兼容“不打补丁”的库：视频自动使用内建软编 VP8/VP9，
#                       SVC 自动降级单层，其余功能完全不受影响
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(video_meeting_client LANGUAGES C CXX)

set(MSC_DIR "" CACHE PATH "libmediasoupclient 工程根目录（方式 B/C 用）")
set(MSC_LIB "" CACHE FILEPATH "mediasoupclient.lib 完整路径（方式 A）")
set(LIBWEBRTC_BINARY_PATH "" CACHE PATH "webrtc.lib 所在目录（方式 A）")
set(LIBWEBRTC_INCLUDE_PATH "" CACHE PATH "webrtc 源码根目录，其下有 api/ rtc_base/ 等（方式 A）")
set(SDPTRANSFORM_LIB "" CACHE FILEPATH "sdptransform.lib 路径（留空则从 MSC_DIR 自动找）")
option(VM_HW_INJECT "注入 MF 硬编解码工厂到 libmediasoupclient（需先给 lib 打补丁）" ON)

# -----------------------------------------------------------------------------
# 路径解析：显式变量优先，缺失项从 MSC_DIR 典型布局推导
# -----------------------------------------------------------------------------
set(_MSC_LIB_PATH "${MSC_LIB}")
if(NOT _MSC_LIB_PATH)
  # 典型产物位置（按命中率排序逐个找）
  foreach(cand
      "${MSC_DIR}/build/Release/mediasoupclient.lib"
      "${MSC_DIR}/build/mediasoupclient.lib"
      "${MSC_DIR}/Release/mediasoupclient.lib"
      "${MSC_DIR}/mediasoupclient.lib")
    if(EXISTS "${cand}")
      set(_MSC_LIB_PATH "${cand}")
      break()
    endif()
  endforeach()
endif()

set(_WEBRTC_LIB_DIR "${LIBWEBRTC_BINARY_PATH}")
if(NOT _WEBRTC_LIB_DIR)
  foreach(cand
      "${MSC_DIR}/deps/libwebrtc/out/msvc/x64/Release"
      "${MSC_DIR}/deps/libwebrtc/out/Release"
      "${MSC_DIR}/deps/libwebrtc/lib/Release"
      "${MSC_DIR}/deps/libwebrtc/lib"
      "${MSC_DIR}/deps/libwebrtc/out/Debug")
    if(EXISTS "${cand}")
      set(_WEBRTC_LIB_DIR "${cand}")
      break()
    endif()
  endforeach()
endif()

set(_WEBRTC_INC "${LIBWEBRTC_INCLUDE_PATH}")
if(NOT _WEBRTC_INC)
  foreach(cand
      "${MSC_DIR}/deps/libwebrtc/src"
      "${MSC_DIR}/deps/libwebrtc")
    if(EXISTS "${cand}/api")
      set(_WEBRTC_INC "${cand}")
      break()
    endif()
  endforeach()
endif()

# sdptransform.lib（libmediasoupclient 的静态依赖，缺失会报 unresolved external）
set(_SDP_LIB "${SDPTRANSFORM_LIB}")
if(NOT _SDP_LIB)
  foreach(cand
      "${MSC_DIR}/build/_deps/libsdptransform-build/sdptransform.lib"
      "${MSC_DIR}/_deps/libsdptransform-build/sdptransform.lib")
    if(EXISTS "${cand}")
      set(_SDP_LIB "${cand}")
      break()
    endif()
  endforeach()
endif()

# nvidia lib
set(NV_CODEC_SDK_DIR "D:\work\free_code\video_meeting\client\third_party\nvidia")

# 校验并报告（找不到直接报错并提示怎么找，避免链接期才暴露）
if(NOT _MSC_LIB_PATH)
  message(FATAL_ERROR
    "找不到 mediasoupclient.lib！请用 -DMSC_LIB=<完整路径> 指定。\n"
    "  查找命令: dir /s /b <你的libmediasoupclient目录>\\mediasoupclient.lib")
endif()
if(NOT _WEBRTC_INC)
  message(FATAL_ERROR
    "找不到 webrtc 头文件根目录（其下应有 api/ rtc_base/ 等）！\n"
    "  请用 -DLIBWEBRTC_INCLUDE_PATH=<webrtc源码根目录> 指定\n"
    "  （与你编译 libmediasoupclient 时传的 -DLIBWEBRTC_INCLUDE_PATH 相同）")
endif()
if(NOT _WEBRTC_LIB_DIR)
  # 头文件有了、lib 目录没找到：给出警告继续（可能 mediasoupclient.lib 静态已含）
  message(WARNING
    "未找到 webrtc.lib 目录，请用 -DLIBWEBRTC_BINARY_PATH=<webrtc.lib所在目录> 指定\n"
    "（与你编译 libmediasoupclient 时传的 -DLIBWEBRTC_BINARY_PATH 相同）")
endif()

message(STATUS "mediasoupclient.lib : ${_MSC_LIB_PATH}")
message(STATUS "sdptransform.lib    : ${_SDP_LIB}")
message(STATUS "webrtc lib 目录     : ${_WEBRTC_LIB_DIR}")
message(STATUS "webrtc 头文件根目录 : ${_WEBRTC_INC}")
message(STATUS "硬编解码注入        : ${VM_HW_INJECT}")

# webrtc m140 公共头要求 C++20；与库保持一致（如你的库用 C++17 编译，改 17 即可）
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码含中文注释，MSVC 必须 /utf-8，否则可能 C4819/C2001
add_compile_options(/utf-8)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_compile_options(/EHsc)
endif()

# 运行时库默认 /MD（与预编译 libwebrtc 一致）。若你的库是 /MT 构建，取消下一行注释：
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(client
  src/main.cpp
  src/room/room_client.cpp
  src/protoo/protoo_client.cpp
  src/net/ws_client.cpp
  src/media/media_engine.cpp
  src/media/camera_capturer.cpp
  src/media/pattern_source.cpp
  src/media/screen_capturer.cpp
  src/media/audio_capture.cpp
  src/media/d3d11_renderer.cpp
  src/media/hw_codecs/d3d11_device.cpp
  src/media/hw_codecs/mf_video_encoder.cpp
  src/media/hw_codecs/mf_encoder_factory.cpp
  src/media/hw_codecs/mf_video_decoder.cpp
  src/media/hw_codecs/mf_decoder_factory.cpp
  src/util/task_queue.cpp
)

set_target_properties(client PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(_LIBCXX_SRC_INC AND _LIBCXX_CONFIG_INC)
  message(STATUS "libc++ src include   : ${_LIBCXX_SRC_INC}")
  message(STATUS "libc++ config include: ${_LIBCXX_CONFIG_INC}")
else()
  message(WARNING "未找到 libc++ 头文件目录，clang-cl + libc++ 构建可能失败。\n"
    "  请用 -D_LIBCXX_SRC_INC=... 和 -D_LIBCXX_CONFIG_INC=... 指定")
endif()

target_include_directories(client PRIVATE
  src
  third_party                 # json.hpp（本客户端代码用）
  third_party/IXWebSocket     # IXWebSocket 头文件
  "${NV_CODEC_SDK_DIR}/include"  # NVENC/NVDEC 头文件
  ${MSC_DIR}/include          # mediasoupclient.hpp（libmediasoupclient 工程的 include/）
  ${_WEBRTC_INC}              # webrtc 根（api/ rtc_base/ modules/ ...）
  ${_WEBRTC_INC}/third_party/abseil-cpp
  ${_WEBRTC_INC}/third_party/libyuv/include
)

target_compile_definitions(client PRIVATE
  WEBRTC_WIN
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

if(VM_HW_INJECT)
  target_compile_definitions(client PRIVATE VM_HW_INJECT=1)   # 启用硬编解码注入（见 media_engine.cpp）
endif()

# webrtc lib 目录加入链接搜索路径
if(_WEBRTC_LIB_DIR)
  link_directories("${_WEBRTC_LIB_DIR}")
endif()

# =============================================================
# 【lib 导入位置】mediasoupclient.lib 在这里被导入
#   - _MSC_LIB_PATH 用绝对路径直接链接（不依赖搜索目录）
#   - webrtc 按名字链接（来自上面 link_directories 的目录）
#   若你的 libwebrtc 拆成多个 lib（如 webrtc.lib + 各组件），缺什么符号
#   就把对应 lib 名补在 webrtc 后面（都在 _WEBRTC_LIB_DIR 里）
# =============================================================
set(_LIBCXX_LIB "" CACHE FILEPATH "libc++.lib 路径（webrtc Chromium 构建产物；留空则自动推导）")
if(NOT _LIBCXX_LIB)
  foreach(cand
      "${_WEBRTC_LIB_DIR}/../buildtools/third_party/libc++/libc++.lib"
      "${_WEBRTC_LIB_DIR}/../../buildtools/third_party/libc++/libc++.lib")
    if(EXISTS "${cand}")
      set(_LIBCXX_LIB "${cand}")
      break()
    endif()
  endforeach()
endif()
if(_LIBCXX_LIB)
  message(STATUS "libc++.lib            : ${_LIBCXX_LIB}")
else()
  message(WARNING "未找到 libc++.lib，webrtc 内部 libc++ 符号可能链接失败。\n"
    "  请用 -D_LIBCXX_LIB=<libc++.lib路径> 指定\n"
    "  或从 webrtc 构建产物的 libc++ .obj 打包：lib /out:libc++.lib *.obj")
endif()

target_link_libraries(client PRIVATE
  "${_MSC_LIB_PATH}"       # ←←← mediasoupclient.lib 导入点
  "${_SDP_LIB}"            # mediasoupclient 的静态依赖
  "${_WEBRTC_LIB_DIR}/webrtc.lib"   # ←←← webrtc.lib（完整路径，不依赖链接目录）
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/IXWebSocket/build/ixwebsocket.lib"
  "${NV_CODEC_SDK_DIR}/lib/nvEncodeAPI64.lib"
  "${NV_CODEC_SDK_DIR}/lib/nvcuvid.lib"
  # —— 系统库（全部系统自带，零第三方依赖；winmm/dwmapi 等为 webrtc Windows 依赖）——
  ws2_32 secur32 crypt32 winmm dwmapi iphlpapi
  dmoguids wmcodecdspuuid amstrmid msdmo   # webrtc 媒体基础依赖
  user32 gdi32 shell32                    # 窗口
  d3d11 dxgi d3dcompiler                  # 渲染 + MF 编解码共享设备
  mfplat mfuuid mfreadwrite ole32         # Media Foundation 硬编解码
  shcore
)

# if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
#   set_target_properties(client PROPERTIES LINK_FLAGS "/FORCE:MULTIPLE")
# endif()